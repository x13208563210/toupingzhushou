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

int64_t NowSteadyUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::wstring SocketErrorText(const wchar_t* action) {
    std::wostringstream stream;
    stream << action << L"，WSA 错误码=" << WSAGetLastError();
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
        return L"\u81EA\u9002\u5E94\u5237\u65B0\u7387";
    }

    std::wostringstream stream;
    stream << profile.fps << L"fps";
    return stream.str();
}

std::wstring ProfileToString(const protocol::StreamProfile& profile) {
    std::wostringstream stream;
    stream << L"编码=" << Utf8ToWide(protocol::CodecToWireName(profile.codec))
           << L" " << profile.width << L"x" << profile.height
           << L" @" << ProfileFrameRateText(profile)
           << L" 码率=" << profile.bitrate
           << L" 视频端口=" << profile.video_port;
    return stream.str();
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

ControlServer::ControlServer(LogFn log_fn, ProfileFn profile_fn, TimeSyncFn time_sync_fn)
    : log_fn_(std::move(log_fn)),
      profile_fn_(std::move(profile_fn)),
      time_sync_fn_(std::move(time_sync_fn)) {}

ControlServer::~ControlServer() {
    Stop();
}

bool ControlServer::Start(uint16_t control_port, uint16_t video_port) {
    if (running_.exchange(true)) {
        return false;
    }

    thread_ = std::thread([this, control_port, video_port] {
        ThreadMain(control_port, video_port);
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
        log_fn_(L"控制服务: 请求关键帧失败。");
        return false;
    }

    log_fn_(L"控制服务: 已向安卓端请求关键帧。");
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
        return false;
    }
    return true;
}

void ControlServer::ThreadMain(uint16_t control_port, uint16_t video_port) {
    SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == kInvalidSocket) {
        log_fn_(L"控制服务: " + SocketErrorText(L"创建 TCP 套接字失败"));
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
        log_fn_(L"控制服务: " + SocketErrorText(L"绑定 TCP 端口失败"));
        closesocket(listen_socket);
        listen_socket_ = static_cast<uintptr_t>(kInvalidSocket);
        running_ = false;
        return;
    }

    if (listen(listen_socket, 1) == SOCKET_ERROR) {
        log_fn_(L"控制服务: " + SocketErrorText(L"开始监听失败"));
        closesocket(listen_socket);
        listen_socket_ = static_cast<uintptr_t>(kInvalidSocket);
        running_ = false;
        return;
    }

    {
        std::wostringstream stream;
        stream << L"控制服务: 正在监听 TCP/" << control_port;
        log_fn_(stream.str());
    }

    while (running_) {
        sockaddr_in client_address{};
        int client_length = sizeof(client_address);
        SOCKET client_socket = accept(listen_socket, reinterpret_cast<sockaddr*>(&client_address), &client_length);
        if (client_socket == kInvalidSocket) {
            if (running_) {
                log_fn_(L"控制服务: " + SocketErrorText(L"接受连接失败"));
            }
            break;
        }

        wchar_t host_buffer[64] = {};
        InetNtopW(AF_INET, &client_address.sin_addr, host_buffer, static_cast<DWORD>(std::size(host_buffer)));
        std::wostringstream stream;
        stream << L"控制服务: 发送端已连接，来源地址 " << host_buffer;
        log_fn_(stream.str());

        {
            std::lock_guard<std::mutex> lock(client_mutex_);
            pending_sync_requests_.clear();
            client_socket_ = static_cast<uintptr_t>(client_socket);
        }

        HandleClient(static_cast<uintptr_t>(client_socket), video_port);

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

void ControlServer::HandleClient(uintptr_t client_socket_value, uint16_t video_port) {
    SOCKET client_socket = static_cast<SOCKET>(client_socket_value);

    const std::string hello_line = ReadLine(client_socket);
    if (hello_line.empty()) {
        log_fn_(L"控制服务: 读取 HELLO 消息失败。");
        return;
    }

    protocol::HelloMessage hello;
    std::string error;
    if (!protocol::ParseHelloMessage(hello_line, &hello, &error)) {
        log_fn_(L"控制服务: HELLO 消息格式无效。");
        return;
    }

    std::wostringstream hello_stream;
    hello_stream << L"控制服务: 收到来自 " << hello.device_name
                 << L" 的 HELLO，可选配置数量=" << hello.profiles.size();
    log_fn_(hello_stream.str());

    protocol::StreamProfile selected = ChooseProfile(hello);
    selected.video_port = video_port;

    if (!SendLine(client_socket, protocol::BuildSelectProfileJson(selected))) {
        log_fn_(L"控制服务: 发送 SELECT_PROFILE 失败。");
        return;
    }

    log_fn_(L"控制服务: 已选择配置 " + ProfileToString(selected));
    profile_fn_(selected);

    while (running_) {
        const std::string line = ReadLine(client_socket);
        if (line.empty()) {
            log_fn_(L"控制服务: 发送端已断开连接。");
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

        log_fn_(L"控制服务: 收到消息 " + Utf8ToWide(line));
    }
}

protocol::StreamProfile ControlServer::ChooseProfile(const protocol::HelloMessage& hello) const {
    for (const auto& profile : hello.profiles) {
        if (profile.codec == protocol::Codec::kAvc &&
            profile.width == 1920 &&
            profile.height == 1080 &&
            profile.adaptive_fps) {
            return profile;
        }
    }

    for (const auto& profile : hello.profiles) {
        if (profile.codec == protocol::Codec::kAvc &&
            profile.width == 1920 &&
            profile.height == 1080 &&
            profile.fps == 60) {
            return profile;
        }
    }

    for (const auto& profile : hello.profiles) {
        if (profile.codec == protocol::Codec::kAvc &&
            profile.width == 2560 &&
            profile.height == 1440 &&
            profile.fps == 30) {
            return profile;
        }
    }

    for (const auto& profile : hello.profiles) {
        if (profile.codec == protocol::Codec::kAvc &&
            profile.width == 2560 &&
            profile.height == 1440 &&
            profile.fps == 60) {
            return profile;
        }
    }

    for (const auto& profile : hello.profiles) {
        if (profile.codec == protocol::Codec::kAvc &&
            profile.adaptive_fps) {
            return profile;
        }
    }

    for (const auto& profile : hello.profiles) {
        if (profile.codec == protocol::Codec::kAvc) {
            return profile;
        }
    }

    return hello.profiles.front();
}
