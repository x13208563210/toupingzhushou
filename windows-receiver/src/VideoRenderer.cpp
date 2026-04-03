#include "VideoRenderer.h"

#include <Windows.h>
#include <d3d10.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_5.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kRendererClassName[] = L"AndroidCastVideoRenderer";
constexpr UINT kRenderMessage = WM_APP + 41;
constexpr UINT kNvidiaVendorId = 0x10DE;

const char kVertexShaderSource[] = R"(
struct VSOut {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID) {
    float2 positions[4] = {
        float2(-1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0, -1.0),
        float2( 1.0,  1.0)
    };
    float2 uvs[4] = {
        float2(0.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 1.0),
        float2(1.0, 0.0)
    };

    VSOut output;
    output.position = float4(positions[id], 0.0, 1.0);
    output.uv = uvs[id];
    return output;
}
)";

const char kBgraPixelShaderSource[] = R"(
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return videoTexture.Sample(videoSampler, uv);
}
)";

const char kNv12PixelShaderSource[] = R"(
Texture2D yTexture : register(t0);
Texture2D uvTexture : register(t1);
SamplerState videoSampler : register(s0);

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float y = yTexture.Sample(videoSampler, uv).r;
    float2 uvSample = uvTexture.Sample(videoSampler, uv).rg - float2(0.5, 0.5);

    float yLinear = saturate((y - 16.0 / 255.0) * 1.16438356);
    float r = saturate(yLinear + 1.79274107 * uvSample.y);
    float g = saturate(yLinear - 0.21324861 * uvSample.x - 0.53290933 * uvSample.y);
    float b = saturate(yLinear + 2.11240179 * uvSample.x);
    return float4(r, g, b, 1.0);
}
)";

void SafeRelease(IUnknown* object) {
    if (object != nullptr) {
        object->Release();
    }
}

std::wstring HrToString(HRESULT hr) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase << hr;
    return stream.str();
}

std::wstring AdapterNameFromDesc(const DXGI_ADAPTER_DESC1& desc) {
    std::wstring name(desc.Description);
    const size_t end = name.find(L'\0');
    if (end != std::wstring::npos) {
        name.resize(end);
    }
    return name;
}

}  // namespace

VideoRenderer::VideoRenderer() = default;

VideoRenderer::~VideoRenderer() {
    CleanupDeviceResources();
}

void VideoRenderer::SetLogFn(LogFn log_fn) {
    log_fn_ = std::move(log_fn);
}

void VideoRenderer::SetPresentFn(PresentFn present_fn) {
    present_fn_ = std::move(present_fn);
}

bool VideoRenderer::Create(HWND parent, HINSTANCE instance) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = VideoRenderer::WndProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = kRendererClassName;
    RegisterClassExW(&window_class);

    window_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        kRendererClassName,
        L"",
        WS_CHILD | WS_VISIBLE,
        0,
        0,
        100,
        100,
        parent,
        nullptr,
        instance,
        this);

    return window_ != nullptr && InitializeDeviceResources();
}

void VideoRenderer::Resize(int x, int y, int width, int height) {
    if (window_ != nullptr) {
        MoveWindow(window_, x, y, width, height, TRUE);
    }
}

void VideoRenderer::Present(DecodedFrame frame) {
    bool should_post = false;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        latest_frame_ = std::move(frame);
        frame_dirty_ = true;
        if (!render_posted_) {
            render_posted_ = true;
            should_post = true;
        }
    }

    if (should_post && window_ != nullptr) {
        PostMessageW(window_, kRenderMessage, 0, 0);
    }
}

LRESULT CALLBACK VideoRenderer::WndProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_NCCREATE) {
        auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
        auto* self = static_cast<VideoRenderer*>(create_struct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->window_ = hwnd;
        }
    }

    auto* self = reinterpret_cast<VideoRenderer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        return self->HandleMessage(message, w_param, l_param);
    }
    return DefWindowProcW(hwnd, message, w_param, l_param);
}

LRESULT VideoRenderer::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_SIZE:
        ResizeSwapChain();
        Render();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(window_, &ps);
        EndPaint(window_, &ps);
        Render();
        return 0;
    }
    case kRenderMessage:
        Render();
        return 0;
    default:
        return DefWindowProcW(window_, message, w_param, l_param);
    }

    return DefWindowProcW(window_, message, w_param, l_param);
}

