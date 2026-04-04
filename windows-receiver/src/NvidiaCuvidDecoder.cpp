#include "NvidiaCuvidDecoder.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <sstream>
#include <utility>

#include <cuda.h>
#include <cudaD3D11.h>
#include <cuviddec.h>
#include <nvcuvid.h>

namespace {

std::wstring Utf8ToWide(const char* value) {
    if (value == nullptr || *value == '\0') {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
    if (size <= 1) {
        std::wstring fallback;
        while (*value != '\0') {
            fallback.push_back(static_cast<unsigned char>(*value));
            ++value;
        }
        return fallback;
    }

    std::wstring result(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value, -1, result.data(), size);
    return result;
}

std::wstring CudaErrorText(CUresult result) {
    const char* error_name = nullptr;
    const char* error_string = nullptr;
    cuGetErrorName(result, &error_name);
    cuGetErrorString(result, &error_string);

    std::wostringstream stream;
    stream << L"CUDA 错误 0x" << std::hex << std::uppercase << static_cast<unsigned int>(result);
    if (error_name != nullptr) {
        stream << L" / " << Utf8ToWide(error_name);
    }
    if (error_string != nullptr) {
        stream << L" / " << Utf8ToWide(error_string);
    }
    return stream.str();
}

template <typename T>
bool LoadCuvidSymbol(HMODULE module, const char* name, T* output) {
    if (module == nullptr || name == nullptr || output == nullptr) {
        return false;
    }
    *output = reinterpret_cast<T>(GetProcAddress(module, name));
    return *output != nullptr;
}

struct CuvidApi {
    HMODULE module = nullptr;
    decltype(&cuvidGetDecoderCaps) get_decoder_caps = nullptr;
    decltype(&cuvidCreateDecoder) create_decoder = nullptr;
    decltype(&cuvidDestroyDecoder) destroy_decoder = nullptr;
    decltype(&cuvidCreateVideoParser) create_video_parser = nullptr;
    decltype(&cuvidDestroyVideoParser) destroy_video_parser = nullptr;
    decltype(&cuvidParseVideoData) parse_video_data = nullptr;
    decltype(&cuvidDecodePicture) decode_picture = nullptr;
    decltype(&cuvidMapVideoFrame64) map_video_frame = nullptr;
    decltype(&cuvidUnmapVideoFrame64) unmap_video_frame = nullptr;
};

bool LoadCuvidApi(CuvidApi* api, std::wstring* error) {
    if (api == nullptr) {
        return false;
    }

    api->module = LoadLibraryW(L"nvcuvid.dll");
    if (api->module == nullptr) {
        if (error != nullptr) {
            *error = L"未找到 nvcuvid.dll。";
        }
        return false;
    }

    const bool ok =
        LoadCuvidSymbol(api->module, "cuvidGetDecoderCaps", &api->get_decoder_caps) &&
        LoadCuvidSymbol(api->module, "cuvidCreateDecoder", &api->create_decoder) &&
        LoadCuvidSymbol(api->module, "cuvidDestroyDecoder", &api->destroy_decoder) &&
        LoadCuvidSymbol(api->module, "cuvidCreateVideoParser", &api->create_video_parser) &&
        LoadCuvidSymbol(api->module, "cuvidDestroyVideoParser", &api->destroy_video_parser) &&
        LoadCuvidSymbol(api->module, "cuvidParseVideoData", &api->parse_video_data) &&
        LoadCuvidSymbol(api->module, "cuvidDecodePicture", &api->decode_picture) &&
        LoadCuvidSymbol(api->module, "cuvidMapVideoFrame64", &api->map_video_frame) &&
        LoadCuvidSymbol(api->module, "cuvidUnmapVideoFrame64", &api->unmap_video_frame);

    if (!ok) {
        if (error != nullptr) {
            *error = L"nvcuvid.dll 已加载，但导出函数不完整。";
        }
        FreeLibrary(api->module);
        *api = {};
        return false;
    }

    return true;
}

void UnloadCuvidApi(CuvidApi* api) {
    if (api == nullptr) {
        return;
    }
    if (api->module != nullptr) {
        FreeLibrary(api->module);
    }
    *api = {};
}

class ScopedCurrentContext {
public:
    explicit ScopedCurrentContext(CUcontext context) {
        if (context != nullptr && cuCtxPushCurrent(context) == CUDA_SUCCESS) {
            active_ = true;
        }
    }

    ~ScopedCurrentContext() {
        if (active_) {
            CUcontext previous = nullptr;
            cuCtxPopCurrent(&previous);
        }
    }

    bool active() const { return active_; }

private:
    bool active_ = false;
};

CUresult SelectCudaDevice(ID3D11Device* d3d_device, CUdevice* device_out) {
    if (device_out == nullptr) {
        return CUDA_ERROR_INVALID_VALUE;
    }

    if (d3d_device != nullptr) {
        unsigned int device_count = 0;
        CUdevice devices[8] = {};
        const CUresult d3d_device_result =
            cuD3D11GetDevices(&device_count, devices, static_cast<unsigned int>(std::size(devices)), d3d_device, CU_D3D11_DEVICE_LIST_ALL);
        if (d3d_device_result == CUDA_SUCCESS && device_count > 0) {
            *device_out = devices[0];
            return CUDA_SUCCESS;
        }

        IDXGIDevice* dxgi_device = nullptr;
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(d3d_device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
            dxgi_device != nullptr &&
            SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
            adapter != nullptr) {
            const CUresult adapter_result = cuD3D11GetDevice(device_out, adapter);
            adapter->Release();
            dxgi_device->Release();
            if (adapter_result == CUDA_SUCCESS) {
                return CUDA_SUCCESS;
            }
        } else {
            if (adapter != nullptr) {
                adapter->Release();
            }
            if (dxgi_device != nullptr) {
                dxgi_device->Release();
            }
        }
    }

    return cuDeviceGet(device_out, 0);
}

struct GraphicsMapGuard {
    CUgraphicsResource resources[2] = {};
    unsigned int count = 0;
    bool mapped = false;

    ~GraphicsMapGuard() {
        if (mapped && count > 0) {
            cuGraphicsUnmapResources(count, resources, nullptr);
        }
    }
};

}  // namespace

struct NvidiaCuvidDecoder::Impl {
    struct OutputTextures {
        ID3D11Texture2D* luma = nullptr;
        ID3D11Texture2D* chroma = nullptr;
        CUgraphicsResource luma_resource = nullptr;
        CUgraphicsResource chroma_resource = nullptr;
        unsigned int width = 0;
        unsigned int height = 0;

