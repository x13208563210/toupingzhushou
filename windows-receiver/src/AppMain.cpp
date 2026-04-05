#include "ControlServer.h"
#include "NvidiaCuvidProbe.h"
#include "UdpVideoReceiver.h"
#include "VideoDecoder.h"
#include "VideoRenderer.h"

#include <Windows.h>
#include <WinSock2.h>
#include <Shellapi.h>
#include <mfapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <utility>

namespace {

constexpr wchar_t kStatusWindowClassName[] = L"AndroidCastReceiverStatusWindow";
constexpr wchar_t kVideoWindowClassName[] = L"AndroidCastReceiverVideoWindow";
constexpr wchar_t kStatusWindowTitle[] = L"粥6y直播投屏助手";
constexpr wchar_t kVideoWindowTitle[] = L"粥6y直播投屏助手 - 投屏画面";
constexpr wchar_t kInstanceMutexName[] = L"Zhou6YLiveCastAssistantSingleton";
constexpr wchar_t kAppBuildLabel[] = L"0.3.9-motion-burst-recovery-2026-04-05";

constexpr wchar_t kTextMeasurePending[] = L"\u6D4B\u91CF\u4E2D";
constexpr wchar_t kTextDashboardWaiting[] = L"\u7B49\u5F85\u5B89\u5353\u53D1\u9001\u7AEF\u5EFA\u7ACB\u63A7\u5236\u8FDE\u63A5\u548C\u89C6\u9891\u6D41";
constexpr wchar_t kTextCurrentConfig[] = L"\u5F53\u524D\u914D\u7F6E";
constexpr wchar_t kTextWaitingConnect[] = L"\u7B49\u5F85\u8FDE\u63A5";
constexpr wchar_t kTextProfilePending[] = L"\u53D1\u9001\u7AEF\u5C1A\u672A\u534F\u5546\u5206\u8FA8\u7387";
constexpr wchar_t kTextRealtimeFps[] = L"\u5185\u5BB9\u6E90\u5E27\u7387";
constexpr wchar_t kTextFirstFramePending[] = L"\u7B49\u5F85\u9996\u5E27\u5448\u73B0";
constexpr wchar_t kTextLatency[] = L"\u4F30\u7B97\u7AEF\u5230\u7AEF\u5EF6\u8FDF";
constexpr wchar_t kTextWaitingSync[] = L"\u7B49\u5F85\u540C\u6B65";
constexpr wchar_t kTextNeedSync[] = L"\u9700\u8981\u5148\u5B8C\u6210\u65F6\u949F\u540C\u6B65";
constexpr wchar_t kTextDecoderQueue[] = L"\u663E\u793A\u4E22\u5E27";
constexpr wchar_t kTextWaitingAccessUnit[] = L"\u7B49\u5F85\u8BBF\u95EE\u5355\u5143";
constexpr wchar_t kTextTrafficInitial[] =
    L"\u63A7\u5236\u901A\u9053\uFF1ATCP/5500\r\n"
    L"\u89C6\u9891\u901A\u9053\uFF1AUDP/55000\r\n"
    L"\u6570\u636E\u5305\uFF1A0    \u5B57\u8282\uFF1A0 B\r\n"
    L"\u5B8C\u6574\u5E27\uFF1A0    \u5173\u952E\u5E27\uFF1A0\r\n"
    L"\u4E22\u5E27\uFF1A0    \u6700\u540E\u5E27 ID\uFF1A0";
constexpr wchar_t kTextRuntimeInitial[] =
    L"\u7F51\u7EDC\u91CD\u7EC4\u901F\u7387\uFF1A\u6D4B\u91CF\u4E2D\r\n"
    L"\u89E3\u7801\u8F93\u51FA\u901F\u7387\uFF1A\u6D4B\u91CF\u4E2D\r\n"
    L"\u6700\u7EC8\u663E\u793A\u901F\u7387\uFF1A\u6D4B\u91CF\u4E2D\r\n"
    L"\u6E32\u67D3\u663E\u5361\uFF1A\u7B49\u5F85\u521D\u59CB\u5316\r\n"
    L"\u753B\u9762\u7A97\u53E3\uFF1A\u6536\u5230\u9996\u5E27\u540E\u5F39\u51FA\r\n"
    L"\u65F6\u949F\u540C\u6B65\uFF1A\u7B49\u5F85\u624B\u673A\u6821\u65F6";
constexpr wchar_t kTextFocusVideo[] = L"\u805A\u7126\u753B\u9762";
constexpr wchar_t kTextOpenLog[] = L"\u6253\u5F00\u65E5\u5FD7";
constexpr wchar_t kTextSwitchToLowLatency[] = L"\u5207\u6362\u5230\u4f4e\u5ef6\u8fdf";
constexpr wchar_t kTextSwitchToSmooth[] = L"\u5207\u6362\u5230\u987a\u6ed1";
constexpr wchar_t kTextFixedLowLatency[] = L"\u56fa\u5b9a\u7ade\u6280\u4f4e\u5ef6\u8fdf";
constexpr wchar_t kTextControlPill[] = L"\u63A7\u5236 TCP/5500";
constexpr wchar_t kTextVideoPill[] = L"\u89C6\u9891 UDP/55000";
constexpr wchar_t kTextTrafficTitle[] = L"\u94FE\u8DEF\u7EDF\u8BA1";
constexpr wchar_t kTextRuntimeTitle[] = L"\u8FD0\u884C\u72B6\u6001";
constexpr wchar_t kTextLogTitle[] = L"\u8FD0\u884C\u65E5\u5FD7";
constexpr wchar_t kTextFilePrefix[] = L"\u6587\u4EF6\uFF1A";
constexpr wchar_t kTextVersionPrefix[] = L"\u7248\u672C ";

constexpr UINT kControlPort = 5500;
constexpr UINT kVideoPort = 55000;
constexpr UINT_PTR kStatsTimerId = 1;
constexpr UINT kLogMessage = WM_APP + 1;
constexpr UINT kStatsMessage = WM_APP + 2;
constexpr UINT kShowVideoWindowMessage = WM_APP + 3;
constexpr int kAppIconResourceId = 101;
constexpr UINT kStatsIntervalMs = 250;
constexpr int64_t kTimeSyncIntervalUs = 1'000'000;
constexpr int64_t kMaxReasonableLatencyUs = 5'000'000;
constexpr int kFocusVideoButtonId = 1001;
constexpr int kOpenLogButtonId = 1002;
constexpr int kPresentationModeButtonId = 1003;
bool g_frontend_mode = false;

constexpr int kStatusWindowWidth = 1660;
constexpr int kStatusWindowHeight = 920;
constexpr int kStatusWindowMinWidth = 900;
constexpr int kStatusWindowMinHeight = 760;
constexpr int kVideoWindowWidth = 1440;
constexpr int kVideoWindowHeight = 900;

constexpr COLORREF kColorWindowBackground = RGB(236, 240, 247);
constexpr COLORREF kColorCardBackground = RGB(250, 252, 255);
constexpr COLORREF kColorHeroBackground = RGB(244, 248, 255);
constexpr COLORREF kColorLogBackground = RGB(246, 249, 253);
constexpr COLORREF kColorBorder = RGB(255, 255, 255);
constexpr COLORREF kColorAccent = RGB(10, 132, 255);
constexpr COLORREF kColorAccentDark = RGB(0, 113, 227);
constexpr COLORREF kColorShadowFar = RGB(223, 229, 239);
constexpr COLORREF kColorShadowNear = RGB(235, 240, 247);
constexpr COLORREF kColorGlassGlowBlue = RGB(222, 234, 255);
constexpr COLORREF kColorGlassGlowPink = RGB(245, 229, 255);
constexpr COLORREF kColorGlassGlowMint = RGB(222, 244, 236);
constexpr COLORREF kColorTextPrimary = RGB(28, 33, 42);
constexpr COLORREF kColorTextSecondary = RGB(108, 117, 132);
constexpr COLORREF kColorWhite = RGB(255, 255, 255);

struct DashboardCard {
    std::wstring title;
    std::wstring value;
    std::wstring note;
};

struct DashboardSnapshot {
    std::wstring subtitle;
    std::array<DashboardCard, 4> summary_cards;
    std::wstring traffic_body;
    std::wstring runtime_body;
};

struct StatusWindowLayout {
    RECT hero{};
    RECT focus_button{};
    RECT mode_button{};
    RECT open_log_button{};
    std::array<RECT, 4> summary_cards{};
    RECT traffic_card{};
    RECT runtime_card{};
    RECT log_card{};
    RECT log_view{};
};

struct AppState {
    HWND status_window = nullptr;
    HWND video_window = nullptr;
    HWND focus_video_button = nullptr;
    HWND presentation_mode_button = nullptr;
    HWND open_log_button = nullptr;
    HWND log_view = nullptr;
    HFONT title_font = nullptr;
    HFONT subtitle_font = nullptr;
    HFONT section_font = nullptr;
    HFONT value_font = nullptr;
    HFONT body_font = nullptr;
    HFONT button_font = nullptr;
    HBRUSH window_background_brush = nullptr;
    HBRUSH log_background_brush = nullptr;
    DashboardSnapshot dashboard{};
    VideoRenderer renderer;
    VideoRenderer::PresentationMode presentation_mode = VideoRenderer::PresentationMode::kLowLatency;
    std::mutex log_mutex;
    std::deque<std::wstring> pending_logs;
    std::wstring log_file_path;
    std::wstring status_file_path;
    std::wstring profile_cache_path;
    std::wstring codec_config_cache_path;
    NvidiaCuvidProbeResult nvdec_probe{};
    protocol::StreamProfile selected_profile;
    bool has_selected_profile = false;
    protocol::StreamProfile cached_startup_profile;
    bool has_cached_startup_profile = false;
    std::vector<uint8_t> cached_codec_config;
    bool has_cached_codec_config = false;
    bool auto_resumed_profile = false;
    bool auto_resume_notice_logged = false;
    std::unique_ptr<ControlServer> control_server;
    std::unique_ptr<UdpVideoReceiver> udp_receiver;
    std::unique_ptr<VideoDecoder> decoder;

    std::mutex metrics_mutex;
    bool has_clock_sync = false;
    int64_t sender_clock_offset_us = 0;
    int64_t sender_clock_rtt_us = 0;
    int64_t latest_latency_us = 0;
    uint64_t latency_sample_count = 0;
    uint64_t total_latency_us = 0;
    int64_t min_latency_us = 0;
    int64_t max_latency_us = 0;
    int64_t last_sync_request_us = 0;
    uint64_t decoded_frame_count = 0;
    uint64_t displayed_frame_count = 0;
    uint64_t present_frame_count = 0;
    int64_t fps_window_start_us = 0;
    double display_fps = 0.0;
    double content_fps = 0.0;
    double receive_fps = 0.0;
    double decode_fps = 0.0;
    uint64_t content_frame_count = 0;
    uint64_t content_fps_window_start_pts_us = 0;
    uint64_t last_present_sender_pts_us = 0;
    int64_t rate_window_start_us = 0;
    uint64_t rate_window_completed_frames = 0;
    uint64_t rate_window_decoded_frames = 0;
    uint64_t rate_window_displayed_frames = 0;
    bool video_window_visible = false;
    bool suppress_video_window_auto_show = false;
    int last_auto_video_client_width = 0;
    int last_auto_video_client_height = 0;
    bool shutting_down = false;
};

void QueueLog(AppState* state, const std::wstring& message);

int RectWidth(const RECT& rect) {
    return rect.right - rect.left;
}

int RectHeight(const RECT& rect) {
    return rect.bottom - rect.top;
}

RECT MakeRect(int left, int top, int right, int bottom) {
    RECT rect{left, top, right, bottom};
    return rect;
}

RECT InsetRectCopy(const RECT& rect, int inset_x, int inset_y) {
    RECT result = rect;
    result.left += inset_x;
    result.top += inset_y;
    result.right -= inset_x;
    result.bottom -= inset_y;
    return result;
}

int64_t NowSteadyUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::wstring FormatLatencyMs(int64_t microseconds) {
    std::wostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(1);
    stream << (static_cast<double>(microseconds) / 1000.0) << L" ms";
    return stream.str();
}

bool IsLatencySampleReasonable(int64_t latency_us) {
    return latency_us >= 0 && latency_us <= kMaxReasonableLatencyUs;
}

std::wstring FormatFpsValue(double fps) {
    std::wostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(1);
    stream << fps << L" fps";
    return stream.str();
}

std::wstring FormatOptionalFps(double fps) {
    return fps > 0.0 ? FormatFpsValue(fps) : kTextMeasurePending;
}

std::wstring FormatProfileFrameRate(const protocol::StreamProfile& profile) {
    if (profile.adaptive_fps) {
        return L"\u81EA\u9002\u5E94\u5237\u65B0\u7387";
    }

    std::wostringstream stream;
    stream << profile.fps << L" fps";
    return stream.str();
}

std::wstring FormatBitrateValue(uint64_t bitrate) {
    std::wostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(1);
    if (bitrate >= 1'000'000) {
        stream << (static_cast<double>(bitrate) / 1'000'000.0) << L" Mbps";
    } else if (bitrate >= 1'000) {
        stream << (static_cast<double>(bitrate) / 1'000.0) << L" Kbps";
    } else {
        stream.precision(0);
        stream << bitrate << L" bps";
    }
    return stream.str();
}

std::wstring FormatDataSize(uint64_t bytes) {
    std::wostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(1);
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        stream << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << L" GB";
    } else if (bytes >= 1024ull * 1024ull) {
        stream << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << L" MB";
    } else if (bytes >= 1024ull) {
        stream << (static_cast<double>(bytes) / 1024.0) << L" KB";
    } else {
        stream.precision(0);
        stream << bytes << L" B";
    }
    return stream.str();
}

const wchar_t* CodecName(protocol::Codec codec) {
    return codec == protocol::Codec::kAvc ? L"AVC" : L"HEVC";
}

std::wstring ExtractFileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

std::wstring ShortenMiddle(const std::wstring& text, size_t max_length) {
    if (text.size() <= max_length || max_length < 8) {
        return text;
    }
    const size_t head = max_length / 2;
    const size_t tail = max_length - head - 3;
    return text.substr(0, head) + L"..." + text.substr(text.size() - tail);
}

UINT GetWindowDpiValue(HWND hwnd) {
    if (hwnd != nullptr) {
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32 != nullptr) {
            using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
            const auto get_dpi_for_window =
                reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow"));
            if (get_dpi_for_window != nullptr) {
                return get_dpi_for_window(hwnd);
            }
        }
    }
    return 96;
}

