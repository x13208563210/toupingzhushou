#pragma once

#include "VideoDecoder.h"

#include <Windows.h>

#include <functional>
#include <mutex>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11SamplerState;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct IDXGISwapChain;
struct IDXGIFactory5;

class VideoRenderer {
public:
    using LogFn = std::function<void(const std::wstring&)>;
    using PresentFn = std::function<void(uint64_t)>;

    VideoRenderer();
    ~VideoRenderer();

    void SetLogFn(LogFn log_fn);
    void SetPresentFn(PresentFn present_fn);
    bool Create(HWND parent, HINSTANCE instance);
    void Resize(int x, int y, int width, int height);
    void Present(DecodedFrame frame);
    HWND window() const { return window_; }
    ID3D11Device* d3d_device() const { return device_; }
    const std::wstring& gpu_name() const { return gpu_name_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    LRESULT HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    bool InitializeDeviceResources();
    bool EnableMultithreadProtection();
    bool CreateSwapChainResources();
    void ResizeSwapChain();
    bool UseExternalTextureFrame(const DecodedFrame& frame);
    bool UseGpuCopiedTextureFrame(const DecodedFrame& frame);
    void ClearExternalVideoResources();
    void Render();
    void CleanupDeviceResources();
    bool CheckTearingSupport() const;

    HWND window_ = nullptr;
    LogFn log_fn_;
    PresentFn present_fn_;
    std::mutex frame_mutex_;
    DecodedFrame latest_frame_;
    bool frame_dirty_ = false;
    bool render_posted_ = false;
    std::wstring gpu_name_;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain* swap_chain_ = nullptr;
    IDXGIFactory5* dxgi_factory5_ = nullptr;
    ID3D11RenderTargetView* render_target_view_ = nullptr;
    ID3D11SamplerState* sampler_state_ = nullptr;
    ID3D11VertexShader* vertex_shader_ = nullptr;
    ID3D11PixelShader* bgra_pixel_shader_ = nullptr;
    ID3D11PixelShader* nv12_pixel_shader_ = nullptr;
    ID3D11Texture2D* external_texture_ = nullptr;
    ID3D11ShaderResourceView* external_srv0_ = nullptr;
    ID3D11ShaderResourceView* external_srv1_ = nullptr;
    DecodedFrameFormat texture_format_ = DecodedFrameFormat::kUnknown;
    int texture_width_ = 0;
    int texture_height_ = 0;
    bool using_external_texture_ = false;
    bool using_gpu_copy_texture_ = false;
    bool logged_gpu_copy_fallback_ = false;
    bool allow_tearing_ = false;
};
