#include "UdpVideoReceiver.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <array>
#include <sstream>
#include <vector>

namespace {

constexpr SOCKET kInvalidSocket = INVALID_SOCKET;
constexpr size_t kPacketHeaderSize = 32;
constexpr uint32_t kMaxPayloadSize = 4u * 1024u * 1024u;
constexpr int kVideoTcpReceiveBufferBytes = 1 << 20;

std::wstring SocketErrorText(const wchar_t* action) {
    std::wostringstream stream;
    stream << action << L"\uff0cWSA \u9519\u8bef\u7801=" << WSAGetLastError();
    return stream.str();
}

bool ReceiveAll(SOCKET socket, uint8_t* buffer, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const int received = recv(
            socket,
            reinterpret_cast<char*>(buffer + offset),
            static_cast<int>(size - offset),
            0);
        if (received <= 0) {
            return false;
        }
        offset += static_cast<size_t>(received);
    }
    return true;
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

    if (client_socket_ != static_cast<uintptr_t>(kInvalidSocket)) {
        shutdown(static_cast<SOCKET>(client_socket_), SD_BOTH);
        closesocket(static_cast<SOCKET>(client_socket_));
        client_socket_ = static_cast<uintptr_t>(kInvalidSocket);
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
    SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == kInvalidSocket) {
        log_fn_(L"\u89c6\u9891\u63a5\u6536: " + SocketErrorText(L"\u521b\u5efa TCP \u89c6\u9891\u5957\u63a5\u5b57\u5931\u8d25"));
        running_ = false;
        return;
    }

    socket_ = static_cast<uintptr_t>(listen_socket);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(video_port);

