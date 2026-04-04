#include "VideoDecoder.h"
#include "NvidiaCuvidDecoder.h"

#include <Windows.h>
#include <codecapi.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {

constexpr size_t kZeroCopySurfacePoolSize = 8;

enum class DecoderBackend {
    kNone,
    kNvidiaCuvid,
    kMediaFoundation,
};

struct DecoderContext {
    DecoderBackend backend = DecoderBackend::kNone;
    protocol::StreamProfile active_profile;
    ComPtr<IMFTransform> transform;
    ComPtr<IMFMediaType> output_type;
    ComPtr<IMFDXGIDeviceManager> device_manager;
    std::unique_ptr<NvidiaCuvidDecoder> nvidia_cuvid_decoder;
    GUID output_subtype = GUID_NULL;
    UINT32 coded_width = 0;
    UINT32 coded_height = 0;
    UINT32 display_width = 0;
    UINT32 display_height = 0;
    LONG stride = 0;
    bool streaming_started = false;
    bool using_d3d_manager = false;
    bool mf_zero_copy_enabled = false;
    uint64_t decoded_frame_count = 0;
    size_t output_surface_index = 0;
    std::vector<ComPtr<ID3D11Texture2D>> output_surfaces;
};

std::wstring HrToString(HRESULT hr) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase << hr;
    return stream.str();
}

LONG GetDefaultStride(IMFMediaType* media_type, UINT32 width) {
    LONG stride = 0;
    if (media_type != nullptr &&
        SUCCEEDED(media_type->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&stride)))) {
        return stride;
    }
    return static_cast<LONG>(width);
}

const wchar_t* SubtypeName(const GUID& subtype) {
    if (subtype == MFVideoFormat_RGB32) {
        return L"RGB32";
    }
    if (subtype == MFVideoFormat_NV12) {
        return L"NV12";
    }
    return L"UNKNOWN";
}

void CloseHandleIfValid(HANDLE* handle) {
    if (handle == nullptr || *handle == nullptr || *handle == INVALID_HANDLE_VALUE) {
        return;
    }
    CloseHandle(*handle);
    *handle = nullptr;
}

void SetCurrentThreadPriorityBestEffort(int priority) {
    SetThreadPriority(GetCurrentThread(), priority);
}

size_t QueueWarningThresholdForProfile(const protocol::StreamProfile& profile, bool smooth_mode) {
    const int fps = std::max(0, profile.fps);
    if (fps <= 0) {
        return smooth_mode ? 18 : 12;
    }
    if (smooth_mode) {
        return static_cast<size_t>(std::clamp(fps / 3, 18, 40));
    }
    return static_cast<size_t>(std::clamp(fps / 5, 12, 28));
}

size_t QueueResyncThresholdForProfile(const protocol::StreamProfile& profile, bool smooth_mode) {
    const int fps = std::max(0, profile.fps);
    if (fps <= 0) {
        return smooth_mode ? 48 : 24;
    }
    if (smooth_mode) {
        return static_cast<size_t>(std::clamp((fps * 2) / 3, 48, 84));
    }
    return static_cast<size_t>(std::clamp(fps / 2, 24, 56));
}

size_t QueueResyncHitThresholdForProfile(const protocol::StreamProfile& profile, bool smooth_mode) {
    const int fps = std::max(0, profile.fps);
    if (fps <= 0) {
        return smooth_mode ? 6 : 4;
    }
    if (smooth_mode) {
        return static_cast<size_t>(std::clamp(fps / 20, 6, 10));
    }
    return static_cast<size_t>(std::clamp(fps / 24, 4, 8));
}

size_t QueueWarningResetThreshold(size_t warning_threshold) {
    return std::max<size_t>(2, warning_threshold / 2);
}

bool ConfigureMediaFoundationTransform(
    const protocol::StreamProfile& profile,
    ID3D11Device* d3d_device,
    DecoderContext* context,
    const VideoDecoder::LogFn& log_fn);

bool ConfigureDecoderBackend(
    const protocol::StreamProfile& profile,
    ID3D11Device* d3d_device,
    DecoderContext* context,
    const VideoDecoder::LogFn& log_fn,
    const VideoDecoder::FrameFn& frame_fn);

