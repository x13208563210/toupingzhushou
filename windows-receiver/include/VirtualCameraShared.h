#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace virtual_camera {

inline constexpr wchar_t kMediaSourceClsidString[] = L"{EEA24C6E-6EAD-4D1F-9D1A-CE0F21C91867}";
inline constexpr wchar_t kFriendlyName[] = L"\u76F4\u64AD\u6295\u5C4F\u52A9\u624B\u865A\u62DF\u6444\u50CF\u5934";
inline constexpr wchar_t kMediaSourceFileName[] = L"live-cast-virtual-camera-media-source.dll";
inline constexpr wchar_t kToolFileName[] = L"live-cast-virtual-camera-tool.exe";
inline constexpr wchar_t kPlaceholderImageFileName[] = L"virtual-camera-default.png";
inline constexpr wchar_t kSharedDirectoryName[] = L"\u76F4\u64AD\u6295\u5C4F\u52A9\u624B";
inline constexpr wchar_t kSharedFrameFileName[] = L"virtual-camera-frame.bin";

inline constexpr uint32_t kFrameMagic = 0x56434153u;
inline constexpr uint32_t kFrameVersion = 1;
inline constexpr uint32_t kPixelFormatBgra32 = 1;
inline constexpr uint32_t kOutputWidth = 1920;
inline constexpr uint32_t kOutputHeight = 1080;
inline constexpr uint32_t kOutputStride = kOutputWidth * 4;
inline constexpr uint32_t kOutputFps = 60;  // 提升至 60fps
inline constexpr uint64_t kFrameIntervalUs = 1'000'000ull / kOutputFps;
inline constexpr size_t kFramePayloadBytes =
    static_cast<size_t>(kOutputStride) * static_cast<size_t>(kOutputHeight);

struct SharedFrameHeader {
    uint32_t magic = kFrameMagic;
    uint32_t version = kFrameVersion;
    uint32_t width = kOutputWidth;
    uint32_t height = kOutputHeight;
    uint32_t stride = kOutputStride;
    uint32_t pixel_format = kPixelFormatBgra32;
    uint64_t frame_counter = 0;
    uint64_t pts_us = 0;
    uint64_t sequence = 0;
    uint64_t last_update_tick_ms = 0;
    uint32_t has_frame = 0;
    uint32_t reserved0 = 0;
    uint64_t reserved1 = 0;
};

static_assert(std::is_standard_layout_v<SharedFrameHeader>);

inline constexpr size_t kSharedMemoryBytes =
    sizeof(SharedFrameHeader) + kFramePayloadBytes;

}  // namespace virtual_camera