bool VideoRenderer::InitializeDeviceResources() {
    RECT client_rect{};
    GetClientRect(window_, &client_rect);

    DXGI_SWAP_CHAIN_DESC swap_chain_desc{};
    swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swap_chain_desc.BufferDesc.Width = std::max(1L, client_rect.right - client_rect.left);
    swap_chain_desc.BufferDesc.Height = std::max(1L, client_rect.bottom - client_rect.top);
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.BufferCount = 2;
    swap_chain_desc.OutputWindow = window_;
    swap_chain_desc.Windowed = TRUE;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    const D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    UINT creation_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL created_level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = E_FAIL;
    IDXGIFactory1* factory1 = nullptr;
    IDXGIFactory6* factory6 = nullptr;
    IDXGIAdapter1* preferred_adapter = nullptr;

    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1)))) {
        if (SUCCEEDED(factory1->QueryInterface(IID_PPV_ARGS(&factory6)))) {
            for (UINT index = 0;; ++index) {
                IDXGIAdapter1* candidate = nullptr;
                if (factory6->EnumAdapterByGpuPreference(
                        index,
                        DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                        IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                if (candidate == nullptr) {
                    continue;
                }

                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(candidate->GetDesc1(&desc)) &&
                    desc.VendorId == kNvidiaVendorId &&
                    (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                    preferred_adapter = candidate;
                    gpu_name_ = AdapterNameFromDesc(desc);
                    break;
                }
                candidate->Release();
            }
        }

        if (preferred_adapter == nullptr) {
            for (UINT index = 0;; ++index) {
                IDXGIAdapter1* candidate = nullptr;
                if (factory1->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                if (candidate == nullptr) {
                    continue;
                }

                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(candidate->GetDesc1(&desc)) &&
                    desc.VendorId == kNvidiaVendorId &&
                    (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                    preferred_adapter = candidate;
                    gpu_name_ = AdapterNameFromDesc(desc);
                    break;
                }
                candidate->Release();
            }
        }

        if (SUCCEEDED(factory1->QueryInterface(IID_PPV_ARGS(&dxgi_factory5_)))) {
            allow_tearing_ = CheckTearingSupport();
        }
    }

    if (allow_tearing_) {
        swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    if (preferred_adapter != nullptr) {
        hr = D3D11CreateDeviceAndSwapChain(
            preferred_adapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            creation_flags,
            feature_levels,
            static_cast<UINT>(std::size(feature_levels)),
            D3D11_SDK_VERSION,
            &swap_chain_desc,
            &swap_chain_,
            &device_,
            &created_level,
            &context_);
        if (FAILED(hr) && log_fn_ != nullptr) {
            log_fn_(L"视频渲染: NVIDIA 设备创建失败，错误码 " + HrToString(hr));
        }
    }

    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            creation_flags,
            feature_levels,
            static_cast<UINT>(std::size(feature_levels)),
            D3D11_SDK_VERSION,
            &swap_chain_desc,
            &swap_chain_,
            &device_,
            &created_level,
            &context_);
        if (SUCCEEDED(hr) && gpu_name_.empty() && device_ != nullptr) {
            IDXGIDevice* dxgi_device = nullptr;
            if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
                IDXGIAdapter* adapter = nullptr;
                if (SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
                    DXGI_ADAPTER_DESC desc{};
                    if (SUCCEEDED(adapter->GetDesc(&desc))) {
                        gpu_name_ = desc.Description;
                    }
                    adapter->Release();
                }
                dxgi_device->Release();
            }
        }
    }

    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            creation_flags,
            feature_levels,
            static_cast<UINT>(std::size(feature_levels)),
            D3D11_SDK_VERSION,
            &swap_chain_desc,
            &swap_chain_,
            &device_,
            &created_level,
            &context_);
        if (FAILED(hr)) {
            SafeRelease(preferred_adapter);
            SafeRelease(factory6);
            SafeRelease(factory1);
            return false;
        }
        gpu_name_ = L"WARP 软件渲染";
    }

    SafeRelease(preferred_adapter);
    SafeRelease(factory6);
    SafeRelease(factory1);

    EnableMultithreadProtection();

    if (log_fn_ != nullptr) {
        std::wostringstream stream;
        stream << L"视频渲染: 已创建 D3D11 设备，显卡="
               << (gpu_name_.empty() ? L"未知" : gpu_name_)
               << L"，特性级别=0x"
               << std::hex << std::uppercase << static_cast<unsigned int>(created_level)
               << L"，flip model=" << L"已启用"
               << L"，tearing=" << (allow_tearing_ ? L"已启用" : L"不可用");
        log_fn_(stream.str());
    }

    ID3DBlob* vertex_blob = nullptr;
    ID3DBlob* bgra_pixel_blob = nullptr;
    ID3DBlob* nv12_pixel_blob = nullptr;
    ID3DBlob* error_blob = nullptr;

    hr = D3DCompile(
        kVertexShaderSource,
        sizeof(kVertexShaderSource) - 1,
        nullptr,
        nullptr,
        nullptr,
        "main",
        "vs_4_0",
        0,
        0,
        &vertex_blob,
        &error_blob);
    if (FAILED(hr)) {
        SafeRelease(error_blob);
        return false;
    }

    hr = D3DCompile(
        kBgraPixelShaderSource,
        sizeof(kBgraPixelShaderSource) - 1,
        nullptr,
        nullptr,
        nullptr,
        "main",
        "ps_4_0",
        0,
        0,
        &bgra_pixel_blob,
        &error_blob);
    SafeRelease(error_blob);
    if (FAILED(hr)) {
        SafeRelease(vertex_blob);
        return false;
    }

    hr = D3DCompile(
        kNv12PixelShaderSource,
        sizeof(kNv12PixelShaderSource) - 1,
        nullptr,
        nullptr,
        nullptr,
        "main",
        "ps_4_0",
        0,
        0,
        &nv12_pixel_blob,
        &error_blob);
    SafeRelease(error_blob);
    if (FAILED(hr)) {
        SafeRelease(vertex_blob);
        SafeRelease(bgra_pixel_blob);
        return false;
    }

    hr = device_->CreateVertexShader(
        vertex_blob->GetBufferPointer(),
        vertex_blob->GetBufferSize(),
        nullptr,
        &vertex_shader_);
    if (FAILED(hr)) {
        SafeRelease(vertex_blob);
        SafeRelease(bgra_pixel_blob);
        SafeRelease(nv12_pixel_blob);
        return false;
    }

    hr = device_->CreatePixelShader(
        bgra_pixel_blob->GetBufferPointer(),
        bgra_pixel_blob->GetBufferSize(),
        nullptr,
        &bgra_pixel_shader_);
    if (FAILED(hr)) {
        SafeRelease(vertex_blob);
        SafeRelease(bgra_pixel_blob);
        SafeRelease(nv12_pixel_blob);
        return false;
    }

    hr = device_->CreatePixelShader(
        nv12_pixel_blob->GetBufferPointer(),
        nv12_pixel_blob->GetBufferSize(),
        nullptr,
        &nv12_pixel_shader_);
    SafeRelease(vertex_blob);
    SafeRelease(bgra_pixel_blob);
    SafeRelease(nv12_pixel_blob);
    if (FAILED(hr)) {
        return false;
    }

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sampler_desc, &sampler_state_))) {
        return false;
    }

    return CreateSwapChainResources();
}