HRESULT CreateOutputSurfaceTexture(
    ID3D11Device* d3d_device,
    DecoderContext* context,
    ID3D11Texture2D** texture_out) {
    if (texture_out == nullptr) {
        return E_POINTER;
    }
    *texture_out = nullptr;
    if (d3d_device == nullptr || context == nullptr) {
        return E_POINTER;
    }

    DXGI_FORMAT texture_format = DXGI_FORMAT_UNKNOWN;
    UINT bind_flags = D3D11_BIND_SHADER_RESOURCE;
    if (context->output_subtype == MFVideoFormat_NV12) {
        texture_format = DXGI_FORMAT_NV12;
        bind_flags |= D3D11_BIND_DECODER;
    } else if (context->output_subtype == MFVideoFormat_RGB32) {
        texture_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    } else {
        return MF_E_INVALIDMEDIATYPE;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = std::max<UINT32>(1, context->coded_width);
    desc.Height = std::max<UINT32>(1, context->coded_height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = texture_format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind_flags;
    return d3d_device->CreateTexture2D(&desc, nullptr, texture_out);
}

bool IsSurfaceAvailable(ID3D11Texture2D* surface) {
    if (surface == nullptr) {
        return false;
    }

    const ULONG ref_count = surface->AddRef();
    surface->Release();
    return ref_count <= 2;
}

void ResetZeroCopyOutputSurfaces(DecoderContext* context) {
    if (context == nullptr) {
        return;
    }
    context->mf_zero_copy_enabled = false;
    context->output_surface_index = 0;
    context->output_surfaces.clear();
}

bool CreateZeroCopyOutputSurfaces(
    ID3D11Device* d3d_device,
    DecoderContext* context,
    const VideoDecoder::LogFn& log_fn) {
    ResetZeroCopyOutputSurfaces(context);
    if (context == nullptr || d3d_device == nullptr || !context->using_d3d_manager) {
        return false;
    }

    for (size_t index = 0; index < kZeroCopySurfacePoolSize; ++index) {
        ComPtr<ID3D11Texture2D> surface;
        const HRESULT hr = CreateOutputSurfaceTexture(d3d_device, context, surface.GetAddressOf());
        if (FAILED(hr)) {
            log_fn(L"视频解码: 创建零拷贝输出纹理失败，错误码 " + HrToString(hr));
            ResetZeroCopyOutputSurfaces(context);
            return false;
        }
        context->output_surfaces.push_back(std::move(surface));
    }

    context->mf_zero_copy_enabled = true;

    std::wostringstream stream;
    stream << L"视频解码: 已启用 D3D11 零拷贝输出，纹理池="
           << context->output_surfaces.size()
           << L"，格式="
           << SubtypeName(context->output_subtype);
    log_fn(stream.str());
    return true;
}

HRESULT CreateDecoderOutputSample(
    DecoderContext* context,
    const MFT_OUTPUT_STREAM_INFO& stream_info,
    const VideoDecoder::LogFn& log_fn,
    ID3D11Device* d3d_device,
    IMFSample** sample_out) {
    if (sample_out == nullptr) {
        return E_POINTER;
    }
    *sample_out = nullptr;

    if ((stream_info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0) {
        return S_OK;
    }

    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        return hr;
    }

    if (context != nullptr && context->mf_zero_copy_enabled) {
        ID3D11Texture2D* surface = nullptr;
        for (size_t attempt = 0; attempt < context->output_surfaces.size(); ++attempt) {
            ID3D11Texture2D* candidate =
                context->output_surfaces[(context->output_surface_index + attempt) % context->output_surfaces.size()].Get();
            if (IsSurfaceAvailable(candidate)) {
                surface = candidate;
                context->output_surface_index =
                    (context->output_surface_index + attempt + 1) % context->output_surfaces.size();
                break;
            }
        }

        ComPtr<ID3D11Texture2D> transient_surface;
        if (surface == nullptr) {
            const HRESULT create_hr = CreateOutputSurfaceTexture(d3d_device, context, transient_surface.GetAddressOf());
            if (FAILED(create_hr)) {
                log_fn(L"视频解码: 纹理池已占满且临时纹理创建失败，错误码 " + HrToString(create_hr));
                return create_hr;
            }
            surface = transient_surface.Get();
        }

        hr = MFCreateDXGISurfaceBuffer(
            __uuidof(ID3D11Texture2D),
            surface,
            0,
            FALSE,
            &buffer);
        if (SUCCEEDED(hr)) {
            hr = sample->AddBuffer(buffer.Get());
        }
        if (SUCCEEDED(hr)) {
            *sample_out = sample.Detach();
            return S_OK;
        }

        log_fn(L"视频解码: 零拷贝输出样本创建失败，错误码 " + HrToString(hr));
        ResetZeroCopyOutputSurfaces(context);
    }

    (void)stream_info;
    (void)sample;
    return MF_E_INVALIDREQUEST;
}

void StopDecoderBackend(DecoderContext* context) {
    if (context == nullptr) {
        return;
    }
    ResetZeroCopyOutputSurfaces(context);
    context->nvidia_cuvid_decoder.reset();
    context->output_type.Reset();
    context->transform.Reset();
    context->device_manager.Reset();
    context->backend = DecoderBackend::kNone;
    context->using_d3d_manager = false;
    context->output_subtype = GUID_NULL;
}

void SoftResyncDecoder(
    DecoderContext* context,
    ID3D11Device* d3d_device,
    const VideoDecoder::LogFn& log_fn,
    const VideoDecoder::FrameFn& frame_fn) {
    if (context == nullptr) {
        return;
    }

    if (context->backend == DecoderBackend::kMediaFoundation) {
        log_fn(L"视频解码: 已执行软重同步，重新初始化 Media Foundation 解码器。");
        ConfigureMediaFoundationTransform(context->active_profile, d3d_device, context, log_fn);
        return;
    }

    if (context->active_profile.width > 0 && context->active_profile.height > 0) {
        ConfigureDecoderBackend(context->active_profile, d3d_device, context, log_fn, frame_fn);
    }
}

bool TrySetOutputType(IMFTransform* transform, IMFMediaType* type, DecoderContext* context) {
    GUID subtype = GUID_NULL;
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
        return false;
    }
    if (FAILED(transform->SetOutputType(0, type, 0))) {
        return false;
    }

    context->output_type = type;
    context->output_subtype = subtype;
    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &context->coded_width, &context->coded_height);
    context->stride = GetDefaultStride(type, context->coded_width);
    return true;
}

