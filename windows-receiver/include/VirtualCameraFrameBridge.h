#pragma once

#include "VideoDecoder.h"

#include <functional>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

class VirtualCameraFrameBridge {
public:
    using LogFn = std::function<void(const std::wstring&)>;

    VirtualCameraFrameBridge();
    ~VirtualCameraFrameBridge();

    void SetLogFn(LogFn log_fn);
    void SetPlaceholderImagePath(std::wstring image_path);

    bool Start(ID3D11Device* device);
    void Stop();
    bool PublishFrame(const DecodedFrame& frame);

    bool running() const { return running_; }
    const std::wstring& shared_frame_path() const { return shared_frame_path_; }

private:
    bool EnsureSharedMemory();
    bool EnsureStagingTexture(int width, int height);
    bool LoadPlaceholderFrame();
    bool PublishPlaceholderFrame();
    void ResetSharedFrame();
    void WriteBgraFrame(
        const uint8_t* source,
        int source_width,
        int source_height,
        int source_stride,
        uint64_t pts_us);
    void PublishBlackFrame();

    LogFn log_fn_;
    std::wstring shared_frame_path_;
    std::wstring placeholder_image_path_;
    HANDLE file_handle_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_handle_ = nullptr;
    void* view_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Texture2D* staging_texture_ = nullptr;
    int staging_width_ = 0;
    int staging_height_ = 0;
    int placeholder_width_ = 0;
    int placeholder_height_ = 0;
    std::vector<uint8_t> placeholder_frame_;
    uint64_t frame_counter_ = 0;
    bool running_ = false;
    bool logged_format_warning_ = false;
    bool logged_publish_failure_ = false;
    bool logged_placeholder_failure_ = false;
    uint64_t last_publish_tick_ms_ = 0;
};
