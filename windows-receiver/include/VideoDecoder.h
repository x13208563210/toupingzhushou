#pragma once

#include "FrameReassembler.h"
#include "Protocol.h"

#include <d3d11.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

enum class DecodedFrameFormat {
    kUnknown,
    kBgra,
    kNv12,
};

struct DecodedFrame {
    int width = 0;
    int height = 0;
    uint64_t pts_us = 0;
    DecodedFrameFormat format = DecodedFrameFormat::kUnknown;
    int stride0 = 0;
    int stride1 = 0;
    size_t plane1_offset = 0;
    std::vector<uint8_t> bytes;
    ID3D11Texture2D* d3d_texture = nullptr;
    UINT d3d_subresource = 0;
    bool gpu_backed = false;

    DecodedFrame() = default;

    ~DecodedFrame() {
        Reset();
    }

    DecodedFrame(const DecodedFrame&) = delete;
    DecodedFrame& operator=(const DecodedFrame&) = delete;

    DecodedFrame(DecodedFrame&& other) noexcept {
        MoveFrom(std::move(other));
    }

    DecodedFrame& operator=(DecodedFrame&& other) noexcept {
        if (this != &other) {
            Reset();
            MoveFrom(std::move(other));
        }
        return *this;
    }

    void Reset() noexcept {
        bytes.clear();
        if (d3d_texture != nullptr) {
            d3d_texture->Release();
            d3d_texture = nullptr;
        }
        width = 0;
        height = 0;
        pts_us = 0;
        format = DecodedFrameFormat::kUnknown;
        stride0 = 0;
        stride1 = 0;
        plane1_offset = 0;
        d3d_subresource = 0;
        gpu_backed = false;
    }

private:
    void MoveFrom(DecodedFrame&& other) noexcept {
        width = other.width;
        height = other.height;
        pts_us = other.pts_us;
        format = other.format;
        stride0 = other.stride0;
        stride1 = other.stride1;
        plane1_offset = other.plane1_offset;
        bytes = std::move(other.bytes);
        d3d_texture = other.d3d_texture;
        d3d_subresource = other.d3d_subresource;
        gpu_backed = other.gpu_backed;

        other.d3d_texture = nullptr;
        other.d3d_subresource = 0;
        other.gpu_backed = false;
        other.width = 0;
        other.height = 0;
        other.pts_us = 0;
        other.format = DecodedFrameFormat::kUnknown;
        other.stride0 = 0;
        other.stride1 = 0;
        other.plane1_offset = 0;
    }
};

class VideoDecoder {
public:
    using LogFn = std::function<void(const std::wstring&)>;
    using FrameFn = std::function<void(DecodedFrame)>;
    using RequestKeyframeFn = std::function<void()>;

    VideoDecoder(LogFn log_fn, FrameFn frame_fn, RequestKeyframeFn request_keyframe_fn = {});
    ~VideoDecoder();

    void Start();
    void Stop();
    void Configure(const protocol::StreamProfile& profile);
    void SubmitAccessUnit(const AccessUnit& access_unit);
    void SetD3DDevice(ID3D11Device* device);
    size_t GetPendingAccessUnitCount();

private:
    void ThreadMain();

    LogFn log_fn_;
    FrameFn frame_fn_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<AccessUnit> queue_;
    bool running_ = false;
    bool profile_dirty_ = false;
    protocol::StreamProfile pending_profile_;
    protocol::StreamProfile active_profile_;
    AccessUnit latest_codec_config_;
    bool has_active_profile_ = false;
    bool has_latest_codec_config_ = false;
    bool waiting_for_keyframe_ = false;
    bool backend_reset_requested_ = false;
    bool soft_resync_requested_ = false;
    bool discontinuity_pending_ = false;
    std::thread thread_;
    ID3D11Device* d3d_device_ = nullptr;
    RequestKeyframeFn request_keyframe_fn_;
};
