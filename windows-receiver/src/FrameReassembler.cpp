#include "FrameReassembler.h"

#include <algorithm>

std::optional<AccessUnit> FrameReassembler::AddPacket(
    const protocol::PacketHeader& header,
    const uint8_t* payload,
    size_t payload_size) {
    if (payload == nullptr || payload_size != header.payload_size || header.packet_count == 0 ||
        header.packet_index >= header.packet_count) {
        return std::nullopt;
    }

    auto& frame = frames_[header.frame_id];
    if (frame.fragments.empty()) {
        frame.packet_count = header.packet_count;
        frame.flags = header.flags;
        frame.pts_us = header.pts_us;
        frame.fragments.resize(header.packet_count);
        frame.received.assign(header.packet_count, false);
    }

    if (frame.packet_count != header.packet_count || frame.fragments.size() != header.packet_count) {
        frames_.erase(header.frame_id);
        ++dropped_frames_;
        return std::nullopt;
    }

    if (!frame.received[header.packet_index]) {
        frame.fragments[header.packet_index].assign(payload, payload + payload_size);
        frame.received[header.packet_index] = true;
        ++frame.received_count;
    }

    DropOlderFrames(header.frame_id);

    if (frame.received_count != frame.packet_count) {
        return std::nullopt;
    }

    AccessUnit unit;
    unit.frame_id = header.frame_id;
    unit.pts_us = frame.pts_us;
    unit.flags = frame.flags;

    size_t total_size = 0;
    for (const auto& fragment : frame.fragments) {
        total_size += fragment.size();
    }
    unit.bytes.reserve(total_size);
    for (const auto& fragment : frame.fragments) {
        unit.bytes.insert(unit.bytes.end(), fragment.begin(), fragment.end());
    }

    frames_.erase(header.frame_id);
    ++completed_frames_;
    return unit;
}

ReassemblerStats FrameReassembler::GetStats() const {
    ReassemblerStats stats;
    stats.completed_frames = completed_frames_;
    stats.dropped_frames = dropped_frames_;
    return stats;
}

void FrameReassembler::DropOlderFrames(uint32_t latest_frame_id) {
    constexpr uint32_t kMaxFrameWindow = 32;
    for (auto it = frames_.begin(); it != frames_.end();) {
        if (latest_frame_id > it->first && latest_frame_id - it->first > kMaxFrameWindow) {
            ++dropped_frames_;
            it = frames_.erase(it);
        } else {
            ++it;
        }
    }
}