bool VideoRenderer::EnableMultithreadProtection() {
    IUnknown* multithread_unknown = nullptr;
    HRESULT hr = E_NOINTERFACE;

    if (device_ != nullptr) {
        hr = device_->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&multithread_unknown));
    }
    if (FAILED(hr) && context_ != nullptr) {
        hr = context_->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&multithread_unknown));
    }
    if (FAILED(hr) || multithread_unknown == nullptr) {
        if (log_fn_ != nullptr) {
            log_fn_(L"\u89c6\u9891\u6e32\u67d3: \u672a\u80fd\u542f\u7528 D3D11 \u591a\u7ebf\u7a0b\u4fdd\u62a4\uff0c\u82e5\u4ecd\u6709\u9ed1\u5c4f\u6216\u82b1\u5c4f\u9700\u7ee7\u7eed\u6392\u67e5\u3002");
        }
        return false;
    }

    auto* multithread = reinterpret_cast<ID3D10Multithread*>(multithread_unknown);
    multithread->SetMultithreadProtected(TRUE);
    const BOOL enabled = multithread->GetMultithreadProtected();
    multithread->Release();

    if (enabled == TRUE && log_fn_ != nullptr) {
        log_fn_(L"\u89c6\u9891\u6e32\u67d3: \u5df2\u542f\u7528 D3D11 \u591a\u7ebf\u7a0b\u4fdd\u62a4\uff0c\u51cf\u5c11\u89e3\u7801\u4e0e\u6e32\u67d3\u5e76\u53d1\u65f6\u7684\u9ed1\u5c4f/\u82b1\u5c4f\u98ce\u9669\u3002");
    }
    return enabled == TRUE;
}