bool SelectOutputType(IMFTransform* transform, DecoderContext* context) {
    ComPtr<IMFMediaType> rgb32_type;
    ComPtr<IMFMediaType> nv12_type;

    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> type;
        const HRESULT hr = transform->GetOutputAvailableType(0, index, &type);
        if (hr == MF_E_NO_MORE_TYPES) {
            break;
        }
        if (FAILED(hr)) {
            return false;
        }

        GUID subtype = GUID_NULL;
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
            continue;
        }
        if (subtype == MFVideoFormat_RGB32 && rgb32_type == nullptr) {
            rgb32_type = type;
        } else if (subtype == MFVideoFormat_NV12 && nv12_type == nullptr) {
            nv12_type = type;
        }
    }

    if (nv12_type != nullptr && TrySetOutputType(transform, nv12_type.Get(), context)) {
        return true;
    }
    if (rgb32_type != nullptr && TrySetOutputType(transform, rgb32_type.Get(), context)) {
        return true;
    }
    return false;
}

bool ConfigureMediaFoundationTransform(
    const protocol::StreamProfile& profile,
    ID3D11Device* d3d_device,
    DecoderContext* context,
    const VideoDecoder::LogFn& log_fn) {
    context->active_profile = profile;
    context->transform.Reset();
    context->output_type.Reset();
    context->device_manager.Reset();
    context->output_subtype = GUID_NULL;
    context->coded_width = static_cast<UINT32>(profile.width);
    context->coded_height = static_cast<UINT32>(profile.height);
    context->display_width = static_cast<UINT32>(profile.width);
    context->display_height = static_cast<UINT32>(profile.height);
    context->stride = static_cast<LONG>(profile.width);
    context->streaming_started = false;
    context->using_d3d_manager = false;
    context->mf_zero_copy_enabled = false;
    context->decoded_frame_count = 0;
    ResetZeroCopyOutputSurfaces(context);

    if (profile.codec != protocol::Codec::kAvc) {
        log_fn(L"视频解码: 当前阶段只实现了 AVC。");
        return false;
    }

    HRESULT hr = CoCreateInstance(
        CLSID_CMSH264DecoderMFT,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&context->transform));
    if (FAILED(hr)) {
        log_fn(L"视频解码: CoCreateInstance 失败，错误码 " + HrToString(hr));
        return false;
    }

    ComPtr<IMFAttributes> transform_attributes;
    if (SUCCEEDED(context->transform->GetAttributes(&transform_attributes)) && transform_attributes != nullptr) {
        transform_attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        transform_attributes->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
        transform_attributes->SetUINT32(MF_SA_D3D_AWARE, TRUE);
    }

    if (d3d_device != nullptr) {
        UINT reset_token = 0;
        hr = MFCreateDXGIDeviceManager(&reset_token, &context->device_manager);
        if (SUCCEEDED(hr)) {
            hr = context->device_manager->ResetDevice(d3d_device, reset_token);
        }
        if (SUCCEEDED(hr)) {
            hr = context->transform->ProcessMessage(
                MFT_MESSAGE_SET_D3D_MANAGER,
                reinterpret_cast<ULONG_PTR>(context->device_manager.Get()));
        }
        if (SUCCEEDED(hr)) {
            context->using_d3d_manager = true;
            log_fn(L"视频解码: 已绑定 D3D11 设备管理器，优先使用 GPU 解码。");
        } else {
            log_fn(L"视频解码: 绑定 D3D11 设备管理器失败，错误码 " + HrToString(hr));
            context->device_manager.Reset();
        }
    }

    ComPtr<ICodecAPI> codec_api;
    if (SUCCEEDED(context->transform.As(&codec_api))) {
        VARIANT variant{};
        variant.vt = VT_UI4;
        variant.ulVal = TRUE;
        codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &variant);
        codec_api->SetValue(&CODECAPI_AVDecVideoAcceleration_H264, &variant);
        variant.ulVal = 1;
        codec_api->SetValue(&CODECAPI_AVDecNumWorkerThreads, &variant);
    }

    ComPtr<IMFMediaType> input_type;
    hr = MFCreateMediaType(&input_type);
    if (FAILED(hr)) {
        log_fn(L"视频解码: MFCreateMediaType 失败，错误码 " + HrToString(hr));
        return false;
    }

    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264_ES);
    MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, profile.width, profile.height);
    MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, profile.fps, 1);
    MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    input_type->SetUINT32(MF_MT_AVG_BITRATE, profile.bitrate);

    hr = context->transform->SetInputType(0, input_type.Get(), 0);
    if (FAILED(hr)) {
        log_fn(L"视频解码: SetInputType 失败，错误码 " + HrToString(hr));
        return false;
    }

    if (!SelectOutputType(context->transform.Get(), context)) {
        log_fn(L"视频解码: 选择输出格式失败。");
        return false;
    }

    if (!CreateZeroCopyOutputSurfaces(d3d_device, context, log_fn)) {
        log_fn(L"视频解码: 零拷贝输出初始化失败，已停止继续配置旧链路。");
        return false;
    }

    context->transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    context->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    context->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    context->streaming_started = true;
    context->backend = DecoderBackend::kMediaFoundation;

    std::wostringstream stream;
    stream << L"视频解码: 已切到 Media Foundation 后备解码 "
           << profile.width << L"x" << profile.height
           << L" @" << (profile.adaptive_fps ? L"\u81EA\u9002\u5E94\u5237\u65B0\u7387" : (std::to_wstring(profile.fps) + L"fps"))
           << L" 输出=" << SubtypeName(context->output_subtype)
           << L" 硬件解码=" << (context->using_d3d_manager ? L"已请求" : L"未请求")
           << L" 零拷贝=" << (context->mf_zero_copy_enabled ? L"已启用" : L"未启用");
    log_fn(stream.str());
    return true;
}

