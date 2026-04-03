#pragma once

#include "FrameReassembler.h"
#include "Protocol.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

struct VideoStats {
    uint64_t total_packets = 0;
    uint64_t total_bytes = 0;
    uint64_t frame_starts = 0;
    uint64_t keyframes = 0;
    uint64_t codec_config_frames = 0;
    uint64_t completed_frames = 0;
    uint64_t dropped_frames = 0;
    uint32_t last_frame_id = 0;
    uint64_t last_pts_us = 0;
};

class UdpVideoReceiver {
public:
    using LogFn = std::function<void(const std::wstring&)>;
    using AccessUnitFn = std::function<void(const AccessUnit&)>;

    UdpVideoReceiver(LogFn log_fn, AccessUnitFn access_unit_fn);
    ~UdpVideoReceiver();

    bool Start(uint16_t video_port);
    void Stop();
    VideoStats GetStats() const;

private:
    void ThreadMain(uint16_t video_port);
    void OnPacket(const protocol::PacketHeader& header, const uint8_t* payload, size_t datagram_size);

    LogFn log_fn_;
    AccessUnitFn access_unit_fn_;
    std::atomic<bool> running_{false};
    uintptr_t socket_ = static_cast<uintptr_t>(-1);
    std::thread thread_;
    mutable FrameReassembler reassembler_;

    std::atomic<uint64_t> total_packets_{0};
    std::atomic<uint64_t> total_bytes_{0};
    std::atomic<uint64_t> frame_starts_{0};
    std::atomic<uint64_t> keyframes_{0};
    std::atomic<uint64_t> codec_config_frames_{0};
    std::atomic<uint64_t> completed_frames_{0};
    std::atomic<uint64_t> dropped_frames_{0};
    std::atomic<uint32_t> last_frame_id_{0};
    std::atomic<uint64_t> last_pts_us_{0};
};