int ScaleByDpi(HWND hwnd, int value) {
    return MulDiv(value, static_cast<int>(GetWindowDpiValue(hwnd)), 96);
}

HFONT CreateUiFont(HWND hwnd, int pixel_size, int weight) {
    return CreateFontW(
        -ScaleByDpi(hwnd, pixel_size),
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei UI");
}

void DeleteFontHandle(HFONT& font) {
    if (font != nullptr) {
        DeleteObject(font);
        font = nullptr;
    }
}

void DeleteBrushHandle(HBRUSH& brush) {
    if (brush != nullptr) {
        DeleteObject(brush);
        brush = nullptr;
    }
}

HICON LoadAppIconHandle(HINSTANCE instance, int icon_size) {
    return reinterpret_cast<HICON>(
        LoadImageW(
            instance,
            MAKEINTRESOURCEW(kAppIconResourceId),
            IMAGE_ICON,
            icon_size,
            icon_size,
            LR_DEFAULTCOLOR));
}

void RecreateUiFonts(AppState* state) {
    if (state == nullptr || state->status_window == nullptr) {
        return;
    }

    DeleteFontHandle(state->title_font);
    DeleteFontHandle(state->subtitle_font);
    DeleteFontHandle(state->section_font);
    DeleteFontHandle(state->value_font);
    DeleteFontHandle(state->body_font);
    DeleteFontHandle(state->button_font);

    state->title_font = CreateUiFont(state->status_window, 30, FW_BOLD);
    state->subtitle_font = CreateUiFont(state->status_window, 15, FW_NORMAL);
    state->section_font = CreateUiFont(state->status_window, 14, 600);
    state->value_font = CreateUiFont(state->status_window, 20, FW_BOLD);
    state->body_font = CreateUiFont(state->status_window, 15, FW_NORMAL);
    state->button_font = CreateUiFont(state->status_window, 14, 600);
}

void ApplyUiToControls(AppState* state) {
    if (state == nullptr) {
        return;
    }

    if (state->focus_video_button != nullptr && state->button_font != nullptr) {
        SendMessageW(state->focus_video_button, WM_SETFONT, reinterpret_cast<WPARAM>(state->button_font), TRUE);
    }
    if (state->presentation_mode_button != nullptr && state->button_font != nullptr) {
        SendMessageW(state->presentation_mode_button, WM_SETFONT, reinterpret_cast<WPARAM>(state->button_font), TRUE);
    }
    if (state->open_log_button != nullptr && state->button_font != nullptr) {
        SendMessageW(state->open_log_button, WM_SETFONT, reinterpret_cast<WPARAM>(state->button_font), TRUE);
    }
    if (state->log_view != nullptr && state->body_font != nullptr) {
        SendMessageW(state->log_view, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);
        const int margin = ScaleByDpi(state->status_window, 10);
        SendMessageW(
            state->log_view,
            EM_SETMARGINS,
            EC_LEFTMARGIN | EC_RIGHTMARGIN,
            MAKELPARAM(margin, margin));
    }
}

std::wstring PresentationModeLabel(VideoRenderer::PresentationMode mode) {
    switch (mode) {
    case VideoRenderer::PresentationMode::kSmooth:
        return L"\u987a\u6ed1\u89c2\u611f";
    case VideoRenderer::PresentationMode::kLowLatency:
    default:
        return L"\u7ade\u6280\u4f4e\u5ef6\u8fdf";
    }
}

std::wstring PresentationModeButtonText(VideoRenderer::PresentationMode mode) {
    (void)mode;
    return std::wstring(kTextFixedLowLatency);
}

void UpdatePresentationModeButton(AppState* state) {
    if (state == nullptr || state->presentation_mode_button == nullptr) {
        return;
    }

    const std::wstring text = PresentationModeButtonText(state->presentation_mode);
    SetWindowTextW(state->presentation_mode_button, text.c_str());
    EnableWindow(state->presentation_mode_button, FALSE);
}

void ApplyPresentationMode(AppState* state, VideoRenderer::PresentationMode mode, bool log_change) {
    if (state == nullptr) {
        return;
    }

    mode = VideoRenderer::PresentationMode::kLowLatency;

    if (state->presentation_mode == mode) {
        UpdatePresentationModeButton(state);
        return;
    }

    state->presentation_mode = mode;
    state->renderer.SetPresentationMode(mode);
    if (state->decoder != nullptr) {
        state->decoder->SetSmoothMode(false);
    }
    UpdatePresentationModeButton(state);

    if (log_change) {
        QueueLog(state, std::wstring(L"\u663e\u793a\u6a21\u5f0f: \u5df2\u5207\u6362\u5230") + PresentationModeLabel(mode) + L"\u3002");
    }

    if (state->status_window != nullptr) {
        PostMessageW(state->status_window, kStatsMessage, 0, 0);
    }
}

bool IsProfileSupportedByNvdec(const NvidiaCuvidProbeResult& probe, const protocol::StreamProfile& profile) {
    if (!probe.cuda_driver_ready || !probe.cuvid_library_ready || !probe.h264_8bit_420_supported) {
        return false;
    }
    if (profile.codec != protocol::Codec::kAvc) {
        return false;
    }
    if (profile.width <= 0 || profile.height <= 0) {
        return false;
    }
    return profile.width <= probe.max_width && profile.height <= probe.max_height;
}

std::wstring BuildNvdecStatusText(const AppState* state) {
    if (state == nullptr) {
        return L"\u672A\u77E5";
    }

    if (!state->nvdec_probe.cuda_driver_ready || !state->nvdec_probe.cuvid_library_ready) {
        return L"\u672A\u5C31\u7EEA";
    }

    if (state->has_selected_profile) {
        return IsProfileSupportedByNvdec(state->nvdec_probe, state->selected_profile)
            ? L"\u5DF2\u5C31\u7EEA\uFF08\u5F53\u524D\u914D\u7F6E\u53EF\u5207\u539F\u751F CUVID\uFF09"
            : L"\u5DF2\u5C31\u7EEA\uFF08\u4F46\u5F53\u524D\u914D\u7F6E\u4E0D\u5728\u652F\u6301\u8303\u56F4\u5185\uFF09";
    }

    return state->nvdec_probe.h264_8bit_420_supported
        ? L"\u5DF2\u5C31\u7EEA\uFF08\u7B49\u5F85\u6D41\u914D\u7F6E\uFF09"
        : L"\u4E0D\u652F\u6301 H.264 8bit 4:2:0";
}

void ResetDashboardSnapshot(AppState* state) {
    if (state == nullptr) {
        return;
    }

    state->dashboard.subtitle = kTextDashboardWaiting;
    state->dashboard.summary_cards = {{
        DashboardCard{kTextCurrentConfig, kTextWaitingConnect, kTextProfilePending},
        DashboardCard{kTextRealtimeFps, kTextMeasurePending, kTextFirstFramePending},
        DashboardCard{kTextLatency, kTextWaitingSync, kTextNeedSync},
        DashboardCard{kTextDecoderQueue, L"0", L"\u5F85\u89E3 0 / \u5DF2\u6536 0 / \u5DF2\u89E3 0 / \u5DF2\u663E 0"},
    }};
    state->dashboard.traffic_body = kTextTrafficInitial;
    state->dashboard.runtime_body = kTextRuntimeInitial;
}

void FocusVideoWindow(AppState* state) {
    if (state == nullptr || state->video_window == nullptr) {
        return;
    }

    state->suppress_video_window_auto_show = false;
    if (!IsWindowVisible(state->video_window)) {
        ShowWindow(state->video_window, SW_SHOWNORMAL);
        UpdateWindow(state->video_window);
        state->video_window_visible = true;
    } else {
        ShowWindow(state->video_window, SW_RESTORE);
    }
    BringWindowToTop(state->video_window);
    SetForegroundWindow(state->video_window);
}

bool AdjustWindowRectForDpiCompat(RECT* rect, DWORD style, BOOL has_menu, DWORD ex_style, UINT dpi) {
    if (rect == nullptr) {
        return false;
    }

    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        const auto adjust_window_rect_for_dpi =
            reinterpret_cast<AdjustWindowRectExForDpiFn>(GetProcAddress(user32, "AdjustWindowRectExForDpi"));
        if (adjust_window_rect_for_dpi != nullptr) {
            return adjust_window_rect_for_dpi(rect, style, has_menu, ex_style, dpi) != FALSE;
        }
    }

    return AdjustWindowRectEx(rect, style, has_menu, ex_style) != FALSE;
}

RECT GetMonitorWorkAreaForWindow(HWND hwnd) {
    RECT work_area{};
    if (hwnd != nullptr) {
        const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (monitor != nullptr) {
            MONITORINFO monitor_info{};
            monitor_info.cbSize = sizeof(monitor_info);
            if (GetMonitorInfoW(monitor, &monitor_info) != FALSE) {
                return monitor_info.rcWork;
            }
        }
    }

    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    return work_area;
}

