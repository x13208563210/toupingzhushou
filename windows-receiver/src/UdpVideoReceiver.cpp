#include "UdpVideoReceiver.h"

#include <WinSock2.h>

#include <array>
#include <sstream>

namespace {

constexpr SOCKET kInvalidSocket = INVALID_SOCKET;

std::wstring SocketErrorText(const wchar_t* action) {
    std::wostringstream stream;
    stream << action << L"\uff0cWSA \u9519\u8bef\u7801=" << WSAGetLastError();
    return stream.str();
}

}  // namespace

UdpVideoReceiver::UdpVideoReceiver(LogFn log_fn, AccessUnitFn access_unit_fn)
    : log_fn_(std::move(log_fn)), access_unit_fn_(std::move(access_unit_fn)) {}

UdpVideoReceiver::~UdpVideoReceiver() {
    Stop();
}

bool UdpVideoReceiver::Start(uint16_t video_port) {
    if (running_.exchange(true)) {
        return false;
    }

    thread_ = std::thread([this, video_port] {
        ThreadMain(video_port);
    });
    return true;
}

void UdpVideoReceiver::Stop() {
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

VideoStats UdpVideoReceiver::GetStats() const {
    VideoStats stats;
    stats.total_packets = total_packets_.load();
    stats.total_bytes = total_bytes_.load();
    stats.frame_starts = frame_starts_.load();
    stats.keyframes = keyframes_.load();
    stats.codec_config_frames = codec_config_frames_.load();
    stats.completed_frames = completed_frames_.load();
    stats.dropped_frames = dropped_frames_.load();
    stats.last_frame_id = last_frame_id_.load();
    stats.last_pts_us = last_pts_us_.load();
    return stats;
}

void UdpVideoReceiver::ThreadMain(uint16_t video_port) {
    SOCKET udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_socket == kInvalidSocket) {
        log_fn_(L"\u89c6\u9891\u63a5\u6536: " + SocketErrorText(L"\u521b\u5efa UDP \u5957\u63a5\u5b57\u5931\u8d25"));
        running_ = false;
        return;
    }

    socket_ = static_cast<uintptr_t>(udp_socket);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(video_port);

    if (bind(udp_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        log_fn_(L"\u89c6\u9891\u63a5\u6536: " + SocketErrorText(L"\u7ed1\u5b9a UDP \u7aef\u53e3\u5931\u8d25"));
        closesocket(udp_socket);
        socket_ = static_cast<uintptr_t>(kInvalidSocket);
        running_ = false;
        return;
    }

    {
        std::wostringstream stream;
        stream << L"\u89c6\u9891\u63a5\u6536: \u6b63\u5728\u76d1\u542c UDP/" << video_port;
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
                log_fn_(L"\u89c6\u9891\u63a5\u6536: " + SocketErrorText(L"\u63a5\u6536 UDP \u6570\u636e\u5931\u8d25"));
            }
            break;
        }

        protocol::PacketHeader header;
        if (!protocol::ParsePacketHeader(buffer.data(), static_cast<size_t>(received), &header)) {
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

void UdpVideoReceiver::OnPacket(const protocol::PacketHeader& header, const uint8_t* payload, size_t datagram_size) {
    total_packets_.fetch_add(1);
    total_bytes_.fetch_add(datagram_size);
    last_frame_id_.store(header.frame_id);
    last_pts_us_.store(header.pts_us);

    if (header.packet_index == 0) {
        frame_starts_.fetch_add(1);
        if ((header.flags & protocol::kFlagKeyframe) != 0) {
            keyframes_.fetch_add(1);
        }
        if ((header.flags & protocol::kFlagCodecConfig) != 0) {
            codec_config_frames_.fetch_add(1);
        }
    }

    const size_t payload_size = datagram_size >= 32 ? datagram_size - 32 : 0;
    const auto access_unit = reassembler_.AddPacket(header, payload, payload_size);
    const auto reassembler_stats = reassembler_.GetStats();
    completed_frames_.store(reassembler_stats.completed_frames);
    dropped_frames_.store(reassembler_stats.dropped_frames);

    if (access_unit.has_value() && access_unit_fn_) {
        access_unit_fn_(access_unit.value());
    }
}