        void ReleaseTexturesOnly() {
            if (luma != nullptr) {
                luma->Release();
                luma = nullptr;
            }
            if (chroma != nullptr) {
                chroma->Release();
                chroma = nullptr;
            }
            width = 0;
            height = 0;
        }
    };

    LogFn log_fn;
    FrameFn frame_fn;
    protocol::StreamProfile active_profile;
    ID3D11Device* d3d_device = nullptr;
    CUdevice cuda_device = 0;
    CUcontext cuda_context = nullptr;
    CuvidApi api;
    CUvideoparser parser = nullptr;
    CUvideodecoder decoder = nullptr;
    unsigned int coded_width = 0;
    unsigned int coded_height = 0;
    unsigned int display_width = 0;
    unsigned int display_height = 0;
    unsigned int surface_height = 0;
    uint64_t decoded_frame_count = 0;
    OutputTextures output_textures;
    bool direct_gpu_path_ready = false;

    ~Impl() {
        Reset();
    }

    void ResetOutputTextures(bool has_context) {
        if (has_context) {
            if (output_textures.luma_resource != nullptr) {
                cuGraphicsUnregisterResource(output_textures.luma_resource);
                output_textures.luma_resource = nullptr;
            }
            if (output_textures.chroma_resource != nullptr) {
                cuGraphicsUnregisterResource(output_textures.chroma_resource);
                output_textures.chroma_resource = nullptr;
            }
        } else {
            output_textures.luma_resource = nullptr;
            output_textures.chroma_resource = nullptr;
        }

        output_textures.ReleaseTexturesOnly();
        direct_gpu_path_ready = false;
    }