    int reuse = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    if (bind(listen_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        log_fn_(L"\u89c6\u9891\u63a5\u6536: " + SocketErrorText(L"\u7ed1\u5b9a TCP \u89c6\u9891\u7aef\u53e3\u5931\u8d25"));
        closesocket(listen_socket);
        socket_ = static_cast<uintptr_t>(kInvalidSocket);
        running_ = false;
        return;
    }

    if (listen(listen_socket, 1) == SOCKET_ERROR) {
        log_fn_(L"\u89c6\u9891\u63a5\u6536: " + SocketErrorText(L"\u5f00\u59cb\u76d1\u542c TCP \u89c6\u9891\u6d41\u5931\u8d25"));
        closesocket(listen_socket);
        socket_ = static_cast<uintptr_t>(kInvalidSocket);
        running_ = false;
        return;
    }

    {
        std::wostringstream stream;
        stream << L"\u89c6\u9891\u63a5\u6536: \u6b63\u5728\u76d1\u542c TCP/" << video_port
               << L"\uff08\u53ef\u9760\u89c6\u9891\u6d41\uff09";
        log_fn_(stream.str());
    }

    std::array<uint8_t, kPacketHeaderSize> header_buffer{};
    while (running_) {
        sockaddr_in client_address{};
        int client_length = sizeof(client_address);
        SOCKET client_socket = accept(
            listen_socket,
            reinterpret_cast<sockaddr*>(&client_address),
            &client_length);
        if (client_socket == kInvalidSocket) {
            if (running_) {
                log_fn_(L"\u89c6\u9891\u63a5\u6536: " + SocketErrorText(L"\u63a5\u53d7 TCP \u89c6\u9891\u8fde\u63a5\u5931\u8d25"));
            }
            break;
        }

        const int nodelay = 1;
        setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        setsockopt(
            client_socket,
            SOL_SOCKET,
            SO_RCVBUF,
            reinterpret_cast<const char*>(&kVideoTcpReceiveBufferBytes),
            sizeof(kVideoTcpReceiveBufferBytes));
        client_socket_ = static_cast<uintptr_t>(client_socket);
        last_completed_frame_id_ = 0;
        last_observed_dropped_frames_ = dropped_frames_.load();

        wchar_t host_buffer[64] = {};
        InetNtopW(AF_INET, &client_address.sin_addr, host_buffer, static_cast<DWORD>(std::size(host_buffer)));
        std::wostringstream connect_stream;
        connect_stream << L"\u89c6\u9891\u63a5\u6536: \u53d1\u9001\u7aef\u89c6\u9891\u6d41\u5df2\u8fde\u63a5\uff0c\u6765\u6e90\u5730\u5740 "
                       << host_buffer;
        log_fn_(connect_stream.str());

        while (running_) {
            if (!ReceiveAll(client_socket, header_buffer.data(), header_buffer.size())) {
                if (running_) {
                    log_fn_(L"\u89c6\u9891\u63a5\u6536: \u53d1\u9001\u7aef\u89c6\u9891\u6d41\u5df2\u65ad\u5f00\u8fde\u63a5\u3002");
                }
                break;
            }

            protocol::PacketHeader header;
            if (!protocol::ParsePacketHeader(header_buffer.data(), header_buffer.size(), &header)) {
                if (log_fn_ != nullptr) {
                    log_fn_(L"\u89c6\u9891\u63a5\u6536: TCP \u89c6\u9891\u5934\u6821\u9a8c\u5931\u8d25\uff0c\u5f53\u524d\u8fde\u63a5\u5c06\u91cd\u7f6e\u3002");
                }
                break;
            }

            if (header.packet_count != 1 || header.packet_index != 0) {
                if (log_fn_ != nullptr) {
                    std::wostringstream stream;
                    stream << L"\u89c6\u9891\u63a5\u6536: TCP \u89c6\u9891\u6d41\u6536\u5230\u975e\u5355\u8bbf\u95ee\u5355\u5143\u5934\uff0c"
                           << L"packetIndex=" << header.packet_index
                           << L"\uff0cpacketCount=" << header.packet_count
                           << L"\uff0c\u5f53\u524d\u8fde\u63a5\u5c06\u91cd\u7f6e\u3002";
                    log_fn_(stream.str());
                }
                break;
            }

            if (header.payload_size > kMaxPayloadSize) {
                if (log_fn_ != nullptr) {
                    std::wostringstream stream;
                    stream << L"\u89c6\u9891\u63a5\u6536: TCP \u89c6\u9891\u5e27\u8fc7\u5927\uff0cpayload="
                           << header.payload_size
                           << L"\uff0c\u5f53\u524d\u8fde\u63a5\u5c06\u91cd\u7f6e\u3002";
                    log_fn_(stream.str());
                }
                break;
            }

            std::vector<uint8_t> payload(header.payload_size);
            if (header.payload_size > 0 &&
                !ReceiveAll(client_socket, payload.data(), payload.size())) {
                if (running_) {
                    log_fn_(L"\u89c6\u9891\u63a5\u6536: TCP \u89c6\u9891\u5e27\u8bfb\u53d6\u672a\u5b8c\u6210\uff0c\u53d1\u9001\u7aef\u5df2\u65ad\u5f00\u3002");
                }
                break;
            }

            OnAccessUnit(header, std::move(payload));
        }

        closesocket(client_socket);
        client_socket_ = static_cast<uintptr_t>(kInvalidSocket);
    }

    closesocket(listen_socket);
    socket_ = static_cast<uintptr_t>(kInvalidSocket);
    running_ = false;
}

void UdpVideoReceiver::OnAccessUnit(const protocol::PacketHeader& header, std::vector<uint8_t> payload) {
    total_packets_.fetch_add(1);
    total_bytes_.fetch_add(static_cast<uint64_t>(kPacketHeaderSize) + static_cast<uint64_t>(payload.size()));
    last_frame_id_.store(header.frame_id);
    last_pts_us_.store(header.pts_us);
    frame_starts_.fetch_add(1);

    if ((header.flags & protocol::kFlagKeyframe) != 0) {
        keyframes_.fetch_add(1);
    }
    if ((header.flags & protocol::kFlagCodecConfig) != 0) {
        codec_config_frames_.fetch_add(1);
    }

    AccessUnit unit;
    unit.frame_id = header.frame_id;
    unit.pts_us = header.pts_us;
    unit.flags = header.flags;
    unit.bytes = std::move(payload);

    bool discontinuity = false;
    uint64_t dropped_increment = 0;
    if (last_completed_frame_id_ != 0 && unit.frame_id > last_completed_frame_id_ + 1) {
        discontinuity = true;
        dropped_increment = static_cast<uint64_t>(unit.frame_id - last_completed_frame_id_ - 1);
    } else if (last_completed_frame_id_ != 0 && unit.frame_id <= last_completed_frame_id_) {
        discontinuity = true;
    }

    if (dropped_increment > 0) {
        dropped_frames_.fetch_add(dropped_increment);
    }

    completed_frames_.fetch_add(1);
    unit.discontinuity = discontinuity;
    if (discontinuity && log_fn_ != nullptr) {
        std::wostringstream stream;
        stream << L"\u89c6\u9891\u63a5\u6536: TCP \u89c6\u9891\u6d41\u68c0\u6d4b\u5230 frameId \u8df3\u53f7\uff0c"
               << L"\u4e0a\u4e00\u5b8c\u6574 frameId=" << last_completed_frame_id_
               << L"\uff0c\u5f53\u524d frameId=" << unit.frame_id
               << L"\uff0c\u4e22\u5e27\u7d2f\u8ba1=" << dropped_frames_.load()
               << L"\u3002";
        log_fn_(stream.str());
    }

    last_completed_frame_id_ = unit.frame_id;
    last_observed_dropped_frames_ = dropped_frames_.load();
    if (access_unit_fn_) {
        access_unit_fn_(unit);
    }
}
