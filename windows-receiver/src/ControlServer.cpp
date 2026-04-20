#include "ControlServer.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <array>
#include <chrono>
#include <regex>
#include <sstream>
#include <utility>

namespace {

constexpr SOCKET kInvalidSocket = INVALID_SOCKET;
constexpr int kMaxStreamFps = 60;
constexpr int kMinWirelessBitrate = 4'000'000;

int SelectWirelessBitrateCap(const protocol::StreamProfile& profile) {
    const bool high_refresh = profile.fps >= 55 || (profile.adaptive_fps && profile.fps >= 50);

    if (profile.width >= 2560 || profile.height >= 1440) {
        return high_refresh ? 24'000'000 : 18'000'000;
    }

    if (profile.width >= 1920 || profile.height >= 1080) {
        return high_refresh ? 16'000'000 : 12'000'000;
    }

    if (profile.width >= 1280 || profile.height >= 720) {
        return high_refresh ? 8'000'000 : 5'000'000;
    }

    return 4'000'000;
}

int64_t NowSteadyUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::wstring SocketErrorText(const wchar_t* action) {
    std::wostringstream stream;
    stream << action << L"\uff0cWSA \u9519\u8bef\u7801=" << WSAGetLastError();
    return stream.str();
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return std::wstring(value.begin(), value.end());
    }

    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring ProfileFrameRateText(const protocol::StreamProfile& profile) {
    if (profile.adaptive_fps) {
        return L"\u81ea\u9002\u5e94\u5237\u65b0\u7387";
    }

    std::wostringstream stream;
    stream << profile.fps << L"fps";
    return stream.str();
}

std::wstring ProfileToString(const protocol::StreamProfile& profile) {
    std::wostringstream stream;
    stream << L"\u7f16\u7801=" << Utf8ToWide(protocol::CodecToWireName(profile.codec))
           << L" " << profile.width << L"x" << profile.height
           << L" @" << ProfileFrameRateText(profile)
           << L" \u7801\u7387=" << profile.bitrate
           << L" \u89c6\u9891\u7aef\u53e3=" << profile.video_port;
    if (profile.audio_enabled) {
        stream << L" \u97f3\u9891=UDP/" << profile.audio_port
               << L" " << profile.audio_sample_rate << L"Hz/"
               << profile.audio_channels << L"ch";
    } else {
        stream << L" \u97f3\u9891=\u5173\u95ed";
    }
    return stream.str();
}

bool IsWithinFrameRateCap(const protocol::StreamProfile& profile) {
    return profile.fps > 0 && profile.fps <= kMaxStreamFps;
}

protocol::StreamProfile CapProfileTo60Fps(protocol::StreamProfile profile) {
    if (profile.fps > kMaxStreamFps) {
        profile.fps = kMaxStreamFps;
    }
    return profile;
}

protocol::StreamProfile ApplyWirelessStabilityPolicy(protocol::StreamProfile profile) {
    profile = CapProfileTo60Fps(profile);
    const int bitrate_cap = std::max(kMinWirelessBitrate, SelectWirelessBitrateCap(profile));
    if (profile.bitrate <= 0 || profile.bitrate > bitrate_cap) {
        profile.bitrate = bitrate_cap;
    }
    return profile;
}

std::string ReadLine(SOCKET socket) {
    std::string line;
    char byte = '\0';
    while (true) {
        const int received = recv(socket, &byte, 1, 0);
        if (received <= 0) {
            return {};
        }
        if (byte == '\n') {
            break;
        }
        if (byte != '\r') {
            line.push_back(byte);
        }
    }
    return line;
}

bool SendLine(SOCKET socket, const std::string& line) {
    std::string payload = line + "\n";
    const char* data = payload.data();
    int remaining = static_cast<int>(payload.size());
    while (remaining > 0) {
        const int sent = send(socket, data, remaining, 0);
        if (sent <= 0) {
            return false;
        }
        data += sent;
        remaining -= sent;
    }
    return true;
}

std::string BuildTimeSyncRequestJson(uint32_t sync_id, int64_t receiver_send_us) {
    std::ostringstream stream;
    stream << "{\"type\":\"TIME_SYNC_REQUEST\""
           << ",\"syncId\":" << sync_id
           << ",\"receiverSendUs\":" << receiver_send_us
           << "}";
    return stream.str();
}

bool ParseTimeSyncResponse(
    const std::string& json_line,
    uint32_t* sync_id,
    int64_t* receiver_send_us,
    int64_t* sender_receive_us,
    int64_t* sender_send_us) {
    const std::regex response_regex(
        R"json("type"\s*:\s*"TIME_SYNC_RESPONSE".*"syncId"\s*:\s*(\d+).*"receiverSendUs"\s*:\s*(\d+).*"senderReceiveUs"\s*:\s*(\d+).*"senderSendUs"\s*:\s*(\d+))json");
    std::smatch match;
    if (!std::regex_search(json_line, match, response_regex)) {
        return false;
    }

    if (sync_id != nullptr) {
        *sync_id = static_cast<uint32_t>(std::stoul(match[1].str()));
    }
    if (receiver_send_us != nullptr) {
        *receiver_send_us = std::stoll(match[2].str());
    }
    if (sender_receive_us != nullptr) {
        *sender_receive_us = std::stoll(match[3].str());
    }
    if (sender_send_us != nullptr) {
        *sender_send_us = std::stoll(match[4].str());
    }
    return true;
}

}  // namespace

