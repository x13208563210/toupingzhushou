#include "VideoDecoder.h"

#include <Windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr size_t kMaxQueuedAccessUnits = 10;
constexpr size_t kQueueTrimTargetAccessUnits = 4;

std::wstring HrToString(DWORD value) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase << value;
    return stream.str();
}

std::wstring FormatLastErrorMessage(DWORD error_code) {
    LPWSTR message_buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(
        flags,
        nullptr,
        error_code,
        0,
        reinterpret_cast<LPWSTR>(&message_buffer),
        0,
        nullptr);
    std::wstring message;
    if (length != 0 && message_buffer != nullptr) {
        message.assign(message_buffer, message_buffer + length);
        while (!message.empty() &&
               (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
            message.pop_back();
        }
    } else {
        message = L"\u672a\u77e5\u9519\u8bef";
    }
    if (message_buffer != nullptr) {
        LocalFree(message_buffer);
    }
    return message;
}

std::wstring Utf8ToWide(const char* text) {
    if (text == nullptr || *text == '\0') {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (required <= 1) {
        std::wstring fallback;
        while (*text != '\0') {
            fallback.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text)));
            ++text;
        }
        return fallback;
    }

    std::wstring wide(static_cast<size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), required);
    return wide;
}

std::wstring GetExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (length >= path.size() - 1) {
        path.resize(path.size() * 2);
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    path.resize(length);

    const size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return {};
    }

    path.resize(separator);
    return path;
}

HMODULE LoadModuleFromExecutableDirectory(const wchar_t* file_name) {
    if (file_name == nullptr || *file_name == L'\0') {
        return nullptr;
    }

    const std::wstring directory = GetExecutableDirectory();
    if (directory.empty()) {
        return LoadLibraryW(file_name);
    }

    std::wstring full_path = directory;
    full_path += L"\\";
    full_path += file_name;

    HMODULE module = LoadLibraryExW(
        full_path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module != nullptr) {
        return module;
    }

    return LoadLibraryW(full_path.c_str());
}

void SetCurrentThreadPriorityBestEffort(int priority) {
    SetThreadPriority(GetCurrentThread(), priority);
}

std::wstring BuildDebugOutputPath(const wchar_t* file_name) {
    if (file_name == nullptr || *file_name == L'\0') {
        return {};
    }

    const std::wstring directory = GetExecutableDirectory();
    if (directory.empty()) {
        return file_name;
    }

    std::wstring path = directory;
    path += L"\\";
    path += file_name;
    return path;
}

bool WriteBinaryFile(const std::wstring& path, const uint8_t* data, size_t size) {
    if (path.empty() || data == nullptr || size == 0) {
        return false;
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
        return false;
    }

    const size_t written = fwrite(data, 1, size, file);
    fclose(file);
    return written == size;
}

bool SaveBgraBitmap(const std::wstring& path, const DecodedFrame& frame) {
    if (path.empty() ||
        frame.format != DecodedFrameFormat::kBgra ||
        frame.width <= 0 ||
        frame.height <= 0 ||
        frame.stride0 < frame.width * 4 ||
        frame.bytes.size() < static_cast<size_t>(frame.stride0) * static_cast<size_t>(frame.height)) {
        return false;
    }

    std::vector<uint8_t> packed_pixels(static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 4);
    for (int row = 0; row < frame.height; ++row) {
        const uint8_t* source = frame.bytes.data() + static_cast<size_t>(row) * static_cast<size_t>(frame.stride0);
        uint8_t* target =
            packed_pixels.data() + static_cast<size_t>(row) * static_cast<size_t>(frame.width) * 4;
        std::memcpy(target, source, static_cast<size_t>(frame.width) * 4);
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
        return false;
    }

    const DWORD pixel_bytes = static_cast<DWORD>(packed_pixels.size());
    BITMAPFILEHEADER file_header{};
    file_header.bfType = 0x4D42;
    file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file_header.bfSize = file_header.bfOffBits + pixel_bytes;

    BITMAPINFOHEADER info_header{};
    info_header.biSize = sizeof(BITMAPINFOHEADER);
    info_header.biWidth = frame.width;
    info_header.biHeight = -frame.height;
    info_header.biPlanes = 1;
    info_header.biBitCount = 32;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = pixel_bytes;

    const bool success =
        fwrite(&file_header, 1, sizeof(file_header), file) == sizeof(file_header) &&
        fwrite(&info_header, 1, sizeof(info_header), file) == sizeof(info_header) &&
        fwrite(packed_pixels.data(), 1, packed_pixels.size(), file) == packed_pixels.size();
    fclose(file);
    return success;
}

bool IsCodecConfigAccessUnit(const AccessUnit& access_unit) {
    return (access_unit.flags & protocol::kFlagCodecConfig) != 0;
}

bool IsKeyframeAccessUnit(const AccessUnit& access_unit) {
    return (access_unit.flags & protocol::kFlagKeyframe) != 0;
}