    void Reset() {
        if (cuda_context != nullptr) {
            ScopedCurrentContext current(cuda_context);
            ResetOutputTextures(current.active());

            if (current.active()) {
                if (decoder != nullptr && api.destroy_decoder != nullptr) {
                    api.destroy_decoder(decoder);
                    decoder = nullptr;
                }
                if (parser != nullptr && api.destroy_video_parser != nullptr) {
                    api.destroy_video_parser(parser);
                    parser = nullptr;
                }
            } else {
                decoder = nullptr;
                parser = nullptr;
            }

            cuCtxDestroy(cuda_context);
            cuda_context = nullptr;
        } else {
            ResetOutputTextures(false);
        }

        UnloadCuvidApi(&api);

        if (d3d_device != nullptr) {
            d3d_device->Release();
            d3d_device = nullptr;
        }

        active_profile = {};
        coded_width = 0;
        coded_height = 0;
        display_width = 0;
        display_height = 0;
        surface_height = 0;
        decoded_frame_count = 0;
    }

    bool CreateOutputTextures(unsigned int width, unsigned int height, std::wstring* error) {
        if (d3d_device == nullptr) {
            if (error != nullptr) {
                *error = L"D3D11 设备不可用。";
            }
            return false;
        }

        ResetOutputTextures(true);

        D3D11_TEXTURE2D_DESC luma_desc{};
        luma_desc.Width = std::max(1u, width);
        luma_desc.Height = std::max(1u, height);
        luma_desc.MipLevels = 1;
        luma_desc.ArraySize = 1;
        luma_desc.Format = DXGI_FORMAT_R8_UNORM;
        luma_desc.SampleDesc.Count = 1;
        luma_desc.Usage = D3D11_USAGE_DEFAULT;
        luma_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(d3d_device->CreateTexture2D(&luma_desc, nullptr, &output_textures.luma))) {
            if (error != nullptr) {
                *error = L"创建 CUDA-D3D11 亮度纹理失败。";
            }
            ResetOutputTextures(false);
            return false;
        }

        D3D11_TEXTURE2D_DESC chroma_desc{};
        chroma_desc.Width = std::max(1u, (width + 1) / 2);
        chroma_desc.Height = std::max(1u, (height + 1) / 2);
        chroma_desc.MipLevels = 1;
        chroma_desc.ArraySize = 1;
        chroma_desc.Format = DXGI_FORMAT_R8G8_UNORM;
        chroma_desc.SampleDesc.Count = 1;
        chroma_desc.Usage = D3D11_USAGE_DEFAULT;
        chroma_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(d3d_device->CreateTexture2D(&chroma_desc, nullptr, &output_textures.chroma))) {
            if (error != nullptr) {
                *error = L"创建 CUDA-D3D11 色度纹理失败。";
            }
            ResetOutputTextures(false);
            return false;
        }

        const CUresult register_luma_result = cuGraphicsD3D11RegisterResource(
            &output_textures.luma_resource,
            output_textures.luma,
            CU_GRAPHICS_REGISTER_FLAGS_NONE);
        if (register_luma_result != CUDA_SUCCESS) {
            if (error != nullptr) {
                *error = L"注册亮度纹理到 CUDA 失败，" + CudaErrorText(register_luma_result);
            }
            ResetOutputTextures(false);
            return false;
        }

        const CUresult register_chroma_result = cuGraphicsD3D11RegisterResource(
            &output_textures.chroma_resource,
            output_textures.chroma,
            CU_GRAPHICS_REGISTER_FLAGS_NONE);
        if (register_chroma_result != CUDA_SUCCESS) {
            if (error != nullptr) {
                *error = L"注册色度纹理到 CUDA 失败，" + CudaErrorText(register_chroma_result);
            }
            ResetOutputTextures(true);
            return false;
        }