void ResizeVideoWindowToSelectedProfile(AppState* state) {
    if (state == nullptr || state->video_window == nullptr || !state->has_selected_profile) {
        return;
    }

    const int source_width = state->selected_profile.width;
    const int source_height = state->selected_profile.height;
    if (source_width <= 0 || source_height <= 0) {
        return;
    }

    if (state->last_auto_video_client_width == source_width &&
        state->last_auto_video_client_height == source_height) {
        return;
    }

    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(state->video_window, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(state->video_window, GWL_EXSTYLE));
    const UINT dpi = GetWindowDpiValue(state->video_window);

    RECT source_window_rect{0, 0, source_width, source_height};
    if (!AdjustWindowRectForDpiCompat(&source_window_rect, style, FALSE, ex_style, dpi)) {
        return;
    }

    const int source_outer_width = RectWidth(source_window_rect);
    const int source_outer_height = RectHeight(source_window_rect);
    const int non_client_width = source_outer_width - source_width;
    const int non_client_height = source_outer_height - source_height;

    RECT work_area = GetMonitorWorkAreaForWindow(state->video_window);
    const int safety_margin = ScaleByDpi(state->video_window, 16);
    work_area.left += safety_margin;
    work_area.top += safety_margin;
    work_area.right -= safety_margin;
    work_area.bottom -= safety_margin;

    const int max_outer_width = std::max(1, RectWidth(work_area));
    const int max_outer_height = std::max(1, RectHeight(work_area));
    const int max_client_width = std::max(1, max_outer_width - non_client_width);
    const int max_client_height = std::max(1, max_outer_height - non_client_height);

    const double scale = std::min(
        1.0,
        std::min(
            static_cast<double>(max_client_width) / static_cast<double>(source_width),
            static_cast<double>(max_client_height) / static_cast<double>(source_height)));

    const int target_client_width = std::max(1, static_cast<int>(std::floor(source_width * scale)));
    const int target_client_height = std::max(1, static_cast<int>(std::floor(source_height * scale)));

    RECT target_window_rect{0, 0, target_client_width, target_client_height};
    if (!AdjustWindowRectForDpiCompat(&target_window_rect, style, FALSE, ex_style, dpi)) {
        return;
    }

    const int target_outer_width = RectWidth(target_window_rect);
    const int target_outer_height = RectHeight(target_window_rect);
    const int clamped_left = std::max(
        work_area.left,
        work_area.left + (RectWidth(work_area) - target_outer_width) / 2);
    const int clamped_top = std::max(
        work_area.top,
        work_area.top + (RectHeight(work_area) - target_outer_height) / 2);

    SetWindowPos(
        state->video_window,
        nullptr,
        clamped_left,
        clamped_top,
        target_outer_width,
        target_outer_height,
        SWP_NOZORDER | SWP_NOACTIVATE);

    state->last_auto_video_client_width = source_width;
    state->last_auto_video_client_height = source_height;

    std::wostringstream stream;
    stream << L"\u6295\u5C4F\u7A97\u53E3\u5DF2\u6309\u6E90\u753B\u8D28\u91CD\u8BBE client area\uFF1A"
           << target_client_width
           << L"x"
           << target_client_height;
    if (scale < 0.999) {
        stream << L"\uFF08\u539F\u59CB "
               << source_width
               << L"x"
               << source_height
               << L"\uFF0C\u56E0\u663E\u793A\u5668\u5DE5\u4F5C\u533A\u4E0D\u8DB3\u5DF2\u7B49\u6BD4\u7F29\u5C0F\uFF09";
    } else {
        stream << L"\uFF08\u539F\u59CB "
               << source_width
               << L"x"
               << source_height
               << L"\uFF0C1:1 \u50CF\u7D20\u663E\u793A\uFF09";
    }
    QueueLog(state, stream.str());
}

void OpenLogFile(AppState* state) {
    if (state == nullptr || state->log_file_path.empty()) {
        return;
    }
    ShellExecuteW(state->status_window, L"open", state->log_file_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ApplyWindowBackdrop(HWND hwnd) {
    if (hwnd == nullptr) {
        return;
    }

    constexpr DWORD kDwmWindowCornerPreference = 33;
    constexpr DWORD kDwmSystemBackdropType = 38;
    constexpr int kWindowCornerRound = 2;
    constexpr int kBackdropTransientWindow = 3;

    const HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll");
    if (dwmapi == nullptr) {
        return;
    }

    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    const auto set_window_attribute =
        reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(dwmapi, "DwmSetWindowAttribute"));
    if (set_window_attribute != nullptr) {
        set_window_attribute(hwnd, kDwmWindowCornerPreference, &kWindowCornerRound, sizeof(kWindowCornerRound));
        set_window_attribute(hwnd, kDwmSystemBackdropType, &kBackdropTransientWindow, sizeof(kBackdropTransientWindow));
    }
    FreeLibrary(dwmapi);
}

void FillRoundedRect(HDC dc, const RECT& rect, int radius, COLORREF fill_color, COLORREF border_color) {
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, fill_color);
    SetDCPenColor(dc, border_color);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
}

void FillEllipseColor(HDC dc, const RECT& rect, COLORREF fill_color) {
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, fill_color);
    SetDCPenColor(dc, fill_color);
    Ellipse(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
}

void FillPolygonColor(HDC dc, const POINT* points, int count, COLORREF fill_color, COLORREF border_color) {
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, fill_color);
    SetDCPenColor(dc, border_color);
    Polygon(dc, points, count);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
}

void DrawSoftShadow(AppState* state, HDC dc, const RECT& rect, int radius, int base_offset) {
    if (state == nullptr) {
        return;
    }

    RECT far_shadow = rect;
    OffsetRect(&far_shadow, 0, ScaleByDpi(state->status_window, base_offset));
    InflateRect(&far_shadow, ScaleByDpi(state->status_window, 2), ScaleByDpi(state->status_window, 3));
    FillRoundedRect(
        dc,
        far_shadow,
        radius + ScaleByDpi(state->status_window, 10),
        kColorShadowFar,
        kColorShadowFar);

    RECT near_shadow = rect;
    OffsetRect(&near_shadow, 0, ScaleByDpi(state->status_window, std::max(2, base_offset / 2)));
    InflateRect(&near_shadow, ScaleByDpi(state->status_window, 1), ScaleByDpi(state->status_window, 1));
    FillRoundedRect(
        dc,
        near_shadow,
        radius + ScaleByDpi(state->status_window, 4),
        kColorShadowNear,
        kColorShadowNear);
}

void DrawGlassCard(AppState* state, HDC dc, const RECT& rect, int radius, COLORREF fill_color, COLORREF border_color, int shadow_offset) {
    DrawSoftShadow(state, dc, rect, radius, shadow_offset);
    FillRoundedRect(dc, rect, radius, fill_color, border_color);

    const int glow_height = std::max(ScaleByDpi(state->status_window, 18), RectHeight(rect) / 4);
    RECT top_glow = rect;
    top_glow.bottom = std::min(rect.bottom, rect.top + glow_height);
    FillRoundedRect(dc, top_glow, radius, RGB(255, 255, 255), RGB(255, 255, 255));
}

void DrawGlassBackdrop(AppState* state, HDC dc, const RECT& client_rect) {
    if (state == nullptr) {
        return;
    }

    RECT glow_blue = MakeRect(
        client_rect.left - ScaleByDpi(state->status_window, 120),
        client_rect.top - ScaleByDpi(state->status_window, 60),
        client_rect.left + ScaleByDpi(state->status_window, 260),
        client_rect.top + ScaleByDpi(state->status_window, 260));
    RECT glow_pink = MakeRect(
        client_rect.right - ScaleByDpi(state->status_window, 320),
        client_rect.top + ScaleByDpi(state->status_window, 20),
        client_rect.right + ScaleByDpi(state->status_window, 40),
        client_rect.top + ScaleByDpi(state->status_window, 320));
    RECT glow_mint = MakeRect(
        client_rect.left + ScaleByDpi(state->status_window, 80),
        client_rect.bottom - ScaleByDpi(state->status_window, 260),
        client_rect.left + ScaleByDpi(state->status_window, 420),
        client_rect.bottom + ScaleByDpi(state->status_window, 60));

    FillEllipseColor(dc, glow_blue, kColorGlassGlowBlue);
    FillEllipseColor(dc, glow_pink, kColorGlassGlowPink);
    FillEllipseColor(dc, glow_mint, kColorGlassGlowMint);
}

void DrawTextBlock(HDC dc, HFONT font, const std::wstring& text, RECT rect, COLORREF color, UINT format) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ old_font = nullptr;
    if (font != nullptr) {
        old_font = SelectObject(dc, font);
    }
    DrawTextW(dc, text.c_str(), -1, &rect, format);
    if (old_font != nullptr) {
        SelectObject(dc, old_font);
    }
}

StatusWindowLayout CalculateStatusWindowLayout(AppState* state) {
    StatusWindowLayout layout{};
    if (state == nullptr || state->status_window == nullptr) {
        return layout;
    }

    RECT client_rect{};
    GetClientRect(state->status_window, &client_rect);
    const int width = std::max(1, RectWidth(client_rect));
    const int height = std::max(1, RectHeight(client_rect));

    const int padding = ScaleByDpi(state->status_window, 20);
    const int gap = ScaleByDpi(state->status_window, 16);
    const int hero_height = ScaleByDpi(state->status_window, 148);
    const int summary_height = ScaleByDpi(state->status_window, 176);
    const int detail_preferred_height = ScaleByDpi(state->status_window, 176);
    const int detail_min_height = ScaleByDpi(state->status_window, 136);
    const int log_min_height = ScaleByDpi(state->status_window, 250);
    const int hero_inner_padding = ScaleByDpi(state->status_window, 24);
    const int button_width = ScaleByDpi(state->status_window, 150);
    const int button_height = ScaleByDpi(state->status_window, 42);
    const int button_gap = ScaleByDpi(state->status_window, 12);

    layout.hero = MakeRect(padding, padding, width - padding, padding + hero_height);

    const int button_top = layout.hero.top + hero_inner_padding;
    layout.open_log_button = MakeRect(
        layout.hero.right - hero_inner_padding - button_width,
        button_top,
        layout.hero.right - hero_inner_padding,
        button_top + button_height);
    layout.mode_button = MakeRect(
        layout.open_log_button.left - button_gap - button_width,
        button_top,
        layout.open_log_button.left - button_gap,
        button_top + button_height);
    layout.focus_button = MakeRect(
        layout.mode_button.left - button_gap - button_width,
        button_top,
        layout.mode_button.left - button_gap,
        button_top + button_height);

    const int summary_top = layout.hero.bottom + gap;
    const int summary_width = std::max(ScaleByDpi(state->status_window, 150), (width - padding * 2 - gap * 3) / 4);
    for (size_t index = 0; index < layout.summary_cards.size(); ++index) {
        const int left = padding + static_cast<int>(index) * (summary_width + gap);
        layout.summary_cards[index] = MakeRect(left, summary_top, left + summary_width, summary_top + summary_height);
    }

    const int detail_top = summary_top + summary_height + gap;
    const int remaining_height = std::max(ScaleByDpi(state->status_window, 420), height - detail_top - padding);
    int detail_height = detail_preferred_height;
    if (remaining_height - detail_height - gap < log_min_height) {
        detail_height = std::max(detail_min_height, remaining_height - gap - log_min_height);
    }
    detail_height = std::max(detail_min_height, detail_height);
    const int detail_width = (width - padding * 2 - gap) / 2;
    layout.traffic_card = MakeRect(padding, detail_top, padding + detail_width, detail_top + detail_height);
    layout.runtime_card = MakeRect(
        layout.traffic_card.right + gap,
        detail_top,
        layout.traffic_card.right + gap + detail_width,
        detail_top + detail_height);

    const int log_top = detail_top + detail_height + gap;
    layout.log_card = MakeRect(padding, log_top, width - padding, height - padding);

    const int card_inner_padding = ScaleByDpi(state->status_window, 20);
    const int log_title_height = ScaleByDpi(state->status_window, 26);
    layout.log_view = MakeRect(
        layout.log_card.left + card_inner_padding,
        layout.log_card.top + card_inner_padding + log_title_height + ScaleByDpi(state->status_window, 10),
        layout.log_card.right - card_inner_padding,
        layout.log_card.bottom - card_inner_padding);
    return layout;
}