bool ConfigureDecoderBackend(
    const protocol::StreamProfile& profile,
    ID3D11Device* d3d_device,
    DecoderContext* context,
    const VideoDecoder::LogFn& log_fn,
    const VideoDecoder::FrameFn& frame_fn) {
    StopDecoderBackend(context);

    if (profile.codec == protocol::Codec::kAvc) {
        auto nvidia_decoder = std::make_unique<NvidiaCuvidDecoder>(log_fn, frame_fn);
        if (nvidia_decoder->Configure(profile, d3d_device)) {
            context->active_profile = profile;
            context->backend = DecoderBackend::kNvidiaCuvid;
            context->nvidia_cuvid_decoder = std::move(nvidia_decoder);
            context->decoded_frame_count = 0;
            return true;
        }

        if (log_fn) {
            log_fn(L"视频解码: NVIDIA CUVID 后端未能接管，回退到 Media Foundation。");
        }
    }

    return ConfigureMediaFoundationTransform(profile, d3d_device, context, log_fn);
}

DecodedFrame CopySampleToFrame(IMFSample* sample, DecoderContext* context, const VideoDecoder::LogFn& log_fn) {
    DecodedFrame frame;
    if (sample == nullptr) {
        return frame;
    }

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) {
        log_fn(L"视频解码: 读取输出 Buffer 失败，错误码 " + HrToString(hr));
        return frame;
    }

    frame.width = static_cast<int>(context->display_width);
    frame.height = static_cast<int>(context->display_height);
    LONGLONG sample_time_100ns = 0;
    if (SUCCEEDED(sample->GetSampleTime(&sample_time_100ns)) && sample_time_100ns > 0) {
        frame.pts_us = static_cast<uint64_t>(sample_time_100ns / 10);
    }

    ComPtr<IMFDXGIBuffer> dxgi_buffer;
    if (SUCCEEDED(buffer.As(&dxgi_buffer))) {
        ComPtr<ID3D11Texture2D> texture;
        UINT subresource = 0;
        if (SUCCEEDED(dxgi_buffer->GetResource(IID_PPV_ARGS(&texture))) &&
            SUCCEEDED(dxgi_buffer->GetSubresourceIndex(&subresource)) &&
            texture != nullptr) {
            D3D11_TEXTURE2D_DESC texture_desc{};
            texture->GetDesc(&texture_desc);

            if (texture_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || context->output_subtype == MFVideoFormat_RGB32) {
                frame.format = DecodedFrameFormat::kBgra;
                frame.stride0 = static_cast<int>(context->display_width * 4);
                frame.stride1 = 0;
                frame.plane1_offset = 0;
            } else if (texture_desc.Format == DXGI_FORMAT_NV12 || context->output_subtype == MFVideoFormat_NV12) {
                frame.format = DecodedFrameFormat::kNv12;
                frame.stride0 = static_cast<int>(context->display_width);
                frame.stride1 = static_cast<int>(context->display_width);
                frame.plane1_offset =
                    static_cast<size_t>(context->display_width) * static_cast<size_t>(context->display_height);
            }

            if (frame.format != DecodedFrameFormat::kUnknown) {
                frame.gpu_backed = true;
                frame.d3d_texture = texture.Detach();
                frame.d3d_subresource = subresource;

                const uint64_t frame_index = context->decoded_frame_count + 1;
                if (frame_index == 1 || (frame_index % 120) == 0) {
                    std::wostringstream stream;
                    stream << L"视频解码: 已输出 GPU 纹理帧 "
                           << frame_index
                           << L"，格式=" << SubtypeName(context->output_subtype)
                           << L"，尺寸=" << frame.width << L"x" << frame.height;
                    log_fn(stream.str());
                }
                return frame;
            }
        }
    }
    log_fn(L"视频解码: 当前样本未返回 GPU 纹理，已按零拷贝模式丢弃该帧。");
    return frame;
}