        output_textures.width = width;
        output_textures.height = height;
        direct_gpu_path_ready = true;
        return true;
    }

    bool EnsureOutputTextures() {
        if (display_width == 0 || display_height == 0 || d3d_device == nullptr) {
            return false;
        }

        if (direct_gpu_path_ready &&
            output_textures.luma != nullptr &&
            output_textures.chroma != nullptr &&
            output_textures.width == display_width &&
            output_textures.height == display_height) {
            return true;
        }

        std::wstring error;
        if (!CreateOutputTextures(display_width, display_height, &error)) {
            if (log_fn != nullptr) {
                log_fn(L"视频解码: CUDA -> D3D11 双纹理输出未能建立，回退 CPU 上传，" + error);
            }
            return false;
        }

        if (log_fn != nullptr) {
            std::wostringstream stream;
            stream << L"视频解码: 已建立 CUDA -> D3D11 双纹理输出，亮度="
                   << display_width << L"x" << display_height
                   << L"，色度=" << ((display_width + 1) / 2) << L"x" << ((display_height + 1) / 2);
            log_fn(stream.str());
        }
        return true;
    }

    bool Configure(const protocol::StreamProfile& profile, ID3D11Device* device) {
        Reset();

        const CUresult init_result = cuInit(0);
        if (init_result != CUDA_SUCCESS) {
            if (log_fn) {
                log_fn(L"视频解码: NVIDIA CUVID 初始化失败，CUDA 驱动不可用，" + CudaErrorText(init_result));
            }
            return false;
        }

        std::wstring cuvid_error;
        if (!LoadCuvidApi(&api, &cuvid_error)) {
            if (log_fn) {
                log_fn(L"视频解码: NVIDIA CUVID 初始化失败，" + cuvid_error);
            }
            Reset();
            return false;
        }

        const CUresult select_device_result = SelectCudaDevice(device, &cuda_device);
        if (select_device_result != CUDA_SUCCESS) {
            if (log_fn) {
                log_fn(L"视频解码: NVIDIA CUVID 初始化失败，无法定位与渲染设备匹配的 CUDA 设备，" +
                       CudaErrorText(select_device_result));
            }
            Reset();
            return false;
        }

        const CUresult create_context_result = cuCtxCreate(&cuda_context, 0, cuda_device);
        if (create_context_result != CUDA_SUCCESS) {
            if (log_fn) {
                log_fn(L"视频解码: NVIDIA CUVID 初始化失败，创建 CUDA 上下文失败，" + CudaErrorText(create_context_result));
            }
            Reset();
            return false;
        }

        ScopedCurrentContext current(cuda_context);
        if (!current.active()) {
            if (log_fn) {
                log_fn(L"视频解码: NVIDIA CUVID 初始化失败，无法切入当前 CUDA 上下文。");
            }
            Reset();
            return false;
        }

        CUVIDPARSERPARAMS parser_params{};
        parser_params.CodecType = cudaVideoCodec_H264;
        parser_params.ulMaxNumDecodeSurfaces = 1;
        parser_params.ulMaxDisplayDelay = 0;
        parser_params.ulClockRate = 10'000'000;
        parser_params.pUserData = this;
        parser_params.pfnSequenceCallback = &HandleVideoSequenceProc;
        parser_params.pfnDecodePicture = &HandlePictureDecodeProc;
        parser_params.pfnDisplayPicture = &HandlePictureDisplayProc;

        const CUresult parser_result = api.create_video_parser(&parser, &parser_params);
        if (parser_result != CUDA_SUCCESS) {
            if (log_fn) {
                log_fn(L"视频解码: NVIDIA CUVID 初始化失败，创建解析器失败，" + CudaErrorText(parser_result));
            }
            Reset();
            return false;
        }

        if (device != nullptr) {
            device->AddRef();
            d3d_device = device;
        }

        active_profile = profile;
        decoded_frame_count = 0;

        if (log_fn) {
            log_fn(L"视频解码: 已切到 NVIDIA CUVID 硬解后端，优先走 CUDA -> D3D11 直通，失败时回退 CPU 上传。");
        }
        return true;
    }

    bool SubmitAccessUnit(const AccessUnit& access_unit, bool discontinuity) {
        if (parser == nullptr || api.parse_video_data == nullptr || access_unit.bytes.empty()) {
            return false;
        }

        ScopedCurrentContext current(cuda_context);
        if (!current.active()) {
            if (log_fn) {
                log_fn(L"视频解码: NVIDIA CUVID 提交失败，无法进入 CUDA 上下文。");
            }
            return false;
        }

        CUVIDSOURCEDATAPACKET packet{};
        packet.payload = const_cast<unsigned char*>(access_unit.bytes.data());
        packet.payload_size = static_cast<unsigned long>(access_unit.bytes.size());
        packet.flags = CUVID_PKT_ENDOFPICTURE;
        if (access_unit.pts_us > 0) {
            packet.flags |= CUVID_PKT_TIMESTAMP;
            packet.timestamp = static_cast<CUvideotimestamp>(access_unit.pts_us) * 10;
        }
        if (discontinuity) {
            packet.flags |= CUVID_PKT_DISCONTINUITY;
        }

        const CUresult parse_result = api.parse_video_data(parser, &packet);
        if (parse_result != CUDA_SUCCESS) {
            if (log_fn) {
                log_fn(L"视频解码: NVIDIA CUVID 解析访问单元失败，" + CudaErrorText(parse_result));
            }
            return false;
        }
        return true;
    }

    int OnSequence(CUVIDEOFORMAT* format) {
        if (format == nullptr) {
            return 0;
        }

        ScopedCurrentContext current(cuda_context);
        if (!current.active()) {
            if (log_fn) {
                log_fn(L"视频解码: NVIDIA CUVID 创建解码器失败，无法进入 CUDA 上下文。");
            }
            return 0;
        }

        CUVIDDECODECAPS caps{};
        caps.eCodecType = format->codec;
        caps.eChromaFormat = format->chroma_format;
        caps.nBitDepthMinus8 = format->bit_depth_luma_minus8;

        const CUresult caps_result = api.get_decoder_caps(&caps);
        if (caps_result != CUDA_SUCCESS) {
            if (log_fn) {
                log_fn(L"视频解码: NVIDIA CUVID 读取解码能力失败，" + CudaErrorText(caps_result));
            }
            return 0;
        }
        if (caps.bIsSupported == 0) {
            if (log_fn) {
                log_fn(L"视频解码: 当前流格式不受 NVIDIA CUVID 支持。");
            }
            return 0;
        }

        if (decoder != nullptr) {
            api.destroy_decoder(decoder);
            decoder = nullptr;
        }

        const long display_left = format->display_area.left;
        const long display_top = format->display_area.top;
        const long display_right =
            format->display_area.right > display_left ? format->display_area.right : static_cast<long>(format->coded_width);
        const long display_bottom =
            format->display_area.bottom > display_top ? format->display_area.bottom : static_cast<long>(format->coded_height);

        display_width = static_cast<unsigned int>(std::max<long>(1, display_right - display_left));
        display_height = static_cast<unsigned int>(std::max<long>(1, display_bottom - display_top));
        coded_width = format->coded_width;
        coded_height = format->coded_height;

        CUVIDDECODECREATEINFO create_info{};
        create_info.CodecType = format->codec;
        create_info.ChromaFormat = format->chroma_format;
        create_info.OutputFormat = cudaVideoSurfaceFormat_NV12;
        create_info.bitDepthMinus8 = format->bit_depth_luma_minus8;
        create_info.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
        create_info.ulNumOutputSurfaces = 2;
        create_info.ulCreationFlags = cudaVideoCreate_PreferCUVID;
        create_info.ulNumDecodeSurfaces = std::max<unsigned int>(format->min_num_decode_surfaces + 2, 4u);
        create_info.ulWidth = format->coded_width;
        create_info.ulHeight = format->coded_height;
        create_info.ulMaxWidth = format->coded_width;
        create_info.ulMaxHeight = format->coded_height;
        create_info.ulTargetWidth = display_width;
        create_info.ulTargetHeight = display_height;
        create_info.display_area.left = static_cast<short>(display_left);
        create_info.display_area.top = static_cast<short>(display_top);
        create_info.display_area.right = static_cast<short>(display_right);
        create_info.display_area.bottom = static_cast<short>(display_bottom);

        const CUresult create_decoder_result = api.create_decoder(&decoder, &create_info);
        if (create_decoder_result != CUDA_SUCCESS) {
            if (log_fn) {
                log_fn(L"视频解码: NVIDIA CUVID 创建硬解实例失败，" + CudaErrorText(create_decoder_result));
            }
            return 0;
        }

        surface_height = create_info.ulTargetHeight;
        EnsureOutputTextures();

        if (log_fn) {
            std::wostringstream stream;
            stream << L"视频解码: NVIDIA CUVID 已准备好，输入="
                   << coded_width << L"x" << coded_height
                   << L"，输出=" << display_width << L"x" << display_height
                   << L"，显示链路=" << (direct_gpu_path_ready ? L"CUDA -> D3D11 直通" : L"CPU 上传回退");
            log_fn(stream.str());
        }

        return static_cast<int>(create_info.ulNumDecodeSurfaces);
    }

    int OnDecodePicture(CUVIDPICPARAMS* pic_params) {
        if (decoder == nullptr || pic_params == nullptr) {
            return 0;
        }

        ScopedCurrentContext current(cuda_context);
        if (!current.active()) {
            return 0;
        }

        const CUresult decode_result = api.decode_picture(decoder, pic_params);
        if (decode_result != CUDA_SUCCESS) {
            if (log_fn) {
                log_fn(L"视频解码: NVIDIA CUVID 解码图片失败，" + CudaErrorText(decode_result));
            }
            return 0;
        }
        return 1;
    }

    bool BuildGpuFrame(unsigned long long mapped_ptr, unsigned int mapped_pitch, uint64_t pts_us, DecodedFrame* frame) {
        if (!direct_gpu_path_ready ||
            output_textures.luma_resource == nullptr ||
            output_textures.chroma_resource == nullptr ||
            frame == nullptr) {
            return false;
        }

        GraphicsMapGuard map_guard;
        map_guard.resources[0] = output_textures.luma_resource;
        map_guard.resources[1] = output_textures.chroma_resource;
        map_guard.count = 2;

        const CUresult map_resources_result = cuGraphicsMapResources(map_guard.count, map_guard.resources, nullptr);
        if (map_resources_result != CUDA_SUCCESS) {
            if (log_fn) {
                log_fn(L"视频解码: CUDA -> D3D11 映射输出纹理失败，" + CudaErrorText(map_resources_result));
            }
            return false;
        }
        map_guard.mapped = true;

        CUarray luma_array = nullptr;
        CUarray chroma_array = nullptr;
        const CUresult luma_array_result =
            cuGraphicsSubResourceGetMappedArray(&luma_array, output_textures.luma_resource, 0, 0);
        if (luma_array_result != CUDA_SUCCESS) {
            if (log_fn) {
                log_fn(L"视频解码: 读取亮度 CUDA 数组失败，" + CudaErrorText(luma_array_result));
            }
            return false;
        }

        const CUresult chroma_array_result =
            cuGraphicsSubResourceGetMappedArray(&chroma_array, output_textures.chroma_resource, 0, 0);
        if (chroma_array_result != CUDA_SUCCESS) {
            if (log_fn) {
                log_fn(L"视频解码: 读取色度 CUDA 数组失败，" + CudaErrorText(chroma_array_result));
            }
            return false;
        }

        CUDA_MEMCPY2D copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = static_cast<CUdeviceptr>(mapped_ptr);
        copy.srcPitch = mapped_pitch;
        copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        copy.dstArray = luma_array;
        copy.WidthInBytes = static_cast<size_t>(display_width);
        copy.Height = static_cast<size_t>(display_height);

        CUresult copy_result = cuMemcpy2D(&copy);
        if (copy_result != CUDA_SUCCESS) {
            if (log_fn) {
                log_fn(L"视频解码: CUDA -> D3D11 复制亮度平面失败，" + CudaErrorText(copy_result));
            }
            return false;
        }

        const size_t chroma_width_texels = std::max<size_t>(1, (display_width + 1) / 2);
        const size_t chroma_height = std::max<size_t>(1, (display_height + 1) / 2);
        copy.srcDevice = static_cast<CUdeviceptr>(
            mapped_ptr + static_cast<unsigned long long>(mapped_pitch) * static_cast<unsigned long long>(surface_height));
        copy.dstArray = chroma_array;
        copy.WidthInBytes = chroma_width_texels * 2;
        copy.Height = chroma_height;

        copy_result = cuMemcpy2D(&copy);
        if (copy_result != CUDA_SUCCESS) {
            if (log_fn) {
                log_fn(L"视频解码: CUDA -> D3D11 复制色度平面失败，" + CudaErrorText(copy_result));
            }
            return false;
        }

        frame->width = static_cast<int>(display_width);
        frame->height = static_cast<int>(display_height);
        frame->pts_us = pts_us;
        frame->format = DecodedFrameFormat::kNv12;
        frame->stride0 = static_cast<int>(display_width);
        frame->stride1 = static_cast<int>(display_width);
        frame->plane1_offset = static_cast<size_t>(frame->stride0) * static_cast<size_t>(frame->height);
        frame->d3d_texture = output_textures.luma;
        frame->d3d_texture_plane1 = output_textures.chroma;
        frame->d3d_subresource = 0;
        frame->gpu_backed = true;
        frame->direct_sample_safe = true;
        frame->separate_textures = true;

        frame->d3d_texture->AddRef();
        frame->d3d_texture_plane1->AddRef();
        return true;
    }

    void BuildCpuFrame(unsigned long long mapped_ptr, unsigned int mapped_pitch, uint64_t pts_us, DecodedFrame* frame) {
        frame->width = static_cast<int>(display_width);
        frame->height = static_cast<int>(display_height);
        frame->pts_us = pts_us;
        frame->format = DecodedFrameFormat::kNv12;
        frame->stride0 = frame->width;
        frame->stride1 = frame->width;
        frame->plane1_offset = static_cast<size_t>(frame->stride0) * static_cast<size_t>(frame->height);

        const size_t chroma_height = static_cast<size_t>((frame->height + 1) / 2);
        frame->bytes.resize(frame->plane1_offset + static_cast<size_t>(frame->stride1) * chroma_height);

        CUDA_MEMCPY2D copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = static_cast<CUdeviceptr>(mapped_ptr);
        copy.srcPitch = mapped_pitch;
        copy.dstMemoryType = CU_MEMORYTYPE_HOST;
        copy.dstHost = frame->bytes.data();
        copy.dstPitch = static_cast<size_t>(frame->stride0);
        copy.WidthInBytes = static_cast<size_t>(frame->stride0);
        copy.Height = static_cast<size_t>(frame->height);
        cuMemcpy2D(&copy);

        copy.srcDevice = static_cast<CUdeviceptr>(
            mapped_ptr + static_cast<unsigned long long>(mapped_pitch) * static_cast<unsigned long long>(surface_height));
        copy.dstHost = frame->bytes.data() + frame->plane1_offset;
        copy.dstPitch = static_cast<size_t>(frame->stride1);
        copy.WidthInBytes = static_cast<size_t>(frame->stride1);
        copy.Height = chroma_height;
        cuMemcpy2D(&copy);
    }

    int OnDisplayPicture(CUVIDPARSERDISPINFO* disp_info) {
        if (decoder == nullptr || disp_info == nullptr) {
            return 0;
        }

        ScopedCurrentContext current(cuda_context);
        if (!current.active()) {
            return 0;
        }

        CUVIDPROCPARAMS proc_params{};
        proc_params.progressive_frame = disp_info->progressive_frame;
        proc_params.second_field = disp_info->repeat_first_field + 1;
        proc_params.top_field_first = disp_info->top_field_first;
        proc_params.unpaired_field = disp_info->repeat_first_field < 0;
        proc_params.output_stream = nullptr;

        unsigned long long mapped_ptr = 0;
        unsigned int mapped_pitch = 0;
        const CUresult map_result =
            api.map_video_frame(decoder, disp_info->picture_index, &mapped_ptr, &mapped_pitch, &proc_params);
        if (map_result != CUDA_SUCCESS) {
            if (log_fn) {
                log_fn(L"视频解码: NVIDIA CUVID 映射解码帧失败，" + CudaErrorText(map_result));
            }
            return 0;
        }

        struct ScopedMappedFrame {
            CuvidApi* api = nullptr;
            CUvideodecoder decoder = nullptr;
            unsigned long long mapped_ptr = 0;
            ~ScopedMappedFrame() {
                if (api != nullptr && decoder != nullptr && mapped_ptr != 0 && api->unmap_video_frame != nullptr) {
                    api->unmap_video_frame(decoder, mapped_ptr);
                }
            }
        } mapped_frame{&api, decoder, mapped_ptr};

        const uint64_t pts_us = disp_info->timestamp > 0 ? static_cast<uint64_t>(disp_info->timestamp / 10) : 0;

        DecodedFrame frame;
        bool used_direct_gpu_path = BuildGpuFrame(mapped_ptr, mapped_pitch, pts_us, &frame);
        if (!used_direct_gpu_path) {
            BuildCpuFrame(mapped_ptr, mapped_pitch, pts_us, &frame);
        }

        ++decoded_frame_count;
        if (log_fn && (decoded_frame_count == 1 || (decoded_frame_count % 120) == 0)) {
            std::wostringstream stream;
            stream << L"视频解码: NVIDIA CUVID 已输出 "
                   << decoded_frame_count
                   << L" 帧，尺寸=" << frame.width << L"x" << frame.height
                   << L"，路径=" << (used_direct_gpu_path ? L"CUDA -> D3D11 直通" : L"NVDEC 硬解 + CPU 上传");
            log_fn(stream.str());
        }

        if (frame_fn) {
            frame_fn(std::move(frame));
        }
        return 1;
    }

    static int CUDAAPI HandleVideoSequenceProc(void* user_data, CUVIDEOFORMAT* format) {
        auto* self = static_cast<Impl*>(user_data);
        return self != nullptr ? self->OnSequence(format) : 0;
    }

    static int CUDAAPI HandlePictureDecodeProc(void* user_data, CUVIDPICPARAMS* pic_params) {
        auto* self = static_cast<Impl*>(user_data);
        return self != nullptr ? self->OnDecodePicture(pic_params) : 0;
    }

    static int CUDAAPI HandlePictureDisplayProc(void* user_data, CUVIDPARSERDISPINFO* disp_info) {
        auto* self = static_cast<Impl*>(user_data);
        return self != nullptr ? self->OnDisplayPicture(disp_info) : 0;
    }
};

NvidiaCuvidDecoder::NvidiaCuvidDecoder(LogFn log_fn, FrameFn frame_fn)
    : impl_(std::make_unique<Impl>()) {
    impl_->log_fn = std::move(log_fn);
    impl_->frame_fn = std::move(frame_fn);
}

NvidiaCuvidDecoder::~NvidiaCuvidDecoder() = default;

bool NvidiaCuvidDecoder::Configure(const protocol::StreamProfile& profile, ID3D11Device* d3d_device) {
    return impl_ != nullptr && impl_->Configure(profile, d3d_device);
}

bool NvidiaCuvidDecoder::SubmitAccessUnit(const AccessUnit& access_unit, bool discontinuity) {
    return impl_ != nullptr && impl_->SubmitAccessUnit(access_unit, discontinuity);
}