bool TrimQueueToLatestDecodableSpan(
    std::deque<AccessUnit>* queue,
    const AccessUnit& latest_codec_config,
    bool has_latest_codec_config) {
    if (queue == nullptr || queue->empty()) {
        return false;
    }

    auto latest_keyframe = queue->end();
    for (auto it = queue->end(); it != queue->begin();) {
        --it;
        if (IsCodecConfigAccessUnit(*it)) {
            continue;
        }
        if (IsKeyframeAccessUnit(*it)) {
            latest_keyframe = it;
            break;
        }
    }

    if (latest_keyframe == queue->end()) {
        return false;
    }

    std::deque<AccessUnit> trimmed_queue;
    if (has_latest_codec_config) {
        trimmed_queue.push_back(latest_codec_config);
    }

    for (auto it = latest_keyframe; it != queue->end(); ++it) {
        if (IsCodecConfigAccessUnit(*it)) {
            continue;
        }
        trimmed_queue.push_back(std::move(*it));
    }

    queue->swap(trimmed_queue);
    return true;
}

AVCodecID CodecIdForProfile(const protocol::StreamProfile& profile) {
    switch (profile.codec) {
    case protocol::Codec::kAvc:
        return AV_CODEC_ID_H264;
    case protocol::Codec::kHevc:
        return AV_CODEC_ID_HEVC;
    case protocol::Codec::kUnknown:
    default:
        return AV_CODEC_ID_NONE;
    }
}

AVPixelFormat OutputPixelFormatForPreference(bool prefer_bgra_output) {
    (void)prefer_bgra_output;
    // Keep the first scrcpy-style replacement conservative:
    // route FFmpeg software decode through BGRA output by default to
    // bypass the current NV12 upload/render path while we stabilize it.
    return AV_PIX_FMT_BGRA;
}

struct FfmpegApi {
    HMODULE avcodec_module = nullptr;
    HMODULE avutil_module = nullptr;
    HMODULE swresample_module = nullptr;
    HMODULE swscale_module = nullptr;

    decltype(&::avcodec_find_decoder) avcodec_find_decoder = nullptr;
    decltype(&::avcodec_alloc_context3) avcodec_alloc_context3 = nullptr;
    decltype(&::avcodec_open2) avcodec_open2 = nullptr;
    decltype(&::avcodec_free_context) avcodec_free_context = nullptr;
    decltype(&::av_parser_init) av_parser_init = nullptr;
    decltype(&::av_parser_parse2) av_parser_parse2 = nullptr;
    decltype(&::av_parser_close) av_parser_close = nullptr;
    decltype(&::avcodec_send_packet) avcodec_send_packet = nullptr;
    decltype(&::avcodec_receive_frame) avcodec_receive_frame = nullptr;
    decltype(&::avcodec_flush_buffers) avcodec_flush_buffers = nullptr;

    decltype(&::av_frame_alloc) av_frame_alloc = nullptr;
    decltype(&::av_frame_free) av_frame_free = nullptr;
    decltype(&::av_frame_unref) av_frame_unref = nullptr;
    decltype(&::av_strerror) av_strerror = nullptr;
    decltype(&::av_image_get_buffer_size) av_image_get_buffer_size = nullptr;
    decltype(&::av_image_fill_arrays) av_image_fill_arrays = nullptr;
    decltype(&::av_version_info) av_version_info = nullptr;

    decltype(&::sws_getCachedContext) sws_getCachedContext = nullptr;
    decltype(&::sws_scale) sws_scale = nullptr;
    decltype(&::sws_freeContext) sws_freeContext = nullptr;

    bool loaded = false;