ControlServer::ControlServer(
    LogFn log_fn,
    ProfileFn profile_fn,
    TimeSyncFn time_sync_fn,
    SessionFn session_fn)
    : log_fn_(std::move(log_fn)),
      profile_fn_(std::move(profile_fn)),
      time_sync_fn_(std::move(time_sync_fn)),
      session_fn_(std::move(session_fn)) {}

ControlServer::~ControlServer() {
    Stop();
}

bool ControlServer::Start(uint16_t control_port, uint16_t video_port, uint16_t audio_port) {
    if (running_.exchange(true)) {
        return false;
    }

    thread_ = std::thread([this, control_port, video_port, audio_port] {
        ThreadMain(control_port, video_port, audio_port);
    });
    return true;
}

void ControlServer::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (listen_socket_ != static_cast<uintptr_t>(kInvalidSocket)) {
        closesocket(static_cast<SOCKET>(listen_socket_));
        listen_socket_ = static_cast<uintptr_t>(kInvalidSocket);
    }

    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        pending_sync_requests_.clear();
        if (client_socket_ != static_cast<uintptr_t>(kInvalidSocket)) {
            shutdown(static_cast<SOCKET>(client_socket_), SD_BOTH);
            closesocket(static_cast<SOCKET>(client_socket_));
            client_socket_ = static_cast<uintptr_t>(kInvalidSocket);
        }
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

bool ControlServer::RequestIdr() {
    std::lock_guard<std::mutex> lock(client_mutex_);
    if (client_socket_ == static_cast<uintptr_t>(kInvalidSocket)) {
        return false;
    }

    if (!SendLine(static_cast<SOCKET>(client_socket_), "{\"type\":\"REQUEST_IDR\"}")) {
        log_fn_(L"\u63a7\u5236\u670d\u52a1: \u8bf7\u6c42\u5173\u952e\u5e27\u5931\u8d25\u3002");
        return false;
    }

    log_fn_(L"\u63a7\u5236\u670d\u52a1: \u5df2\u5411\u5b89\u5353\u7aef\u8bf7\u6c42\u5173\u952e\u5e27\u3002");
    return true;
}

bool ControlServer::RequestTimeSync() {
    std::lock_guard<std::mutex> lock(client_mutex_);
    if (client_socket_ == static_cast<uintptr_t>(kInvalidSocket)) {
        return false;
    }

    const uint32_t sync_id = next_sync_id_.fetch_add(1);
    const int64_t receiver_send_us = NowSteadyUs();
    pending_sync_requests_[sync_id] = receiver_send_us;
    if (!SendLine(static_cast<SOCKET>(client_socket_), BuildTimeSyncRequestJson(sync_id, receiver_send_us))) {
        pending_sync_requests_.erase(sync_id);
        log_fn_(L"\u63a7\u5236\u670d\u52a1: \u53d1\u9001\u65f6\u949f\u540c\u6b65\u8bf7\u6c42\u5931\u8d25\u3002");
        return false;
    }
    return true;
}

