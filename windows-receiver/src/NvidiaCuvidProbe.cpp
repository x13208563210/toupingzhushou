#include "NvidiaCuvidProbe.h"

#include <Windows.h>

#include <sstream>

#if ANDROID_CAST_HAVE_NVIDIA_CUVID_PROBE
#include <cuda.h>
#include <nvcuvid.h>
#endif

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

#if ANDROID_CAST_HAVE_NVIDIA_CUVID_PROBE

template <typename T>
bool LoadSymbol(HMODULE module, const char* name, T* output) {
    if (module == nullptr || name == nullptr || output == nullptr) {
        return false;
    }
    *output = reinterpret_cast<T>(GetProcAddress(module, name));
    return *output != nullptr;
}

template <typename T>
bool LoadAnySymbol(HMODULE module, const char* primary_name, const char* fallback_name, T* output) {
    return LoadSymbol(module, primary_name, output) || LoadSymbol(module, fallback_name, output);
}

struct CudaApi {
    HMODULE module = nullptr;
    decltype(&cuInit) init = nullptr;
    decltype(&cuGetErrorName) get_error_name = nullptr;
    decltype(&cuGetErrorString) get_error_string = nullptr;
    decltype(&cuDeviceGetCount) device_get_count = nullptr;
    decltype(&cuDeviceGet) device_get = nullptr;
    decltype(&cuDeviceGetName) device_get_name = nullptr;
    decltype(&cuCtxCreate) ctx_create = nullptr;
    decltype(&cuCtxSetCurrent) ctx_set_current = nullptr;
    decltype(&cuCtxDestroy) ctx_destroy = nullptr;
};

std::wstring CudaErrorText(const CudaApi& cuda, CUresult result) {
    const char* error_name = nullptr;
    const char* error_string = nullptr;
    if (cuda.get_error_name != nullptr) {
        cuda.get_error_name(result, &error_name);
    }
    if (cuda.get_error_string != nullptr) {
        cuda.get_error_string(result, &error_string);
    }

    std::wostringstream stream;
    stream << L"CUDA \u9519\u8BEF 0x" << std::hex << std::uppercase
           << static_cast<unsigned int>(result);
    if (error_name != nullptr) {
        stream << L" / " << Utf8ToWide(error_name);
    }
    if (error_string != nullptr) {
        stream << L" / " << Utf8ToWide(error_string);
    }
    return stream.str();
}

bool LoadCudaApi(CudaApi* api, std::wstring* error) {
    if (api == nullptr) {
        return false;
    }

    *api = {};
    api->module = LoadLibraryW(L"nvcuda.dll");
    if (api->module == nullptr) {
        if (error != nullptr) {
            *error = L"\u672A\u627E\u5230 nvcuda.dll\u3002";
        }
        return false;
    }

    const bool ok =
        LoadSymbol(api->module, "cuInit", &api->init) &&
        LoadSymbol(api->module, "cuGetErrorName", &api->get_error_name) &&
        LoadSymbol(api->module, "cuGetErrorString", &api->get_error_string) &&
        LoadSymbol(api->module, "cuDeviceGetCount", &api->device_get_count) &&
        LoadSymbol(api->module, "cuDeviceGet", &api->device_get) &&
        LoadSymbol(api->module, "cuDeviceGetName", &api->device_get_name) &&
        LoadAnySymbol(api->module, "cuCtxCreate_v2", "cuCtxCreate", &api->ctx_create) &&
        LoadSymbol(api->module, "cuCtxSetCurrent", &api->ctx_set_current) &&
        LoadAnySymbol(api->module, "cuCtxDestroy_v2", "cuCtxDestroy", &api->ctx_destroy);

    if (!ok) {
        if (error != nullptr) {
            *error = L"nvcuda.dll \u5DF2\u52A0\u8F7D\uff0C\u4F46\u5BFC\u51FA\u51FD\u6570\u4E0D\u5B8C\u6574\u3002";
        }
        FreeLibrary(api->module);
        *api = {};
        return false;
    }

    return true;
}

void UnloadCudaApi(CudaApi* api) {
    if (api == nullptr) {
        return;
    }
    if (api->module != nullptr) {
        FreeLibrary(api->module);
    }
    *api = {};
}