bool VideoRenderer::CreateSwapChainResources() {
    if (swap_chain_ == nullptr || device_ == nullptr) {
        return false;
    }

    ID3D11Texture2D* back_buffer = nullptr;
    const HRESULT hr = swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer));
    if (FAILED(hr)) {
        return false;
    }

    const HRESULT rtv_hr = device_->CreateRenderTargetView(back_buffer, nullptr, &render_target_view_);
    SafeRelease(back_buffer);
    return SUCCEEDED(rtv_hr);
}

void VideoRenderer::ResizeSwapChain() {
    if (swap_chain_ == nullptr || context_ == nullptr) {
        return;
    }

    RECT client_rect{};
    GetClientRect(window_, &client_rect);
    const UINT width = std::max<LONG>(1, client_rect.right - client_rect.left);
    const UINT height = std::max<LONG>(1, client_rect.bottom - client_rect.top);

    context_->OMSetRenderTargets(0, nullptr, nullptr);
    SafeRelease(render_target_view_);
    render_target_view_ = nullptr;

    if (SUCCEEDED(swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, allow_tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0))) {
        CreateSwapChainResources();
    }
}

void VideoRenderer::ClearExternalVideoResources() {
    SafeRelease(external_srv0_);
    external_srv0_ = nullptr;
    SafeRelease(external_srv1_);
    external_srv1_ = nullptr;
    SafeRelease(external_texture_);
    external_texture_ = nullptr;
    using_external_texture_ = false;
    using_gpu_copy_texture_ = false;
    texture_format_ = DecodedFrameFormat::kUnknown;
    texture_width_ = 0;
    texture_height_ = 0;
}

bool VideoRenderer::UseExternalTextureFrame(const DecodedFrame& frame) {
    if (device_ == nullptr ||
        frame.d3d_texture == nullptr ||
        frame.width <= 0 ||
        frame.height <= 0) {
        return false;
    }

    const bool same_external_texture =
        using_external_texture_ &&
        !using_gpu_copy_texture_ &&
        external_texture_ == frame.d3d_texture &&
        texture_format_ == frame.format &&
        texture_width_ == frame.width &&
        texture_height_ == frame.height &&
        external_srv0_ != nullptr &&
        (frame.format != DecodedFrameFormat::kNv12 || external_srv1_ != nullptr);
    if (same_external_texture) {
        return true;
    }

    if (frame.d3d_subresource != 0) {
        return false;
    }

    ClearExternalVideoResources();

    external_texture_ = frame.d3d_texture;
    external_texture_->AddRef();

    if (frame.format == DecodedFrameFormat::kBgra) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        if (FAILED(device_->CreateShaderResourceView(external_texture_, &srv_desc, &external_srv0_))) {
            ClearExternalVideoResources();
            return false;
        }
    } else if (frame.format == DecodedFrameFormat::kNv12) {
        D3D11_SHADER_RESOURCE_VIEW_DESC y_srv_desc{};
        y_srv_desc.Format = DXGI_FORMAT_R8_UNORM;
        y_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        y_srv_desc.Texture2D.MipLevels = 1;
        if (FAILED(device_->CreateShaderResourceView(external_texture_, &y_srv_desc, &external_srv0_))) {
            ClearExternalVideoResources();
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC uv_srv_desc{};
        uv_srv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
        uv_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        uv_srv_desc.Texture2D.MipLevels = 1;
        if (FAILED(device_->CreateShaderResourceView(external_texture_, &uv_srv_desc, &external_srv1_))) {
            ClearExternalVideoResources();
            return false;
        }
    } else {
        ClearExternalVideoResources();
        return false;
    }

    using_external_texture_ = true;
    texture_format_ = frame.format;
    texture_width_ = frame.width;
    texture_height_ = frame.height;
    return true;
}