void DrainOutput(
    DecoderContext* context,
    ID3D11Device* d3d_device,
    const VideoDecoder::LogFn& log_fn,
    const VideoDecoder::FrameFn& frame_fn) {
    if (context->transform == nullptr) {
        return;
    }

    MFT_OUTPUT_STREAM_INFO stream_info{};
    if (FAILED(context->transform->GetOutputStreamInfo(0, &stream_info))) {
        return;
    }

    while (true) {
        ComPtr<IMFSample> sample;
        IMFSample* output_sample_input = nullptr;
        const HRESULT sample_hr = CreateDecoderOutputSample(
            context,
            stream_info,
            log_fn,
            d3d_device,
            &output_sample_input);
        if (FAILED(sample_hr)) {
            log_fn(L"视频解码: 创建输出样本失败，错误码 " + HrToString(sample_hr));
            return;
        }
        if (output_sample_input != nullptr) {
            sample.Attach(output_sample_input);
        }

        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = 0;
        output.pSample = sample.Get();
        DWORD status = 0;

        const HRESULT hr = context->transform->ProcessOutput(0, 1, &output, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            break;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!SelectOutputType(context->transform.Get(), context)) {
                log_fn(L"视频解码: 处理流格式变化失败。");
            } else {
                if (!CreateZeroCopyOutputSurfaces(d3d_device, context, log_fn)) {
                    log_fn(L"视频解码: 流格式变化后无法继续建立零拷贝输出。");
                    break;
                }
            }
            continue;
        }
        if (FAILED(hr)) {
            log_fn(L"视频解码: ProcessOutput 失败，错误码 " + HrToString(hr));
            break;
        }

        IMFSample* output_sample = output.pSample;
        if (output_sample != nullptr) {
            DecodedFrame frame = CopySampleToFrame(output_sample, context, log_fn);
            if (frame.gpu_backed || !frame.bytes.empty()) {
                ++context->decoded_frame_count;
                if (context->decoded_frame_count == 1 || (context->decoded_frame_count % 120) == 0) {
                    std::wostringstream stream;
                    stream << L"视频解码: 已输出 "
                           << context->decoded_frame_count
                           << L" 帧，尺寸="
                           << frame.width << L"x" << frame.height
                           << L"，路径=" << (frame.gpu_backed ? L"GPU 直出" : L"CPU 回拷");
                    log_fn(stream.str());
                }
                frame_fn(std::move(frame));
            }
        }
        if (output.pSample != nullptr && output.pSample != sample.Get()) {
            output.pSample->Release();
            output.pSample = nullptr;
        }
        if (output.pEvents != nullptr) {
            output.pEvents->Release();
        }
    }
}

}  // namespace