struct CuvidApi {
    HMODULE module = nullptr;
    decltype(&cuvidGetDecoderCaps) cuvid_get_decoder_caps = nullptr;
};

struct ScopedCudaContext {
    const CudaApi* cuda = nullptr;
    CUcontext context = nullptr;

    ~ScopedCudaContext() {
        if (cuda != nullptr && context != nullptr) {
            if (cuda->ctx_set_current != nullptr) {
                cuda->ctx_set_current(nullptr);
            }
            if (cuda->ctx_destroy != nullptr) {
                cuda->ctx_destroy(context);
            }
        }
    }
};

bool LoadCuvidApi(CuvidApi* api, std::wstring* error) {
    if (api == nullptr) {
        return false;
    }

    *api = {};
    api->module = LoadLibraryW(L"nvcuvid.dll");
    if (api->module == nullptr) {
        if (error != nullptr) {
            *error = L"\u672A\u627E\u5230 nvcuvid.dll\u3002";
        }
        return false;
    }

    if (!LoadSymbol(api->module, "cuvidGetDecoderCaps", &api->cuvid_get_decoder_caps)) {
        if (error != nullptr) {
            *error = L"nvcuvid.dll \u5DF2\u52A0\u8F7D\uff0C\u4F46\u7F3A\u5C11 cuvidGetDecoderCaps \u5BFC\u51FA\u3002";
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

bool CreateProbeContext(const CudaApi& cuda, CUdevice device, ScopedCudaContext* scoped_context, std::wstring* error) {
    if (scoped_context == nullptr) {
        return false;
    }

    scoped_context->cuda = &cuda;
    const CUresult create_result = cuda.ctx_create(&scoped_context->context, 0, device);
    if (create_result != CUDA_SUCCESS) {
        if (error != nullptr) {
            *error = L"\u521B\u5EFA CUDA \u4E0A\u4E0B\u6587\u5931\u8D25\uff0C" + CudaErrorText(cuda, create_result);
        }
        return false;
    }

    const CUresult set_current_result = cuda.ctx_set_current(scoped_context->context);
    if (set_current_result != CUDA_SUCCESS) {
        if (error != nullptr) {
            *error = L"\u8BBE\u7F6E CUDA \u5F53\u524D\u4E0A\u4E0B\u6587\u5931\u8D25\uff0C" + CudaErrorText(cuda, set_current_result);
        }
        return false;
    }

    return true;
}

#endif

}  // namespace

NvidiaCuvidProbeResult ProbeNvidiaCuvidSupport() {
    NvidiaCuvidProbeResult result;

#if !ANDROID_CAST_HAVE_NVIDIA_CUVID_PROBE
    result.summary = L"NVIDIA NVDEC \u63A2\u6D4B\uff1A\u5F53\u524D\u6784\u5EFA\u672A\u63A5\u5165 CUDA/CUVID \u5F00\u53D1\u5934\u6587\u4EF6\u3002";
    return result;
#else
    CudaApi cuda;
    std::wstring cuda_load_error;
    if (!LoadCudaApi(&cuda, &cuda_load_error)) {
        result.summary = L"NVIDIA NVDEC \u63A2\u6D4B\uff1A" + cuda_load_error;
        return result;
    }

    const CUresult init_result = cuda.init(0);
    if (init_result != CUDA_SUCCESS) {
        result.summary = L"NVIDIA NVDEC \u63A2\u6D4B\uff1ACUDA \u9A71\u52A8\u521D\u59CB\u5316\u5931\u8D25\uff0C" + CudaErrorText(cuda, init_result);
        UnloadCudaApi(&cuda);
        return result;
    }
    result.cuda_driver_ready = true;

    int device_count = 0;
    const CUresult count_result = cuda.device_get_count(&device_count);
    if (count_result != CUDA_SUCCESS) {
        result.summary = L"NVIDIA NVDEC \u63A2\u6D4B\uff1A\u65E0\u6CD5\u8BFB\u53D6 CUDA \u8BBE\u5907\u6570\u91CF\uff0C" + CudaErrorText(cuda, count_result);
        UnloadCudaApi(&cuda);
        return result;
    }
    if (device_count <= 0) {
        result.summary = L"NVIDIA NVDEC \u63A2\u6D4B\uff1A\u5F53\u524D\u6CA1\u6709\u53EF\u7528\u7684 CUDA \u8BBE\u5907\u3002";
        UnloadCudaApi(&cuda);
        return result;
    }

    CUdevice device = 0;
    const CUresult device_result = cuda.device_get(&device, 0);
    if (device_result != CUDA_SUCCESS) {
        result.summary = L"NVIDIA NVDEC \u63A2\u6D4B\uff1A\u65E0\u6CD5\u6253\u5F00\u7B2C\u4E00\u5757 CUDA \u8BBE\u5907\uff0C" + CudaErrorText(cuda, device_result);
        UnloadCudaApi(&cuda);
        return result;
    }

    char device_name[256] = {};
    if (cuda.device_get_name(device_name, static_cast<int>(sizeof(device_name)), device) == CUDA_SUCCESS) {
        result.gpu_name = Utf8ToWide(device_name);
    }

    ScopedCudaContext probe_context;
    std::wstring context_error;
    if (!CreateProbeContext(cuda, device, &probe_context, &context_error)) {
        result.summary = L"NVIDIA NVDEC \u63A2\u6D4B\uff1A\u5DF2\u627E\u5230 CUDA \u8BBE\u5907\uff0C\u4F46\u521D\u59CB\u5316\u63A2\u6D4B\u4E0A\u4E0B\u6587\u5931\u8D25\uff0C" + context_error;
        UnloadCudaApi(&cuda);
        return result;
    }

    CuvidApi api;
    std::wstring load_error;
    if (!LoadCuvidApi(&api, &load_error)) {
        result.summary = L"NVIDIA NVDEC \u63A2\u6D4B\uff1ACUDA \u5DF2\u5C31\u7EEA\uff0C\u4F46 CUVID \u8FD0\u884C\u5E93\u4E0D\u53EF\u7528\uff0C" + load_error;
        UnloadCudaApi(&cuda);
        return result;
    }
    result.cuvid_library_ready = true;

    CUVIDDECODECAPS caps{};
    caps.eCodecType = cudaVideoCodec_H264;
    caps.eChromaFormat = cudaVideoChromaFormat_420;
    caps.nBitDepthMinus8 = 0;

    const CUresult caps_result = api.cuvid_get_decoder_caps(&caps);
    if (caps_result != CUDA_SUCCESS) {
        result.summary = L"NVIDIA NVDEC \u63A2\u6D4B\uff1ACUVID \u5DF2\u52A0\u8F7D\uff0C\u4F46\u8BFB\u53D6 H.264 \u89E3\u7801\u80FD\u529B\u5931\u8D25\uff0C" + CudaErrorText(cuda, caps_result);
        UnloadCuvidApi(&api);
        UnloadCudaApi(&cuda);
        return result;
    }

    result.h264_8bit_420_supported = caps.bIsSupported != 0;
    result.max_width = static_cast<int>(caps.nMaxWidth);
    result.max_height = static_cast<int>(caps.nMaxHeight);
    result.max_mb_count = caps.nMaxMBCount;

    std::wostringstream stream;
    stream << L"NVIDIA NVDEC \u63A2\u6D4B\uff1ACUDA="
           << (result.cuda_driver_ready ? L"\u5DF2\u5C31\u7EEA" : L"\u672A\u5C31\u7EEA")
           << L"\uFF0CCUVID="
           << (result.cuvid_library_ready ? L"\u5DF2\u5C31\u7EEA" : L"\u672A\u5C31\u7EEA")
           << L"\uFF0CH.264 8bit 4:2:0="
           << (result.h264_8bit_420_supported ? L"\u652F\u6301" : L"\u4E0D\u652F\u6301");
    if (!result.gpu_name.empty()) {
        stream << L"\uFF0CGPU=" << result.gpu_name;
    }
    if (result.max_width > 0 && result.max_height > 0) {
        stream << L"\uFF0C\u6700\u5927\u5206\u8FA8\u7387 " << result.max_width << L"x" << result.max_height;
    }
    result.summary = stream.str();

    UnloadCuvidApi(&api);
    UnloadCudaApi(&cuda);
    return result;
#endif
}