bool VideoRenderer::UseGpuCopiedTextureFrame(const DecodedFrame& frame) {
    if (device_ == nullptr ||
        context_ == nullptr ||
        frame.d3d_texture == nullptr ||
        frame.width <= 0 ||
        frame.height <= 0 ||
        frame.format == DecodedFrameFormat::kUnknown) {
        return false;
    }

    D3D11_TEXTURE2D_DESC source_desc{};
    frame.d3d_texture->GetDesc(&source_desc);
    if (source_desc.Width == 0 || source_desc.Height == 0) {
        return false;
    }

    const bool can_reuse_texture =
        using_external_texture_ &&
        using_gpu_copy_texture_ &&
        external_texture_ != nullptr &&
        texture_format_ == frame.format &&
        texture_width_ == frame.width &&
        texture_height_ == frame.height &&
        external_srv0_ != nullptr &&
        (frame.format != DecodedFrameFormat::kNv12 || external_srv1_ != nullptr);

    if (!can_reuse_texture) {
        ClearExternalVideoResources();

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = source_desc.Width;
        desc.Height = source_desc.Height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        if (frame.format == DecodedFrameFormat::kBgra) {
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        } else if (frame.format == DecodedFrameFormat::kNv12) {
            desc.Format = DXGI_FORMAT_NV12;
        } else {
            return false;
        }

        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &external_texture_))) {
            ClearExternalVideoResources();
            return false;
        }

        if (frame.format == DecodedFrameFormat::kBgra) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
            srv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Texture2D.MipLevels = 1;
            if (FAILED(device_->CreateShaderResourceView(external_texture_, &srv_desc, &external_srv0_))) {
                ClearExternalVideoResources();
                return false;
            }
        } else {
            D3D11_SHADER_RESOURCE_VIEW_DESC y_srv_desc{};
            y_srv_desc.Format = DXGI_FORMAT_R8_UNORM;
            y_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            y_srv_desc.Texture2D.MipLevels = 1;
            if (FAILED(device_->CreateShaderResourceView(external_texture_, &y_srv_desc, &external_srv0_))) {
                ClearExternalVideoResources();
                return false;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC uv_srv_desc{};
            uv_srv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
            uv_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            uv_srv_desc.Texture2D.MipLevels = 1;
            if (FAILED(device_->CreateShaderResourceView(external_texture_, &uv_srv_desc, &external_srv1_))) {
                ClearExternalVideoResources();
                return false;
            }
        }

        using_external_texture_ = true;
        using_gpu_copy_texture_ = true;
        texture_format_ = frame.format;
        texture_width_ = frame.width;
        texture_height_ = frame.height;
    }

    context_->CopySubresourceRegion(
        external_texture_,
        0,
        0,
        0,
        0,
        frame.d3d_texture,
        frame.d3d_subresource,
        nullptr);
    return true;
}

