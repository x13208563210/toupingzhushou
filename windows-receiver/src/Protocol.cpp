#include "Protocol.h"

#include <Windows.h>

#include <array>
#include <regex>
#include <sstream>

namespace protocol {
namespace {

constexpr int kMaxStreamFps = 120;

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

uint16_t ReadU16(const uint8_t* data) {
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

uint32_t ReadU32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

uint64_t ReadU64(const uint8_t* data) {
    return (static_cast<uint64_t>(data[0]) << 56) |
           (static_cast<uint64_t>(data[1]) << 48) |
           (static_cast<uint64_t>(data[2]) << 40) |
           (static_cast<uint64_t>(data[3]) << 32) |
           (static_cast<uint64_t>(data[4]) << 24) |
           (static_cast<uint64_t>(data[5]) << 16) |
           (static_cast<uint64_t>(data[6]) << 8) |
           static_cast<uint64_t>(data[7]);
}

int ClampProfileFps(int fps) {
    if (fps <= 0) {
        return fps;
    }
    return fps > kMaxStreamFps ? kMaxStreamFps : fps;
}

}  // namespace

Codec CodecFromWireName(const std::string& value) {
    if (value == "avc" || value == "AVC") {
        return Codec::kAvc;
    }
    if (value == "hevc" || value == "HEVC") {
        return Codec::kHevc;
    }
    return Codec::kUnknown;
}

std::string CodecToWireName(Codec codec) {
    switch (codec) {
    case Codec::kAvc:
        return "avc";
    case Codec::kHevc:
        return "hevc";
    default:
        return "unknown";
    }
}

bool ParseHelloMessage(const std::string& json_line, HelloMessage* out, std::string* error) {
    if (out == nullptr) {
        return false;
    }

    *out = HelloMessage{};

    const std::regex device_regex(R"json("deviceName"\s*:\s*"([^"]*)")json");
    std::smatch device_match;
    if (std::regex_search(json_line, device_match, device_regex)) {
        out->device_name = Utf8ToWide(device_match[1].str());
    }

    const std::regex audio_regex(
        R"json("audio"\s*:\s*\{\s*"enabled"\s*:\s*(true|false)\s*,\s*"sampleRate"\s*:\s*(\d+)\s*,\s*"channels"\s*:\s*(\d+)\s*,\s*"format"\s*:\s*"([^"]+)")json");
    std::smatch audio_match;
    if (std::regex_search(json_line, audio_match, audio_regex)) {
        out->audio_enabled = audio_match[1].str() == "true";
        out->audio_sample_rate = std::stoi(audio_match[2].str());
        out->audio_channels = std::stoi(audio_match[3].str());
    }

    const std::regex profile_regex(
        R"json(\{"codec":"([^"]+)","width":(\d+),"height":(\d+),"fps":(\d+)(?:,"adaptiveFps":(true|false))?,"bitrate":(\d+)\})json");
    auto begin = std::sregex_iterator(json_line.begin(), json_line.end(), profile_regex);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        StreamProfile profile;
        profile.codec = CodecFromWireName((*it)[1].str());
        profile.width = std::stoi((*it)[2].str());
        profile.height = std::stoi((*it)[3].str());
        profile.fps = ClampProfileFps(std::stoi((*it)[4].str()));
        profile.adaptive_fps = (*it)[5].matched && (*it)[5].str() == "true";
        profile.bitrate = std::stoi((*it)[6].str());
        out->profiles.push_back(profile);
    }

    if (out->profiles.empty()) {
        if (error != nullptr) {
            *error = "HELLO did not contain any parseable profiles.";
        }
        return false;
    }
    return true;
}

std::string BuildSelectProfileJson(const StreamProfile& profile) {
    const int fps = ClampProfileFps(profile.fps);
    std::ostringstream stream;
    stream << "{\"type\":\"SELECT_PROFILE\""
           << ",\"codec\":\"" << CodecToWireName(profile.codec) << "\""
           << ",\"width\":" << profile.width
           << ",\"height\":" << profile.height
           << ",\"fps\":" << fps
           << ",\"adaptiveFps\":" << (profile.adaptive_fps ? "true" : "false")
           << ",\"bitrate\":" << profile.bitrate
           << ",\"videoPort\":" << profile.video_port
           << ",\"audioEnabled\":" << (profile.audio_enabled ? "true" : "false")
           << ",\"audioPort\":" << profile.audio_port
           << ",\"audioSampleRate\":" << profile.audio_sample_rate
           << ",\"audioChannels\":" << profile.audio_channels
           << "}";
    return stream.str();
}

bool ParsePacketHeader(const uint8_t* data, size_t size, PacketHeader* out) {
    if (data == nullptr || out == nullptr || size < 32) {
        return false;
    }

    PacketHeader header;
    header.magic = ReadU16(data + 0);
    header.version = data[2];
    header.flags = data[3];
    header.stream_id = ReadU32(data + 4);
    header.frame_id = ReadU32(data + 8);
    header.packet_index = ReadU16(data + 12);
    header.packet_count = ReadU16(data + 14);
    header.payload_size = ReadU32(data + 16);
    header.pts_us = ReadU64(data + 20);
    header.reserved = ReadU32(data + 28);

    if (header.magic != kMagic) {
        return false;
    }

    *out = header;
    return true;
}

}  // namespace protocol