void ControlServer::ThreadMain(uint16_t control_port, uint16_t video_port, uint16_t audio_port) {
    SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == kInvalidSocket) {
        log_fn_(L"\u63a7\u5236\u670d\u52a1: " + SocketErrorText(L"\u521b\u5efa TCP \u5957\u63a5\u5b57\u5931\u8d25"));
        running_ = false;
        return;
    }

    listen_socket_ = static_cast<uintptr_t>(listen_socket);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(control_port);

    int reuse = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    if (bind(listen_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        log_fn_(L"\u63a7\u5236\u670d\u52a1: " + SocketErrorText(L"\u7ed1\u5b9a TCP \u7aef\u53e3\u5931\u8d25"));
        closesocket(listen_socket);
        listen_socket_ = static_cast<uintptr_t>(kInvalidSocket);
        running_ = false;
        return;
    }

    if (listen(listen_socket, 1) == SOCKET_ERROR) {
        log_fn_(L"\u63a7\u5236\u670d\u52a1: " + SocketErrorText(L"\u5f00\u59cb\u76d1\u542c\u5931\u8d25"));
        closesocket(listen_socket);
        listen_socket_ = static_cast<uintptr_t>(kInvalidSocket);
        running_ = false;
        return;
    }

    {
        std::wostringstream stream;
        stream << L"\u63a7\u5236\u670d\u52a1: \u6b63\u5728\u76d1\u542c TCP/" << control_port;
        log_fn_(stream.str());
    }

    while (running_) {
        sockaddr_in client_address{};
        int client_length = sizeof(client_address);
        SOCKET client_socket = accept(listen_socket, reinterpret_cast<sockaddr*>(&client_address), &client_length);
        if (client_socket == kInvalidSocket) {
            if (running_) {
                log_fn_(L"\u63a7\u5236\u670d\u52a1: " + SocketErrorText(L"\u63a5\u53d7\u8fde\u63a5\u5931\u8d25"));
            }
            break;
        }

        wchar_t host_buffer[64] = {};
        InetNtopW(AF_INET, &client_address.sin_addr, host_buffer, static_cast<DWORD>(std::size(host_buffer)));
        std::wostringstream stream;
        stream << L"\u63a7\u5236\u670d\u52a1: \u53d1\u9001\u7aef\u5df2\u8fde\u63a5\uff0c\u6765\u6e90\u5730\u5740 " << host_buffer;
        log_fn_(stream.str());

        {
            std::lock_guard<std::mutex> lock(client_mutex_);
            pending_sync_requests_.clear();
            client_socket_ = static_cast<uintptr_t>(client_socket);
        }

        HandleClient(static_cast<uintptr_t>(client_socket), video_port, audio_port, host_buffer);

        {
            std::lock_guard<std::mutex> lock(client_mutex_);
            pending_sync_requests_.clear();
            if (client_socket_ == static_cast<uintptr_t>(client_socket)) {
                client_socket_ = static_cast<uintptr_t>(kInvalidSocket);
            }
        }
        closesocket(client_socket);
    }

    closesocket(listen_socket);
    listen_socket_ = static_cast<uintptr_t>(kInvalidSocket);
    running_ = false;
}