void VideoRenderer::Render() {
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        render_posted_ = false;
    }

    if (context_ == nullptr || swap_chain_ == nullptr || render_target_view_ == nullptr) {
        return;
    }

    DecodedFrame frame_to_upload;
    bool has_new_frame = false;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (frame_dirty_) {
            frame_to_upload = std::move(latest_frame_);
            latest_frame_ = DecodedFrame{};
            frame_dirty_ = false;
            has_new_frame = true;
        }
    }

    if (has_new_frame &&
        frame_to_upload.width > 0 &&
        frame_to_upload.height > 0) {
        if (frame_to_upload.gpu_backed && frame_to_upload.d3d_texture != nullptr) {
            if (frame_to_upload.d3d_texture != nullptr) {
                if (UseGpuCopiedTextureFrame(frame_to_upload)) {
                    if (!logged_gpu_copy_fallback_ && log_fn_ != nullptr) {
                        log_fn_(L"\u89c6\u9891\u6e32\u67d3: \u5df2\u9ed8\u8ba4\u542f\u7528 GPU \u663e\u5b58\u5185\u590d\u5236\u663e\u793a\u8def\u5f84\uff0c\u4fdd\u6301 N \u5361\u89e3\u7801 + GPU \u6e32\u67d3\uff0c\u540c\u65f6\u907f\u514d\u76f4\u63a5\u91c7\u6837\u89e3\u7801\u7eb9\u7406\u5bfc\u81f4\u7684\u9ed1\u5c4f\u3002");
                        log_fn_(L"视频渲染: 已切换到 GPU 复制显示路径，以兼容解码器输出纹理。");
                        logged_gpu_copy_fallback_ = true;
                    }
                } else if (!UseExternalTextureFrame(frame_to_upload) && log_fn_ != nullptr) {
                    log_fn_(L"\u89c6\u9891\u6e32\u67d3: GPU \u590d\u5236\u663e\u793a\u8def\u5f84\u4e0e\u76f4\u63a5\u7eb9\u7406\u91c7\u6837\u90fd\u4e0d\u53ef\u7528\uff0c\u5f53\u524d\u5e27\u65e0\u6cd5\u663e\u793a\u3002");
                    log_fn_(L"视频渲染: 当前帧纹理无法直接显示，也无法完成 GPU 复制。");
                }
            }
        }
    }

    const float clear_color[4] = {0.02f, 0.02f, 0.02f, 1.0f};
    context_->OMSetRenderTargets(1, &render_target_view_, nullptr);
    context_->ClearRenderTargetView(render_target_view_, clear_color);

    ID3D11ShaderResourceView* active_srv0 = external_srv0_;
    ID3D11ShaderResourceView* active_srv1 = external_srv1_;
    const bool can_draw_bgra =
        texture_format_ == DecodedFrameFormat::kBgra &&
        active_srv0 != nullptr &&
        texture_width_ > 0 &&
        texture_height_ > 0;
    const bool can_draw_nv12 =
        texture_format_ == DecodedFrameFormat::kNv12 &&
        active_srv0 != nullptr &&
        active_srv1 != nullptr &&
        texture_width_ > 0 &&
        texture_height_ > 0;

    if (can_draw_bgra || can_draw_nv12) {
        RECT client_rect{};
        GetClientRect(window_, &client_rect);
        const float client_width = static_cast<float>(std::max<LONG>(1, client_rect.right - client_rect.left));
        const float client_height = static_cast<float>(std::max<LONG>(1, client_rect.bottom - client_rect.top));
        const float scale = std::min(
            client_width / static_cast<float>(texture_width_),
            client_height / static_cast<float>(texture_height_));
        const float draw_width = static_cast<float>(texture_width_) * scale;
        const float draw_height = static_cast<float>(texture_height_) * scale;
        const float draw_x = (client_width - draw_width) * 0.5f;
        const float draw_y = (client_height - draw_height) * 0.5f;

        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = draw_x;
        viewport.TopLeftY = draw_y;
        viewport.Width = std::max(1.0f, draw_width);
        viewport.Height = std::max(1.0f, draw_height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        context_->RSSetViewports(1, &viewport);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->VSSetShader(vertex_shader_, nullptr, 0);
        context_->PSSetSamplers(0, 1, &sampler_state_);

        if (can_draw_bgra) {
            context_->PSSetShader(bgra_pixel_shader_, nullptr, 0);
            ID3D11ShaderResourceView* srvs[1] = {active_srv0};
            context_->PSSetShaderResources(0, 1, srvs);
            context_->Draw(4, 0);
            ID3D11ShaderResourceView* null_srvs[1] = {nullptr};
            context_->PSSetShaderResources(0, 1, null_srvs);
        } else {
            context_->PSSetShader(nv12_pixel_shader_, nullptr, 0);
            ID3D11ShaderResourceView* srvs[2] = {active_srv0, active_srv1};
            context_->PSSetShaderResources(0, 2, srvs);
            context_->Draw(4, 0);
            ID3D11ShaderResourceView* null_srvs[2] = {nullptr, nullptr};
            context_->PSSetShaderResources(0, 2, null_srvs);
        }
    }

    swap_chain_->Present(0, 0);
    if (has_new_frame && frame_to_upload.pts_us != 0 && present_fn_ != nullptr) {
        present_fn_(frame_to_upload.pts_us);
    }
}

bool VideoRenderer::CheckTearingSupport() const {
    if (dxgi_factory5_ == nullptr) {
        return false;
    }

    BOOL allow_tearing = FALSE;
    const HRESULT hr = dxgi_factory5_->CheckFeatureSupport(
        DXGI_FEATURE_PRESENT_ALLOW_TEARING,
        &allow_tearing,
        sizeof(allow_tearing));
    return SUCCEEDED(hr) && allow_tearing == TRUE;
}

void VideoRenderer::CleanupDeviceResources() {
    ClearExternalVideoResources();
    SafeRelease(sampler_state_);
    sampler_state_ = nullptr;
    SafeRelease(vertex_shader_);
    vertex_shader_ = nullptr;
    SafeRelease(bgra_pixel_shader_);
    bgra_pixel_shader_ = nullptr;
    SafeRelease(nv12_pixel_shader_);
    nv12_pixel_shader_ = nullptr;
    SafeRelease(render_target_view_);
    render_target_view_ = nullptr;
    SafeRelease(swap_chain_);
    swap_chain_ = nullptr;
    SafeRelease(dxgi_factory5_);
    dxgi_factory5_ = nullptr;
    SafeRelease(context_);
    context_ = nullptr;
    SafeRelease(device_);
    device_ = nullptr;
    allow_tearing_ = false;
}
