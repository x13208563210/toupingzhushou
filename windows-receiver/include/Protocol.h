#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace protocol {

enum class Codec {
    kUnknown,
    kAvc,
    kHevc,
};

struct StreamProfile {
    Codec codec = Codec::kUnknown;
    int width = 0;
    int height = 0;
    int fps = 0;
    bool adaptive_fps = false;
    int bitrate = 0;
    int video_port = 0;
};

struct HelloMessage {
    std::wstring device_name;
    std::vector<StreamProfile> profiles;
};

struct PacketHeader {
    uint16_t magic = 0;
    uint8_t version = 0;
    uint8_t flags = 0;
    uint32_t stream_id = 0;
    uint32_t frame_id = 0;
    uint16_t packet_index = 0;
    uint16_t packet_count = 0;
    uint32_t payload_size = 0;
    uint64_t pts_us = 0;
    uint32_t reserved = 0;
};

constexpr uint16_t kMagic = 0x5343;
constexpr int kFlagKeyframe = 1 << 0;
constexpr int kFlagCodecConfig = 1 << 1;

Codec CodecFromWireName(const std::string& value);
std::string CodecToWireName(Codec codec);
bool ParseHelloMessage(const std::string& json_line, HelloMessage* out, std::string* error);
std::string BuildSelectProfileJson(const StreamProfile& profile);
bool ParsePacketHeader(const uint8_t* data, size_t size, PacketHeader* out);

}  // namespace protocol