VideoDecoder::VideoDecoder(LogFn log_fn, FrameFn frame_fn, RequestKeyframeFn request_keyframe_fn)
    : log_fn_(std::move(log_fn)),
      frame_fn_(std::move(frame_fn)),
      request_keyframe_fn_(std::move(request_keyframe_fn)) {}

VideoDecoder::~VideoDecoder() {
    Stop();
    if (d3d_device_ != nullptr) {
        d3d_device_->Release();
        d3d_device_ = nullptr;
    }
}

void VideoDecoder::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return;
    }
    running_ = true;
    thread_ = std::thread([this] { ThreadMain(); });
}

void VideoDecoder::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void VideoDecoder::Configure(const protocol::StreamProfile& profile) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_profile_ = profile;
        active_profile_ = profile;
        has_active_profile_ = true;
        profile_dirty_ = true;
        queue_.clear();
        latest_codec_config_ = AccessUnit{};
        has_latest_codec_config_ = false;
        waiting_for_keyframe_ = false;
        queue_warning_active_ = false;
        queue_resync_overload_hits_ = 0;
        backend_reset_requested_ = false;
        soft_resync_requested_ = false;
        discontinuity_pending_ = true;
    }
    cv_.notify_all();
}

void VideoDecoder::SetD3DDevice(ID3D11Device* device) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (d3d_device_ == device) {
        return;
    }
    if (d3d_device_ != nullptr) {
        d3d_device_->Release();
    }
    d3d_device_ = device;
    if (d3d_device_ != nullptr) {
        d3d_device_->AddRef();
    }
}

