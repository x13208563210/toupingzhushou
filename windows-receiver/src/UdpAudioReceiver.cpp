#include "UdpAudioReceiver.h"

#include <WinSock2.h>

#include <array>
#include <sstream>

namespace {

constexpr SOCKET kInvalidSocket = INVALID_SOCKET;
constexpr uint32_t kAudioStreamId = 2;
constexpr uint32_t kFrameIdResetThreshold = 128;

std::wstring SocketErrorText(const wchar_t* action) {
    std::wostringstream stream;
    stream << action << L"\uff0cWSA \u9519\u8bef\u7801=" << WSAGetLastError();
    return stream.str();
}

}  // namespace

UdpAudioReceiver::UdpAudioReceiver(LogFn log_fn, AccessUnitFn access_unit_fn)
    : log_fn_(std::move(log_fn)), access_unit_fn_(std::move(access_unit_fn)) {}

UdpAudioReceiver::~UdpAudioReceiver() {
    Stop();
}

bool UdpAudioReceiver::Start(uint16_t audio_port) {
    if (running_.exchange(true)) {
        return false;
    }

    thread_ = std::thread([this, audio_port] {
        ThreadMain(audio_port);
    });
    return true;
}

void UdpAudioReceiver::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (socket_ != static_cast<uintptr_t>(kInvalidSocket)) {
        closesocket(static_cast<SOCKET>(socket_));
        socket_ = static_cast<uintptr_t>(kInvalidSocket);
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

AudioStats UdpAudioReceiver::GetStats() const {
    AudioStats stats;
    stats.total_packets = total_packets_.load();
    stats.total_bytes = total_bytes_.load();
    stats.completed_frames = completed_frames_.load();
    stats.dropped_frames = dropped_frames_.load();
    stats.last_frame_id = last_frame_id_.load();
    stats.last_pts_us = last_pts_us_.load();
    return stats;
}

void UdpAudioReceiver::ThreadMain(uint16_t audio_port) {
    SOCKET udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_socket == kInvalidSocket) {
        log_fn_(L"\u97f3\u9891\u63a5\u6536: " + SocketErrorText(L"\u521b\u5efa UDP \u5957\u63a5\u5b57\u5931\u8d25"));
        running_ = false;
        return;
    }

    socket_ = static_cast<uintptr_t>(udp_socket);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(audio_port);

    if (bind(udp_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        log_fn_(L"\u97f3\u9891\u63a5\u6536: " + SocketErrorText(L"\u7ed1\u5b9a UDP \u7aef\u53e3\u5931\u8d25"));
        closesocket(udp_socket);
        socket_ = static_cast<uintptr_t>(kInvalidSocket);
        running_ = false;
        return;
    }

    {
        std::wostringstream stream;
        stream << L"\u97f3\u9891\u63a5\u6536: \u6b63\u5728\u76d1\u542c UDP/" << audio_port;
        log_fn_(stream.str());
    }

    std::array<uint8_t, 65536> buffer{};
    while (running_) {
        sockaddr_in from{};
        int from_length = sizeof(from);
        const int received = recvfrom(
            udp_socket,
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &from_length);

        if (received <= 0) {
            if (running_) {
                log_fn_(L"\u97f3\u9891\u63a5\u6536: " + SocketErrorText(L"\u63a5\u6536 UDP \u6570\u636e\u5931\u8d25"));
            }
            break;
        }

        protocol::PacketHeader header;
        if (!protocol::ParsePacketHeader(buffer.data(), static_cast<size_t>(received), &header)) {
            continue;
        }
        if (header.stream_id != kAudioStreamId) {
            continue;
        }

        OnPacket(
            header,
            buffer.data() + 32,
            static_cast<size_t>(received));
    }

    closesocket(udp_socket);
    socket_ = static_cast<uintptr_t>(kInvalidSocket);
    running_ = false;
}

void UdpAudioReceiver::OnPacket(const protocol::PacketHeader& header, const uint8_t* payload, size_t datagram_size) {
    const uint32_t previous_frame_id = last_frame_id_.load();
    if (previous_frame_id > header.frame_id && previous_frame_id - header.frame_id > kFrameIdResetThreshold) {
        reassembler_.Reset();
        log_fn_(L"\u97f3\u9891\u63a5\u6536: \u68c0\u6d4b\u5230\u65b0\u7684\u97f3\u9891\u4f1a\u8bdd\uff0c\u5df2\u6e05\u7a7a\u91cd\u7ec4\u7f13\u5b58\u3002");
    }

    total_packets_.fetch_add(1);
    total_bytes_.fetch_add(datagram_size);
    last_frame_id_.store(header.frame_id);
    last_pts_us_.store(header.pts_us);

    const size_t payload_size = datagram_size >= 32 ? datagram_size - 32 : 0;
    const auto access_unit = reassembler_.AddPacket(header, payload, payload_size);
    const auto reassembler_stats = reassembler_.GetStats();
    completed_frames_.store(reassembler_stats.completed_frames);
    dropped_frames_.store(reassembler_stats.dropped_frames);

    if (access_unit.has_value() && access_unit_fn_) {
        access_unit_fn_(access_unit.value());
    }
}