void ControlServer::HandleClient(
    uintptr_t client_socket_value,
    uint16_t video_port,
    uint16_t audio_port,
    const std::wstring& sender_host) {
    SOCKET client_socket = static_cast<SOCKET>(client_socket_value);

    const std::string hello_line = ReadLine(client_socket);
    if (hello_line.empty()) {
        log_fn_(L"\u63a7\u5236\u670d\u52a1: \u8bfb\u53d6 HELLO \u6d88\u606f\u5931\u8d25\u3002");
        return;
    }

    protocol::HelloMessage hello;
    std::string error;
    if (!protocol::ParseHelloMessage(hello_line, &hello, &error)) {
        log_fn_(L"\u63a7\u5236\u670d\u52a1: HELLO \u6d88\u606f\u683c\u5f0f\u65e0\u6548\u3002");
        return;
    }

    std::wostringstream hello_stream;
    hello_stream << L"\u63a7\u5236\u670d\u52a1: \u6536\u5230\u6765\u81ea " << hello.device_name
                 << L" \u7684 HELLO\uff0c\u53ef\u9009\u914d\u7f6e\u6570\u91cf=" << hello.profiles.size()
                 << L"\uff0c\u97f3\u9891=" << (hello.audio_enabled ? L"\u53ef\u7528" : L"\u5173\u95ed");
    log_fn_(hello_stream.str());
    if (session_fn_) {
        session_fn_(sender_host, hello.device_name, true);
    }

    protocol::StreamProfile requested = ChooseProfile(hello);
    protocol::StreamProfile selected = ApplyWirelessStabilityPolicy(requested);
    selected.video_port = video_port;
    if (hello.audio_enabled && hello.audio_sample_rate > 0 && hello.audio_channels > 0) {
        selected.audio_enabled = true;
        selected.audio_port = audio_port;
        selected.audio_sample_rate = hello.audio_sample_rate;
        selected.audio_channels = hello.audio_channels;
    }

    if (!SendLine(client_socket, protocol::BuildSelectProfileJson(selected))) {
        log_fn_(L"\u63a7\u5236\u670d\u52a1: \u53d1\u9001 SELECT_PROFILE \u5931\u8d25\u3002");
        return;
    }

    log_fn_(L"\u63a7\u5236\u670d\u52a1: \u5df2\u9009\u62e9\u914d\u7f6e " + ProfileToString(selected));
    if (selected.bitrate != requested.bitrate || selected.fps != requested.fps) {
        std::wostringstream policy_stream;
        policy_stream << L"\u63A7\u5236\u670D\u52A1\uFF1A\u5DF2\u5957\u7528\u4FDD\u5B88\u578B\u65E0\u7EBF\u7A33\u5B9A\u7B56\u7565\uFF0C"
                      << L"\u7801\u7387 " << requested.bitrate << L" -> " << selected.bitrate;
        if (selected.fps != requested.fps) {
            policy_stream << L"\uff0cfps " << requested.fps << L" -> " << selected.fps;
        }
        policy_stream << L"\uFF0C\u4F18\u5148\u51CF\u5C11\u5FEB\u901F\u6643\u52D5\u65F6\u7684\u82B1\u5C4F\u548C\u679C\u51BB\u3002";
        log_fn_(policy_stream.str());
    }
    profile_fn_(selected);
    RequestTimeSync();

    while (running_) {
        const std::string line = ReadLine(client_socket);
        if (line.empty()) {
            log_fn_(L"\u63a7\u5236\u670d\u52a1: \u53d1\u9001\u7aef\u5df2\u65ad\u5f00\u8fde\u63a5\u3002");
            break;
        }

        uint32_t sync_id = 0;
        int64_t receiver_send_us = 0;
        int64_t sender_receive_us = 0;
        int64_t sender_send_us = 0;
        if (ParseTimeSyncResponse(line, &sync_id, &receiver_send_us, &sender_receive_us, &sender_send_us)) {
            const int64_t receiver_receive_us = NowSteadyUs();
            int64_t original_receiver_send_us = receiver_send_us;
            {
                std::lock_guard<std::mutex> lock(client_mutex_);
                const auto it = pending_sync_requests_.find(sync_id);
                if (it != pending_sync_requests_.end()) {
                    original_receiver_send_us = it->second;
                    pending_sync_requests_.erase(it);
                }
            }

            const int64_t rtt_us = (receiver_receive_us - original_receiver_send_us) - (sender_send_us - sender_receive_us);
            const int64_t offset_us =
                ((sender_receive_us - original_receiver_send_us) + (sender_send_us - receiver_receive_us)) / 2;

            if (time_sync_fn_) {
                time_sync_fn_(offset_us, std::max<int64_t>(0, rtt_us));
            }
            continue;
        }

        log_fn_(L"\u63a7\u5236\u670d\u52a1: \u6536\u5230\u6d88\u606f " + Utf8ToWide(line));
    }

    if (session_fn_) {
        session_fn_(sender_host, hello.device_name, false);
    }
}

protocol::StreamProfile ControlServer::ChooseProfile(const protocol::HelloMessage& hello) const {
    for (const auto& profile : hello.profiles) {
        if (profile.codec == protocol::Codec::kAvc &&
            profile.width == 1920 &&
            profile.height == 1080 &&
            profile.adaptive_fps &&
            IsWithinFrameRateCap(profile)) {
            return CapProfileTo60Fps(profile);
        }
    }

    for (const auto& profile : hello.profiles) {
        if (profile.codec == protocol::Codec::kAvc &&
            profile.width == 1920 &&
            profile.height == 1080 &&
            profile.fps == 60 &&
            IsWithinFrameRateCap(profile)) {
            return CapProfileTo60Fps(profile);
        }
    }

    for (const auto& profile : hello.profiles) {
        if (profile.codec == protocol::Codec::kAvc &&
            profile.width == 2560 &&
            profile.height == 1440 &&
            profile.fps == 30 &&
            IsWithinFrameRateCap(profile)) {
            return CapProfileTo60Fps(profile);
        }
    }

    for (const auto& profile : hello.profiles) {
        if (profile.codec == protocol::Codec::kAvc &&
            profile.width == 2560 &&
            profile.height == 1440 &&
            profile.fps == 60 &&
            IsWithinFrameRateCap(profile)) {
            return CapProfileTo60Fps(profile);
        }
    }

    for (const auto& profile : hello.profiles) {
        if (profile.codec == protocol::Codec::kAvc &&
            profile.adaptive_fps &&
            IsWithinFrameRateCap(profile)) {
            return CapProfileTo60Fps(profile);
        }
    }

    for (const auto& profile : hello.profiles) {
        if (profile.codec == protocol::Codec::kAvc &&
            IsWithinFrameRateCap(profile)) {
            return CapProfileTo60Fps(profile);
        }
    }

    return CapProfileTo60Fps(hello.profiles.front());
}