void VideoDecoder::SetSmoothMode(bool enabled) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (smooth_mode_ == enabled) {
            return;
        }

        smooth_mode_ = enabled;
        queue_warning_active_ = false;
        queue_resync_overload_hits_ = 0;
        changed = true;
    }

    if (changed && log_fn_) {
        log_fn_(enabled
            ? L"视频解码: 已切换到顺滑观感策略，队列抖动时会优先保住连续性。"
            : L"视频解码: 已切换到竞技低延迟策略，队列积压时会更快回到最新画面。");
    }
}

size_t VideoDecoder::GetPendingAccessUnitCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void VideoDecoder::SubmitAccessUnit(const AccessUnit& access_unit) {
    bool should_request_keyframe = false;
    std::wstring deferred_log;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }

        const bool is_codec_config = (access_unit.flags & protocol::kFlagCodecConfig) != 0;
        const bool is_keyframe = (access_unit.flags & protocol::kFlagKeyframe) != 0;
        const protocol::StreamProfile threshold_profile =
            has_active_profile_ ? active_profile_ : pending_profile_;
        const bool smooth_mode = smooth_mode_;
        const size_t warning_threshold = QueueWarningThresholdForProfile(threshold_profile, smooth_mode);
        const size_t resync_threshold = QueueResyncThresholdForProfile(threshold_profile, smooth_mode);
        const size_t resync_hit_threshold = QueueResyncHitThresholdForProfile(threshold_profile, smooth_mode);

        if (is_codec_config) {
            latest_codec_config_ = access_unit;
            has_latest_codec_config_ = true;
        }

        if (waiting_for_keyframe_) {
            if (is_codec_config) {
                return;
            }
            if (!is_keyframe) {
                return;
            }

            waiting_for_keyframe_ = false;
            queue_.clear();
            queue_warning_active_ = false;
            queue_resync_overload_hits_ = 0;
            if (has_latest_codec_config_) {
                queue_.push_back(latest_codec_config_);
            }
            queue_.push_back(access_unit);
            soft_resync_requested_ = true;
            discontinuity_pending_ = true;
            deferred_log = L"解码队列: 已收到新的关键帧，恢复低延迟解码。";
        } else {
            queue_.push_back(access_unit);
            const size_t queue_depth = queue_.size();

            if (queue_depth >= warning_threshold && !queue_warning_active_) {
                std::wostringstream stream;
                stream << L"解码队列: 待解码访问单元已积压到 "
                       << queue_depth
                       << L" 帧，如继续上升将主动重同步。";
                deferred_log = stream.str();
                queue_warning_active_ = true;
            } else if (queue_depth <= QueueWarningResetThreshold(warning_threshold)) {
                queue_warning_active_ = false;
            }

            if (queue_depth >= resync_threshold) {
                ++queue_resync_overload_hits_;
                if (queue_resync_overload_hits_ >= resync_hit_threshold) {
                    queue_warning_active_ = false;
                    queue_resync_overload_hits_ = 0;
                    if (is_keyframe) {
                        queue_.clear();
                        soft_resync_requested_ = true;
                        discontinuity_pending_ = true;
                        if (has_latest_codec_config_) {
                            queue_.push_back(latest_codec_config_);
                        }
                        queue_.push_back(access_unit);
                        deferred_log = L"解码队列: 积压过多，已丢弃旧帧并从最新关键帧继续。";
                    } else if (smooth_mode) {
                        waiting_for_keyframe_ = true;
                        should_request_keyframe = true;
                        deferred_log =
                            L"解码队列: 顺滑模式下检测到持续积压，已继续消化排队帧，并请求新的关键帧切回最新画面。";
                    } else {
                        queue_.clear();
                        soft_resync_requested_ = true;
                        discontinuity_pending_ = true;
                        waiting_for_keyframe_ = true;
                        should_request_keyframe = true;
                        deferred_log = L"解码队列: 积压过多，已丢弃旧帧并等待新的关键帧重同步。";
                    }
                }
            } else {
                queue_resync_overload_hits_ = 0;
            }
        }
    }

    if (!deferred_log.empty()) {
        log_fn_(deferred_log);
    }
    if (should_request_keyframe && request_keyframe_fn_) {
        request_keyframe_fn_();
    }
    cv_.notify_all();
}

