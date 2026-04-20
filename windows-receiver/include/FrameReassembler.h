#pragma once

#include "Protocol.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

struct AccessUnit {
    std::vector<uint8_t> bytes;
    uint32_t frame_id = 0;
    uint64_t pts_us = 0;
    uint8_t flags = 0;
    bool discontinuity = false;
};

struct ReassemblerStats {
    uint64_t completed_frames = 0;
    uint64_t dropped_frames = 0;
};

class FrameReassembler {
public:
    std::optional<AccessUnit> AddPacket(
        const protocol::PacketHeader& header,
        const uint8_t* payload,
        size_t payload_size);

    ReassemblerStats GetStats() const;
    void Reset();

private:
    struct PartialFrame {
        uint16_t packet_count = 0;
        uint16_t received_count = 0;
        uint8_t flags = 0;
        uint64_t pts_us = 0;
        std::vector<std::vector<uint8_t>> fragments;
        std::vector<bool> received;
    };

    void DropOlderFrames(uint32_t latest_frame_id);

    std::unordered_map<uint32_t, PartialFrame> frames_;
    uint64_t completed_frames_ = 0;
    uint64_t dropped_frames_ = 0;
};