    bool Load(const VideoDecoder::LogFn& log_fn) {
        if (loaded) {
            return true;
        }

        avutil_module = LoadModuleFromExecutableDirectory(L"avutil-59.dll");
        swresample_module = LoadModuleFromExecutableDirectory(L"swresample-5.dll");
        swscale_module = LoadModuleFromExecutableDirectory(L"swscale-8.dll");
        avcodec_module = LoadModuleFromExecutableDirectory(L"avcodec-61.dll");
        if (avcodec_module == nullptr ||
            avutil_module == nullptr ||
            swresample_module == nullptr ||
            swscale_module == nullptr) {
            const DWORD error = GetLastError();
            if (log_fn != nullptr) {
                std::wstring line =
                    L"\u89c6\u9891\u89e3\u7801: FFmpeg \u8fd0\u884c\u65f6 DLL \u52a0\u8f7d\u5931\u8d25\uff0c"
                    L"\u9519\u8bef=" +
                    HrToString(error) + L"\uff0c" + FormatLastErrorMessage(error);
                log_fn(line);
            }
            Unload();
            return false;
        }

#define ANDROID_CAST_LOAD_PROC(module, proc_name)                                                     \
    do {                                                                                              \
        proc_name = reinterpret_cast<decltype(proc_name)>(GetProcAddress(module, #proc_name));        \
        if (proc_name == nullptr) {                                                                   \
            if (log_fn != nullptr) {                                                                  \
                log_fn(std::wstring(L"\u89c6\u9891\u89e3\u7801: FFmpeg \u7f3a\u5c11\u7b26\u53f7 ") +  \
                       L#proc_name + L"\u3002");                                                     \
            }                                                                                         \
            Unload();                                                                                 \
            return false;                                                                             \
        }                                                                                             \
    } while (false)

        ANDROID_CAST_LOAD_PROC(avcodec_module, avcodec_find_decoder);
        ANDROID_CAST_LOAD_PROC(avcodec_module, avcodec_alloc_context3);
        ANDROID_CAST_LOAD_PROC(avcodec_module, avcodec_open2);
        ANDROID_CAST_LOAD_PROC(avcodec_module, avcodec_free_context);
        ANDROID_CAST_LOAD_PROC(avcodec_module, av_parser_init);
        ANDROID_CAST_LOAD_PROC(avcodec_module, av_parser_parse2);
        ANDROID_CAST_LOAD_PROC(avcodec_module, av_parser_close);
        ANDROID_CAST_LOAD_PROC(avcodec_module, avcodec_send_packet);
        ANDROID_CAST_LOAD_PROC(avcodec_module, avcodec_receive_frame);
        ANDROID_CAST_LOAD_PROC(avcodec_module, avcodec_flush_buffers);

        ANDROID_CAST_LOAD_PROC(avutil_module, av_frame_alloc);
        ANDROID_CAST_LOAD_PROC(avutil_module, av_frame_free);
        ANDROID_CAST_LOAD_PROC(avutil_module, av_frame_unref);
        ANDROID_CAST_LOAD_PROC(avutil_module, av_strerror);
        ANDROID_CAST_LOAD_PROC(avutil_module, av_image_get_buffer_size);
        ANDROID_CAST_LOAD_PROC(avutil_module, av_image_fill_arrays);
        ANDROID_CAST_LOAD_PROC(avutil_module, av_version_info);

        ANDROID_CAST_LOAD_PROC(swscale_module, sws_getCachedContext);
        ANDROID_CAST_LOAD_PROC(swscale_module, sws_scale);
        ANDROID_CAST_LOAD_PROC(swscale_module, sws_freeContext);

#undef ANDROID_CAST_LOAD_PROC

        loaded = true;
        if (log_fn != nullptr && av_version_info != nullptr) {
            log_fn(std::wstring(L"\u89c6\u9891\u89e3\u7801: \u5df2\u542f\u7528 FFmpeg ") +
                   Utf8ToWide(av_version_info()) +
                   L"\uFF0C\u4F7F\u7528 scrcpy \u98CE\u683C\u7684\u4F4E\u7F13\u51B2\u89E3\u7801\u4E3B\u94FE\u3002");
        }
        return true;
    }

    void Unload() {
        if (swresample_module != nullptr) {
            FreeLibrary(swresample_module);
            swresample_module = nullptr;
        }
        if (swscale_module != nullptr) {
            FreeLibrary(swscale_module);
            swscale_module = nullptr;
        }
        if (avutil_module != nullptr) {
            FreeLibrary(avutil_module);
            avutil_module = nullptr;
        }
        if (avcodec_module != nullptr) {
            FreeLibrary(avcodec_module);
            avcodec_module = nullptr;
        }

        avcodec_find_decoder = nullptr;
        avcodec_alloc_context3 = nullptr;
        avcodec_open2 = nullptr;
        avcodec_free_context = nullptr;
        av_parser_init = nullptr;
        av_parser_parse2 = nullptr;
        av_parser_close = nullptr;
        avcodec_send_packet = nullptr;
        avcodec_receive_frame = nullptr;
        avcodec_flush_buffers = nullptr;

        av_frame_alloc = nullptr;
        av_frame_free = nullptr;
        av_frame_unref = nullptr;
        av_strerror = nullptr;
        av_image_get_buffer_size = nullptr;
        av_image_fill_arrays = nullptr;
        av_version_info = nullptr;

        sws_getCachedContext = nullptr;
        sws_scale = nullptr;
        sws_freeContext = nullptr;

        loaded = false;
    }

    std::wstring FormatAvError(int error_code) const {
        if (av_strerror == nullptr) {
            return HrToString(static_cast<DWORD>(error_code));
        }

        std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
        buffer.fill('\0');
        if (av_strerror(error_code, buffer.data(), buffer.size()) == 0) {
            return Utf8ToWide(buffer.data());
        }

        return HrToString(static_cast<DWORD>(error_code));
    }
};

struct DecoderRuntime {
    FfmpegApi api;
    protocol::StreamProfile active_profile{};
    AVCodecContext* codec_context = nullptr;
    AVCodecParserContext* parser_context = nullptr;
    AVFrame* decoded_frame = nullptr;
    SwsContext* sws_context = nullptr;
    AVPixelFormat sws_output_format = AV_PIX_FMT_NONE;
    int sws_width = 0;
    int sws_height = 0;
    bool logged_first_output_frame = false;
    bool dumped_first_bgra_bitmap = false;
    bool dumped_first_startup_stream = false;
};

void ResetSwsContext(DecoderRuntime* runtime) {
    if (runtime == nullptr || runtime->sws_context == nullptr || runtime->api.sws_freeContext == nullptr) {
        return;
    }
    runtime->api.sws_freeContext(runtime->sws_context);
    runtime->sws_context = nullptr;
    runtime->sws_output_format = AV_PIX_FMT_NONE;
    runtime->sws_width = 0;
    runtime->sws_height = 0;
}

void CloseDecoderRuntime(DecoderRuntime* runtime) {
    if (runtime == nullptr) {
        return;
    }

    ResetSwsContext(runtime);

    if (runtime->parser_context != nullptr && runtime->api.av_parser_close != nullptr) {
        runtime->api.av_parser_close(runtime->parser_context);
        runtime->parser_context = nullptr;
    }
    if (runtime->decoded_frame != nullptr && runtime->api.av_frame_free != nullptr) {
        runtime->api.av_frame_free(&runtime->decoded_frame);
        runtime->decoded_frame = nullptr;
    }
    if (runtime->codec_context != nullptr && runtime->api.avcodec_free_context != nullptr) {
        runtime->api.avcodec_free_context(&runtime->codec_context);
        runtime->codec_context = nullptr;
    }
}

bool OpenDecoderRuntime(
    DecoderRuntime* runtime,
    const protocol::StreamProfile& profile,
    const VideoDecoder::LogFn& log_fn) {
    if (runtime == nullptr) {
        return false;
    }

    CloseDecoderRuntime(runtime);

    if (!runtime->api.Load(log_fn)) {
        return false;
    }

    const AVCodecID codec_id = CodecIdForProfile(profile);
    if (codec_id == AV_CODEC_ID_NONE) {
        if (log_fn != nullptr) {
            log_fn(L"\u89c6\u9891\u89e3\u7801: \u5f53\u524d\u89c4\u683c\u4e0d\u652f\u6301\u7684\u89c6\u9891\u7f16\u7801\u3002");
        }
        return false;
    }

    const AVCodec* codec = runtime->api.avcodec_find_decoder(codec_id);
    if (codec == nullptr) {
        if (log_fn != nullptr) {
            log_fn(L"\u89c6\u9891\u89e3\u7801: FFmpeg \u672a\u627e\u5230\u53ef\u7528\u89e3\u7801\u5668\u3002");
        }
        return false;
    }

    runtime->codec_context = runtime->api.avcodec_alloc_context3(codec);
    if (runtime->codec_context == nullptr) {
        if (log_fn != nullptr) {
            log_fn(L"\u89c6\u9891\u89e3\u7801: \u521b\u5efa FFmpeg \u89e3\u7801\u4e0a\u4e0b\u6587\u5931\u8d25\u3002");
        }
        return false;
    }

    runtime->codec_context->thread_count = 1;
    runtime->codec_context->thread_type = FF_THREAD_SLICE;
    runtime->codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    runtime->codec_context->flags2 |= AV_CODEC_FLAG2_FAST;
    runtime->codec_context->width = profile.width;
    runtime->codec_context->height = profile.height;

    const int open_result = runtime->api.avcodec_open2(runtime->codec_context, codec, nullptr);
    if (open_result < 0) {
        if (log_fn != nullptr) {
            log_fn(
                std::wstring(L"\u89c6\u9891\u89e3\u7801: \u6253\u5f00 FFmpeg \u89e3\u7801\u5668\u5931\u8d25\uff0c") +
                runtime->api.FormatAvError(open_result));
        }
        CloseDecoderRuntime(runtime);
        return false;
    }

    runtime->decoded_frame = runtime->api.av_frame_alloc();
    if (runtime->decoded_frame == nullptr) {
        if (log_fn != nullptr) {
            log_fn(L"\u89c6\u9891\u89e3\u7801: \u521b\u5efa FFmpeg \u89e3\u7801\u5e27\u5931\u8d25\u3002");
        }
        CloseDecoderRuntime(runtime);
        return false;
    }

    runtime->parser_context = runtime->api.av_parser_init(codec_id);
    if (runtime->parser_context == nullptr) {
        if (log_fn != nullptr) {
            log_fn(L"\u89c6\u9891\u89e3\u7801: \u521b\u5efa FFmpeg \u7801\u6d41 parser \u5931\u8d25\u3002");
        }
        CloseDecoderRuntime(runtime);
        return false;
    }

    runtime->active_profile = profile;
    runtime->logged_first_output_frame = false;
    runtime->dumped_first_bgra_bitmap = false;
    runtime->dumped_first_startup_stream = false;

    if (log_fn != nullptr) {
        std::wostringstream stream;
        stream << L"\u89c6\u9891\u89e3\u7801: \u5df2\u5207\u5230 FFmpeg \u8f6f\u89e3\u4f4e\u5ef6\u8fdf\u94fe\u8def "
               << profile.width << L"x" << profile.height
               << L" @"
               << (profile.adaptive_fps ? L"\u81ea\u9002\u5e94\u5237\u65b0\u7387" : (std::to_wstring(profile.fps) + L"fps"))
               << L"\uFF0C\u9ED8\u8BA4\u8F93\u51FA=BGRA";
        log_fn(stream.str());
    }

    return true;
}

bool ConvertDecodedFrame(
    DecoderRuntime* runtime,
    const AVFrame* source_frame,
    bool prefer_bgra_output,
    uint64_t fallback_pts_us,
    DecodedFrame* output_frame,
    const VideoDecoder::LogFn& log_fn) {
    if (runtime == nullptr ||
        source_frame == nullptr ||
        output_frame == nullptr ||
        runtime->codec_context == nullptr) {
        return false;
    }

    if (source_frame->width <= 0 ||
        source_frame->height <= 0 ||
        source_frame->format == AV_PIX_FMT_NONE) {
        return false;
    }

    const AVPixelFormat destination_format = OutputPixelFormatForPreference(prefer_bgra_output);
    runtime->sws_context = runtime->api.sws_getCachedContext(
        runtime->sws_context,
        source_frame->width,
        source_frame->height,
        static_cast<AVPixelFormat>(source_frame->format),
        source_frame->width,
        source_frame->height,
        destination_format,
        SWS_FAST_BILINEAR,
        nullptr,
        nullptr,
        nullptr);
    if (runtime->sws_context == nullptr) {
        if (log_fn != nullptr) {
            log_fn(L"\u89c6\u9891\u89e3\u7801: \u521b\u5efa swscale \u8f6c\u6362\u4e0a\u4e0b\u6587\u5931\u8d25\u3002");
        }
        return false;
    }

    runtime->sws_output_format = destination_format;
    runtime->sws_width = source_frame->width;
    runtime->sws_height = source_frame->height;

    const int buffer_size = runtime->api.av_image_get_buffer_size(
        destination_format,
        source_frame->width,
        source_frame->height,
        1);
    if (buffer_size <= 0) {
        if (log_fn != nullptr) {
            log_fn(L"\u89c6\u9891\u89e3\u7801: \u8ba1\u7b97\u8f93\u51fa\u5e27\u7f13\u51b2\u5927\u5c0f\u5931\u8d25\u3002");
        }
        return false;
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(buffer_size));
    std::array<uint8_t*, 4> destination_planes{};
    std::array<int, 4> destination_linesizes{};
    if (runtime->api.av_image_fill_arrays(
            destination_planes.data(),
            destination_linesizes.data(),
            bytes.data(),
            destination_format,
            source_frame->width,
            source_frame->height,
            1) < 0) {
        if (log_fn != nullptr) {
            log_fn(L"\u89c6\u9891\u89e3\u7801: \u521d\u59cb\u5316\u8f93\u51fa\u5e27\u5e73\u9762\u5931\u8d25\u3002");
        }
        return false;
    }

    const int scaled_height = runtime->api.sws_scale(
        runtime->sws_context,
        source_frame->data,
        source_frame->linesize,
        0,
        source_frame->height,
        destination_planes.data(),
        destination_linesizes.data());
    if (scaled_height != source_frame->height) {
        if (log_fn != nullptr) {
            log_fn(L"\u89c6\u9891\u89e3\u7801: swscale \u8f6c\u6362\u5931\u8d25\u3002");
        }
        return false;
    }

    output_frame->Reset();
    output_frame->width = source_frame->width;
    output_frame->height = source_frame->height;
    if (source_frame->best_effort_timestamp != AV_NOPTS_VALUE &&
        source_frame->best_effort_timestamp > 0) {
        output_frame->pts_us = static_cast<uint64_t>(source_frame->best_effort_timestamp);
    } else if (source_frame->pts != AV_NOPTS_VALUE && source_frame->pts > 0) {
        output_frame->pts_us = static_cast<uint64_t>(source_frame->pts);
    } else {
        output_frame->pts_us = fallback_pts_us;
    }
    output_frame->bytes = std::move(bytes);
    output_frame->gpu_backed = false;
    output_frame->direct_sample_safe = false;
    output_frame->separate_textures = false;
    output_frame->d3d_texture = nullptr;
    output_frame->d3d_texture_plane1 = nullptr;
    output_frame->d3d_subresource = 0;

    if (destination_format == AV_PIX_FMT_BGRA) {
        output_frame->format = DecodedFrameFormat::kBgra;
        output_frame->stride0 = destination_linesizes[0];
        output_frame->stride1 = 0;
        output_frame->plane1_offset = 0;
    } else {
        output_frame->format = DecodedFrameFormat::kNv12;
        output_frame->stride0 = destination_linesizes[0];
        output_frame->stride1 = destination_linesizes[1];
        output_frame->plane1_offset = static_cast<size_t>(destination_planes[1] - destination_planes[0]);
    }

    if (!runtime->logged_first_output_frame && log_fn != nullptr) {
        std::wostringstream stream;
        stream << L"\u89c6\u9891\u89e3\u7801: \u9996\u4E2A\u8F93\u51FA\u5E27\u5DF2\u786E\u8BA4\uFF0C"
               << L"\u683C\u5F0F=" << (output_frame->format == DecodedFrameFormat::kBgra ? L"BGRA" : L"NV12")
               << L"\uFF0Cstride0=" << output_frame->stride0
               << L"\uFF0Cstride1=" << output_frame->stride1
               << L"\uFF0Cwidth=" << output_frame->width
               << L"\uFF0Cheight=" << output_frame->height;
        log_fn(stream.str());
        runtime->logged_first_output_frame = true;
    }

    if (!runtime->dumped_first_bgra_bitmap && output_frame->format == DecodedFrameFormat::kBgra) {
        const std::wstring bitmap_path = BuildDebugOutputPath(L"receiver-debug-first-decoded-frame.bmp");
        if (SaveBgraBitmap(bitmap_path, *output_frame)) {
            if (log_fn != nullptr) {
                log_fn(L"\u89c6\u9891\u89e3\u7801: \u5df2\u5199\u51fa\u9996\u4e2a\u89e3\u7801 BGRA \u5e27\u8c03\u8bd5\u56fe\u50cf -> " + bitmap_path);
            }
            runtime->dumped_first_bgra_bitmap = true;
        } else if (log_fn != nullptr) {
            log_fn(L"\u89c6\u9891\u89e3\u7801: \u5199\u51fa\u9996\u4e2a\u89e3\u7801 BGRA \u5e27\u8c03\u8bd5\u56fe\u50cf\u5931\u8d25\u3002");
        }
    }

    return true;
}

bool ReceiveFrames(
    DecoderRuntime* runtime,
    bool prefer_bgra_output,
    uint64_t fallback_pts_us,
    const VideoDecoder::FrameFn& frame_fn,
    const VideoDecoder::LogFn& log_fn) {
    if (runtime == nullptr || runtime->codec_context == nullptr || runtime->decoded_frame == nullptr) {
        return false;
    }

    while (true) {
        const int receive_result =
            runtime->api.avcodec_receive_frame(runtime->codec_context, runtime->decoded_frame);
        if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
            return true;
        }
        if (receive_result < 0) {
            if (log_fn != nullptr) {
                log_fn(
                    std::wstring(L"\u89c6\u9891\u89e3\u7801: \u63d0\u53d6\u89e3\u7801\u5e27\u5931\u8d25\uff0c") +
                    runtime->api.FormatAvError(receive_result));
            }
            runtime->api.av_frame_unref(runtime->decoded_frame);
            return false;
        }

        DecodedFrame output_frame;
        const bool converted = ConvertDecodedFrame(
            runtime,
            runtime->decoded_frame,
            prefer_bgra_output,
            fallback_pts_us,
            &output_frame,
            log_fn);
        runtime->api.av_frame_unref(runtime->decoded_frame);
        if (!converted) {
            return false;
        }

        if (frame_fn != nullptr) {
            frame_fn(std::move(output_frame));
        }
    }
}

bool SubmitPacket(
    DecoderRuntime* runtime,
    const AccessUnit& access_unit,
    bool prefer_bgra_output,
    const VideoDecoder::FrameFn& frame_fn,
    const VideoDecoder::LogFn& log_fn) {
    if (runtime == nullptr || runtime->codec_context == nullptr || access_unit.bytes.empty()) {
        return false;
    }

    AVPacket packet{};
    packet.data = const_cast<uint8_t*>(access_unit.bytes.data());
    packet.size = static_cast<int>(access_unit.bytes.size());
    packet.pts = access_unit.pts_us > 0 ? static_cast<int64_t>(access_unit.pts_us) : AV_NOPTS_VALUE;
    packet.dts = packet.pts;
    packet.duration = 0;
    if (IsKeyframeAccessUnit(access_unit)) {
        packet.flags |= AV_PKT_FLAG_KEY;
    }

    int send_result = runtime->api.avcodec_send_packet(runtime->codec_context, &packet);
    if (send_result == AVERROR(EAGAIN)) {
        if (!ReceiveFrames(runtime, prefer_bgra_output, access_unit.pts_us, frame_fn, log_fn)) {
            return false;
        }
        send_result = runtime->api.avcodec_send_packet(runtime->codec_context, &packet);
    }

    if (send_result < 0) {
        if (log_fn != nullptr) {
            log_fn(
                std::wstring(L"\u89c6\u9891\u89e3\u7801: \u63d0\u4ea4\u8bbf\u95ee\u5355\u5143\u5931\u8d25\uff0c") +
                runtime->api.FormatAvError(send_result));
        }
        return false;
    }

    return ReceiveFrames(runtime, prefer_bgra_output, access_unit.pts_us, frame_fn, log_fn);
}

void ResetCodecAndParserState(
    DecoderRuntime* runtime,
    const VideoDecoder::LogFn& log_fn) {
    if (runtime == nullptr) {
        return;
    }

    if (runtime->codec_context != nullptr && runtime->api.avcodec_flush_buffers != nullptr) {
        runtime->api.avcodec_flush_buffers(runtime->codec_context);
    }

    if (runtime->api.av_parser_close != nullptr && runtime->parser_context != nullptr) {
        runtime->api.av_parser_close(runtime->parser_context);
        runtime->parser_context = nullptr;
    }

    const AVCodecID codec_id = CodecIdForProfile(runtime->active_profile);
    if (codec_id == AV_CODEC_ID_NONE || runtime->api.av_parser_init == nullptr) {
        return;
    }

    runtime->parser_context = runtime->api.av_parser_init(codec_id);
    if (runtime->parser_context == nullptr && log_fn != nullptr) {
        log_fn(L"\u89c6\u9891\u89e3\u7801: \u91cd\u7f6e FFmpeg parser \u5931\u8d25\u3002");
    }
}

bool SubmitEncodedChunk(
    DecoderRuntime* runtime,
    const AccessUnit& access_unit,
    bool prefer_bgra_output,
    const VideoDecoder::FrameFn& frame_fn,
    const VideoDecoder::LogFn& log_fn) {
    if (runtime == nullptr || runtime->codec_context == nullptr || access_unit.bytes.empty()) {
        return false;
    }

    if (runtime->parser_context == nullptr || runtime->api.av_parser_parse2 == nullptr) {
        return SubmitPacket(runtime, access_unit, prefer_bgra_output, frame_fn, log_fn);
    }

    const uint8_t* input = access_unit.bytes.data();
    int remaining = static_cast<int>(access_unit.bytes.size());
    int64_t next_pts = access_unit.pts_us > 0 ? static_cast<int64_t>(access_unit.pts_us) : AV_NOPTS_VALUE;

    while (remaining > 0) {
        uint8_t* packet_data = nullptr;
        int packet_size = 0;
        const int consumed =
            runtime->api.av_parser_parse2(
                runtime->parser_context,
                runtime->codec_context,
                &packet_data,
                &packet_size,
                input,
                remaining,
                next_pts,
                next_pts,
                0);
        if (consumed < 0) {
            if (log_fn != nullptr) {
                log_fn(
                    std::wstring(L"\u89c6\u9891\u89e3\u7801: \u89e3\u6790\u7801\u6d41 chunk \u5931\u8d25\uff0c") +
                    runtime->api.FormatAvError(consumed));
            }
            return false;
        }
        if (consumed == 0 && packet_size == 0) {
            break;
        }

        input += consumed;
        remaining -= consumed;

        if (packet_size <= 0 || packet_data == nullptr) {
            next_pts = AV_NOPTS_VALUE;
            continue;
        }

        AccessUnit parsed_unit = access_unit;
        parsed_unit.bytes.assign(packet_data, packet_data + packet_size);

        const int64_t parser_pts =
            runtime->parser_context->pts != AV_NOPTS_VALUE
                ? runtime->parser_context->pts
                : runtime->parser_context->dts;
        if (parser_pts > 0) {
            parsed_unit.pts_us = static_cast<uint64_t>(parser_pts);
        }
        if (runtime->parser_context->key_frame == 1) {
            parsed_unit.flags |= protocol::kFlagKeyframe;
        }

        if (!SubmitPacket(runtime, parsed_unit, prefer_bgra_output, frame_fn, log_fn)) {
            return false;
        }
        next_pts = AV_NOPTS_VALUE;
    }

    return true;
}

}  // namespace

VideoDecoder::VideoDecoder(LogFn log_fn, FrameFn frame_fn, RequestKeyframeFn request_keyframe_fn)
    : log_fn_(std::move(log_fn)),
      frame_fn_(std::move(frame_fn)),
      request_keyframe_fn_(std::move(request_keyframe_fn)) {}

VideoDecoder::~VideoDecoder() {
    Stop();
}

void VideoDecoder::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return;
    }

    running_ = true;
    thread_ = std::thread([this] {
        ThreadMain();
    });
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

    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    waiting_for_keyframe_ = false;
    proactive_keyframe_requested_ = false;
    queue_warning_active_ = false;
}