void DrawPill(HDC dc, HFONT font, const RECT& rect, const std::wstring& text, COLORREF fill_color, COLORREF text_color) {
    FillRoundedRect(dc, rect, RectHeight(rect), fill_color, fill_color);
    RECT text_rect = rect;
    DrawTextBlock(dc, font, text, text_rect, text_color, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawBrandGlyph(HDC dc, const RECT& rect) {
    const int radius = std::max(16, RectHeight(rect) / 4);
    FillRoundedRect(dc, rect, radius, RGB(226, 236, 255), RGB(255, 255, 255));

    RECT glow_rect = MakeRect(
        rect.left - RectWidth(rect) / 8,
        rect.top - RectHeight(rect) / 7,
        rect.left + RectWidth(rect) * 3 / 4,
        rect.top + RectHeight(rect) * 3 / 5);
    FillEllipseColor(dc, glow_rect, RGB(245, 249, 255));

    const int width = RectWidth(rect);
    const int height = RectHeight(rect);
    const int left = rect.left;
    const int top = rect.top;

    POINT brand_z[] = {
        {left + width * 20 / 100, top + height * 20 / 100},
        {left + width * 80 / 100, top + height * 20 / 100},
        {left + width * 47 / 100, top + height * 49 / 100},
        {left + width * 72 / 100, top + height * 49 / 100},
        {left + width * 33 / 100, top + height * 80 / 100},
        {left + width * 79 / 100, top + height * 80 / 100},
        {left + width * 79 / 100, top + height * 89 / 100},
        {left + width * 20 / 100, top + height * 89 / 100},
        {left + width * 52 / 100, top + height * 58 / 100},
        {left + width * 20 / 100, top + height * 58 / 100},
    };
    FillPolygonColor(dc, brand_z, static_cast<int>(_countof(brand_z)), RGB(79, 114, 255), RGB(79, 114, 255));
}

void DrawSummaryCard(AppState* state, HDC dc, const RECT& rect, const DashboardCard& card, bool accent_style) {
    const int radius = ScaleByDpi(state->status_window, 22);
    const int inner_padding = ScaleByDpi(state->status_window, 20);
    const COLORREF fill_color = accent_style ? kColorHeroBackground : kColorCardBackground;
    const COLORREF border_color = accent_style ? RGB(232, 240, 255) : kColorBorder;
    const COLORREF value_color = accent_style ? kColorAccent : kColorTextPrimary;

    DrawGlassCard(state, dc, rect, radius, fill_color, border_color, 9);

    RECT title_rect = MakeRect(
        rect.left + inner_padding,
        rect.top + inner_padding,
        rect.right - inner_padding,
        rect.top + inner_padding + ScaleByDpi(state->status_window, 22));
    DrawTextBlock(dc, state->section_font, card.title, title_rect, kColorTextSecondary, DT_LEFT | DT_TOP | DT_SINGLELINE);

    RECT value_rect = MakeRect(
        rect.left + inner_padding,
        title_rect.bottom + ScaleByDpi(state->status_window, 10),
        rect.right - inner_padding,
        title_rect.bottom + ScaleByDpi(state->status_window, 34));
    DrawTextBlock(dc, state->value_font, card.value, value_rect, value_color, DT_LEFT | DT_TOP | DT_SINGLELINE);

    RECT note_rect = MakeRect(
        rect.left + inner_padding,
        value_rect.bottom + ScaleByDpi(state->status_window, 10),
        rect.right - inner_padding,
        rect.bottom - inner_padding);
    DrawTextBlock(dc, state->body_font, card.note, note_rect, kColorTextSecondary, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void DrawSectionCard(
    AppState* state,
    HDC dc,
    const RECT& rect,
    const std::wstring& title,
    const std::wstring& body) {
    const int radius = ScaleByDpi(state->status_window, 22);
    const int inner_padding = ScaleByDpi(state->status_window, 20);
    DrawGlassCard(state, dc, rect, radius, kColorCardBackground, kColorBorder, 10);

    RECT title_rect = MakeRect(
        rect.left + inner_padding,
        rect.top + inner_padding,
        rect.right - inner_padding,
        rect.top + inner_padding + ScaleByDpi(state->status_window, 24));
    DrawTextBlock(dc, state->section_font, title, title_rect, kColorTextPrimary, DT_LEFT | DT_TOP | DT_SINGLELINE);

    RECT body_rect = MakeRect(
        rect.left + inner_padding,
        title_rect.bottom + ScaleByDpi(state->status_window, 12),
        rect.right - inner_padding,
        rect.bottom - inner_padding);
    DrawTextBlock(dc, state->body_font, body, body_rect, kColorTextSecondary, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void DrawActionButton(AppState* state, const DRAWITEMSTRUCT* draw_item) {
    if (draw_item == nullptr) {
        return;
    }

    const bool accent_style = draw_item->CtlID == kFocusVideoButtonId;
    const bool pressed = (draw_item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (draw_item->itemState & ODS_DISABLED) != 0;
    COLORREF fill_color = accent_style ? (pressed ? kColorAccentDark : kColorAccent) : RGB(252, 253, 255);
    COLORREF border_color = accent_style ? fill_color : kColorBorder;
    COLORREF text_color = accent_style ? kColorWhite : kColorTextPrimary;

    if (disabled) {
        fill_color = RGB(234, 238, 244);
        border_color = RGB(220, 226, 233);
        text_color = RGB(149, 158, 170);
    } else if (!accent_style && pressed) {
        fill_color = RGB(239, 243, 248);
    }

    if (!pressed) {
        DrawSoftShadow(state, draw_item->hDC, draw_item->rcItem, std::max(14, RectHeight(draw_item->rcItem) / 2), 6);
    }
    FillRoundedRect(draw_item->hDC, draw_item->rcItem, std::max(14, RectHeight(draw_item->rcItem) / 2), fill_color, border_color);

    wchar_t text_buffer[64]{};
    GetWindowTextW(draw_item->hwndItem, text_buffer, static_cast<int>(_countof(text_buffer)));
    RECT text_rect = draw_item->rcItem;
    DrawTextBlock(
        draw_item->hDC,
        state != nullptr ? state->button_font : nullptr,
        text_buffer,
        text_rect,
        text_color,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if ((draw_item->itemState & ODS_FOCUS) != 0) {
        RECT focus_rect = draw_item->rcItem;
        InflateRect(&focus_rect, -4, -4);
        DrawFocusRect(draw_item->hDC, &focus_rect);
    }
}

void PaintStatusWindow(AppState* state, HWND hwnd, HDC target_dc) {
    RECT client_rect{};
    GetClientRect(hwnd, &client_rect);
    const int width = std::max(1, RectWidth(client_rect));
    const int height = std::max(1, RectHeight(client_rect));

    const HDC memory_dc = CreateCompatibleDC(target_dc);
    const HBITMAP bitmap = CreateCompatibleBitmap(target_dc, width, height);
    const HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);

    HBRUSH background_brush = state != nullptr ? state->window_background_brush : nullptr;
    if (background_brush == nullptr) {
        background_brush = CreateSolidBrush(kColorWindowBackground);
    }
    FillRect(memory_dc, &client_rect, background_brush);
    DrawGlassBackdrop(state, memory_dc, client_rect);

    if (state != nullptr) {
        const StatusWindowLayout layout = CalculateStatusWindowLayout(state);
        const int hero_radius = ScaleByDpi(hwnd, 28);
        const int hero_inner_padding = ScaleByDpi(hwnd, 24);
        const int icon_size = ScaleByDpi(hwnd, 70);
        const int pill_height = ScaleByDpi(hwnd, 30);
        const int pill_width = ScaleByDpi(hwnd, 122);
        const int pill_gap = ScaleByDpi(hwnd, 10);
        const int version_pill_width = ScaleByDpi(hwnd, 286);
        const int version_pill_gap = ScaleByDpi(hwnd, 12);

        DrawGlassCard(state, memory_dc, layout.hero, hero_radius, kColorHeroBackground, RGB(255, 255, 255), 12);

        const RECT icon_rect = MakeRect(
            layout.hero.left + hero_inner_padding,
            layout.hero.top + hero_inner_padding,
            layout.hero.left + hero_inner_padding + icon_size,
            layout.hero.top + hero_inner_padding + icon_size);
        DrawBrandGlyph(memory_dc, icon_rect);

        const int title_left = icon_rect.right + ScaleByDpi(hwnd, 20);
        const int version_pill_right = static_cast<int>(layout.focus_button.left) - ScaleByDpi(hwnd, 20);
        const int version_pill_left = std::max(
            title_left + ScaleByDpi(hwnd, 180),
            version_pill_right - version_pill_width);
        const int title_right = std::max(
            title_left + ScaleByDpi(hwnd, 120),
            version_pill_left - version_pill_gap);
        RECT title_rect = MakeRect(
            title_left,
            layout.hero.top + hero_inner_padding,
            title_right,
            layout.hero.top + hero_inner_padding + ScaleByDpi(hwnd, 36));
        DrawTextBlock(memory_dc, state->title_font, kStatusWindowTitle, title_rect, kColorTextPrimary, DT_LEFT | DT_TOP | DT_SINGLELINE);

        RECT version_pill_rect = MakeRect(
            version_pill_left,
            layout.hero.top + hero_inner_padding + ScaleByDpi(hwnd, 2),
            version_pill_right,
            layout.hero.top + hero_inner_padding + ScaleByDpi(hwnd, 2) + pill_height);
        DrawPill(
            memory_dc,
            state->body_font,
            version_pill_rect,
            std::wstring(kTextVersionPrefix) + kAppBuildLabel,
            RGB(255, 255, 255),
            kColorTextSecondary);

        RECT subtitle_rect = MakeRect(
            title_left,
            title_rect.bottom + ScaleByDpi(hwnd, 8),
            title_right,
            title_rect.bottom + ScaleByDpi(hwnd, 44));
        DrawTextBlock(
            memory_dc,
            state->subtitle_font,
            state->dashboard.subtitle,
            subtitle_rect,
            kColorTextSecondary,
            DT_LEFT | DT_TOP | DT_WORDBREAK);

        RECT control_pill = MakeRect(
            title_left,
            layout.hero.bottom - hero_inner_padding - pill_height,
            title_left + pill_width,
            layout.hero.bottom - hero_inner_padding);
        RECT video_pill = MakeRect(
            control_pill.right + pill_gap,
            control_pill.top,
            control_pill.right + pill_gap + pill_width,
            control_pill.bottom);
        DrawPill(memory_dc, state->subtitle_font, control_pill, kTextControlPill, kColorAccent, kColorWhite);
        DrawPill(memory_dc, state->subtitle_font, video_pill, kTextVideoPill, RGB(255, 255, 255), kColorTextPrimary);

        for (size_t index = 0; index < layout.summary_cards.size(); ++index) {
            DrawSummaryCard(state, memory_dc, layout.summary_cards[index], state->dashboard.summary_cards[index], index == 0);
        }

        DrawSectionCard(state, memory_dc, layout.traffic_card, kTextTrafficTitle, state->dashboard.traffic_body);
        DrawSectionCard(state, memory_dc, layout.runtime_card, kTextRuntimeTitle, state->dashboard.runtime_body);

        DrawGlassCard(state, memory_dc, layout.log_card, ScaleByDpi(hwnd, 22), kColorCardBackground, kColorBorder, 11);
        RECT log_title_rect = MakeRect(
            layout.log_card.left + ScaleByDpi(hwnd, 20),
            layout.log_card.top + ScaleByDpi(hwnd, 20),
            layout.log_card.right - ScaleByDpi(hwnd, 20),
            layout.log_card.top + ScaleByDpi(hwnd, 48));
        DrawTextBlock(memory_dc, state->section_font, kTextLogTitle, log_title_rect, kColorTextPrimary, DT_LEFT | DT_TOP | DT_SINGLELINE);

        const std::wstring log_hint = std::wstring(kTextFilePrefix) + ShortenMiddle(ExtractFileName(state->log_file_path), 28);
        RECT log_hint_rect = log_title_rect;
        DrawTextBlock(memory_dc, state->body_font, log_hint, log_hint_rect, kColorTextSecondary, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    }

    BitBlt(target_dc, 0, 0, width, height, memory_dc, 0, 0, SRCCOPY);

    SelectObject(memory_dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);

    if (state == nullptr || state->window_background_brush != nullptr) {
        return;
    }
    DeleteObject(background_brush);
}

std::wstring GetExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (length >= path.size() - 1) {
        path.resize(path.size() * 2, L'\0');
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    path.resize(length);

    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    return path.substr(0, slash);
}

std::wstring BuildLogFilePath() {
    return GetExecutableDirectory() + L"\\receiver-log.txt";
}

std::wstring BuildStatusFilePath() {
    return GetExecutableDirectory() + L"\\receiver-status.json";
}

std::wstring BuildProfileCachePath() {
    return GetExecutableDirectory() + L"\\receiver-profile-cache.json";
}

std::wstring BuildCodecConfigCachePath() {
    return GetExecutableDirectory() + L"\\receiver-codec-config.bin";
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        return {};
    }

    std::string result(size, '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        size,
        nullptr,
        nullptr);
    return result;
}

std::string ReadUtf8File(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) {
        return {};
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return {};
    }
    const long file_size = ftell(file);
    if (file_size <= 0) {
        fclose(file);
        return {};
    }
    rewind(file);

    std::string text(static_cast<size_t>(file_size), '\0');
    const size_t read_size = fread(text.data(), 1, text.size(), file);
    fclose(file);
    text.resize(read_size);

    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

std::vector<uint8_t> ReadBinaryFile(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) {
        return {};
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return {};
    }
    const long file_size = ftell(file);
    if (file_size <= 0) {
        fclose(file);
        return {};
    }
    rewind(file);

    std::vector<uint8_t> data(static_cast<size_t>(file_size));
    const size_t read_size = fread(data.data(), 1, data.size(), file);
    fclose(file);
    data.resize(read_size);
    return data;
}

std::string EscapeJsonUtf8(const std::string& value) {
    std::ostringstream stream;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\':
            stream << "\\\\";
            break;
        case '"':
            stream << "\\\"";
            break;
        case '\r':
            stream << "\\r";
            break;
        case '\n':
            stream << "\\n";
            break;
        case '\t':
            stream << "\\t";
            break;
        default:
            if (ch < 0x20) {
                stream << "\\u"
                       << std::hex
                       << std::uppercase
                       << std::setw(4)
                       << std::setfill('0')
                       << static_cast<int>(ch);
            } else {
                stream << static_cast<char>(ch);
            }
            break;
        }
    }
    return stream.str();
}

std::string QuoteJsonWide(const std::wstring& value) {
    return "\"" + EscapeJsonUtf8(WideToUtf8(value)) + "\"";
}

bool LoadCachedSelectedProfile(const std::wstring& path, protocol::StreamProfile* out) {
    if (out == nullptr) {
        return false;
    }

    const std::string json_text = ReadUtf8File(path);
    if (json_text.empty()) {
        return false;
    }

    const std::regex profile_regex(
        R"json((?:"selectedProfile"\s*:\s*)?\{\s*"codec"\s*:\s*"([^"]+)"\s*,\s*"width"\s*:\s*(\d+)\s*,\s*"height"\s*:\s*(\d+)\s*,\s*"fps"\s*:\s*(\d+)\s*,\s*"adaptiveFps"\s*:\s*(true|false)\s*,\s*"bitrate"\s*:\s*(\d+)\s*\})json");
    std::smatch match;
    if (!std::regex_search(json_text, match, profile_regex)) {
        return false;
    }

    protocol::StreamProfile profile;
    profile.codec = protocol::CodecFromWireName(match[1].str());
    profile.width = std::stoi(match[2].str());
    profile.height = std::stoi(match[3].str());
    profile.fps = std::stoi(match[4].str());
    profile.adaptive_fps = match[5].str() == "true";
    profile.bitrate = std::stoi(match[6].str());
    profile.video_port = kVideoPort;
    if (profile.codec == protocol::Codec::kUnknown ||
        profile.width <= 0 ||
        profile.height <= 0 ||
        profile.fps <= 0 ||
        profile.bitrate <= 0) {
        return false;
    }

    *out = profile;
    return true;
}

std::string BuildProfileCacheJson(const protocol::StreamProfile& profile) {
    std::ostringstream json;
    json << "{"
         << "\"codec\":\"" << protocol::CodecToWireName(profile.codec) << "\","
         << "\"width\":" << profile.width << ","
         << "\"height\":" << profile.height << ","
         << "\"fps\":" << profile.fps << ","
         << "\"adaptiveFps\":" << (profile.adaptive_fps ? "true" : "false") << ","
         << "\"bitrate\":" << profile.bitrate
         << "}\n";
    return json.str();
}

void WriteUtf8File(const std::wstring& path, const std::string& utf8_text) {
    if (path.empty()) {
        return;
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
        return;
    }

    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    fwrite(bom, 1, sizeof(bom), file);
    if (!utf8_text.empty()) {
        fwrite(utf8_text.data(), 1, utf8_text.size(), file);
    }
    fclose(file);
}

void WriteBinaryFile(const std::wstring& path, const std::vector<uint8_t>& data) {
    if (path.empty() || data.empty()) {
        return;
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
        return;
    }

    fwrite(data.data(), 1, data.size(), file);
    fclose(file);
}

void ResetLogFile(const std::wstring& path) {
    if (path.empty()) {
        return;
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
        return;
    }

    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    fwrite(bom, 1, sizeof(bom), file);

    const std::string header = WideToUtf8(
        L"\u5B89\u5353\u6295\u5C4F\u63A5\u6536\u7AEF\u65E5\u5FD7\r\n"
        L"========================================\r\n");
    if (!header.empty()) {
        fwrite(header.data(), 1, header.size(), file);
    }
    fclose(file);
}

void AppendLogFileLine(const std::wstring& path, const std::wstring& line) {
    if (path.empty()) {
        return;
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"ab") != 0 || file == nullptr) {
        return;
    }

    const std::string utf8 = WideToUtf8(line + L"\r\n");
    if (!utf8.empty()) {
        fwrite(utf8.data(), 1, utf8.size(), file);
    }
    fclose(file);
}

void AppendLogText(HWND edit_control, const std::wstring& line) {
    const int current_length = GetWindowTextLengthW(edit_control);
    SendMessageW(edit_control, EM_SETSEL, current_length, current_length);

    std::wstring payload = line + L"\r\n";
    SendMessageW(edit_control, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(payload.c_str()));
}

void QueueLog(AppState* state, const std::wstring& message) {
    if (state == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state->log_mutex);
        AppendLogFileLine(state->log_file_path, message);
        state->pending_logs.push_back(message);
    }

    if (state->status_window != nullptr) {
        PostMessageW(state->status_window, kLogMessage, 0, 0);
    }
}

void UpdateLatencyEstimate(AppState* state, uint64_t sender_pts_us) {
    if (state == nullptr || sender_pts_us == 0) {
        return;
    }

    const int64_t receiver_present_us = NowSteadyUs();
    {
        std::lock_guard<std::mutex> lock(state->metrics_mutex);
        if (state->has_clock_sync) {
            const int64_t estimated_latency_us =
                receiver_present_us + state->sender_clock_offset_us - static_cast<int64_t>(sender_pts_us);
            if (IsLatencySampleReasonable(estimated_latency_us)) {
                state->latest_latency_us = estimated_latency_us;
                state->latency_sample_count += 1;
                state->total_latency_us += static_cast<uint64_t>(estimated_latency_us);
                if (state->latency_sample_count == 1) {
                    state->min_latency_us = estimated_latency_us;
                    state->max_latency_us = estimated_latency_us;
                } else {
                    state->min_latency_us = std::min(state->min_latency_us, estimated_latency_us);
                    state->max_latency_us = std::max(state->max_latency_us, estimated_latency_us);
                }
            }
        }

        if (state->fps_window_start_us == 0) {
            state->fps_window_start_us = receiver_present_us;
            state->present_frame_count = 0;
        }
        if (state->content_fps_window_start_pts_us == 0 ||
            sender_pts_us < state->content_fps_window_start_pts_us ||
            sender_pts_us <= state->last_present_sender_pts_us) {
            state->content_fps_window_start_pts_us = sender_pts_us;
            state->content_frame_count = 1;
        } else {
            state->content_frame_count += 1;
            const uint64_t content_elapsed_us = sender_pts_us - state->content_fps_window_start_pts_us;
            if (content_elapsed_us >= 500'000 && state->content_frame_count > 1) {
                state->content_fps =
                    static_cast<double>(state->content_frame_count - 1) * 1'000'000.0 /
                    static_cast<double>(content_elapsed_us);
                state->content_fps_window_start_pts_us = sender_pts_us;
                state->content_frame_count = 1;
            }
        }
        state->last_present_sender_pts_us = sender_pts_us;
        state->displayed_frame_count += 1;
        state->present_frame_count += 1;
        const int64_t fps_elapsed_us = receiver_present_us - state->fps_window_start_us;
        if (fps_elapsed_us >= 500'000) {
            state->display_fps =
                static_cast<double>(state->present_frame_count) * 1'000'000.0 / static_cast<double>(fps_elapsed_us);
            state->fps_window_start_us = receiver_present_us;
            state->present_frame_count = 0;
        }
    }
}

void UpdateClockSync(AppState* state, int64_t offset_us, int64_t rtt_us) {
    if (state == nullptr) {
        return;
    }

    bool first_sync = false;
    bool should_reset_latency = false;
    {
        std::lock_guard<std::mutex> lock(state->metrics_mutex);
        first_sync = !state->has_clock_sync;
        if (!first_sync) {
            const int64_t offset_delta = offset_us - state->sender_clock_offset_us;
            should_reset_latency =
                offset_delta > kMaxReasonableLatencyUs || offset_delta < -kMaxReasonableLatencyUs;
        }
        state->has_clock_sync = true;
        state->sender_clock_offset_us = offset_us;
        state->sender_clock_rtt_us = rtt_us;
        if (first_sync || should_reset_latency) {
            state->latest_latency_us = 0;
            state->latency_sample_count = 0;
            state->total_latency_us = 0;
            state->min_latency_us = 0;
            state->max_latency_us = 0;
        }
    }

    if (first_sync) {
        std::wostringstream stream;
        stream << L"\u65F6\u949F\u540C\u6B65\u5DF2\u5B8C\u6210\uFF0C\u5F80\u8FD4="
               << FormatLatencyMs(rtt_us)
               << L"\uFF0C\u7AEF\u5230\u7AEF\u5EF6\u8FDF\u7EDF\u8BA1\u5DF2\u6062\u590D\u3002";
        QueueLog(state, stream.str());
    }

    if (state->status_window != nullptr) {
        PostMessageW(state->status_window, kStatsMessage, 0, 0);
    }
}

void ResetRuntimeMetricsForStream(AppState* state) {
    if (state == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(state->metrics_mutex);
    state->has_clock_sync = false;
    state->sender_clock_offset_us = 0;
    state->sender_clock_rtt_us = 0;
    state->latest_latency_us = 0;
    state->latency_sample_count = 0;
    state->total_latency_us = 0;
    state->min_latency_us = 0;
    state->max_latency_us = 0;
    state->decoded_frame_count = 0;
    state->displayed_frame_count = 0;
    state->present_frame_count = 0;
    state->fps_window_start_us = 0;
    state->display_fps = 0.0;
    state->content_fps = 0.0;
    state->receive_fps = 0.0;
    state->decode_fps = 0.0;
    state->content_frame_count = 0;
    state->content_fps_window_start_pts_us = 0;
    state->last_present_sender_pts_us = 0;
    state->rate_window_start_us = 0;
    state->rate_window_completed_frames = 0;
    state->rate_window_decoded_frames = 0;
    state->rate_window_displayed_frames = 0;
    state->last_sync_request_us = 0;
}

std::wstring FormatProfileSummaryForLog(const protocol::StreamProfile& profile) {
    std::wostringstream stream;
    stream << (profile.codec == protocol::Codec::kAvc ? L"AVC" : L"HEVC")
           << L" "
           << profile.width
           << L"x"
           << profile.height
           << L" / "
           << FormatProfileFrameRate(profile)
           << L" / "
           << FormatBitrateValue(profile.bitrate);
    return stream.str();
}

void ApplyStreamProfile(AppState* state, const protocol::StreamProfile& profile, bool auto_resumed) {
    if (state == nullptr) {
        return;
    }

    state->selected_profile = profile;
    state->has_selected_profile = true;
    state->auto_resumed_profile = auto_resumed;
    state->suppress_video_window_auto_show = false;
    ResetRuntimeMetricsForStream(state);

    if (state->decoder != nullptr) {
        state->decoder->Configure(profile);
    }

    ResizeVideoWindowToSelectedProfile(state);

    if (state->control_server != nullptr) {
        if (!auto_resumed) {
            state->control_server->RequestIdr();
        }
        state->control_server->RequestTimeSync();
    }

    if (state->status_window != nullptr) {
        PostMessageW(state->status_window, kStatsMessage, 0, 0);
    }
}

void SubmitCachedCodecConfig(AppState* state, uint32_t frame_id) {
    if (state == nullptr ||
        state->decoder == nullptr ||
        !state->has_cached_codec_config ||
        state->cached_codec_config.empty()) {
        return;
    }

    AccessUnit cached_unit;
    cached_unit.bytes = state->cached_codec_config;
    cached_unit.frame_id = frame_id;
    cached_unit.pts_us = 0;
    cached_unit.flags = static_cast<uint8_t>(protocol::kFlagCodecConfig | protocol::kFlagKeyframe);
    state->decoder->SubmitAccessUnit(cached_unit);
}

void WriteStatusSnapshot(
    AppState* state,
    const VideoStats& stats,
    size_t decoder_queue_depth,
    bool has_clock_sync,
    int64_t sender_clock_rtt_us,
    int64_t latest_latency_us,
    int64_t average_latency_us,
    int64_t min_latency_us,
    int64_t max_latency_us,
    uint64_t latency_sample_count,
    uint64_t decoded_frame_count,
    uint64_t displayed_frame_count,
    uint64_t display_dropped_frames,
    double content_fps,
    double receive_fps,
    double decode_fps,
    double display_fps) {
    if (state == nullptr || state->status_file_path.empty()) {
        return;
    }

    auto format_number = [](double value) {
        std::ostringstream stream;
        stream.setf(std::ios::fixed);
        stream.precision(2);
        stream << value;
        return stream.str();
    };

    std::ostringstream json;
    json << "{\n";
    json << "  \"frontendMode\": " << (g_frontend_mode ? "true" : "false") << ",\n";
    json << "  \"updatedAtUs\": " << NowSteadyUs() << ",\n";
    json << "  \"controlPort\": " << kControlPort << ",\n";
    json << "  \"videoPort\": " << kVideoPort << ",\n";
    json << "  \"totalPackets\": " << stats.total_packets << ",\n";
    json << "  \"totalBytes\": " << stats.total_bytes << ",\n";
    json << "  \"completedFrames\": " << stats.completed_frames << ",\n";
    json << "  \"decodedFrames\": " << decoded_frame_count << ",\n";
    json << "  \"displayedFrames\": " << displayed_frame_count << ",\n";
    json << "  \"displayDroppedFrames\": " << display_dropped_frames << ",\n";
    json << "  \"droppedFrames\": " << stats.dropped_frames << ",\n";
    json << "  \"keyframes\": " << stats.keyframes << ",\n";
    json << "  \"lastFrameId\": " << stats.last_frame_id << ",\n";
    json << "  \"lastPtsUs\": " << stats.last_pts_us << ",\n";
    json << "  \"decoderQueueDepth\": " << decoder_queue_depth << ",\n";
    json << "  \"hasClockSync\": " << (has_clock_sync ? "true" : "false") << ",\n";
    json << "  \"latencySampleCount\": " << latency_sample_count << ",\n";
    json << "  \"currentLatencyMs\": " << format_number(static_cast<double>(latest_latency_us) / 1000.0) << ",\n";
    json << "  \"averageLatencyMs\": " << format_number(static_cast<double>(average_latency_us) / 1000.0) << ",\n";
    json << "  \"minLatencyMs\": " << format_number(static_cast<double>(min_latency_us) / 1000.0) << ",\n";
    json << "  \"maxLatencyMs\": " << format_number(static_cast<double>(max_latency_us) / 1000.0) << ",\n";
    json << "  \"clockRttMs\": " << format_number(static_cast<double>(sender_clock_rtt_us) / 1000.0) << ",\n";
    json << "  \"contentFps\": " << format_number(content_fps) << ",\n";
    json << "  \"networkReassembleFps\": " << format_number(receive_fps) << ",\n";
    json << "  \"decodeOutputFps\": " << format_number(decode_fps) << ",\n";
    json << "  \"finalDisplayFps\": " << format_number(display_fps) << ",\n";
    json << "  \"receiveFps\": " << format_number(receive_fps) << ",\n";
    json << "  \"decodeFps\": " << format_number(decode_fps) << ",\n";
    json << "  \"displayFps\": " << format_number(display_fps) << ",\n";
    json << "  \"presentationMode\": " << QuoteJsonWide(PresentationModeLabel(state->presentation_mode)) << ",\n";
    json << "  \"gpuName\": " << QuoteJsonWide(state->renderer.gpu_name()) << ",\n";
    json << "  \"nvdecCudaReady\": " << (state->nvdec_probe.cuda_driver_ready ? "true" : "false") << ",\n";
    json << "  \"nvdecCuvidReady\": " << (state->nvdec_probe.cuvid_library_ready ? "true" : "false") << ",\n";
    json << "  \"nvdecH264Supported\": " << (state->nvdec_probe.h264_8bit_420_supported ? "true" : "false") << ",\n";
    json << "  \"nvdecMaxWidth\": " << state->nvdec_probe.max_width << ",\n";
    json << "  \"nvdecMaxHeight\": " << state->nvdec_probe.max_height << ",\n";
    json << "  \"nvdecStatus\": " << QuoteJsonWide(BuildNvdecStatusText(state)) << ",\n";
    json << "  \"videoWindowReady\": " << (state->video_window != nullptr ? "true" : "false") << ",\n";
    json << "  \"logFilePath\": " << QuoteJsonWide(state->log_file_path) << ",\n";
    json << "  \"statusFilePath\": " << QuoteJsonWide(state->status_file_path) << ",\n";
    json << "  \"selectedProfile\": ";
    if (state->has_selected_profile) {
        json << "{"
             << "\"codec\": "
             << QuoteJsonWide(state->selected_profile.codec == protocol::Codec::kAvc ? L"AVC" : L"HEVC")
             << ", "
             << "\"width\": " << state->selected_profile.width << ", "
             << "\"height\": " << state->selected_profile.height << ", "
             << "\"fps\": " << state->selected_profile.fps << ", "
             << "\"adaptiveFps\": " << (state->selected_profile.adaptive_fps ? "true" : "false") << ", "
             << "\"bitrate\": " << state->selected_profile.bitrate
             << "}";
    } else {
        json << "null";
    }
    json << "\n}\n";

    WriteUtf8File(state->status_file_path, json.str());
}

void UpdateStatusLabel(AppState* state) {
    if (state == nullptr || state->udp_receiver == nullptr) {
        return;
    }

    const VideoStats stats = state->udp_receiver->GetStats();
    const size_t decoder_queue_depth =
        state->decoder != nullptr ? state->decoder->GetPendingAccessUnitCount() : 0;
    bool has_clock_sync = false;
    int64_t sender_clock_rtt_us = 0;
    int64_t latest_latency_us = 0;
    int64_t average_latency_us = 0;
    int64_t min_latency_us = 0;
    int64_t max_latency_us = 0;
    uint64_t latency_sample_count = 0;
    uint64_t decoded_frame_count = 0;
    uint64_t displayed_frame_count = 0;
    uint64_t display_dropped_frames = 0;
    double content_fps = 0.0;
    double receive_fps = 0.0;
    double decode_fps = 0.0;
    double display_fps = 0.0;
    const int64_t now_us = NowSteadyUs();
    {
        std::lock_guard<std::mutex> lock(state->metrics_mutex);
        has_clock_sync = state->has_clock_sync;
        sender_clock_rtt_us = state->sender_clock_rtt_us;
        latest_latency_us = state->latest_latency_us;
        latency_sample_count = state->latency_sample_count;
        decoded_frame_count = state->decoded_frame_count;
        displayed_frame_count = state->displayed_frame_count;
        content_fps = state->content_fps;
        if (latency_sample_count > 0) {
            average_latency_us = static_cast<int64_t>(state->total_latency_us / latency_sample_count);
            min_latency_us = state->min_latency_us;
            max_latency_us = state->max_latency_us;
        }

        if (state->rate_window_start_us == 0 ||
            stats.completed_frames < state->rate_window_completed_frames ||
            decoded_frame_count < state->rate_window_decoded_frames ||
            displayed_frame_count < state->rate_window_displayed_frames) {
            state->rate_window_start_us = now_us;
            state->rate_window_completed_frames = stats.completed_frames;
            state->rate_window_decoded_frames = decoded_frame_count;
            state->rate_window_displayed_frames = displayed_frame_count;
        } else {
            const int64_t rate_elapsed_us = now_us - state->rate_window_start_us;
            if (rate_elapsed_us >= 500'000) {
                state->receive_fps =
                    static_cast<double>(stats.completed_frames - state->rate_window_completed_frames) *
                    1'000'000.0 / static_cast<double>(rate_elapsed_us);
                state->decode_fps =
                    static_cast<double>(decoded_frame_count - state->rate_window_decoded_frames) *
                    1'000'000.0 / static_cast<double>(rate_elapsed_us);
                state->display_fps =
                    static_cast<double>(displayed_frame_count - state->rate_window_displayed_frames) *
                    1'000'000.0 / static_cast<double>(rate_elapsed_us);
                state->rate_window_start_us = now_us;
                state->rate_window_completed_frames = stats.completed_frames;
                state->rate_window_decoded_frames = decoded_frame_count;
                state->rate_window_displayed_frames = displayed_frame_count;
            }
        }

        receive_fps = state->receive_fps;
        decode_fps = state->decode_fps;
        display_fps = state->display_fps;
    }

    if (decoded_frame_count > displayed_frame_count) {
        display_dropped_frames = decoded_frame_count - displayed_frame_count;
    }
    const double current_fps = content_fps > 0.0 ? content_fps : display_fps;

    if (state->has_selected_profile) {
        std::wostringstream subtitle_stream;
        if (state->auto_resumed_profile) {
            subtitle_stream << L"\u68C0\u6D4B\u5230\u624B\u673A\u7AEF\u4ECD\u5728\u6301\u7EED\u9001\u5E27\uFF0C\u5DF2\u4F7F\u7528\u4E0A\u6B21\u914D\u7F6E\u81EA\u52A8\u63A5\u7BA1\u89E3\u7801 "
                            << state->selected_profile.width
                            << L"x"
                            << state->selected_profile.height
                            << L" / "
                            << CodecName(state->selected_profile.codec)
                            << L" / "
                            << FormatProfileFrameRate(state->selected_profile);
        } else {
            subtitle_stream << L"\u53D1\u9001\u7AEF\u5DF2\u8FDE\u63A5\uFF0C\u5F53\u524D\u914D\u7F6E "
                            << state->selected_profile.width
                            << L"x"
                            << state->selected_profile.height
                            << L" / "
                            << CodecName(state->selected_profile.codec)
                            << L" / "
                            << FormatProfileFrameRate(state->selected_profile);
        }
        state->dashboard.subtitle = subtitle_stream.str();

        std::wostringstream config_value;
        config_value << state->selected_profile.width << L"x" << state->selected_profile.height;
        std::wostringstream config_note;
        config_note << CodecName(state->selected_profile.codec)
                    << L" / "
                    << FormatProfileFrameRate(state->selected_profile)
                    << L" / "
                    << FormatBitrateValue(state->selected_profile.bitrate)
                    << L" / "
                    << PresentationModeLabel(state->presentation_mode);
        state->dashboard.summary_cards[0] = {kTextCurrentConfig, config_value.str(), config_note.str()};
    } else {
        state->dashboard.subtitle = kTextDashboardWaiting;
        state->dashboard.summary_cards[0] = {kTextCurrentConfig, kTextWaitingConnect, kTextProfilePending};
    }

    std::wostringstream fps_note;
    fps_note << L"\u91CD\u7EC4 " << FormatOptionalFps(receive_fps)
             << L" / \u89E3\u7801 " << FormatOptionalFps(decode_fps)
             << L" / \u663E\u793A " << FormatOptionalFps(display_fps);
    state->dashboard.summary_cards[1] = {kTextRealtimeFps, FormatOptionalFps(content_fps), fps_note.str()};

    if (has_clock_sync && latency_sample_count > 0) {
        std::wostringstream latency_note;
        latency_note << L"\u5E73\u5747 " << FormatLatencyMs(average_latency_us) << L" / \u5F80\u8FD4 " << FormatLatencyMs(sender_clock_rtt_us);
        state->dashboard.summary_cards[2] = {kTextLatency, FormatLatencyMs(latest_latency_us), latency_note.str()};
    } else if (has_clock_sync) {
        state->dashboard.summary_cards[2] = {
            kTextLatency,
            L"\u7B49\u5F85\u6837\u672C",
            L"\u65F6\u949F\u5DF2\u540C\u6B65\uFF0C\u6B63\u5728\u79EF\u7D2F\u753B\u9762\u6837\u672C"};
    } else {
        state->dashboard.summary_cards[2] = {kTextLatency, kTextWaitingSync, L"\u7B49\u5F85\u624B\u673A\u65F6\u949F\u540C\u6B65"};
    }

    std::wostringstream queue_note;
    queue_note << L"\u5F85\u89E3 " << decoder_queue_depth
               << L" / \u5DF2\u6536 " << stats.completed_frames
               << L" / \u5DF2\u89E3 " << decoded_frame_count
               << L" / \u5DF2\u663E " << displayed_frame_count;
    state->dashboard.summary_cards[3] = {kTextDecoderQueue, std::to_wstring(display_dropped_frames), queue_note.str()};

    std::wostringstream traffic_stream;
    traffic_stream << L"\u63A7\u5236\u901A\u9053\uFF1ATCP/" << kControlPort
                   << L"\r\n\u89C6\u9891\u901A\u9053\uFF1AUDP/" << kVideoPort
                   << L"\r\n\u6570\u636E\u5305\uFF1A" << stats.total_packets << L"    \u5B57\u8282\uFF1A" << FormatDataSize(stats.total_bytes)
                   << L"\r\n\u5B8C\u6574\u5E27\uFF1A" << stats.completed_frames << L"    \u5173\u952E\u5E27\uFF1A" << stats.keyframes
                   << L"\r\n\u4E22\u5E27\uFF1A" << stats.dropped_frames << L"    \u6700\u540E\u5E27 ID\uFF1A" << stats.last_frame_id;
    state->dashboard.traffic_body = traffic_stream.str();

    const bool video_visible = state->video_window != nullptr && IsWindowVisible(state->video_window);

    std::wostringstream runtime_stream;
    runtime_stream << L"\u7F51\u7EDC\u91CD\u7EC4\u901F\u7387\uFF1A" << FormatOptionalFps(receive_fps)
                   << L"\r\n\u89E3\u7801\u8F93\u51FA\u901F\u7387\uFF1A" << FormatOptionalFps(decode_fps)
                   << L"\r\n\u6700\u7EC8\u663E\u793A\u901F\u7387\uFF1A" << FormatOptionalFps(display_fps)
                   << L"\r\n\u663E\u793A\u6A21\u5F0F\uFF1A" << PresentationModeLabel(state->presentation_mode)
                   << L"\r\n\u6E32\u67D3\u663E\u5361\uFF1A"
                   << (state->renderer.gpu_name().empty() ? L"\u672A\u8BC6\u522B" : ShortenMiddle(state->renderer.gpu_name(), 38))
                   << L"\r\nNVIDIA NVDEC\uFF1A" << BuildNvdecStatusText(state)
                   << L"\r\n\u753B\u9762\u7A97\u53E3\uFF1A"
                   << (video_visible
                        ? L"\u5DF2\u5C31\u7EEA"
                        : (state->suppress_video_window_auto_show
                            ? L"\u5DF2\u624B\u52A8\u5173\u95ED\uFF0C\u53EF\u70B9\u201C\u805A\u7126\u753B\u9762\u201D\u91CD\u65B0\u6253\u5F00"
                            : L"\u5F85\u6295\u5C4F\u540E\u5F39\u51FA"));
    if (has_clock_sync) {
        runtime_stream << L"\r\n\u65F6\u949F\u540C\u6B65\uFF1A\u5DF2\u540C\u6B65\uFF0C\u5F80\u8FD4 " << FormatLatencyMs(sender_clock_rtt_us);
    } else {
        runtime_stream << L"\r\n\u65F6\u949F\u540C\u6B65\uFF1A\u7B49\u5F85\u624B\u673A\u6821\u65F6";
    }
    state->dashboard.runtime_body = runtime_stream.str();

    WriteStatusSnapshot(
        state,
        stats,
        decoder_queue_depth,
        has_clock_sync,
        sender_clock_rtt_us,
        latest_latency_us,
        average_latency_us,
        min_latency_us,
        max_latency_us,
        latency_sample_count,
        decoded_frame_count,
        displayed_frame_count,
        display_dropped_frames,
        content_fps,
        receive_fps,
        decode_fps,
        display_fps);

    if (state->status_window != nullptr) {
        std::wstring status_title = std::wstring(kStatusWindowTitle) + L" v" + kAppBuildLabel;
        if (state->has_selected_profile) {
            std::wostringstream title_suffix;
            title_suffix << L" - "
                         << state->selected_profile.width
                         << L"x"
                         << state->selected_profile.height
                         << L"@"
                         << FormatProfileFrameRate(state->selected_profile);
            status_title += title_suffix.str();
        }
        SetWindowTextW(state->status_window, status_title.c_str());
        InvalidateRect(state->status_window, nullptr, FALSE);
    }

    if (state->video_window != nullptr) {
        std::wostringstream title_stream;
        title_stream << kVideoWindowTitle << L" v" << kAppBuildLabel;
        if (state->has_selected_profile) {
            title_stream << L" - "
                         << state->selected_profile.width
                         << L"x"
                         << state->selected_profile.height
                         << L"@"
                         << FormatProfileFrameRate(state->selected_profile);
        }
        if (current_fps > 0.0) {
            title_stream << L" - \u5F53\u524D\u753B\u9762\u5E27\u7387 " << FormatFpsValue(current_fps);
        } else {
            title_stream << L" - \u5F53\u524D\u753B\u9762\u5E27\u7387 \u6D4B\u91CF\u4E2D";
        }
        SetWindowTextW(state->video_window, title_stream.str().c_str());
    }
}

void LayoutStatusWindow(AppState* state) {
    if (state == nullptr || state->status_window == nullptr) {
        return;
    }

    const StatusWindowLayout layout = CalculateStatusWindowLayout(state);
    if (state->focus_video_button != nullptr) {
        MoveWindow(
            state->focus_video_button,
            layout.focus_button.left,
            layout.focus_button.top,
            RectWidth(layout.focus_button),
            RectHeight(layout.focus_button),
            TRUE);
    }
    if (state->presentation_mode_button != nullptr) {
        MoveWindow(
            state->presentation_mode_button,
            layout.mode_button.left,
            layout.mode_button.top,
            RectWidth(layout.mode_button),
            RectHeight(layout.mode_button),
            TRUE);
    }
    if (state->open_log_button != nullptr) {
        MoveWindow(
            state->open_log_button,
            layout.open_log_button.left,
            layout.open_log_button.top,
            RectWidth(layout.open_log_button),
            RectHeight(layout.open_log_button),
            TRUE);
    }
    if (state->log_view != nullptr) {
        MoveWindow(
            state->log_view,
            layout.log_view.left,
            layout.log_view.top,
            RectWidth(layout.log_view),
            RectHeight(layout.log_view),
            TRUE);
    }
    InvalidateRect(state->status_window, nullptr, TRUE);
}

void LayoutVideoWindow(AppState* state) {
    if (state == nullptr || state->video_window == nullptr) {
        return;
    }

    RECT client_rect{};
    GetClientRect(state->video_window, &client_rect);
    const int width = std::max(1L, client_rect.right - client_rect.left);
    const int height = std::max(1L, client_rect.bottom - client_rect.top);
    state->renderer.Resize(0, 0, width, height);
}

void BeginShutdown(AppState* state, HWND origin) {
    if (state == nullptr || state->shutting_down) {
        return;
    }

    state->shutting_down = true;

    if (state->status_window != nullptr && state->status_window != origin && IsWindow(state->status_window)) {
        DestroyWindow(state->status_window);
    }
    if (state->video_window != nullptr && state->video_window != origin && IsWindow(state->video_window)) {
        DestroyWindow(state->video_window);
    }
    if (origin != nullptr && IsWindow(origin)) {
        DestroyWindow(origin);
    }
}

LRESULT CALLBACK VideoWindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_NCCREATE) {
        auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
        auto* state = static_cast<AppState*>(create_struct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        if (state != nullptr) {
            state->video_window = hwnd;
        }
    }

    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE: {
        auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
        if (state == nullptr) {
            return -1;
        }
        if (!state->renderer.Create(hwnd, create_struct->hInstance)) {
            return -1;
        }
        LayoutVideoWindow(state);
        return 0;
    }
    case WM_SIZE:
        LayoutVideoWindow(state);
        return 0;
    case WM_SHOWWINDOW:
        if (state != nullptr) {
            state->video_window_visible = w_param != FALSE;
        }
        return 0;
    case WM_CLOSE:
        if (state != nullptr && !state->shutting_down) {
            state->suppress_video_window_auto_show = true;
            ShowWindow(hwnd, SW_HIDE);
            state->video_window_visible = false;
            UpdateStatusLabel(state);
            return 0;
        }
        BeginShutdown(state, hwnd);
        return 0;
    case WM_DESTROY:
        if (state != nullptr) {
            state->video_window = nullptr;
            state->video_window_visible = false;
        }
        return 0;
    default:
        return DefWindowProcW(hwnd, message, w_param, l_param);
    }
}

LRESULT CALLBACK StatusWindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE: {
        auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
        auto* new_state = new AppState();
        new_state->status_window = hwnd;
        new_state->log_file_path = BuildLogFilePath();
        new_state->status_file_path = BuildStatusFilePath();
        new_state->profile_cache_path = BuildProfileCachePath();
        new_state->codec_config_cache_path = BuildCodecConfigCachePath();
        ResetLogFile(new_state->log_file_path);
        ResetDashboardSnapshot(new_state);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new_state));

        protocol::StreamProfile cached_profile;
        if (LoadCachedSelectedProfile(new_state->profile_cache_path, &cached_profile) ||
            LoadCachedSelectedProfile(new_state->status_file_path, &cached_profile)) {
            new_state->cached_startup_profile = cached_profile;
            new_state->has_cached_startup_profile = true;
        }
        new_state->cached_codec_config = ReadBinaryFile(new_state->codec_config_cache_path);
        new_state->has_cached_codec_config = !new_state->cached_codec_config.empty();

        new_state->window_background_brush = CreateSolidBrush(kColorWindowBackground);
        new_state->log_background_brush = CreateSolidBrush(kColorLogBackground);
        ApplyWindowBackdrop(hwnd);
        RecreateUiFonts(new_state);

        new_state->focus_video_button = CreateWindowExW(
            0,
            L"BUTTON",
            kTextFocusVideo,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0,
            0,
            100,
            40,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFocusVideoButtonId)),
            create_struct->hInstance,
            nullptr);
        new_state->presentation_mode_button = CreateWindowExW(
            0,
            L"BUTTON",
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0,
            0,
            100,
            40,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPresentationModeButtonId)),
            create_struct->hInstance,
            nullptr);
        new_state->open_log_button = CreateWindowExW(
            0,
            L"BUTTON",
            kTextOpenLog,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0,
            0,
            100,
            40,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOpenLogButtonId)),
            create_struct->hInstance,
            nullptr);
        new_state->log_view = CreateWindowExW(
            0,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            0,
            0,
            100,
            100,
            hwnd,
            nullptr,
            create_struct->hInstance,
            nullptr);
        ApplyUiToControls(new_state);
        UpdatePresentationModeButton(new_state);
        new_state->nvdec_probe = ProbeNvidiaCuvidSupport();

        new_state->renderer.SetLogFn(
            [new_state](const std::wstring& line) { QueueLog(new_state, line); });
        new_state->renderer.SetPresentationMode(new_state->presentation_mode);
        new_state->renderer.SetPresentFn(
            [new_state](uint64_t sender_pts_us) {
                if (new_state->status_window != nullptr &&
                    new_state->video_window != nullptr &&
                    !new_state->suppress_video_window_auto_show &&
                    !IsWindowVisible(new_state->video_window)) {
                    PostMessageW(new_state->status_window, kShowVideoWindowMessage, 0, 0);
                }
                UpdateLatencyEstimate(new_state, sender_pts_us);
            });

        new_state->video_window = CreateWindowExW(
            0,
            kVideoWindowClassName,
            kVideoWindowTitle,
            WS_OVERLAPPEDWINDOW,
            760,
            40,
            kVideoWindowWidth,
            kVideoWindowHeight,
            nullptr,
            nullptr,
            create_struct->hInstance,
            new_state);
        if (new_state->video_window == nullptr) {
            DeleteBrushHandle(new_state->window_background_brush);
            DeleteBrushHandle(new_state->log_background_brush);
            DeleteFontHandle(new_state->title_font);
            DeleteFontHandle(new_state->subtitle_font);
            DeleteFontHandle(new_state->section_font);
            DeleteFontHandle(new_state->value_font);
            DeleteFontHandle(new_state->body_font);
            DeleteFontHandle(new_state->button_font);
            delete new_state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return -1;
        }

        new_state->decoder = std::make_unique<VideoDecoder>(
            [new_state](const std::wstring& line) { QueueLog(new_state, line); },
            [new_state](DecodedFrame frame) {
                {
                    std::lock_guard<std::mutex> lock(new_state->metrics_mutex);
                    new_state->decoded_frame_count += 1;
                }
                new_state->renderer.Present(std::move(frame));
            },
            [new_state]() {
                if (new_state->control_server != nullptr) {
                    new_state->control_server->RequestIdr();
                }
            });
        new_state->decoder->SetD3DDevice(new_state->renderer.d3d_device());
        new_state->decoder->SetSmoothMode(false);
        new_state->decoder->Start();

        new_state->control_server = std::make_unique<ControlServer>(
            [new_state](const std::wstring& line) { QueueLog(new_state, line); },
            [new_state](const protocol::StreamProfile& profile) {
                new_state->cached_startup_profile = profile;
                new_state->has_cached_startup_profile = true;
                new_state->auto_resume_notice_logged = false;
                WriteUtf8File(new_state->profile_cache_path, BuildProfileCacheJson(profile));
                ApplyStreamProfile(new_state, profile, false);
            },
            [new_state](int64_t offset_us, int64_t rtt_us) {
                UpdateClockSync(new_state, offset_us, rtt_us);
            });

        new_state->udp_receiver = std::make_unique<UdpVideoReceiver>(
            [new_state](const std::wstring& line) { QueueLog(new_state, line); },
            [new_state](const AccessUnit& unit) {
                const bool is_codec_config = (unit.flags & protocol::kFlagCodecConfig) != 0;
                if (is_codec_config && !unit.bytes.empty()) {
                    new_state->cached_codec_config = unit.bytes;
                    new_state->has_cached_codec_config = true;
                    WriteBinaryFile(new_state->codec_config_cache_path, new_state->cached_codec_config);
                }

                if (!new_state->has_selected_profile &&
                    new_state->has_cached_startup_profile &&
                    (new_state->has_cached_codec_config || is_codec_config)) {
                    ApplyStreamProfile(new_state, new_state->cached_startup_profile, true);
                    if (!is_codec_config) {
                        SubmitCachedCodecConfig(new_state, unit.frame_id);
                    }
                    if (!new_state->auto_resume_notice_logged) {
                        QueueLog(
                            new_state,
                            L"\u81EA\u52A8\u63A5\u7BA1: \u68C0\u6D4B\u5230\u624B\u673A\u7AEF\u5DF2\u5728\u6301\u7EED\u9001\u5E27\uFF0C\u5DF2\u4F7F\u7528\u4E0A\u6B21\u914D\u7F6E\u4E0E\u7F13\u5B58\u7684\u7801\u6D41\u53C2\u6570\u76F4\u63A5\u6062\u590D\u89E3\u7801\u3002");
                        new_state->auto_resume_notice_logged = true;
                    }
                }
                if (new_state->decoder != nullptr) {
                    new_state->decoder->SubmitAccessUnit(unit);
                }
            });

        new_state->control_server->Start(kControlPort, kVideoPort);
        new_state->udp_receiver->Start(kVideoPort);
        SetTimer(hwnd, kStatsTimerId, kStatsIntervalMs, nullptr);
        LayoutStatusWindow(new_state);
        UpdateStatusLabel(new_state);
        if (new_state->has_cached_startup_profile) {
            QueueLog(
                new_state,
                std::wstring(L"\u542F\u52A8\u6062\u590D: \u5DF2\u8BFB\u53D6\u4E0A\u6B21\u914D\u7F6E\u7F13\u5B58 ")
                    + FormatProfileSummaryForLog(new_state->cached_startup_profile)
                    + (new_state->has_cached_codec_config
                        ? L"\uFF0C\u82E5\u624B\u673A\u7AEF\u4ECD\u5728\u6301\u7EED\u9001\u5E27\u5C06\u81EA\u52A8\u63A5\u7BA1\u89E3\u7801\u3002"
                        : L"\uFF0C\u4F46\u672C\u5730\u6682\u65E0\u53EF\u7528\u7684\u7801\u6D41\u53C2\u6570\u7F13\u5B58\u3002"));
        }
        QueueLog(new_state, new_state->nvdec_probe.summary);
        QueueLog(new_state, std::wstring(L"接收端版本：") + kAppBuildLabel);
        QueueLog(new_state, L"\u72B6\u6001\u7A97\u53E3\u5DF2\u5C31\u7EEA\u3002");
        QueueLog(new_state, L"\u753B\u9762\u7A97\u53E3\u5DF2\u521B\u5EFA\uFF0C\u6536\u5230\u753B\u9762\u540E\u518D\u663E\u793A\u3002");
        QueueLog(new_state, std::wstring(L"\u65E5\u5FD7\u6587\u4EF6\uFF1A") + new_state->log_file_path);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* minmax = reinterpret_cast<MINMAXINFO*>(l_param);
        minmax->ptMinTrackSize.x = ScaleByDpi(hwnd, kStatusWindowMinWidth);
        minmax->ptMinTrackSize.y = ScaleByDpi(hwnd, kStatusWindowMinHeight);
        return 0;
    }
    case WM_DPICHANGED:
        if (state != nullptr) {
            const RECT* suggested_rect = reinterpret_cast<const RECT*>(l_param);
            if (suggested_rect != nullptr) {
                SetWindowPos(
                    hwnd,
                    nullptr,
                    suggested_rect->left,
                    suggested_rect->top,
                    suggested_rect->right - suggested_rect->left,
                    suggested_rect->bottom - suggested_rect->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            RecreateUiFonts(state);
            ApplyUiToControls(state);
            LayoutStatusWindow(state);
        }
        return 0;
    case WM_SIZE:
        LayoutStatusWindow(state);
        return 0;
    case WM_TIMER:
        if (w_param == kStatsTimerId) {
            if (state != nullptr && state->control_server != nullptr) {
                const int64_t now_us = NowSteadyUs();
                bool should_sync = false;
                {
                    std::lock_guard<std::mutex> lock(state->metrics_mutex);
                    if (now_us - state->last_sync_request_us >= kTimeSyncIntervalUs) {
                        state->last_sync_request_us = now_us;
                        should_sync = true;
                    }
                }
                if (should_sync) {
                    state->control_server->RequestTimeSync();
                }
            }
            UpdateStatusLabel(state);
        }
        return 0;
    case kStatsMessage:
        UpdateStatusLabel(state);
        return 0;
    case kShowVideoWindowMessage:
        if (state != nullptr && state->video_window != nullptr && !IsWindowVisible(state->video_window)) {
            ShowWindow(state->video_window, SW_SHOWNOACTIVATE);
            UpdateWindow(state->video_window);
            state->video_window_visible = true;
            UpdateStatusLabel(state);
        }
        return 0;
    case kLogMessage:
        if (state != nullptr && state->log_view != nullptr) {
            std::deque<std::wstring> logs;
            {
                std::lock_guard<std::mutex> lock(state->log_mutex);
                logs.swap(state->pending_logs);
            }
            for (const auto& line : logs) {
                AppendLogText(state->log_view, line);
            }
        }
        return 0;
    case WM_COMMAND:
        if (HIWORD(w_param) == BN_CLICKED) {
            switch (LOWORD(w_param)) {
            case kFocusVideoButtonId:
                FocusVideoWindow(state);
                return 0;
            case kPresentationModeButtonId:
                if (state != nullptr) {
                    ApplyPresentationMode(state, VideoRenderer::PresentationMode::kLowLatency, false);
                }
                return 0;
            case kOpenLogButtonId:
                OpenLogFile(state);
                return 0;
            default:
                break;
            }
        }
        break;
    case WM_DRAWITEM: {
        auto* draw_item = reinterpret_cast<DRAWITEMSTRUCT*>(l_param);
        if (draw_item != nullptr && draw_item->CtlType == ODT_BUTTON) {
            DrawActionButton(state, draw_item);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        if (state != nullptr && reinterpret_cast<HWND>(l_param) == state->log_view) {
            auto* dc = reinterpret_cast<HDC>(w_param);
            SetTextColor(dc, kColorTextPrimary);
            SetBkColor(dc, kColorLogBackground);
            return reinterpret_cast<INT_PTR>(state->log_background_brush);
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint_struct{};
        const HDC dc = BeginPaint(hwnd, &paint_struct);
        PaintStatusWindow(state, hwnd, dc);
        EndPaint(hwnd, &paint_struct);
        return 0;
    }
    case WM_CLOSE:
        BeginShutdown(state, hwnd);
        return 0;
    case WM_DESTROY:
        if (state != nullptr) {
            KillTimer(hwnd, kStatsTimerId);
            state->control_server.reset();
            state->udp_receiver.reset();
            state->decoder.reset();
            DeleteFontHandle(state->title_font);
            DeleteFontHandle(state->subtitle_font);
            DeleteFontHandle(state->section_font);
            DeleteFontHandle(state->value_font);
            DeleteFontHandle(state->body_font);
            DeleteFontHandle(state->button_font);
            DeleteBrushHandle(state->window_background_brush);
            DeleteBrushHandle(state->log_background_brush);
            delete state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, w_param, l_param);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show_command) {
    const std::wstring command_line_text = command_line != nullptr ? command_line : L"";
    g_frontend_mode = command_line_text.find(L"--frontend-mode") != std::wstring::npos;

    HANDLE instance_mutex = CreateMutexW(nullptr, TRUE, kInstanceMutexName);
    if (instance_mutex == nullptr) {
        MessageBoxW(nullptr, L"\u521B\u5EFA\u5355\u4F8B\u4E92\u65A5\u9501\u5931\u8D25\u3002", kStatusWindowTitle, MB_ICONERROR | MB_OK);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"粥6y直播投屏助手已经在运行。", kStatusWindowTitle, MB_ICONINFORMATION | MB_OK);
        CloseHandle(instance_mutex);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    WSADATA wsadata{};
    if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
        CloseHandle(instance_mutex);
        CoUninitialize();
        MessageBoxW(nullptr, L"\u7F51\u7EDC\u7EC4\u4EF6\u521D\u59CB\u5316\u5931\u8D25\u3002", kStatusWindowTitle, MB_ICONERROR | MB_OK);
        return 1;
    }

    if (FAILED(MFStartup(MF_VERSION))) {
        CloseHandle(instance_mutex);
        WSACleanup();
        CoUninitialize();
        MessageBoxW(nullptr, L"\u5A92\u4F53\u57FA\u7840\u7EC4\u4EF6\u521D\u59CB\u5316\u5931\u8D25\u3002", kStatusWindowTitle, MB_ICONERROR | MB_OK);
        return 1;
    }

    WNDCLASSEXW status_window_class{};
    status_window_class.cbSize = sizeof(status_window_class);
    status_window_class.style = CS_HREDRAW | CS_VREDRAW;
    status_window_class.lpfnWndProc = StatusWindowProc;
    status_window_class.hInstance = instance;
    status_window_class.hIcon = LoadAppIconHandle(instance, GetSystemMetrics(SM_CXICON));
    status_window_class.hIconSm = LoadAppIconHandle(instance, GetSystemMetrics(SM_CXSMICON));
    status_window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    status_window_class.hbrBackground = nullptr;
    status_window_class.lpszClassName = kStatusWindowClassName;
    RegisterClassExW(&status_window_class);

    WNDCLASSEXW video_window_class{};
    video_window_class.cbSize = sizeof(video_window_class);
    video_window_class.lpfnWndProc = VideoWindowProc;
    video_window_class.hInstance = instance;
    video_window_class.hIcon = LoadAppIconHandle(instance, GetSystemMetrics(SM_CXICON));
    video_window_class.hIconSm = LoadAppIconHandle(instance, GetSystemMetrics(SM_CXSMICON));
    video_window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    video_window_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    video_window_class.lpszClassName = kVideoWindowClassName;
    RegisterClassExW(&video_window_class);

    HWND status_window = CreateWindowExW(
        0,
        kStatusWindowClassName,
        kStatusWindowTitle,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | (g_frontend_mode ? 0 : WS_VISIBLE),
        40,
        40,
        kStatusWindowWidth,
        kStatusWindowHeight,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (status_window == nullptr) {
        CloseHandle(instance_mutex);
        MFShutdown();
        WSACleanup();
        CoUninitialize();
        MessageBoxW(nullptr, L"\u521B\u5EFA\u63A5\u6536\u7AEF\u4E3B\u7A97\u53E3\u5931\u8D25\u3002", kStatusWindowTitle, MB_ICONERROR | MB_OK);
        return 1;
    }

    ShowWindow(status_window, g_frontend_mode ? SW_HIDE : show_command);
    if (!g_frontend_mode) {
        UpdateWindow(status_window);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    MFShutdown();
    WSACleanup();
    CoUninitialize();
    ReleaseMutex(instance_mutex);
    CloseHandle(instance_mutex);
    return static_cast<int>(message.wParam);
}