void VideoDecoder::ThreadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetCurrentThreadPriorityBestEffort(THREAD_PRIORITY_HIGHEST);

    DecoderContext context;

    while (true) {
        AccessUnit access_unit;
        bool has_item = false;
        bool should_exit = false;
        bool profile_dirty = false;
        bool soft_resync = false;
        bool mark_discontinuity = false;
        protocol::StreamProfile profile;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return !running_ || profile_dirty_ || soft_resync_requested_ || !queue_.empty();
            });

            should_exit = !running_;
            if (profile_dirty_) {
                profile = pending_profile_;
                profile_dirty_ = false;
                profile_dirty = true;
            } else if (soft_resync_requested_) {
                soft_resync_requested_ = false;
                soft_resync = true;
            }

            if (!queue_.empty()) {
                access_unit = std::move(queue_.front());
                queue_.pop_front();
                has_item = true;
                mark_discontinuity = discontinuity_pending_;
            }
        }

        if (profile_dirty) {
            ConfigureDecoderBackend(profile, d3d_device_, &context, log_fn_, frame_fn_);
        }
        if (soft_resync) {
            SoftResyncDecoder(&context, d3d_device_, log_fn_, frame_fn_);
        }
        if (should_exit) {
            break;
        }
        if (!has_item || context.backend == DecoderBackend::kNone) {
            continue;
        }

        if (context.backend == DecoderBackend::kNvidiaCuvid) {
            const bool submitted =
                context.nvidia_cuvid_decoder != nullptr &&
                context.nvidia_cuvid_decoder->SubmitAccessUnit(access_unit, mark_discontinuity);
            if (submitted) {
                if (mark_discontinuity) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    discontinuity_pending_ = false;
                }
            } else if (log_fn_) {
                log_fn_(L"视频解码: NVIDIA CUVID 提交访问单元失败，当前帧已丢弃。");
            }
            continue;
        }

        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFSample> sample;
        const DWORD length = static_cast<DWORD>(access_unit.bytes.size());
        if (FAILED(MFCreateMemoryBuffer(length, &buffer)) ||
            FAILED(MFCreateSample(&sample))) {
            continue;
        }

        uint8_t* destination = nullptr;
        DWORD max_length = 0;
        if (FAILED(buffer->Lock(&destination, &max_length, nullptr))) {
            continue;
        }
        memcpy(destination, access_unit.bytes.data(), access_unit.bytes.size());
        buffer->Unlock();
        buffer->SetCurrentLength(length);
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(static_cast<LONGLONG>(access_unit.pts_us) * 10);
        if (context.active_profile.fps > 0) {
            sample->SetSampleDuration(10'000'000LL / static_cast<LONGLONG>(context.active_profile.fps));
        }
        if ((access_unit.flags & protocol::kFlagKeyframe) != 0) {
            sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        }
        if (mark_discontinuity) {
            sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
        }

        bool submitted = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const HRESULT input_hr = context.transform->ProcessInput(0, sample.Get(), 0);
            if (input_hr == MF_E_NOTACCEPTING) {
                if (attempt == 0) {
                    log_fn_(L"视频解码: 解码器暂时不接收新输入，正在优先排空输出队列。");
                }
                DrainOutput(&context, d3d_device_, log_fn_, frame_fn_);
                continue;
            }
            if (FAILED(input_hr)) {
                log_fn_(L"视频解码: ProcessInput 失败，错误码 " + HrToString(input_hr));
                break;
            }

            submitted = true;
            if (mark_discontinuity) {
                std::lock_guard<std::mutex> lock(mutex_);
                discontinuity_pending_ = false;
            }
            DrainOutput(&context, d3d_device_, log_fn_, frame_fn_);
            break;
        }

        if (!submitted) {
            log_fn_(L"视频解码: 当前访问单元提交失败，已丢弃以保持低延迟。");
        }
    }

    StopDecoderBackend(&context);
    CoUninitialize();
}