void VideoDecoder::Configure(const protocol::StreamProfile& profile) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_profile_ = profile;
        active_profile_ = profile;
        profile_dirty_ = true;
        has_active_profile_ = true;
        queue_.clear();
        latest_codec_config_ = AccessUnit{};
        has_latest_codec_config_ = false;
        waiting_for_keyframe_ = true;
        proactive_keyframe_requested_ = false;
        queue_warning_active_ = false;
    }
    cv_.notify_all();
}

void VideoDecoder::SubmitAccessUnit(const AccessUnit& access_unit) {
    bool trimmed_queue = false;
    bool dropped_old_units = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (IsCodecConfigAccessUnit(access_unit)) {
            latest_codec_config_ = access_unit;
            has_latest_codec_config_ = true;

            for (auto it = queue_.begin(); it != queue_.end();) {
                if (IsCodecConfigAccessUnit(*it)) {
                    it = queue_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        queue_.push_back(access_unit);

        if (queue_.size() > kMaxQueuedAccessUnits) {
            trimmed_queue = TrimQueueToLatestDecodableSpan(&queue_, latest_codec_config_, has_latest_codec_config_);
            while (queue_.size() > kQueueTrimTargetAccessUnits) {
                if (trimmed_queue && IsKeyframeAccessUnit(queue_.front())) {
                    break;
                }
                if (queue_.size() <= 1) {
                    break;
                }
                if (!trimmed_queue || !IsCodecConfigAccessUnit(queue_.front())) {
                    queue_.pop_front();
                    dropped_old_units = true;
                } else {
                    queue_.pop_front();
                }
            }
        }

        if (queue_.size() <= 2) {
            queue_warning_active_ = false;
            proactive_keyframe_requested_ = false;
        }
    }

    if (trimmed_queue && log_fn_ != nullptr) {
        log_fn_(
            L"\u89c6\u9891\u89e3\u7801: \u5df2\u6309 scrcpy \u98ce\u683c\u4e22\u5f03\u65e7\u89c6\u9891\u5355\u5143\uff0c"
            L"\u53ea\u4fdd\u7559\u6700\u65b0\u53ef\u89e3\u7801 span\u3002");
    }
    if (dropped_old_units && log_fn_ != nullptr) {
        log_fn_(
            L"\u89c6\u9891\u89e3\u7801: \u961f\u5217\u8fc7\u6df1\uff0c\u5df2\u4e22\u5f03\u6700\u65e7\u7f16\u7801\u5757\u4ee5\u8ffd\u5b9e\u65f6\u3002");
    }

    cv_.notify_all();
}

void VideoDecoder::SetD3DDevice(ID3D11Device* device) {
    std::lock_guard<std::mutex> lock(mutex_);
    d3d_device_ = device;
}

void VideoDecoder::SetSmoothMode(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    smooth_mode_ = enabled;
}

void VideoDecoder::SetPreferBgraOutput(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    prefer_bgra_output_ = enabled;
}

size_t VideoDecoder::GetPendingAccessUnitCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void VideoDecoder::ThreadMain() {
    SetCurrentThreadPriorityBestEffort(THREAD_PRIORITY_HIGHEST);

    DecoderRuntime runtime;

    while (true) {
        AccessUnit access_unit;
        AccessUnit latest_codec_config_snapshot;
        bool has_item = false;
        bool has_latest_codec_config_snapshot = false;
        bool should_exit = false;
        bool profile_dirty = false;
        bool prefer_bgra_output = false;
        protocol::StreamProfile profile{};

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return !running_ || profile_dirty_ || !queue_.empty();
            });

            should_exit = !running_;
            if (profile_dirty_) {
                profile = pending_profile_;
                profile_dirty_ = false;
                profile_dirty = true;
            }
            prefer_bgra_output = prefer_bgra_output_;

            if (!queue_.empty()) {
                access_unit = std::move(queue_.front());
                queue_.pop_front();
                has_item = true;
                if (has_latest_codec_config_) {
                    latest_codec_config_snapshot = latest_codec_config_;
                    has_latest_codec_config_snapshot = true;
                }
            }
        }

        if (profile_dirty) {
            if (!OpenDecoderRuntime(&runtime, profile, log_fn_)) {
                std::lock_guard<std::mutex> lock(mutex_);
                waiting_for_keyframe_ = true;
            }
        }

        if (should_exit) {
            break;
        }
        if (!has_item || runtime.codec_context == nullptr) {
            continue;
        }

        if (IsCodecConfigAccessUnit(access_unit)) {
            if (!SubmitEncodedChunk(&runtime, access_unit, prefer_bgra_output, frame_fn_, log_fn_)) {
                ResetCodecAndParserState(&runtime, log_fn_);
            }
            continue;
        }

        if (access_unit.discontinuity) {
            ResetCodecAndParserState(&runtime, log_fn_);

            const bool current_is_keyframe = IsKeyframeAccessUnit(access_unit);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.clear();
                waiting_for_keyframe_ = !current_is_keyframe;
                proactive_keyframe_requested_ = !current_is_keyframe;
                queue_warning_active_ = false;
            }

            if (log_fn_ != nullptr) {
                log_fn_(
                    current_is_keyframe
                        ? L"\u89c6\u9891\u89e3\u7801: \u68c0\u6d4b\u5230\u4e2d\u9014\u65ad\u5e27\uff0c\u5df2\u5148\u5237\u65b0\u89e3\u7801\u5668\u72b6\u6001\u540e\u76f4\u63a5\u63a5\u5165\u5f53\u524d\u5173\u952e\u5e27\u3002"
                        : L"\u89c6\u9891\u89e3\u7801: \u68c0\u6d4b\u5230\u4e2d\u9014\u65ad\u5e27\uff0c\u5df2\u6e05\u7a7a\u65e7\u89e3\u7801\u72b6\u6001\u5e76\u7b49\u5f85\u65b0\u5173\u952e\u5e27\u3002");
            }

            if (!current_is_keyframe) {
                if (request_keyframe_fn_ != nullptr) {
                    request_keyframe_fn_();
                }
                continue;
            }

            if (has_latest_codec_config_snapshot && !latest_codec_config_snapshot.bytes.empty()) {
                if (!SubmitEncodedChunk(
                        &runtime,
                        latest_codec_config_snapshot,
                        prefer_bgra_output,
                        frame_fn_,
                        log_fn_)) {
                    if (log_fn_ != nullptr) {
                        log_fn_(L"\u89c6\u9891\u89e3\u7801: \u65ad\u5e27\u540e\u91cd\u63d0\u4ea4 codec config \u5931\u8d25\u3002");
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (waiting_for_keyframe_ && !IsKeyframeAccessUnit(access_unit)) {
                continue;
            }
            if (IsKeyframeAccessUnit(access_unit)) {
                waiting_for_keyframe_ = false;
                proactive_keyframe_requested_ = false;
            }
        }

        if (!runtime.dumped_first_startup_stream && IsKeyframeAccessUnit(access_unit)) {
            std::vector<uint8_t> startup_stream;
            if (has_latest_codec_config_snapshot && !latest_codec_config_snapshot.bytes.empty()) {
                startup_stream.insert(
                    startup_stream.end(),
                    latest_codec_config_snapshot.bytes.begin(),
                    latest_codec_config_snapshot.bytes.end());
            }
            startup_stream.insert(startup_stream.end(), access_unit.bytes.begin(), access_unit.bytes.end());
            const std::wstring stream_path = BuildDebugOutputPath(L"receiver-debug-first-startup-stream.h264");
            if (WriteBinaryFile(stream_path, startup_stream.data(), startup_stream.size())) {
                if (log_fn_ != nullptr) {
                    log_fn_(L"\u89c6\u9891\u89e3\u7801: \u5df2\u5199\u51fa\u9996\u4e2a startup \u7801\u6d41\u8c03\u8bd5\u6587\u4ef6 -> " + stream_path);
                }
                runtime.dumped_first_startup_stream = true;
            } else if (log_fn_ != nullptr) {
                log_fn_(L"\u89c6\u9891\u89e3\u7801: \u5199\u51fa startup \u7801\u6d41\u8c03\u8bd5\u6587\u4ef6\u5931\u8d25\u3002");
            }
        }

        if (!SubmitEncodedChunk(&runtime, access_unit, prefer_bgra_output, frame_fn_, log_fn_)) {
            ResetCodecAndParserState(&runtime, log_fn_);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.clear();
                if (has_latest_codec_config_) {
                    queue_.push_back(latest_codec_config_);
                }
                waiting_for_keyframe_ = true;
                proactive_keyframe_requested_ = true;
                queue_warning_active_ = false;
            }

            if (log_fn_ != nullptr) {
                log_fn_(
                    L"\u89c6\u9891\u89e3\u7801: \u5f53\u524d\u8bbf\u95ee\u5355\u5143\u89e3\u7801\u5931\u8d25\uff0c"
                    L"\u5df2\u6e05\u7a7a\u65e7\u961f\u5217\u5e76\u7b49\u5f85\u65b0\u5173\u952e\u5e27\u3002");
            }
            if (request_keyframe_fn_ != nullptr) {
                request_keyframe_fn_();
            }
        }
    }

    CloseDecoderRuntime(&runtime);
    runtime.api.Unload();
}
