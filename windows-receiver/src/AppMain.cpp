// ============================================================================
// 头文件包含区域
// ============================================================================

// 项目内部头文件 - 包含各个功能模块的控制器和接收器
#include "AirPlayReceiverController.h"      // AirPlay 接收器控制器 (苹果投屏)
#include "AudioPlayer.h"                     // 音频播放器 - 处理 PCM 音频输出
#include "ControlServer.h"                   // 控制服务器 - TCP 控制通道
#include "DanmakuController.h"
#include "VoiceIntentResolver.h"
#include "EmbeddedWebUiBridge.h"             // 嵌入式 Web UI 桥接
#include "LocalMusicPlayer.h"                // 本地音乐播放器 - 语音点歌功能
#include "NvidiaCuvidProbe.h"                // NVIDIA CUDA/CUVID 探测 - 硬件解码检测
#include "UdpAudioReceiver.h"                // UDP 音频接收器 - 接收手机端音频流
#include "UdpVideoReceiver.h"                // UDP 视频接收器 - 接收手机端视频流
#include "VideoDecoder.h"                    // 视频解码器 - H.264/HEVC 解码
#include "VideoRenderer.h"                   // 视频渲染器 - Direct3D 11 渲染
#include "VoiceCommandController.h"          // 语音命令控制器 - 离线语音识别
#include "VoiceMusicLibrary.h"               // 语音音乐库 - 语音点歌曲库管理
#include "VirtualCameraController.h"         // 虚拟摄像头控制器 - OBS 等软件使用
#include "VirtualCameraFrameBridge.h"        // 虚拟摄像头帧桥接 - GPU 纹理拷贝
#include "VirtualCameraShared.h"             // 虚拟摄像头共享定义 - 常量配置

// Windows 系统 API 头文件
#include <Windows.h>                         // Windows 核心 API
#include <windowsx.h>                        // Windows 控件宏
#include <WinSock2.h>                        // Winsock 网络库
#include <Shellapi.h>                        // Shell API - 文件操作、打开文件等
#include <mfapi.h>                           // Media Foundation API - 多媒体框架
#include <UIAutomationClient.h>              // UI Automation - 自动回复消息输入与发送
#include <winrt/base.h>                      // WinRT COM 智能指针
#include <winrt/Windows.Foundation.h>        // WinRT 基础组件
#include <winrt/Windows.Foundation.Collections.h>  // WinRT 集合
#include <winrt/Windows.Media.Control.h>   // WinRT 媒体控制 - 系统媒体会话控制

// C++ 标准库头文件
#include <algorithm>                         // 算法库 - std::clamp, std::min, std::max 等
#include <atomic>                            // 原子操作 - 线程安全的原子变量
#include <array>                             // 数组容器 - 固定大小数组
#include <chrono>                            // 时间库 - 高精度时间戳
#include <cmath>                             // 数学库 - 数学函数
#include <cstdio>                            // C 标准输入输出 - printf, fopen 等
#include <cstdint>                           // 固定宽度整数类型 - uint8_t, int64_t 等
#include <cwctype>
#include <deque>                             // 双端队列 - 日志队列
#include <filesystem>                        // 文件系统 - 路径操作、文件拷贝
#include <fstream>                           // 文件流 - 文件读写
#include <iomanip>                           // IO 流格式化 - std::setw, std::setfill
#include <memory>                            // 智能指针 - std::unique_ptr
#include <mutex>                             // 互斥锁 - 线程同步
#include <regex>                             // 正则表达式 - JSON 解析
#include <sstream>                           // 字符串流 - 字符串格式化
#include <string>                            // 字符串 - std::wstring
#include <unordered_set>
#include <condition_variable>
#include <random>
#include <utility>                           // 工具库 - std::move 等

// ============================================================================
// 匿名命名空间 - 内部常量和数据结构定义
// ============================================================================
namespace {

// ============================================================================
// 窗口类名和标题定义
// ============================================================================

// 状态窗口类名 - 主窗口 (控制面板)
constexpr wchar_t kStatusWindowClassName[] = L"AndroidCastReceiverStatusWindow";
// 视频窗口类名 - 投屏画面窗口
constexpr wchar_t kVideoWindowClassName[] = L"AndroidCastReceiverVideoWindow";
// 状态窗口标题 - 显示"直播投屏助手"
constexpr wchar_t kStatusWindowTitle[] = L"\u76F4\u64AD\u6295\u5C4F\u52A9\u624B";
// 视频窗口标题 - 显示"直播投屏助手 - 投屏画面"
constexpr wchar_t kVideoWindowTitle[] = L"\u76F4\u64AD\u6295\u5C4F\u52A9\u624B - \u6295\u5C4F\u753B\u9762";

// ============================================================================
// 应用程序唯一性和版本标识
// ============================================================================

// 实例互斥体名称 - 防止程序多开 (周 6Y LiveCastAssistant 单例)
constexpr wchar_t kInstanceMutexName[] = L"Zhou6YLiveCastAssistantSingleton";
// 应用构建标签 - 版本号 + 功能标识 + 日期
constexpr wchar_t kAppBuildLabel[] = L"0.4.10-2026-04-26";
constexpr wchar_t kVoiceRuntimePackageFolder[] = L"sherpa-onnx-v1.12.36-win-x64-shared-MD-Release-no-tts";
constexpr wchar_t kVoiceModelFolderName[] = L"sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30";
// AirPlay 服务器名称 - 显示给 iPhone/iPad 的设备名
constexpr wchar_t kAirPlayServerName[] = L"\u76F4\u64AD\u6295\u5C4F\u52A9\u624B";

// ============================================================================
// UI 文本常量 - 状态显示文字
// ============================================================================

// 测量中 - 表示正在等待数据
constexpr wchar_t kTextMeasurePending[] = L"\u6D4B\u91CF\u4E2D";
// 等待安卓发送端建立控制连接和视频流
constexpr wchar_t kTextDashboardWaiting[] = L"\u7B49\u5F85\u5B89\u5353\u53D1\u9001\u7AEF\u5EFA\u7ACB\u63A7\u5236\u8FDE\u63A5\u548C\u89C6\u9891\u6D41";
// 当前配置 - 显示当前使用的流配置
constexpr wchar_t kTextCurrentConfig[] = L"\u5F53\u524D\u914D\u7F6E";
// 等待连接 - 尚未建立连接
constexpr wchar_t kTextWaitingConnect[] = L"\u7B49\u5F85\u8FDE\u63A5";
// 发送端尚未协商分辨率
constexpr wchar_t kTextProfilePending[] = L"\u53D1\u9001\u7AEF\u5C1A\u672A\u534F\u5546\u5206\u8FA8\u7387";
// 内容源帧率 - 显示视频源帧率
constexpr wchar_t kTextRealtimeFps[] = L"\u5185\u5BB9\u6E90\u5E27\u7387";
// 等待首帧呈现
constexpr wchar_t kTextFirstFramePending[] = L"\u7B49\u5F85\u9996\u5E27\u5448\u73B0";
// 估算端到端延迟
constexpr wchar_t kTextLatency[] = L"\u4F30\u7B97\u7AEF\u5230\u7AEF\u5EF6\u8FDF";
// 等待同步 - 等待时钟同步
constexpr wchar_t kTextWaitingSync[] = L"\u7B49\u5F85\u540C\u6B65";
// 需要先完成时钟同步
constexpr wchar_t kTextNeedSync[] = L"\u9700\u8981\u5148\u5B8C\u6210\u65F6\u949F\u540C\u6B65";
// 显示丢帧 - 解码器队列深度
constexpr wchar_t kTextDecoderQueue[] = L"\u663E\u793A\u4E22\u5E27";
// 等待访问单元 - 等待解码数据
constexpr wchar_t kTextWaitingAccessUnit[] = L"\u7B49\u5F85\u8BBF\u95EE\u5355\u5143";
constexpr wchar_t kTextTrafficInitial[] =
    L"\u63A7\u5236\u901A\u9053\uFF1ATCP/5500\r\n"
    L"\u89C6\u9891\u901A\u9053\uFF1ATCP/55000\r\n"
    L"\u97F3\u9891\u901A\u9053\uFF1AUDP/55001\r\n"
    L"\u6570\u636E\u5305\uFF1A0    \u5B57\u8282\uFF1A0 B\r\n"
    L"\u5B8C\u6574\u5E27\uFF1A0    \u5173\u952E\u5E27\uFF1A0\r\n"
    L"\u4E22\u5E27\uFF1A0    \u6700\u540E\u5E27 ID\uFF1A0";
constexpr wchar_t kTextRuntimeInitial[] =
    L"\u7F51\u7EDC\u91CD\u7EC4\u901F\u7387\uFF1A\u6D4B\u91CF\u4E2D\r\n"
    L"\u89E3\u7801\u8F93\u51FA\u901F\u7387\uFF1A\u6D4B\u91CF\u4E2D\r\n"
    L"\u6700\u7EC8\u663E\u793A\u901F\u7387\uFF1A\u6D4B\u91CF\u4E2D\r\n"
    L"\u97F3\u9891\u72B6\u6001\uFF1A\u7B49\u5F85\u534F\u5546\r\n"
    L"\u82F9\u679C\u6295\u5C4F\uFF1A\u7B49\u5F85\u542F\u52A8\r\n"
    L"\u8BED\u97F3\u63A7\u5236\uFF1A\u672A\u5F00\u542F\r\n"
    L"\u6E32\u67D3\u663E\u5361\uFF1A\u7B49\u5F85\u521D\u59CB\u5316\r\n"
    L"\u753B\u9762\u7A97\u53E3\uFF1A\u6536\u5230\u9996\u5E27\u540E\u5F39\u51FA\r\n"
    L"\u65F6\u949F\u540C\u6B65\uFF1A\u7B49\u5F85\u624B\u673A\u6821\u65F6";
constexpr wchar_t kTextFocusVideo[] = L"\u805A\u7126\u753B\u9762";
constexpr wchar_t kTextOpenLog[] = L"\u6253\u5F00\u65E5\u5FD7";
constexpr wchar_t kTextSwitchToLowLatency[] = L"\u5207\u6362\u5230\u4f4e\u5ef6\u8fdf";
constexpr wchar_t kTextSwitchToSmooth[] = L"\u5207\u6362\u5230\u987a\u6ed1";
constexpr wchar_t kTextFixedLowLatency[] = L"\u56fa\u5b9a\u7ade\u6280\u4f4e\u5ef6\u8fdf";
constexpr wchar_t kTextControlPill[] = L"\u63A7\u5236 TCP/5500";
constexpr wchar_t kTextVideoPill[] = L"\u89C6\u9891 TCP/55000";
constexpr wchar_t kTextTrafficTitle[] = L"\u94FE\u8DEF\u7EDF\u8BA1";
constexpr wchar_t kTextRuntimeTitle[] = L"\u8FD0\u884C\u72B6\u6001";
constexpr wchar_t kTextLogTitle[] = L"\u8FD0\u884C\u65E5\u5FD7";
constexpr wchar_t kTextFilePrefix[] = L"\u6587\u4EF6\uFF1A";
constexpr wchar_t kTextVersionPrefix[] = L"\u7248\u672C ";
constexpr wchar_t kTextAppDisplayTitle[] = L"\u76F4\u64AD\u6295\u5C4F\u52A9\u624B";
constexpr wchar_t kTextVideoDisplayTitle[] = L"\u76F4\u64AD\u6295\u5C4F\u52A9\u624B - \u6295\u5C4F\u753B\u9762";
constexpr wchar_t kTextOverviewPage[] = L"\u603B\u89C8";
constexpr wchar_t kTextConnectPage[] = L"\u8BBE\u5907\u8FDE\u63A5";
constexpr wchar_t kTextDisplayPage[] = L"\u753B\u9762\u663E\u793A";
constexpr wchar_t kTextDiagnosticsPage[] = L"\u8BCA\u65AD\u6062\u590D";
constexpr wchar_t kTextVoicePage[] = L"\u8BED\u97F3\u63A7\u5236";
constexpr wchar_t kTextLogsPage[] = L"\u65E5\u5FD7\u5BFC\u51FA";
constexpr wchar_t kTextFeatureDescription[] = L"\u529F\u80FD\u8BF4\u660E\uFF1A\u540C\u4E00\u4E2A PC \u7A0B\u5E8F\u91CC\u5B8C\u6210\u63A5\u6536\u3001\u663E\u793A\u3001\u8BED\u97F3\u548C\u6392\u969C";
constexpr wchar_t kTextFeatureGroups[] = L"\u529F\u80FD\u5206\u7EC4\uFF1A\u8FDE\u63A5 / \u663E\u793A / \u8BED\u97F3 / \u8BCA\u65AD / \u65E5\u5FD7";
constexpr wchar_t kTextAuthor[] = L"\u4F5C\u8005\uFF1A\u7CA56y";
constexpr wchar_t kTextVoiceControlDisabled[] = L"\u672A\u5F00\u542F";
constexpr wchar_t kTextVoiceControlListening[] = L"\u79BB\u7EBF\u76D1\u542C\u4E2D\uFF1A\u53EF\u63A7\u5236\u5A92\u4F53\u4E5F\u53EF\u8BED\u97F3\u70B9\u6B4C";
constexpr wchar_t kTextVoiceControlUnavailable[] = L"\u79BB\u7EBF\u8BED\u97F3\u7EC4\u4EF6\u4E0D\u53EF\u7528";

constexpr UINT kControlPort = 5500;
constexpr UINT kVideoPort = 55000;
constexpr UINT kAudioPort = 55001;
constexpr UINT_PTR kStatsTimerId = 1;
constexpr UINT kLogMessage = WM_APP + 1;
constexpr UINT kStatsMessage = WM_APP + 2;
constexpr UINT kShowVideoWindowMessage = WM_APP + 3;
constexpr UINT kVoiceCommandMessage = WM_APP + 4;
constexpr UINT kVirtualCameraStateMessage = WM_APP + 5;
constexpr UINT kApplyStreamProfileMessage = WM_APP + 6;
constexpr UINT kSessionUpdateMessage = WM_APP + 7;
constexpr int kMaxStreamFps = 60;
constexpr int kAppIconResourceId = 101;
constexpr UINT kStatsIntervalMs = 250;
constexpr int64_t kTimeSyncIntervalUs = 1'000'000;
constexpr int64_t kMaxReasonableLatencyUs = 5'000'000;
constexpr int64_t kAudioVideoLateDropUs = 25'000;
constexpr int64_t kVideoLateDropUs = 250'000;
constexpr ULONGLONG kVoiceMediaCommandDebounceMs = 1500;
constexpr ULONGLONG kVoiceMusicCommandDebounceMs = 1500;
constexpr DWORD kVoiceCommandRecoverySuppressMs = 1800;
constexpr int kFocusVideoButtonId = 1001;
constexpr int kOpenLogButtonId = 1002;
constexpr int kPresentationModeButtonId = 1003;
constexpr int kNavOverviewButtonId = 1101;
constexpr int kNavConnectButtonId = 1102;
constexpr int kNavDisplayButtonId = 1103;
constexpr int kNavDiagnosticsButtonId = 1104;
constexpr int kNavLogsButtonId = 1105;
constexpr int kNavVoiceButtonId = 1106;
constexpr int kStatusWindowWidth = 1120;
constexpr int kStatusWindowHeight = 580;
constexpr int kStatusWindowMinWidth = 760;
constexpr int kStatusWindowMinHeight = 430;
constexpr int kStatusWindowResizeBorder = 6;
constexpr int kVideoWindowWidth = 1440;
constexpr int kVideoWindowHeight = 900;
constexpr size_t kMaxAutoReplyRecentLogs = 12;
constexpr size_t kMaxAutoReplyMatchQueueSize = 24;
constexpr size_t kMaxAutoReplyQueueSize = 8;
constexpr size_t kMaxAutoReplyRecentEchoes = 8;
constexpr double kAutoReplyInputAnchorXRatio = 0.33;
constexpr int kAutoReplyInputAnchorBottomOffset = 44;
constexpr int kAutoReplySendPointXPermilleDefault = 860;
constexpr int kAutoReplySendPointYPermilleDefault = 927;
constexpr int kAutoReplyInputFocusDelayMs = 180;
constexpr int kAutoReplyInputAfterClearDelayMs = 50;
constexpr int kAutoReplyInputAfterPasteDelayMs = 180;
constexpr int kAutoReplyAfterSendCheckDelayMs = 420;
constexpr DWORD kAutoReplySendPointCaptureTimeoutMs = 60'000;
constexpr ULONGLONG kAutoReplyEchoIgnoreWindowMs = 8'000;

constexpr COLORREF kColorWindowBackground = RGB(245, 245, 247);
constexpr COLORREF kColorEmbeddedWindowBackground = RGB(0, 0, 0);
constexpr COLORREF kColorCardBackground = RGB(255, 255, 255);
constexpr COLORREF kColorHeroBackground = RGB(251, 251, 253);
constexpr COLORREF kColorLogBackground = RGB(251, 251, 253);
constexpr COLORREF kColorBorder = RGB(228, 228, 232);
constexpr COLORREF kColorAccent = RGB(0, 113, 227);
constexpr COLORREF kColorAccentDark = RGB(0, 102, 204);
constexpr COLORREF kColorShadowFar = RGB(232, 232, 237);
constexpr COLORREF kColorShadowNear = RGB(239, 239, 243);
constexpr COLORREF kColorGlassGlowBlue = RGB(252, 252, 255);
constexpr COLORREF kColorGlassGlowPink = RGB(248, 248, 251);
constexpr COLORREF kColorGlassGlowMint = RGB(241, 241, 245);
constexpr COLORREF kColorEmbeddedGlassGlowLeft = RGB(76, 84, 98);
constexpr COLORREF kColorEmbeddedGlassGlowEdge = RGB(100, 111, 128);
constexpr COLORREF kColorEmbeddedGlassGlowGround = RGB(40, 45, 54);
constexpr COLORREF kColorTextPrimary = RGB(29, 29, 31);
constexpr COLORREF kColorTextSecondary = RGB(110, 110, 115);
constexpr COLORREF kColorWhite = RGB(255, 255, 255);
constexpr COLORREF kColorSidebarBackground = RGB(249, 249, 251);
constexpr COLORREF kColorSidebarSurface = RGB(249, 249, 251);
constexpr COLORREF kColorSidebarBorder = RGB(229, 229, 234);
constexpr COLORREF kColorSidebarText = RGB(66, 66, 69);
constexpr COLORREF kColorSidebarMuted = RGB(134, 134, 139);
constexpr COLORREF kColorSidebarActive = RGB(255, 255, 255);
constexpr COLORREF kColorSidebarActiveBorder = RGB(236, 236, 240);
constexpr COLORREF kColorSidebarTextActive = RGB(0, 102, 204);
constexpr COLORREF kColorSidebarIndicator = RGB(0, 113, 227);
constexpr COLORREF kColorTopbarBackground = RGB(255, 255, 255);
constexpr COLORREF kColorTopbarBorder = RGB(236, 236, 240);
constexpr COLORREF kColorFocusRing = RGB(0, 113, 227);
constexpr wchar_t kUiDisplayFontFamily[] = L"Segoe UI Variable Display";
constexpr wchar_t kUiTextFontFamily[] = L"Segoe UI Variable Text";

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

enum class StatusPage {
    kOverview,
    kConnect,
    kDisplay,
    kVoiceControl,
    kDiagnostics,
    kLogs,
};

struct SessionSnapshot {
    std::wstring device_name;
    std::wstring host;
    bool control_connected = false;
    int64_t session_started_us = 0;
    int64_t last_session_change_us = 0;
    int64_t last_present_us = 0;
};

struct SenderIdentity {
    std::wstring display_name;
    std::wstring version_label;
};

struct StatusWindowLayout {
    RECT shell{};
    RECT topbar{};
    RECT sidebar{};
    std::array<RECT, 6> nav_buttons{};
    RECT content{};
    RECT hero_main{};
    RECT hero_side{};
    RECT focus_button{};
    RECT mode_button{};
    RECT open_log_button{};
    std::array<RECT, 4> summary_cards{};
    RECT detail_left{};
    RECT detail_right{};
    RECT detail_full{};
    RECT log_view{};
};

struct PendingAutoReplyJob {
    std::wstring source_text;
    std::wstring speaker;
    std::wstring content;
    std::wstring reply_text;
    ULONGLONG due_tick = 0;
};

struct PendingAutoReplyEvent {
    bool is_gift = false;
    std::wstring text;
    ULONGLONG received_tick = 0;
};

struct RecentAutoReplyEcho {
    std::wstring normalized_text;
    ULONGLONG tick = 0;
};

enum class AutoReplyCaptureTarget {
    kNone,
    kInput,
    kSend,
};

struct AppState {
    HWND status_window = nullptr;
    HWND video_window = nullptr;
    bool web_ui_ready = false;
    std::array<HWND, 6> nav_buttons{};
    HWND focus_video_button = nullptr;
    HWND presentation_mode_button = nullptr;
    HWND open_log_button = nullptr;
    HWND log_view = nullptr;
    HFONT title_font = nullptr;
    HFONT subtitle_font = nullptr;
    HFONT section_font = nullptr;
    HFONT value_font = nullptr;
    HFONT spotlight_value_font = nullptr;
    HFONT body_font = nullptr;
    HFONT button_font = nullptr;
    HBRUSH window_background_brush = nullptr;
    HBRUSH log_background_brush = nullptr;
    std::unique_ptr<EmbeddedWebUiBridge> web_ui_bridge;
    DashboardSnapshot dashboard{};
    VideoRenderer renderer;
    VideoRenderer::PresentationMode presentation_mode = VideoRenderer::PresentationMode::kLowLatency;
    StatusPage current_page = StatusPage::kOverview;
    std::mutex log_mutex;
    std::deque<std::wstring> pending_logs;
    std::deque<std::wstring> recent_logs;
    std::wstring log_file_path;
    std::wstring status_file_path;
    std::wstring web_ui_folder_path;
    std::wstring web_ui_user_data_path;
    std::wstring profile_cache_path;
    std::wstring codec_config_cache_path;
    std::wstring virtual_camera_tool_path;
    std::wstring virtual_camera_media_source_path;
    std::wstring virtual_camera_placeholder_path;
    std::wstring airplay_runtime_root_path;
    std::wstring airplay_log_path;
    std::wstring voice_runtime_root_path;
    std::wstring voice_model_path;
    std::wstring voice_music_root_path;
    std::wstring danmaku_capture_root_path;
    std::wstring danmaku_region_file_path;
    std::wstring auto_reply_root_path;
    std::wstring auto_reply_config_path;
    std::unique_ptr<VoiceCommandController> voice_controller;
    std::unique_ptr<VoiceMusicLibrary> voice_music_library;
    std::unique_ptr<LocalMusicPlayer> local_music_player;
    std::unique_ptr<DanmakuController> danmaku_controller;
    std::mutex auto_reply_mutex;
    std::condition_variable auto_reply_match_cv;
    std::condition_variable auto_reply_cv;
    std::deque<PendingAutoReplyEvent> auto_reply_match_queue;
    std::deque<PendingAutoReplyJob> auto_reply_queue;
    std::deque<std::wstring> auto_reply_recent_logs;
    std::deque<RecentAutoReplyEcho> auto_reply_recent_echoes;
    std::thread auto_reply_match_thread;
    std::thread auto_reply_thread;
    std::thread auto_reply_capture_thread;
    bool auto_reply_enabled = false;
    bool auto_reply_send_enabled = false;
    bool auto_reply_stop_requested = false;
    bool auto_reply_capture_running = false;
    bool auto_reply_capture_stop_requested = false;
    AutoReplyCaptureTarget auto_reply_capture_target = AutoReplyCaptureTarget::kNone;
    int auto_reply_cooldown_seconds = 12;
    int auto_reply_delay_ms = 1600;
    int auto_reply_input_point_x_permille = 329;
    int auto_reply_input_point_y_permille = 927;
    bool auto_reply_input_point_configured = false;
    int auto_reply_send_point_x_permille = kAutoReplySendPointXPermilleDefault;
    int auto_reply_send_point_y_permille = kAutoReplySendPointYPermilleDefault;
    bool auto_reply_send_point_configured = false;
    ULONGLONG auto_reply_last_trigger_tick = 0;
    std::wstring auto_reply_exact_rules_text;
    std::wstring auto_reply_keyword_rules_text;
    std::wstring auto_reply_fallback_text;
    std::wstring auto_reply_status = L"\u81ea\u52a8\u56de\u590d\u672a\u5f00\u542f";
    std::wstring auto_reply_target_title;
    std::wstring auto_reply_last_match_text;
    std::wstring auto_reply_last_reply_text;
    std::mutex voice_phrase_mutex;
    std::deque<std::wstring> pending_voice_phrases;
    std::unordered_set<std::wstring> voice_exact_phrases;
    bool voice_control_enabled = false;
    std::wstring voice_control_status = kTextVoiceControlDisabled;
    ULONGLONG last_voice_media_command_tick = 0;
    ULONGLONG last_voice_music_command_tick = 0;
    NvidiaCuvidProbeResult nvdec_probe{};
    protocol::StreamProfile selected_profile;
    bool has_selected_profile = false;
    protocol::StreamProfile cached_startup_profile;
    bool has_cached_startup_profile = false;
    std::vector<uint8_t> cached_codec_config;
    bool has_cached_codec_config = false;
    bool auto_resumed_profile = false;
    bool auto_resume_notice_logged = false;
    std::atomic<bool> auto_resume_profile_message_pending{false};
    std::unique_ptr<ControlServer> control_server;
    std::unique_ptr<UdpVideoReceiver> udp_receiver;
    std::unique_ptr<UdpAudioReceiver> audio_receiver;
    std::unique_ptr<VideoDecoder> decoder;
    std::unique_ptr<AudioPlayer> audio_player;
    AirPlayReceiverController airplay_controller;
    VirtualCameraFrameBridge virtual_camera_bridge;
    VirtualCameraController virtual_camera_controller;
    mutable std::mutex session_mutex;
    SessionSnapshot session{};

    std::mutex metrics_mutex;
    bool has_clock_sync = false;
    int64_t sender_clock_offset_us = 0;
    int64_t sender_clock_rtt_us = 0;
    int64_t last_stale_video_drop_request_us = 0;
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

struct PendingStreamProfileMessage {
    protocol::StreamProfile profile;
    bool auto_resumed = false;
    bool submit_cached_codec_config = false;
    uint32_t cached_codec_config_frame_id = 0;
    std::vector<uint8_t> cached_codec_config;
    bool log_auto_resume_notice = false;
};

struct PendingSessionUpdateMessage {
    std::wstring sender_host;
    std::wstring device_name;
    bool connected = false;
};

void QueueLog(AppState* state, const std::wstring& message);
void PostApplyStreamProfileMessage(
    AppState* state,
    const protocol::StreamProfile& profile,
    bool auto_resumed,
    bool submit_cached_codec_config = false,
    uint32_t cached_codec_config_frame_id = 0,
    std::vector<uint8_t> cached_codec_config = {},
    bool log_auto_resume_notice = false);
void PostSessionUpdateMessage(
    AppState* state,
    const std::wstring& sender_host,
    const std::wstring& device_name,
    bool connected);
void UpdateStatusLabel(AppState* state);
std::wstring GetExecutableDirectory();
std::wstring BuildWebUiFolderPath();
std::wstring BuildWebUiUserDataPath();
std::wstring BuildAirPlayStatusText(const AppState* state);
void RefreshAirPlayState(AppState* state);
void StartAirPlayReceiver(AppState* state, bool automatic);
void StopAirPlayReceiver(AppState* state, bool write_log);
std::wstring BuildVirtualCameraStatusText(const AppState* state);
void NotifyVirtualCameraStateChanged(AppState* state);
void HandleVirtualCameraControllerStateChanged(AppState* state);
void RefreshVirtualCameraState(AppState* state);
void StopVirtualCamera(AppState* state, bool write_log);
void InstallVirtualCamera(AppState* state);
void StartVirtualCamera(AppState* state);
std::wstring BuildVirtualCameraPlaceholderPath();
std::wstring BuildVoiceRuntimeRootPath();
std::wstring BuildVoiceModelPath();
std::wstring BuildVoiceModelRuntimePath(const std::wstring& model_source_path);
std::wstring BuildDanmakuCaptureRootPath();
std::wstring BuildDanmakuRegionFilePath();
std::wstring BuildAutoReplyRootPath();
std::wstring BuildAutoReplyConfigPath();
bool EnsureDirectoryPath(const std::wstring& path);
std::wstring BuildVoiceControlStatusText(const AppState* state);
std::vector<std::wstring> BuildVoiceRecognitionGrammar(AppState* state);
void RefreshVoiceRecognitionContext(AppState* state, bool restart_if_enabled, bool write_log);
bool StartVoiceControl(AppState* state);
void StopVoiceControl(AppState* state, bool write_log);
void HandleVoiceCommandMessage(AppState* state);
std::wstring BuildVoiceMusicRootPath();
void EnsureVoiceMusicScaffold(AppState* state);
void SetEmbeddedWebUiMode(AppState* state, bool enabled);
void DestroyNativeStatusControls(AppState* state);
void ExecuteVoiceMusicProjectCreate(
    AppState* state,
    const std::wstring& project_name,
    const std::wstring& alias_payload);
void RefreshVoiceMusicLibrary(AppState* state);
void StopLocalMusicPlayback(AppState* state);
void OpenDirectoryPath(AppState* state, const std::wstring& directory);
void OpenVoiceMusicDirectory(AppState* state);
void OpenVoiceModelDirectory(AppState* state);
void OpenAutoReplyDirectory(AppState* state);
void EnsureAutoReplyScaffold(AppState* state);
void LoadAutoReplyConfig(AppState* state);
void SaveAutoReplyConfig(AppState* state);
void ToggleAutoReplyEnabled(AppState* state);
void ToggleAutoReplySendEnabled(AppState* state);
void SaveAutoReplyTextConfig(AppState* state, const std::wstring& rules_text, const std::wstring& fallback_text);
void SaveAutoReplyTimingConfig(AppState* state, const std::wstring& timing_text);
void StartAutoReplyInputPointCapture(AppState* state);
void StartAutoReplySendPointCapture(AppState* state);
void AutoReplySendPointCaptureLoop(AppState* state);
void StopAutoReplySendPointCapture(AppState* state);
void HandleAutoReplyDanmakuEvent(AppState* state, bool is_gift, const std::wstring& text);
void AutoReplyMatchWorkerLoop(AppState* state);
void AutoReplyWorkerLoop(AppState* state);
void StopAutoReplyWorker(AppState* state);
bool FileExists(const std::wstring& path);
std::wstring TrimWhitespace(const std::wstring& text);
std::string WideToUtf8(const std::wstring& value);
std::wstring WideFromUtf8(const std::string& value);
std::string ReadUtf8File(const std::wstring& path);
void WriteUtf8File(const std::wstring& path, const std::string& utf8_text);
std::string QuoteJsonWide(const std::wstring& value);

struct AutoReplyRule {
    std::wstring label;
    std::vector<std::wstring> keywords;
    std::vector<std::wstring> replies;
    bool wildcard = false;
};

enum class AutoReplyMatchKind {
    kNone,
    kExact,
    kKeyword,
    kFallback,
};

struct AutoReplyWindowCandidate {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring class_name;
    int score = 0;
};

struct AutoReplyEnumContext {
    HWND owner_window = nullptr;
    HWND video_window = nullptr;
    std::vector<AutoReplyWindowCandidate>* candidates = nullptr;
};

std::wstring ReadWindowTextForAutoReply(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return {};
    }

    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return {};
    }

    std::wstring text(static_cast<size_t>(length), L'\0');
    const int copied = GetWindowTextW(hwnd, text.data(), length + 1);
    if (copied <= 0) {
        return {};
    }
    text.resize(static_cast<size_t>(copied));
    return TrimWhitespace(text);
}

std::wstring ReadWindowClassForAutoReply(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return {};
    }

    wchar_t class_name[256] = {};
    const int copied = GetClassNameW(hwnd, class_name, static_cast<int>(std::size(class_name)));
    if (copied <= 0) {
        return {};
    }
    return TrimWhitespace(class_name);
}

bool IsPointInsideRect(const POINT& point, const RECT& rect) {
    return point.x >= rect.left &&
        point.x < rect.right &&
        point.y >= rect.top &&
        point.y < rect.bottom;
}

std::wstring AutoReplyNowText() {
    SYSTEMTIME local_time{};
    GetLocalTime(&local_time);
    std::wostringstream stream;
    stream << std::setfill(L'0')
           << std::setw(2) << local_time.wHour
           << L":"
           << std::setw(2) << local_time.wMinute
           << L":"
           << std::setw(2) << local_time.wSecond;
    return stream.str();
}

void RequestAutoReplySnapshotRefresh(AppState* state) {
    if (state != nullptr && state->status_window != nullptr) {
        PostMessageW(state->status_window, kStatsMessage, 0, 0);
    }
}

void AppendAutoReplyRecentLog(AppState* state, const std::wstring& message, bool queue_log = true) {
    if (state == nullptr) {
        return;
    }

    const std::wstring trimmed = TrimWhitespace(message);
    if (trimmed.empty()) {
        return;
    }

    const std::wstring line = AutoReplyNowText() + L" " + trimmed;
    {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        state->auto_reply_recent_logs.push_back(line);
        while (state->auto_reply_recent_logs.size() > kMaxAutoReplyRecentLogs) {
            state->auto_reply_recent_logs.pop_front();
        }
    }

    if (queue_log) {
        QueueLog(state, std::wstring(L"\u81ea\u52a8\u56de\u590d\uff1a") + trimmed);
    }
    RequestAutoReplySnapshotRefresh(state);
}

std::wstring BuildAutoReplyModeText(bool enabled, bool send_enabled) {
    if (!enabled) {
        return L"\u81ea\u52a8\u56de\u590d\u672a\u5f00\u542f";
    }
    if (send_enabled) {
        return L"\u81ea\u52a8\u56de\u590d\u5df2\u5f00\u542f\uff0c\u53d1\u9001\u5df2\u5f00\u542f";
    }
    return L"\u81ea\u52a8\u56de\u590d\u5df2\u5f00\u542f\uff0c\u5f53\u524d\u4ec5\u9884\u89c8";
}

std::wstring ToLowerAutoReply(const std::wstring& text) {
    std::wstring lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return lowered;
}

std::wstring NormalizeAutoReplyMatchText(const std::wstring& text) {
    return ToLowerAutoReply(TrimWhitespace(text));
}

bool ContainsAutoReplyToken(const std::wstring& text, const std::wstring& token) {
    if (text.empty() || token.empty()) {
        return false;
    }
    return ToLowerAutoReply(text).find(ToLowerAutoReply(token)) != std::wstring::npos;
}

void PruneRecentAutoReplyEchoesLocked(AppState* state, ULONGLONG now_tick) {
    if (state == nullptr) {
        return;
    }
    while (!state->auto_reply_recent_echoes.empty() &&
           now_tick - state->auto_reply_recent_echoes.front().tick > kAutoReplyEchoIgnoreWindowMs) {
        state->auto_reply_recent_echoes.pop_front();
    }
}

void RememberRecentAutoReplyEchoLocked(AppState* state, const std::wstring& reply_text, ULONGLONG now_tick) {
    if (state == nullptr) {
        return;
    }

    const std::wstring normalized = NormalizeAutoReplyMatchText(reply_text);
    if (normalized.empty()) {
        return;
    }

    PruneRecentAutoReplyEchoesLocked(state, now_tick);
    state->auto_reply_recent_echoes.push_back({normalized, now_tick});
    while (state->auto_reply_recent_echoes.size() > kMaxAutoReplyRecentEchoes) {
        state->auto_reply_recent_echoes.pop_front();
    }
}

bool IsRecentAutoReplyEchoLocked(
    AppState* state,
    const std::wstring& source_text,
    const std::wstring& content,
    ULONGLONG now_tick) {
    if (state == nullptr) {
        return false;
    }

    PruneRecentAutoReplyEchoesLocked(state, now_tick);
    if (state->auto_reply_recent_echoes.empty()) {
        return false;
    }

    const std::wstring normalized_content = NormalizeAutoReplyMatchText(content);
    const std::wstring normalized_source = NormalizeAutoReplyMatchText(source_text);
    for (const auto& entry : state->auto_reply_recent_echoes) {
        if (!normalized_content.empty() && normalized_content == entry.normalized_text) {
            return true;
        }
        if (normalized_content.empty() &&
            !normalized_source.empty() &&
            normalized_source == entry.normalized_text) {
            return true;
        }
    }
    return false;
}

int ClampAutoReplyCooldown(int seconds) {
    return std::clamp(seconds, 0, 600);
}

int ClampAutoReplyDelay(int delay_ms) {
    return std::clamp(delay_ms, 0, 20'000);
}

std::vector<std::wstring> SplitAutoReplyLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    std::wstringstream stream(text);
    std::wstring line;
    while (std::getline(stream, line)) {
        line = TrimWhitespace(line);
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

#if 0
std::vector<std::wstring> SplitAutoReplyByDelimiters(const std::wstring& text) {
    std::vector<std::wstring> parts;
    std::wstring current;
    for (wchar_t ch : text) {
        if (ch == L'|' || ch == L',' || ch == L'，' || ch == L'/' || ch == L'、' || ch == L';' || ch == L'；') {
            const std::wstring trimmed = TrimWhitespace(current);
            if (!trimmed.empty()) {
                parts.push_back(trimmed);
            }
            current.clear();
            continue;
        }
        current.push_back(ch);
    }

    const std::wstring trimmed = TrimWhitespace(current);
    if (!trimmed.empty()) {
        parts.push_back(trimmed);
    }
    return parts;
}

#endif

std::vector<std::wstring> SplitAutoReplyByDelimiters(const std::wstring& text) {
    std::vector<std::wstring> parts;
    std::wstring current;
    for (wchar_t ch : text) {
        if (ch == L'|' || ch == L',' || ch == L'\uFF0C' || ch == L'/' || ch == L'\u3001' || ch == L';' || ch == L'\uFF1B') {
            const std::wstring trimmed = TrimWhitespace(current);
            if (!trimmed.empty()) {
                parts.push_back(trimmed);
            }
            current.clear();
            continue;
        }
        current.push_back(ch);
    }

    const std::wstring trimmed = TrimWhitespace(current);
    if (!trimmed.empty()) {
        parts.push_back(trimmed);
    }
    return parts;
}

std::vector<std::wstring> ParseAutoReplyReplyPool(const std::wstring& text) {
    std::vector<std::wstring> replies;
    for (const auto& line : SplitAutoReplyLines(text)) {
        for (const auto& item : SplitAutoReplyByDelimiters(line)) {
            if (!item.empty()) {
                replies.push_back(item);
            }
        }
    }
    return replies;
}

#if 0
std::vector<AutoReplyRule> ParseAutoReplyRules(const std::wstring& rules_text) {
    std::vector<AutoReplyRule> rules;
    for (const auto& raw_line : SplitAutoReplyLines(rules_text)) {
        if (raw_line.starts_with(L"#") || raw_line.starts_with(L"//")) {
            continue;
        }

        const size_t separator = raw_line.find(L'=');
        const size_t fullwidth_separator = raw_line.find(L'＝');
        const size_t split_pos =
            separator != std::wstring::npos
                ? separator
                : fullwidth_separator;
        if (split_pos == std::wstring::npos) {
            continue;
        }

        AutoReplyRule rule;
        rule.label = TrimWhitespace(raw_line.substr(0, split_pos));
        rule.replies = ParseAutoReplyReplyPool(raw_line.substr(split_pos + 1));
        if (rule.replies.empty()) {
            continue;
        }

        if (rule.label == L"*") {
            rule.wildcard = true;
        } else {
            rule.keywords = SplitAutoReplyByDelimiters(rule.label);
            if (rule.keywords.empty()) {
                continue;
            }
        }
        rules.push_back(std::move(rule));
    }
    return rules;
}

#endif

std::vector<AutoReplyRule> ParseAutoReplyRules(const std::wstring& rules_text) {
    std::vector<AutoReplyRule> rules;
    for (const auto& raw_line : SplitAutoReplyLines(rules_text)) {
        if (raw_line.starts_with(L"#") || raw_line.starts_with(L"//")) {
            continue;
        }

        const size_t separator = raw_line.find(L'=');
        const size_t fullwidth_separator = raw_line.find(L'\uFF1D');
        const size_t split_pos =
            separator != std::wstring::npos
                ? separator
                : fullwidth_separator;
        if (split_pos == std::wstring::npos) {
            continue;
        }

        AutoReplyRule rule;
        rule.label = TrimWhitespace(raw_line.substr(0, split_pos));
        rule.replies = ParseAutoReplyReplyPool(raw_line.substr(split_pos + 1));
        if (rule.replies.empty()) {
            continue;
        }

        if (rule.label == L"*") {
            rule.wildcard = true;
        } else {
            rule.keywords = SplitAutoReplyByDelimiters(rule.label);
            if (rule.keywords.empty()) {
                continue;
            }
        }
        rules.push_back(std::move(rule));
    }
    return rules;
}

std::wstring ReplaceAllAutoReply(std::wstring text, const std::wstring& from, const std::wstring& to) {
    if (from.empty()) {
        return text;
    }

    size_t position = 0;
    while ((position = text.find(from, position)) != std::wstring::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
    return text;
}

#if 0
bool ParseAutoReplySpeakerContent(
    const std::wstring& text,
    std::wstring* speaker,
    std::wstring* content) {
    const std::wstring trimmed = TrimWhitespace(text);
    const size_t fullwidth_colon = trimmed.find(L'：');
    const size_t ascii_colon = trimmed.find(L':');
    const size_t separator =
        fullwidth_colon != std::wstring::npos
            ? fullwidth_colon
            : ascii_colon;

    if (separator != std::wstring::npos) {
        const std::wstring left = TrimWhitespace(trimmed.substr(0, separator));
        const std::wstring right = TrimWhitespace(trimmed.substr(separator + 1));
        if (!left.empty() && !right.empty()) {
            if (speaker != nullptr) {
                *speaker = left;
            }
            if (content != nullptr) {
                *content = right;
            }
            return true;
        }
    }

    if (speaker != nullptr) {
        speaker->clear();
    }
    if (content != nullptr) {
        *content = trimmed;
    }
    return false;
}

#endif

bool ParseAutoReplySpeakerContent(
    const std::wstring& text,
    std::wstring* speaker,
    std::wstring* content) {
    const std::wstring trimmed = TrimWhitespace(text);
    const size_t fullwidth_colon = trimmed.find(L'\uFF1A');
    const size_t ascii_colon = trimmed.find(L':');
    const size_t separator =
        fullwidth_colon != std::wstring::npos
            ? fullwidth_colon
            : ascii_colon;

    if (separator != std::wstring::npos) {
        const std::wstring left = TrimWhitespace(trimmed.substr(0, separator));
        const std::wstring right = TrimWhitespace(trimmed.substr(separator + 1));
        if (!left.empty() && !right.empty()) {
            if (speaker != nullptr) {
                *speaker = left;
            }
            if (content != nullptr) {
                *content = right;
            }
            return true;
        }
    }

    if (speaker != nullptr) {
        speaker->clear();
    }
    if (content != nullptr) {
        *content = trimmed;
    }
    return false;
}

std::wstring PickRandomAutoReply(const std::vector<std::wstring>& values) {
    if (values.empty()) {
        return {};
    }

    static thread_local std::mt19937 engine(std::random_device{}());
    std::uniform_int_distribution<size_t> distribution(0, values.size() - 1);
    return values[distribution(engine)];
}

std::wstring RenderAutoReplyTemplate(
    const std::wstring& reply_template,
    const std::wstring& speaker,
    const std::wstring& content,
    const std::wstring& source_text) {
    std::wstring rendered = reply_template;
    const std::wstring fallback_name = speaker.empty() ? L"\u89c2\u4f17" : speaker;
    const std::wstring fallback_content = content.empty() ? source_text : content;
    rendered = ReplaceAllAutoReply(rendered, L"{name}", fallback_name);
    rendered = ReplaceAllAutoReply(rendered, L"{speaker}", fallback_name);
    rendered = ReplaceAllAutoReply(rendered, L"{content}", fallback_content);
    return TrimWhitespace(rendered);
}

std::wstring MatchExactAutoReplyText(
    const std::vector<AutoReplyRule>& rules,
    const std::wstring& source_text,
    const std::wstring& content,
    std::wstring* matched_label) {
    const std::wstring normalized_content = NormalizeAutoReplyMatchText(content.empty() ? source_text : content);
    const std::wstring normalized_source = NormalizeAutoReplyMatchText(source_text);

    const AutoReplyRule* best_rule = nullptr;
    std::wstring best_keyword;
    size_t best_keyword_length = 0;

    for (const auto& rule : rules) {
        if (rule.wildcard) {
            continue;
        }

        for (const auto& keyword : rule.keywords) {
            const std::wstring normalized_keyword = NormalizeAutoReplyMatchText(keyword);
            if (normalized_keyword.empty()) {
                continue;
            }

            const bool matched =
                normalized_content == normalized_keyword ||
                (!normalized_source.empty() && normalized_source == normalized_keyword);
            if (!matched) {
                continue;
            }

            const size_t keyword_length = normalized_keyword.size();
            if (best_rule == nullptr || keyword_length > best_keyword_length) {
                best_rule = &rule;
                best_keyword = keyword;
                best_keyword_length = keyword_length;
            }
        }
    }

    if (best_rule == nullptr) {
        return {};
    }

    if (matched_label != nullptr) {
        *matched_label = best_keyword.empty() ? best_rule->label : best_keyword;
    }
    return PickRandomAutoReply(best_rule->replies);
}

std::wstring MatchKeywordAutoReplyText(
    const std::vector<AutoReplyRule>& rules,
    const std::wstring& source_text,
    const std::wstring& content,
    std::wstring* matched_label) {
    const std::wstring normalized_content = NormalizeAutoReplyMatchText(content.empty() ? source_text : content);
    const std::wstring normalized_source = NormalizeAutoReplyMatchText(source_text);

    const AutoReplyRule* wildcard_rule = nullptr;
    const AutoReplyRule* best_rule = nullptr;
    std::wstring best_keyword;
    size_t best_keyword_length = 0;

    for (const auto& rule : rules) {
        if (rule.wildcard) {
            if (wildcard_rule == nullptr) {
                wildcard_rule = &rule;
            }
            continue;
        }

        for (const auto& keyword : rule.keywords) {
            const std::wstring normalized_keyword = NormalizeAutoReplyMatchText(keyword);
            if (normalized_keyword.empty()) {
                continue;
            }

            const bool matched =
                normalized_content.find(normalized_keyword) != std::wstring::npos ||
                (!normalized_source.empty() &&
                 normalized_source.find(normalized_keyword) != std::wstring::npos);
            if (!matched) {
                continue;
            }

            const size_t keyword_length = normalized_keyword.size();
            if (best_rule == nullptr || keyword_length > best_keyword_length) {
                best_rule = &rule;
                best_keyword = keyword;
                best_keyword_length = keyword_length;
            }
        }
    }

    if (best_rule != nullptr) {
        if (matched_label != nullptr) {
            *matched_label = best_keyword.empty() ? best_rule->label : best_keyword;
        }
        return PickRandomAutoReply(best_rule->replies);
    }

    if (wildcard_rule != nullptr) {
        if (matched_label != nullptr) {
            *matched_label = L"*";
        }
        return PickRandomAutoReply(wildcard_rule->replies);
    }

    return {};
}

std::wstring MatchAutoReplyText(
    const std::vector<AutoReplyRule>& exact_rules,
    const std::vector<AutoReplyRule>& keyword_rules,
    const std::vector<std::wstring>& fallback_replies,
    const std::wstring& source_text,
    const std::wstring& content,
    std::wstring* matched_label,
    AutoReplyMatchKind* match_kind) {
    const std::wstring exact_reply = MatchExactAutoReplyText(
        exact_rules,
        source_text,
        content,
        matched_label);
    if (!exact_reply.empty()) {
        if (match_kind != nullptr) {
            *match_kind = AutoReplyMatchKind::kExact;
        }
        return exact_reply;
    }

    const std::wstring keyword_reply = MatchKeywordAutoReplyText(
        keyword_rules,
        source_text,
        content,
        matched_label);
    if (!keyword_reply.empty()) {
        if (match_kind != nullptr) {
            *match_kind = AutoReplyMatchKind::kKeyword;
        }
        return keyword_reply;
    }

    if (!fallback_replies.empty()) {
        if (matched_label != nullptr) {
            *matched_label = L"\u9ed8\u8ba4";
        }
        if (match_kind != nullptr) {
            *match_kind = AutoReplyMatchKind::kFallback;
        }
        return PickRandomAutoReply(fallback_replies);
    }

    if (match_kind != nullptr) {
        *match_kind = AutoReplyMatchKind::kNone;
    }
    return {};
}

std::wstring BuildAutoReplyMatchKindText(AutoReplyMatchKind match_kind) {
    switch (match_kind) {
    case AutoReplyMatchKind::kExact:
        return L"\u7cbe\u786e";
    case AutoReplyMatchKind::kKeyword:
        return L"\u5173\u952e\u8bcd";
    case AutoReplyMatchKind::kFallback:
        return L"\u9ed8\u8ba4";
    default:
        return L"\u672a\u5339\u914d";
    }
}

bool LocateAutoReplyJsonValue(const std::wstring& json_text, const wchar_t* field_name, size_t* value_pos) {
    if (field_name == nullptr || *field_name == L'\0' || value_pos == nullptr) {
        return false;
    }

    const std::wstring marker = std::wstring(L"\"") + field_name + L"\"";
    const size_t field_pos = json_text.find(marker);
    if (field_pos == std::wstring::npos) {
        return false;
    }

    size_t cursor = field_pos + marker.size();
    while (cursor < json_text.size() && std::iswspace(json_text[cursor])) {
        ++cursor;
    }
    if (cursor >= json_text.size() || json_text[cursor] != L':') {
        return false;
    }
    ++cursor;
    while (cursor < json_text.size() && std::iswspace(json_text[cursor])) {
        ++cursor;
    }
    if (cursor >= json_text.size()) {
        return false;
    }

    *value_pos = cursor;
    return true;
}

std::wstring UnescapeAutoReplyJsonString(const std::wstring& value) {
    std::wstring result;
    result.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        const wchar_t ch = value[index];
        if (ch != L'\\' || index + 1 >= value.size()) {
            result.push_back(ch);
            continue;
        }

        const wchar_t escaped = value[++index];
        switch (escaped) {
        case L'"':
            result.push_back(L'"');
            break;
        case L'\\':
            result.push_back(L'\\');
            break;
        case L'/':
            result.push_back(L'/');
            break;
        case L'b':
            result.push_back(L'\b');
            break;
        case L'f':
            result.push_back(L'\f');
            break;
        case L'n':
            result.push_back(L'\n');
            break;
        case L'r':
            result.push_back(L'\r');
            break;
        case L't':
            result.push_back(L'\t');
            break;
        case L'u':
            if (index + 4 < value.size()) {
                wchar_t* end = nullptr;
                const std::wstring hex = value.substr(index + 1, 4);
                const unsigned long codepoint = std::wcstoul(hex.c_str(), &end, 16);
                if (end != nullptr && *end == L'\0') {
                    result.push_back(static_cast<wchar_t>(codepoint));
                    index += 4;
                    break;
                }
            }
            result.push_back(escaped);
            break;
        default:
            result.push_back(escaped);
            break;
        }
    }
    return result;
}

std::wstring ExtractAutoReplyJsonString(const std::wstring& json_text, const wchar_t* field_name) {
    size_t value_pos = 0;
    if (!LocateAutoReplyJsonValue(json_text, field_name, &value_pos) ||
        json_text[value_pos] != L'"') {
        return {};
    }

    ++value_pos;
    std::wstring raw_value;
    bool escaped = false;
    for (; value_pos < json_text.size(); ++value_pos) {
        const wchar_t ch = json_text[value_pos];
        if (!escaped && ch == L'"') {
            return UnescapeAutoReplyJsonString(raw_value);
        }
        if (!escaped && ch == L'\\') {
            escaped = true;
            raw_value.push_back(ch);
            continue;
        }
        escaped = false;
        raw_value.push_back(ch);
    }
    return {};
}

bool ExtractAutoReplyJsonBool(const std::wstring& json_text, const wchar_t* field_name, bool fallback) {
    size_t value_pos = 0;
    if (!LocateAutoReplyJsonValue(json_text, field_name, &value_pos)) {
        return fallback;
    }
    if (json_text.compare(value_pos, 4, L"true") == 0) {
        return true;
    }
    if (json_text.compare(value_pos, 5, L"false") == 0) {
        return false;
    }
    return fallback;
}

int ExtractAutoReplyJsonInt(const std::wstring& json_text, const wchar_t* field_name, int fallback) {
    size_t value_pos = 0;
    if (!LocateAutoReplyJsonValue(json_text, field_name, &value_pos)) {
        return fallback;
    }

    size_t end_pos = value_pos;
    if (json_text[end_pos] == L'-') {
        ++end_pos;
    }
    while (end_pos < json_text.size() && std::iswdigit(json_text[end_pos])) {
        ++end_pos;
    }
    if (end_pos == value_pos) {
        return fallback;
    }

    try {
        return std::stoi(json_text.substr(value_pos, end_pos - value_pos));
    } catch (...) {
        return fallback;
    }
}

void EnsureDanmakuReadyForAutoReply(AppState* state) {
    if (state == nullptr || state->danmaku_controller == nullptr) {
        return;
    }

    state->danmaku_controller->StartUiAutomationProbe();
    const auto snapshot = state->danmaku_controller->GetSnapshot();
    if (!snapshot.running) {
        state->danmaku_controller->Start();
    }
}

int ScoreAutoReplyWindow(const std::wstring& title, const std::wstring& class_name) {
    int score = 0;
    if (class_name == L"FinderLiveCommentFloatWnd") {
        score += 420;
    }
    if (class_name == L"FinderLiveMainWnd") {
        score += 80;
    }
    if (ContainsAutoReplyToken(title, L"\u4e92\u52a8\u6d88\u606f")) {
        score += 260;
    }
    if (ContainsAutoReplyToken(title, L"\u89c6\u9891\u53f7\u76f4\u64ad\u4f34\u4fa3")) {
        score += 100;
    }
    if (ContainsAutoReplyToken(title, L"\u76f4\u64ad\u4f34\u4fa3")) {
        score += 40;
    }
    if (ContainsAutoReplyToken(title, L"\u89c6\u9891\u53f7")) {
        score += 20;
    }
    return score;
}

BOOL CALLBACK CollectAutoReplyWindowsProc(HWND hwnd, LPARAM lparam) {
    auto* context = reinterpret_cast<AutoReplyEnumContext*>(lparam);
    if (context == nullptr || context->candidates == nullptr) {
        return TRUE;
    }
    if (hwnd == nullptr || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return TRUE;
    }
    if (hwnd == context->owner_window || hwnd == context->video_window) {
        return TRUE;
    }

    const std::wstring title = ReadWindowTextForAutoReply(hwnd);
    const std::wstring class_name = ReadWindowClassForAutoReply(hwnd);
    const int score = ScoreAutoReplyWindow(title, class_name);
    if (score <= 0) {
        return TRUE;
    }

    context->candidates->push_back(AutoReplyWindowCandidate{
        hwnd,
        title,
        class_name,
        score,
    });
    return TRUE;
}

std::vector<AutoReplyWindowCandidate> CollectAutoReplyWindows(AppState* state) {
    std::vector<AutoReplyWindowCandidate> candidates;
    AutoReplyEnumContext context{};
    context.owner_window = state != nullptr ? state->status_window : nullptr;
    context.video_window = state != nullptr ? state->video_window : nullptr;
    context.candidates = &candidates;
    EnumWindows(CollectAutoReplyWindowsProc, reinterpret_cast<LPARAM>(&context));

    std::sort(candidates.begin(), candidates.end(), [](const AutoReplyWindowCandidate& left, const AutoReplyWindowCandidate& right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        if (left.title.size() != right.title.size()) {
            return left.title.size() < right.title.size();
        }
        return left.class_name < right.class_name;
    });
    return candidates;
}

std::wstring TakeAutoReplyBstrAndFree(BSTR value) {
    if (value == nullptr) {
        return {};
    }
    std::wstring text(value, SysStringLen(value));
    SysFreeString(value);
    return TrimWhitespace(text);
}

std::wstring ReadAutoReplyElementName(IUIAutomationElement* element) {
    if (element == nullptr) {
        return {};
    }
    BSTR value = nullptr;
    if (FAILED(element->get_CurrentName(&value))) {
        return {};
    }
    return TakeAutoReplyBstrAndFree(value);
}

std::wstring ReadAutoReplyElementClassName(IUIAutomationElement* element) {
    if (element == nullptr) {
        return {};
    }
    BSTR value = nullptr;
    if (FAILED(element->get_CurrentClassName(&value))) {
        return {};
    }
    return TakeAutoReplyBstrAndFree(value);
}

std::vector<winrt::com_ptr<IUIAutomationElement>> CollectAutoReplyElements(
    IUIAutomation* automation,
    IUIAutomationElement* root) {
    std::vector<winrt::com_ptr<IUIAutomationElement>> elements;
    if (automation == nullptr || root == nullptr) {
        return elements;
    }

    winrt::com_ptr<IUIAutomationCondition> true_condition;
    winrt::com_ptr<IUIAutomationElementArray> found_elements;
    if (FAILED(automation->CreateTrueCondition(true_condition.put())) ||
        !true_condition ||
        FAILED(root->FindAll(TreeScope_Descendants, true_condition.get(), found_elements.put())) ||
        !found_elements) {
        return elements;
    }

    int length = 0;
    found_elements->get_Length(&length);
    for (int index = 0; index < length; ++index) {
        winrt::com_ptr<IUIAutomationElement> element;
        if (FAILED(found_elements->GetElement(index, element.put())) || !element) {
            continue;
        }
        elements.push_back(std::move(element));
    }
    return elements;
}

bool AutoReplyElementHasValuePattern(IUIAutomationElement* element) {
    if (element == nullptr) {
        return false;
    }
    winrt::com_ptr<IUIAutomationValuePattern> pattern;
    return SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(pattern.put()))) && pattern;
}

bool AutoReplyElementHasInvokePattern(IUIAutomationElement* element) {
    if (element == nullptr) {
        return false;
    }
    winrt::com_ptr<IUIAutomationInvokePattern> pattern;
    return SUCCEEDED(element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(pattern.put()))) && pattern;
}

int ScoreAutoReplyInputElement(IUIAutomationElement* element, const RECT& window_rect) {
    if (element == nullptr) {
        return -10'000;
    }

    CONTROLTYPEID control_type = 0;
    BOOL offscreen = FALSE;
    BOOL enabled = TRUE;
    RECT bounds{};
    element->get_CurrentControlType(&control_type);
    element->get_CurrentIsOffscreen(&offscreen);
    element->get_CurrentIsEnabled(&enabled);
    element->get_CurrentBoundingRectangle(&bounds);

    if (offscreen || !enabled) {
        return -10'000;
    }

    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) {
        return -10'000;
    }

    int score = 0;
    if (control_type == UIA_EditControlTypeId) {
        score += 220;
    } else if (control_type == UIA_DocumentControlTypeId) {
        score += 160;
    } else if (control_type == UIA_CustomControlTypeId) {
        score += 120;
    } else if (control_type == UIA_GroupControlTypeId || control_type == UIA_PaneControlTypeId) {
        score += 20;
    } else {
        return -1'000;
    }

    if (width >= 160) {
        score += 40;
    }
    if (height >= 24 && height <= 120) {
        score += 30;
    }
    if (bounds.top >= window_rect.bottom - 260) {
        score += 80;
    }
    if (bounds.top >= window_rect.top + ((window_rect.bottom - window_rect.top) / 2)) {
        score += 20;
    }

    const std::wstring name = ReadAutoReplyElementName(element);
    const std::wstring class_name = ReadAutoReplyElementClassName(element);
    if (ContainsAutoReplyToken(name, L"\u53d1\u9001\u6d88\u606f") ||
        ContainsAutoReplyToken(name, L"\u8bf4\u70b9\u4ec0\u4e48") ||
        ContainsAutoReplyToken(name, L"\u8bc4\u8bba") ||
        ContainsAutoReplyToken(name, L"\u8f93\u5165")) {
        score += 90;
    }
    if (ContainsAutoReplyToken(class_name, L"Edit")) {
        score += 50;
    }
    if (ContainsAutoReplyToken(name, L"\u53d1\u9001")) {
        score -= 40;
    }
    if (AutoReplyElementHasValuePattern(element)) {
        score += 40;
    }
    return score;
}

int ScoreAutoReplySendButtonElement(
    IUIAutomationElement* element,
    const RECT& window_rect,
    const RECT& input_rect) {
    if (element == nullptr) {
        return -10'000;
    }

    CONTROLTYPEID control_type = 0;
    BOOL offscreen = FALSE;
    BOOL enabled = TRUE;
    RECT bounds{};
    element->get_CurrentControlType(&control_type);
    element->get_CurrentIsOffscreen(&offscreen);
    element->get_CurrentIsEnabled(&enabled);
    element->get_CurrentBoundingRectangle(&bounds);

    if (offscreen || !enabled) {
        return -10'000;
    }

    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) {
        return -10'000;
    }

    int score = 0;
    if (control_type == UIA_ButtonControlTypeId) {
        score += 220;
    } else if (control_type == UIA_CustomControlTypeId) {
        score += 140;
    } else if (control_type == UIA_TextControlTypeId) {
        score += 20;
    } else {
        return -1'000;
    }

    const std::wstring name = ReadAutoReplyElementName(element);
    if (ContainsAutoReplyToken(name, L"\u53d1\u9001")) {
        score += 180;
    }
    if (ContainsAutoReplyToken(name, L"\u8bc4\u8bba")) {
        score += 40;
    }
    if (ContainsAutoReplyToken(name, L"\u4e92\u52a8\u6d88\u606f")) {
        score -= 120;
    }
    if (AutoReplyElementHasInvokePattern(element)) {
        score += 40;
    }

    if (width >= 36 && width <= 180) {
        score += 15;
    }
    if (height >= 24 && height <= 88) {
        score += 10;
    }
    if (bounds.top >= window_rect.bottom - 260) {
        score += 50;
    }

    if (input_rect.right > input_rect.left && input_rect.bottom > input_rect.top) {
        const int input_center_y = (input_rect.top + input_rect.bottom) / 2;
        const int center_y = (bounds.top + bounds.bottom) / 2;
        if (std::abs(center_y - input_center_y) <= 80) {
            score += 60;
        }
        if (bounds.left >= input_rect.right - 40) {
            score += 40;
        }
    }
    return score;
}

bool TrySetAutoReplyElementValue(IUIAutomationElement* element, const std::wstring& value) {
    if (element == nullptr || value.empty()) {
        return false;
    }

    winrt::com_ptr<IUIAutomationValuePattern> value_pattern;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(value_pattern.put()))) &&
        value_pattern) {
        BSTR value_bstr = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
        if (value_bstr == nullptr) {
            return false;
        }
        const HRESULT result = value_pattern->SetValue(value_bstr);
        SysFreeString(value_bstr);
        return SUCCEEDED(result);
    }

    UIA_HWND native_hwnd = nullptr;
    if (SUCCEEDED(element->get_CurrentNativeWindowHandle(&native_hwnd))) {
        const HWND hwnd = reinterpret_cast<HWND>(native_hwnd);
        if (hwnd == nullptr || !IsWindow(hwnd)) {
            return false;
        }
        const std::wstring class_name = ReadWindowClassForAutoReply(hwnd);
        if (ContainsAutoReplyToken(class_name, L"Edit")) {
            SendMessageW(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(value.c_str()));
            return true;
        }
    }

    return false;
}

bool TryInvokeAutoReplyElement(IUIAutomationElement* element) {
    if (element == nullptr) {
        return false;
    }

    winrt::com_ptr<IUIAutomationInvokePattern> invoke_pattern;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invoke_pattern.put()))) &&
        invoke_pattern) {
        return SUCCEEDED(invoke_pattern->Invoke());
    }
    return false;
}

void SendAutoReplyKey(WORD virtual_key) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = virtual_key;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = virtual_key;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(static_cast<UINT>(std::size(inputs)), inputs, sizeof(INPUT));
}

void SendAutoReplyCtrlChord(WORD virtual_key) {
    INPUT inputs[4]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = virtual_key;
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = virtual_key;
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(static_cast<UINT>(std::size(inputs)), inputs, sizeof(INPUT));
}

void SendAutoReplyUnicodeText(const std::wstring& text) {
    std::vector<INPUT> inputs;
    inputs.reserve(text.size() * 2);
    for (wchar_t ch : text) {
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = ch;
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(down);

        INPUT up = down;
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(up);
    }
    if (!inputs.empty()) {
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }
}

bool ActivateAutoReplyWindow(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return false;
    }

    const HWND foreground_before = GetForegroundWindow();
    const DWORD current_thread_id = GetCurrentThreadId();
    const DWORD foreground_thread_id =
        foreground_before != nullptr ? GetWindowThreadProcessId(foreground_before, nullptr) : 0;
    const DWORD target_thread_id = GetWindowThreadProcessId(hwnd, nullptr);

    bool attached_foreground = false;
    bool attached_target = false;
    if (foreground_thread_id != 0 && foreground_thread_id != current_thread_id) {
        attached_foreground = AttachThreadInput(current_thread_id, foreground_thread_id, TRUE) != FALSE;
    }
    if (target_thread_id != 0 &&
        target_thread_id != current_thread_id &&
        target_thread_id != foreground_thread_id) {
        attached_target = AttachThreadInput(current_thread_id, target_thread_id, TRUE) != FALSE;
    }

    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
    SetWindowPos(
        hwnd,
        HWND_TOP,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    BringWindowToTop(hwnd);
    SetActiveWindow(hwnd);
    SetForegroundWindow(hwnd);
    Sleep(80);

    const HWND foreground = GetForegroundWindow();
    const bool activated =
        foreground == hwnd ||
        (foreground != nullptr && (IsChild(hwnd, foreground) || IsChild(foreground, hwnd)));

    if (attached_target) {
        AttachThreadInput(current_thread_id, target_thread_id, FALSE);
    }
    if (attached_foreground) {
        AttachThreadInput(current_thread_id, foreground_thread_id, FALSE);
    }

    if (activated) {
        return true;
    }

    RECT window_rect{};
    return GetWindowRect(hwnd, &window_rect) != FALSE &&
        IsWindowVisible(hwnd) &&
        !IsIconic(hwnd) &&
        window_rect.right > window_rect.left &&
        window_rect.bottom > window_rect.top;
}

int ClampAutoReplyPermille(int value) {
    return std::clamp(value, 0, 1000);
}

const wchar_t* AutoReplyCaptureTargetToJson(AutoReplyCaptureTarget target) {
    switch (target) {
    case AutoReplyCaptureTarget::kInput:
        return L"input";
    case AutoReplyCaptureTarget::kSend:
        return L"send";
    default:
        return L"";
    }
}

POINT BuildAutoReplyPointFromPermille(const RECT& window_rect, int x_permille, int y_permille) {
    const LONG width = std::max<LONG>(0, window_rect.right - window_rect.left);
    const LONG height = std::max<LONG>(0, window_rect.bottom - window_rect.top);
    POINT point{};
    point.x = window_rect.left +
        static_cast<int>(std::llround(static_cast<double>(width) * static_cast<double>(x_permille) / 1000.0));
    point.y = window_rect.top +
        static_cast<int>(std::llround(static_cast<double>(height) * static_cast<double>(y_permille) / 1000.0));
    return point;
}

POINT BuildAutoReplyInputAnchorPoint(const RECT& window_rect, AppState* state) {
    int x_permille = 329;
    int y_permille = 927;
    if (state != nullptr) {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        x_permille = ClampAutoReplyPermille(state->auto_reply_input_point_x_permille);
        y_permille = ClampAutoReplyPermille(state->auto_reply_input_point_y_permille);
    }
    return BuildAutoReplyPointFromPermille(window_rect, x_permille, y_permille);
}

std::wstring BuildAutoReplyPointText(
    AutoReplyCaptureTarget current_target,
    bool configured,
    int x_permille,
    int y_permille,
    bool capture_running,
    AutoReplyCaptureTarget running_target) {
    const bool is_input = current_target == AutoReplyCaptureTarget::kInput;
    if (capture_running && running_target == current_target) {
        return is_input
            ? L"\u6b63\u5728\u8bb0\u5f55\uff0c\u8bf7\u70b9\u51fb\u201c\u6dfb\u52a0\u8bc4\u8bba\u2026\u201d\u8f93\u5165\u6846"
            : L"\u6b63\u5728\u8bb0\u5f55\uff0c\u8bf7\u70b9\u51fb\u201c\u53d1\u9001\u201d\u6309\u94ae";
    }

    if (!configured) {
        return is_input
            ? L"\u672a\u8bb0\u5f55\uff0c\u5f53\u524d\u4f7f\u7528\u9ed8\u8ba4\u8f93\u5165\u6846\u70b9\u4f4d"
            : L"\u672a\u8bb0\u5f55\uff0c\u5f53\u524d\u4f7f\u7528\u9ed8\u8ba4\u53d1\u9001\u6309\u94ae\u70b9\u4f4d";
    }

    std::wostringstream stream;
    stream.setf(std::ios::fixed);
    stream << std::setprecision(1)
           << L"\u5df2\u8bb0\u5f55 X " << (static_cast<double>(x_permille) / 10.0)
           << L"%  Y " << (static_cast<double>(y_permille) / 10.0) << L"%";
    return stream.str();
}

POINT BuildAutoReplySendButtonPoint(const RECT& window_rect, AppState* state) {
    int x_permille = kAutoReplySendPointXPermilleDefault;
    int y_permille = kAutoReplySendPointYPermilleDefault;
    if (state != nullptr) {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        x_permille = ClampAutoReplyPermille(state->auto_reply_send_point_x_permille);
        y_permille = ClampAutoReplyPermille(state->auto_reply_send_point_y_permille);
    }

    return BuildAutoReplyPointFromPermille(window_rect, x_permille, y_permille);
}

void ClickAutoReplyAnchorPoint(const POINT& point) {
    POINT original_cursor{};
    const bool has_original_cursor = GetCursorPos(&original_cursor) != FALSE;

    SetCursorPos(point.x, point.y);
    Sleep(90);

    INPUT inputs[2]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(static_cast<UINT>(std::size(inputs)), inputs, sizeof(INPUT));
    Sleep(90);

    if (has_original_cursor) {
        SetCursorPos(original_cursor.x, original_cursor.y);
    }
}

bool WriteAutoReplyClipboardText(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) {
        return false;
    }

    if (!EmptyClipboard()) {
        CloseClipboard();
        return false;
    }

    const SIZE_T byte_size = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, byte_size);
    if (memory == nullptr) {
        CloseClipboard();
        return false;
    }

    auto* buffer = static_cast<wchar_t*>(GlobalLock(memory));
    if (buffer == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }

    std::copy(text.c_str(), text.c_str() + text.size() + 1, buffer);
    GlobalUnlock(memory);

    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}

std::wstring ReadAutoReplyClipboardText() {
    if (!OpenClipboard(nullptr)) {
        return {};
    }

    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle == nullptr) {
        CloseClipboard();
        return {};
    }

    auto* data = static_cast<const wchar_t*>(GlobalLock(handle));
    if (data == nullptr) {
        CloseClipboard();
        return {};
    }

    std::wstring text(data);
    GlobalUnlock(handle);
    CloseClipboard();
    return TrimWhitespace(text);
}

void RestoreAutoReplyClipboard(IDataObject* original_clipboard, bool restore_available) {
    if (!restore_available) {
        return;
    }
    if (original_clipboard != nullptr) {
        if (SUCCEEDED(OleSetClipboard(original_clipboard))) {
            OleFlushClipboard();
        }
        return;
    }

    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        CloseClipboard();
    }
}

std::wstring ReadAutoReplyInputTextByClipboard(
    AppState* state,
    const RECT& window_rect,
    IDataObject* original_clipboard,
    bool restore_available) {
    const POINT anchor = BuildAutoReplyInputAnchorPoint(window_rect, state);
    ClickAutoReplyAnchorPoint(anchor);
    Sleep(120);
    SendAutoReplyCtrlChord(static_cast<WORD>('A'));
    Sleep(80);
    SendAutoReplyCtrlChord(static_cast<WORD>('C'));
    Sleep(150);
    const std::wstring copied_text = ReadAutoReplyClipboardText();
    RestoreAutoReplyClipboard(original_clipboard, restore_available);
    return copied_text;
}

bool SendAutoReplyByAnchor(
    AppState* state,
    HWND target_window,
    const RECT& window_rect,
    const std::wstring& reply_text,
    std::wstring* error_text) {
    if (target_window == nullptr || !IsWindow(target_window)) {
        if (error_text != nullptr) {
            *error_text = L"\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\u65e0\u6548";
        }
        return false;
    }

    winrt::com_ptr<IDataObject> original_clipboard;
    const bool restore_clipboard =
        SUCCEEDED(OleGetClipboard(original_clipboard.put())) || original_clipboard != nullptr;

    const POINT anchor = BuildAutoReplyInputAnchorPoint(window_rect, state);
    ClickAutoReplyAnchorPoint(anchor);
    Sleep(kAutoReplyInputFocusDelayMs);

    SendAutoReplyCtrlChord(static_cast<WORD>('A'));
    Sleep(kAutoReplyInputAfterClearDelayMs);
    SendAutoReplyKey(VK_BACK);
    Sleep(kAutoReplyInputAfterClearDelayMs);

    if (!WriteAutoReplyClipboardText(reply_text)) {
        RestoreAutoReplyClipboard(original_clipboard.get(), restore_clipboard);
        if (error_text != nullptr) {
            *error_text = L"\u65e0\u6cd5\u5199\u5165\u81ea\u52a8\u56de\u590d\u526a\u8d34\u677f";
        }
        return false;
    }

    SendAutoReplyCtrlChord(static_cast<WORD>('V'));
    Sleep(kAutoReplyInputAfterPasteDelayMs);
    RestoreAutoReplyClipboard(original_clipboard.get(), restore_clipboard);
    Sleep(kAutoReplyInputAfterClearDelayMs);

    const POINT send_button = BuildAutoReplySendButtonPoint(window_rect, state);
    ClickAutoReplyAnchorPoint(send_button);
    Sleep(kAutoReplyAfterSendCheckDelayMs);

    const std::wstring read_back_text =
        ReadAutoReplyInputTextByClipboard(state, window_rect, original_clipboard.get(), restore_clipboard);
    if (TrimWhitespace(read_back_text) == TrimWhitespace(reply_text)) {
        if (error_text != nullptr) {
            *error_text = L"\u53d1\u9001\u6309\u94ae\u70b9\u4f4d\u672a\u547d\u4e2d\uff0c\u8bf7\u5148\u8bb0\u5f55\u53d1\u9001\u6309\u94ae";
        }
        return false;
    }

    return true;
}

bool SendAutoReplyText(
    AppState* state,
    const std::wstring& reply_text,
    std::wstring* target_title,
    std::wstring* error_text) {
    if (reply_text.empty()) {
        if (error_text != nullptr) {
            *error_text = L"\u56de\u590d\u5185\u5bb9\u4e3a\u7a7a";
        }
        return false;
    }

    const auto candidates = CollectAutoReplyWindows(state);
    if (candidates.empty()) {
        if (error_text != nullptr) {
            *error_text = L"\u672a\u627e\u5230\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97";
        }
        return false;
    }

    const auto& target = candidates.front();
    if (target_title != nullptr) {
        *target_title = !target.title.empty() ? target.title : target.class_name;
    }

    RECT window_rect{};
    if (!GetWindowRect(target.hwnd, &window_rect) ||
        window_rect.right <= window_rect.left ||
        window_rect.bottom <= window_rect.top) {
        if (error_text != nullptr) {
            *error_text = L"\u65e0\u6cd5\u83b7\u53d6\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\u4f4d\u7f6e";
        }
        return false;
    }

    if (!ActivateAutoReplyWindow(target.hwnd)) {
        AppendAutoReplyRecentLog(
            state,
            L"\u53d1\u9001\u524d\u672a\u80fd\u5207\u5230\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\uff0c\u6539\u7528\u5750\u6807\u70b9\u51fb\u7ee7\u7eed\u53d1\u9001");
    }

    const HRESULT ole_result = OleInitialize(nullptr);
    if (FAILED(ole_result) && ole_result != RPC_E_CHANGED_MODE) {
        if (error_text != nullptr) {
            *error_text = L"\u81ea\u52a8\u56de\u590d OLE \u521d\u59cb\u5316\u5931\u8d25";
        }
        return false;
    }

    const bool succeeded = SendAutoReplyByAnchor(state, target.hwnd, window_rect, reply_text, error_text);

    if (SUCCEEDED(ole_result)) {
        OleUninitialize();
    }
    return succeeded;
}

void EnsureAutoReplyScaffold(AppState* state) {
    if (state == nullptr || state->auto_reply_root_path.empty()) {
        return;
    }

    if (!EnsureDirectoryPath(state->auto_reply_root_path)) {
        return;
    }

    const std::wstring note_path = state->auto_reply_root_path + L"\\\u81ea\u52a8\u56de\u590d\u8bf4\u660e.txt";
    if (!FileExists(note_path)) {
        const std::string note_text = WideToUtf8(
            L"\u5f39\u5e55\u81ea\u52a8\u56de\u590d\u8bf4\u660e\r\n"
            L"========================\r\n"
            L"1. \u7cbe\u786e\u5339\u914d\uff1a\u6bcf\u884c\u4e00\u6761\uff0c\u5199\u6210 \u5b8c\u6574\u5f39\u5e55=\u56de\u590d1|\u56de\u590d2\r\n"
            L"2. \u5173\u952e\u8bcd\u5339\u914d\uff1a\u6bcf\u884c\u4e00\u6761\uff0c\u5199\u6210 \u5173\u952e\u8bcd1|\u5173\u952e\u8bcd2=\u56de\u590d1|\u56de\u590d2\r\n"
            L"3. \u5173\u952e\u8bcd\u547d\u4e2d\u591a\u6761\u65f6\uff0c\u4f1a\u4f18\u5148\u9009\u66f4\u957f\u3001\u66f4\u5177\u4f53\u7684\u5173\u952e\u8bcd\r\n"
            L"4. \u9ed8\u8ba4\u56de\u590d\u652f\u6301\u6309\u884c\u6216 | \u968f\u673a\u62bd\u53d6\uff0c\u5728\u4e24\u79cd\u89c4\u5219\u90fd\u6ca1\u547d\u4e2d\u65f6\u4f7f\u7528\r\n"
            L"5. \u53ef\u7528\u5360\u4f4d\u7b26\uff1a{name} {speaker} {content}\r\n"
            L"6. \u201c\u53d1\u9001\u6d88\u606f\u201d\u5173\u95ed\u65f6\uff0c\u53ea\u505a\u89c4\u5219\u9884\u89c8\uff0c\u4e0d\u4f1a\u771f\u6b63\u53d1\u9001\r\n");
        WriteUtf8File(note_path, note_text);
    }

    if (!FileExists(state->auto_reply_config_path)) {
        SaveAutoReplyConfig(state);
    }
}

void LoadAutoReplyConfig(AppState* state) {
    if (state == nullptr || state->auto_reply_config_path.empty()) {
        return;
    }

    EnsureDirectoryPath(state->auto_reply_root_path);
    const std::string json_utf8 = ReadUtf8File(state->auto_reply_config_path);
    if (json_utf8.empty()) {
        SaveAutoReplyConfig(state);
        return;
    }

    const std::wstring json_text = WideFromUtf8(json_utf8);
    bool enabled = ExtractAutoReplyJsonBool(json_text, L"enabled", false);
    bool send_enabled = ExtractAutoReplyJsonBool(json_text, L"sendEnabled", false);
    if (!enabled) {
        send_enabled = false;
    }

    {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        state->auto_reply_enabled = enabled;
        state->auto_reply_send_enabled = send_enabled;
        state->auto_reply_cooldown_seconds =
            ClampAutoReplyCooldown(ExtractAutoReplyJsonInt(json_text, L"cooldownSeconds", 12));
        state->auto_reply_delay_ms =
            ClampAutoReplyDelay(ExtractAutoReplyJsonInt(json_text, L"delayMs", 1600));
        state->auto_reply_input_point_configured =
            ExtractAutoReplyJsonBool(json_text, L"inputPointConfigured", false);
        state->auto_reply_input_point_x_permille = ClampAutoReplyPermille(
            ExtractAutoReplyJsonInt(json_text, L"inputPointXPermille", 329));
        state->auto_reply_input_point_y_permille = ClampAutoReplyPermille(
            ExtractAutoReplyJsonInt(json_text, L"inputPointYPermille", 927));
        state->auto_reply_send_point_configured =
            ExtractAutoReplyJsonBool(json_text, L"sendPointConfigured", false);
        state->auto_reply_send_point_x_permille = ClampAutoReplyPermille(
            ExtractAutoReplyJsonInt(json_text, L"sendPointXPermille", kAutoReplySendPointXPermilleDefault));
        state->auto_reply_send_point_y_permille = ClampAutoReplyPermille(
            ExtractAutoReplyJsonInt(json_text, L"sendPointYPermille", kAutoReplySendPointYPermilleDefault));
        state->auto_reply_exact_rules_text = ExtractAutoReplyJsonString(json_text, L"exactRulesText");
        size_t keyword_rules_value_pos = 0;
        if (LocateAutoReplyJsonValue(json_text, L"keywordRulesText", &keyword_rules_value_pos)) {
            state->auto_reply_keyword_rules_text = ExtractAutoReplyJsonString(json_text, L"keywordRulesText");
        } else {
            state->auto_reply_keyword_rules_text = ExtractAutoReplyJsonString(json_text, L"rulesText");
        }
        state->auto_reply_fallback_text = ExtractAutoReplyJsonString(json_text, L"fallbackRepliesText");
        state->auto_reply_status = BuildAutoReplyModeText(
            state->auto_reply_enabled,
            state->auto_reply_send_enabled);
    }

    RequestAutoReplySnapshotRefresh(state);
}

void SaveAutoReplyConfig(AppState* state) {
    if (state == nullptr || state->auto_reply_config_path.empty()) {
        return;
    }

    EnsureDirectoryPath(state->auto_reply_root_path);

    bool enabled = false;
    bool send_enabled = false;
    int cooldown_seconds = 12;
    int delay_ms = 1600;
    bool input_point_configured = false;
    int input_point_x_permille = 329;
    int input_point_y_permille = 927;
    bool send_point_configured = false;
    int send_point_x_permille = kAutoReplySendPointXPermilleDefault;
    int send_point_y_permille = kAutoReplySendPointYPermilleDefault;
    std::wstring exact_rules_text;
    std::wstring keyword_rules_text;
    std::wstring fallback_text;
    {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        enabled = state->auto_reply_enabled;
        send_enabled = state->auto_reply_send_enabled;
        cooldown_seconds = ClampAutoReplyCooldown(state->auto_reply_cooldown_seconds);
        delay_ms = ClampAutoReplyDelay(state->auto_reply_delay_ms);
        input_point_configured = state->auto_reply_input_point_configured;
        input_point_x_permille = ClampAutoReplyPermille(state->auto_reply_input_point_x_permille);
        input_point_y_permille = ClampAutoReplyPermille(state->auto_reply_input_point_y_permille);
        send_point_configured = state->auto_reply_send_point_configured;
        send_point_x_permille = ClampAutoReplyPermille(state->auto_reply_send_point_x_permille);
        send_point_y_permille = ClampAutoReplyPermille(state->auto_reply_send_point_y_permille);
        exact_rules_text = state->auto_reply_exact_rules_text;
        keyword_rules_text = state->auto_reply_keyword_rules_text;
        fallback_text = state->auto_reply_fallback_text;
    }

    std::ostringstream json;
    json << "{\n";
    json << "  \"enabled\": " << (enabled ? "true" : "false") << ",\n";
    json << "  \"sendEnabled\": " << (send_enabled ? "true" : "false") << ",\n";
    json << "  \"cooldownSeconds\": " << cooldown_seconds << ",\n";
    json << "  \"delayMs\": " << delay_ms << ",\n";
    json << "  \"inputPointConfigured\": " << (input_point_configured ? "true" : "false") << ",\n";
    json << "  \"inputPointXPermille\": " << input_point_x_permille << ",\n";
    json << "  \"inputPointYPermille\": " << input_point_y_permille << ",\n";
    json << "  \"sendPointConfigured\": " << (send_point_configured ? "true" : "false") << ",\n";
    json << "  \"sendPointXPermille\": " << send_point_x_permille << ",\n";
    json << "  \"sendPointYPermille\": " << send_point_y_permille << ",\n";
    json << "  \"exactRulesText\": " << QuoteJsonWide(exact_rules_text) << ",\n";
    json << "  \"keywordRulesText\": " << QuoteJsonWide(keyword_rules_text) << ",\n";
    json << "  \"rulesText\": " << QuoteJsonWide(keyword_rules_text) << ",\n";
    json << "  \"fallbackRepliesText\": " << QuoteJsonWide(fallback_text) << "\n";
    json << "}\n";
    WriteUtf8File(state->auto_reply_config_path, json.str());
}

void OpenAutoReplyDirectory(AppState* state) {
    if (state == nullptr) {
        return;
    }
    EnsureAutoReplyScaffold(state);
    OpenDirectoryPath(state, state->auto_reply_root_path);
    AppendAutoReplyRecentLog(state, L"\u5df2\u6253\u5f00\u81ea\u52a8\u56de\u590d\u76ee\u5f55");
}

void AutoReplySendPointCaptureLoop(AppState* state) {
    if (state == nullptr) {
        return;
    }

    bool last_down = false;
    const ULONGLONG start_tick = GetTickCount64();
    for (;;) {
        bool stop_requested = false;
        AutoReplyCaptureTarget capture_target = AutoReplyCaptureTarget::kNone;
        {
            std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
            stop_requested = state->auto_reply_capture_stop_requested;
            capture_target = state->auto_reply_capture_target;
        }
        if (stop_requested) {
            break;
        }

        const bool capturing_input = capture_target == AutoReplyCaptureTarget::kInput;
        if (GetTickCount64() - start_tick >= kAutoReplySendPointCaptureTimeoutMs) {
            {
                std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
                state->auto_reply_capture_running = false;
                state->auto_reply_capture_stop_requested = false;
                state->auto_reply_capture_target = AutoReplyCaptureTarget::kNone;
                state->auto_reply_status = capturing_input
                    ? L"\u8bb0\u5f55\u8f93\u5165\u6846\u8d85\u65f6"
                    : L"\u8bb0\u5f55\u53d1\u9001\u6309\u94ae\u8d85\u65f6";
            }
            AppendAutoReplyRecentLog(
                state,
                capturing_input
                    ? L"\u8bb0\u5f55\u8f93\u5165\u6846\u8d85\u65f6\uff0c\u8bf7\u70b9\u51fb\u201c\u91cd\u65b0\u8bb0\u5f55\u201d\u540e\u518d\u8bd5"
                    : L"\u8bb0\u5f55\u53d1\u9001\u6309\u94ae\u8d85\u65f6\uff0c\u8bf7\u70b9\u51fb\u201c\u91cd\u65b0\u8bb0\u5f55\u201d\u540e\u518d\u8bd5");
            RequestAutoReplySnapshotRefresh(state);
            return;
        }

        const bool is_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (is_down && !last_down) {
            POINT cursor{};
            GetCursorPos(&cursor);

            const auto candidates = CollectAutoReplyWindows(state);
            if (!candidates.empty()) {
                const auto& target = candidates.front();
                RECT window_rect{};
                if (GetWindowRect(target.hwnd, &window_rect) &&
                    window_rect.right > window_rect.left &&
                    window_rect.bottom > window_rect.top &&
                    IsPointInsideRect(cursor, window_rect)) {
                    const int width = window_rect.right - window_rect.left;
                    const int height = window_rect.bottom - window_rect.top;
                    const int x_permille = ClampAutoReplyPermille(
                        static_cast<int>(std::lround(
                            static_cast<double>(cursor.x - window_rect.left) * 1000.0 / static_cast<double>(width))));
                    const int y_permille = ClampAutoReplyPermille(
                        static_cast<int>(std::lround(
                            static_cast<double>(cursor.y - window_rect.top) * 1000.0 / static_cast<double>(height))));

                    const std::wstring target_title = !target.title.empty() ? target.title : target.class_name;
                    {
                        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
                        state->auto_reply_capture_running = false;
                        state->auto_reply_capture_stop_requested = false;
                        state->auto_reply_capture_target = AutoReplyCaptureTarget::kNone;
                        state->auto_reply_target_title = target_title;
                        if (capturing_input) {
                            state->auto_reply_input_point_configured = true;
                            state->auto_reply_input_point_x_permille = x_permille;
                            state->auto_reply_input_point_y_permille = y_permille;
                            state->auto_reply_status = L"\u5df2\u8bb0\u5f55\u8f93\u5165\u6846\u4f4d\u7f6e";
                        } else {
                            state->auto_reply_send_point_configured = true;
                            state->auto_reply_send_point_x_permille = x_permille;
                            state->auto_reply_send_point_y_permille = y_permille;
                            state->auto_reply_status = L"\u5df2\u8bb0\u5f55\u53d1\u9001\u6309\u94ae\u4f4d\u7f6e";
                        }
                    }
                    SaveAutoReplyConfig(state);
                    AppendAutoReplyRecentLog(
                        state,
                        BuildAutoReplyPointText(
                            capturing_input ? AutoReplyCaptureTarget::kInput : AutoReplyCaptureTarget::kSend,
                            true,
                            x_permille,
                            y_permille,
                            false,
                            AutoReplyCaptureTarget::kNone));
                    RequestAutoReplySnapshotRefresh(state);
                    return;
                }
            }

            AppendAutoReplyRecentLog(
                state,
                capturing_input
                    ? L"\u70b9\u51fb\u6ca1\u6709\u843d\u5728\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\u91cc\uff0c\u8bf7\u91cd\u65b0\u70b9\u201c\u6dfb\u52a0\u8bc4\u8bba\u2026\u201d\u8f93\u5165\u6846"
                    : L"\u70b9\u51fb\u6ca1\u6709\u843d\u5728\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\u91cc\uff0c\u8bf7\u91cd\u65b0\u70b9\u53d1\u9001\u6309\u94ae");
        }

        last_down = is_down;
        Sleep(25);
    }

    {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        state->auto_reply_capture_running = false;
        state->auto_reply_capture_stop_requested = false;
        state->auto_reply_capture_target = AutoReplyCaptureTarget::kNone;
    }
    RequestAutoReplySnapshotRefresh(state);
}

void StartAutoReplyInputPointCapture(AppState* state) {
    if (state == nullptr) {
        return;
    }

    const auto candidates = CollectAutoReplyWindows(state);
    if (candidates.empty()) {
        AppendAutoReplyRecentLog(state, L"\u672a\u627e\u5230\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\uff0c\u65e0\u6cd5\u8bb0\u5f55\u8f93\u5165\u6846");
        return;
    }

    if (state->auto_reply_capture_thread.joinable()) {
        StopAutoReplySendPointCapture(state);
    }

    const auto& target = candidates.front();
    ActivateAutoReplyWindow(target.hwnd);
    {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        state->auto_reply_capture_running = true;
        state->auto_reply_capture_stop_requested = false;
        state->auto_reply_capture_target = AutoReplyCaptureTarget::kInput;
        state->auto_reply_target_title = !target.title.empty() ? target.title : target.class_name;
        state->auto_reply_status = L"\u8bf7\u70b9\u51fb\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\u5e95\u90e8\u7684\u201c\u6dfb\u52a0\u8bc4\u8bba\u2026\u201d\u8f93\u5165\u6846";
    }
    AppendAutoReplyRecentLog(
        state,
        L"\u5f00\u59cb\u8bb0\u5f55\u8f93\u5165\u6846\uff0c\u8bf7\u70b9\u51fb\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\u5e95\u90e8\u201c\u6dfb\u52a0\u8bc4\u8bba\u2026\u201d");
    RequestAutoReplySnapshotRefresh(state);

    state->auto_reply_capture_thread = std::thread(AutoReplySendPointCaptureLoop, state);
}

void StartAutoReplySendPointCapture(AppState* state) {
    if (state == nullptr) {
        return;
    }

    const auto candidates = CollectAutoReplyWindows(state);
    if (candidates.empty()) {
        AppendAutoReplyRecentLog(state, L"\u672a\u627e\u5230\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\uff0c\u65e0\u6cd5\u8bb0\u5f55\u53d1\u9001\u6309\u94ae");
        return;
    }

    if (state->auto_reply_capture_thread.joinable()) {
        StopAutoReplySendPointCapture(state);
    }

    const auto& target = candidates.front();
    ActivateAutoReplyWindow(target.hwnd);
    {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        state->auto_reply_capture_running = true;
        state->auto_reply_capture_stop_requested = false;
        state->auto_reply_capture_target = AutoReplyCaptureTarget::kSend;
        state->auto_reply_target_title = !target.title.empty() ? target.title : target.class_name;
        state->auto_reply_status = L"\u8bf7\u70b9\u51fb\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\u53f3\u4e0b\u89d2\u7684\u53d1\u9001\u6309\u94ae";
    }
    AppendAutoReplyRecentLog(
        state,
        L"\u5f00\u59cb\u8bb0\u5f55\u53d1\u9001\u6309\u94ae\uff0c\u8bf7\u70b9\u51fb\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\u53f3\u4e0b\u89d2\u201c\u53d1\u9001\u201d");
    RequestAutoReplySnapshotRefresh(state);

    state->auto_reply_capture_thread = std::thread(AutoReplySendPointCaptureLoop, state);
}

void StopAutoReplySendPointCapture(AppState* state) {
    if (state == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        state->auto_reply_capture_stop_requested = true;
    }

    if (state->auto_reply_capture_thread.joinable()) {
        state->auto_reply_capture_thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        state->auto_reply_capture_target = AutoReplyCaptureTarget::kNone;
    }
}

void ToggleAutoReplyEnabled(AppState* state) {
    if (state == nullptr) {
        return;
    }

    bool enabled = false;
    bool send_enabled = false;
    {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        state->auto_reply_enabled = !state->auto_reply_enabled;
        if (!state->auto_reply_enabled) {
            state->auto_reply_match_queue.clear();
            state->auto_reply_send_enabled = false;
            state->auto_reply_queue.clear();
            state->auto_reply_recent_echoes.clear();
        }
        enabled = state->auto_reply_enabled;
        send_enabled = state->auto_reply_send_enabled;
        state->auto_reply_status = BuildAutoReplyModeText(enabled, send_enabled);
    }

    if (enabled) {
        EnsureDanmakuReadyForAutoReply(state);
    } else {
        state->auto_reply_match_cv.notify_all();
        state->auto_reply_cv.notify_all();
    }

    SaveAutoReplyConfig(state);
    AppendAutoReplyRecentLog(
        state,
        enabled
            ? L"\u81ea\u52a8\u56de\u590d\u5df2\u5f00\u542f"
            : L"\u81ea\u52a8\u56de\u590d\u5df2\u5173\u95ed\uff0c\u5f85\u53d1\u9001\u961f\u5217\u5df2\u6e05\u7a7a");
}

void ToggleAutoReplySendEnabled(AppState* state) {
    if (state == nullptr) {
        return;
    }

    bool enabled = false;
    bool send_enabled = false;
    bool cleared_send_queue = false;
    {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        state->auto_reply_send_enabled = !state->auto_reply_send_enabled;
        if (state->auto_reply_send_enabled) {
            state->auto_reply_enabled = true;
        } else {
            state->auto_reply_queue.clear();
            state->auto_reply_recent_echoes.clear();
            cleared_send_queue = true;
        }
        enabled = state->auto_reply_enabled;
        send_enabled = state->auto_reply_send_enabled;
        state->auto_reply_status = BuildAutoReplyModeText(enabled, send_enabled);
    }

    if (enabled) {
        EnsureDanmakuReadyForAutoReply(state);
    } else {
        state->auto_reply_match_cv.notify_all();
        state->auto_reply_cv.notify_all();
    }
    if (cleared_send_queue) {
        state->auto_reply_cv.notify_all();
    }

    SaveAutoReplyConfig(state);
    AppendAutoReplyRecentLog(
        state,
        send_enabled
            ? L"\u81ea\u52a8\u56de\u590d\u53d1\u9001\u5df2\u5f00\u542f"
            : L"\u81ea\u52a8\u56de\u590d\u53d1\u9001\u5df2\u5173\u95ed\uff0c\u5df2\u5207\u56de\u4ec5\u9884\u89c8");
}

void SaveAutoReplyTextConfig(
    AppState* state,
    const std::wstring& rules_payload,
    const std::wstring& fallback_text) {
    if (state == nullptr) {
        return;
    }

    std::wstring exact_rules_text;
    std::wstring keyword_rules_text;
    std::wstring resolved_fallback_text = fallback_text;
    if (!rules_payload.empty() && rules_payload.front() == L'{') {
        exact_rules_text = ExtractAutoReplyJsonString(rules_payload, L"exactRulesText");
        size_t keyword_rules_value_pos = 0;
        if (LocateAutoReplyJsonValue(rules_payload, L"keywordRulesText", &keyword_rules_value_pos)) {
            keyword_rules_text = ExtractAutoReplyJsonString(rules_payload, L"keywordRulesText");
        } else {
            keyword_rules_text = ExtractAutoReplyJsonString(rules_payload, L"rulesText");
        }
        resolved_fallback_text = ExtractAutoReplyJsonString(rules_payload, L"fallbackRepliesText");
    } else {
        keyword_rules_text = rules_payload;
    }

    {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        state->auto_reply_exact_rules_text = exact_rules_text;
        state->auto_reply_keyword_rules_text = keyword_rules_text;
        state->auto_reply_fallback_text = resolved_fallback_text;
        state->auto_reply_status = BuildAutoReplyModeText(
            state->auto_reply_enabled,
            state->auto_reply_send_enabled);
    }

    SaveAutoReplyConfig(state);
    const size_t exact_rule_count = ParseAutoReplyRules(exact_rules_text).size();
    const size_t keyword_rule_count = ParseAutoReplyRules(keyword_rules_text).size();
    const size_t fallback_reply_count = ParseAutoReplyReplyPool(resolved_fallback_text).size();
    AppendAutoReplyRecentLog(
        state,
        std::wstring(L"\u5df2\u4fdd\u5b58\u81ea\u52a8\u56de\u590d\u89c4\u5219\uff0c\u7cbe\u786e ") +
            std::to_wstring(exact_rule_count) +
            L" \u6761\uff0c\u5173\u952e\u8bcd " +
            std::to_wstring(keyword_rule_count) +
            L" \u6761\uff0c\u9ed8\u8ba4 " +
            std::to_wstring(fallback_reply_count) +
            L" \u6761");
}

void SaveAutoReplyTimingConfig(AppState* state, const std::wstring& timing_text) {
    if (state == nullptr) {
        return;
    }

    int cooldown_seconds = 12;
    int delay_ms = 1600;
    {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        cooldown_seconds = state->auto_reply_cooldown_seconds;
        delay_ms = state->auto_reply_delay_ms;
    }

    const auto parts = SplitAutoReplyByDelimiters(timing_text);
    if (!parts.empty()) {
        try {
            cooldown_seconds = std::stoi(parts[0]);
        } catch (...) {
        }
    }
    if (parts.size() >= 2) {
        try {
            delay_ms = std::stoi(parts[1]);
        } catch (...) {
        }
    }

    cooldown_seconds = ClampAutoReplyCooldown(cooldown_seconds);
    delay_ms = ClampAutoReplyDelay(delay_ms);
    {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        state->auto_reply_cooldown_seconds = cooldown_seconds;
        state->auto_reply_delay_ms = delay_ms;
        state->auto_reply_status = BuildAutoReplyModeText(
            state->auto_reply_enabled,
            state->auto_reply_send_enabled);
    }

    SaveAutoReplyConfig(state);
    AppendAutoReplyRecentLog(
        state,
        std::wstring(L"\u5df2\u4fdd\u5b58\u65f6\u95f4\u8bbe\u7f6e\uff0c\u51b7\u5374 ") +
            std::to_wstring(cooldown_seconds) +
            L" \u79d2\uff0c\u5ef6\u65f6 " +
            std::to_wstring(delay_ms) +
            L" ms");
}

void HandleAutoReplyDanmakuEvent(AppState* state, bool is_gift, const std::wstring& text) {
    if (state == nullptr) {
        return;
    }

    const std::wstring source_text = TrimWhitespace(text);
    if (source_text.empty() || is_gift) {
        return;
    }

    bool should_queue = false;
    bool dropped_old = false;
    {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        if (!state->auto_reply_enabled || state->auto_reply_stop_requested) {
            return;
        }
        if (state->auto_reply_match_queue.size() >= kMaxAutoReplyMatchQueueSize) {
            state->auto_reply_match_queue.pop_front();
            dropped_old = true;
        }
        state->auto_reply_match_queue.push_back(PendingAutoReplyEvent{
            false,
            source_text,
            GetTickCount64(),
        });
        state->auto_reply_last_match_text = source_text;
        state->auto_reply_status = L"\u6536\u5230\u65b0\u5f39\u5e55\uff0c\u6b63\u5728\u5339\u914d\u81ea\u52a8\u56de\u590d\u89c4\u5219";
        should_queue = true;
    }

    if (dropped_old) {
        AppendAutoReplyRecentLog(state, L"\u5339\u914d\u961f\u5217\u5df2\u6ee1\uff0c\u5df2\u4e22\u5f03\u6700\u65e9\u4e00\u6761\u5f85\u5339\u914d\u5f39\u5e55");
    } else if (should_queue) {
        RequestAutoReplySnapshotRefresh(state);
    }
    state->auto_reply_match_cv.notify_one();
}

void AutoReplyMatchWorkerLoop(AppState* state) {
    if (state == nullptr) {
        return;
    }

    for (;;) {
        PendingAutoReplyEvent pending_event;
        {
            std::unique_lock<std::mutex> lock(state->auto_reply_mutex);
            state->auto_reply_match_cv.wait(lock, [state] {
                return state->auto_reply_stop_requested || !state->auto_reply_match_queue.empty();
            });
            if (state->auto_reply_stop_requested) {
                return;
            }
            pending_event = std::move(state->auto_reply_match_queue.front());
            state->auto_reply_match_queue.pop_front();
        }

        const std::wstring source_text = TrimWhitespace(pending_event.text);
        if (source_text.empty() || pending_event.is_gift) {
            continue;
        }

        bool enabled = false;
        std::wstring exact_rules_text;
        std::wstring keyword_rules_text;
        std::wstring fallback_text;
        {
            std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
            enabled = state->auto_reply_enabled;
            exact_rules_text = state->auto_reply_exact_rules_text;
            keyword_rules_text = state->auto_reply_keyword_rules_text;
            fallback_text = state->auto_reply_fallback_text;
        }
        if (!enabled) {
            continue;
        }

        std::wstring speaker;
        std::wstring content;
        ParseAutoReplySpeakerContent(source_text, &speaker, &content);
        const ULONGLONG event_tick = pending_event.received_tick != 0 ? pending_event.received_tick : GetTickCount64();

        bool ignore_recent_echo = false;
        {
            std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
            ignore_recent_echo = IsRecentAutoReplyEchoLocked(state, source_text, content, event_tick);
            if (ignore_recent_echo) {
                state->auto_reply_last_match_text = source_text;
                state->auto_reply_status = L"\u5df2\u5ffd\u7565\u521a\u53d1\u51fa\u7684\u81ea\u52a8\u56de\u590d\u56de\u663e";
            }
        }
        if (ignore_recent_echo) {
            AppendAutoReplyRecentLog(
                state,
                std::wstring(L"\u5ffd\u7565\u81ea\u52a8\u56de\u590d\u56de\u663e\uff1a") + source_text);
            continue;
        }

        const auto exact_rules = ParseAutoReplyRules(exact_rules_text);
        const auto keyword_rules = ParseAutoReplyRules(keyword_rules_text);
        const auto fallback_replies = ParseAutoReplyReplyPool(fallback_text);
        std::wstring matched_label;
        AutoReplyMatchKind match_kind = AutoReplyMatchKind::kNone;
        std::wstring reply_text = MatchAutoReplyText(
            exact_rules,
            keyword_rules,
            fallback_replies,
            source_text,
            content,
            &matched_label,
            &match_kind);
        reply_text = RenderAutoReplyTemplate(reply_text, speaker, content, source_text);
        if (reply_text.empty()) {
            {
                std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
                state->auto_reply_last_match_text = source_text;
                state->auto_reply_status = L"\u672a\u547d\u4e2d\u81ea\u52a8\u56de\u590d\u89c4\u5219";
            }
            RequestAutoReplySnapshotRefresh(state);
            continue;
        }

        std::wstring target_title;
        if (state->danmaku_controller != nullptr) {
            const auto snapshot = state->danmaku_controller->GetSnapshot();
            target_title = snapshot.ui_probe_target_title;
        }

        bool should_queue_send = false;
        bool dropped_old = false;
        bool used_send_enabled = false;
        {
            std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
            if (!state->auto_reply_enabled) {
                continue;
            }

            const ULONGLONG cooldown_ms =
                static_cast<ULONGLONG>(std::max(0, state->auto_reply_cooldown_seconds)) * 1000ULL;
            if (cooldown_ms > 0 &&
                state->auto_reply_last_trigger_tick != 0 &&
                event_tick - state->auto_reply_last_trigger_tick < cooldown_ms) {
                state->auto_reply_last_match_text = source_text;
                state->auto_reply_status = L"\u51b7\u5374\u4e2d\uff0c\u5f53\u524d\u5ffd\u7565\u65b0\u5f39\u5e55";
                RequestAutoReplySnapshotRefresh(state);
                continue;
            }

            state->auto_reply_last_trigger_tick = event_tick;
            state->auto_reply_last_match_text = source_text;
            state->auto_reply_last_reply_text = reply_text;
            if (!target_title.empty()) {
                state->auto_reply_target_title = target_title;
            }

            used_send_enabled = state->auto_reply_send_enabled;
            if (used_send_enabled) {
                if (state->auto_reply_queue.size() >= kMaxAutoReplyQueueSize) {
                    state->auto_reply_queue.pop_front();
                    dropped_old = true;
                }
                PendingAutoReplyJob job;
                job.source_text = source_text;
                job.speaker = speaker;
                job.content = content;
                job.reply_text = reply_text;
                job.due_tick = event_tick + static_cast<ULONGLONG>(std::max(0, state->auto_reply_delay_ms));
                state->auto_reply_queue.push_back(std::move(job));
                state->auto_reply_status = L"\u5df2\u5339\u914d\u89c4\u5219\uff0c\u7b49\u5f85\u53d1\u9001\u81ea\u52a8\u56de\u590d";
                should_queue_send = true;
            } else {
                state->auto_reply_status = L"\u5df2\u5339\u914d\u89c4\u5219\uff0c\u5f53\u524d\u4ec5\u9884\u89c8";
            }
        }

        if (dropped_old) {
            AppendAutoReplyRecentLog(state, L"\u961f\u5217\u5df2\u6ee1\uff0c\u5df2\u4e22\u5f03\u6700\u65e9\u4e00\u6761\u5f85\u53d1\u9001\u56de\u590d");
        }

        std::wstring log_line =
            std::wstring(L"\u547d\u4e2d[") +
            BuildAutoReplyMatchKindText(match_kind) +
            (matched_label.empty() ? std::wstring{} : std::wstring(L":") + matched_label) +
            L"] \u2192 " +
            reply_text;

        if (should_queue_send) {
            state->auto_reply_cv.notify_one();
            AppendAutoReplyRecentLog(state, log_line + L"\uff0c\u5df2\u5165\u961f");
        } else if (used_send_enabled) {
            RequestAutoReplySnapshotRefresh(state);
        } else {
            AppendAutoReplyRecentLog(state, log_line + L"\uff0c\u4ec5\u9884\u89c8");
        }
    }
}

void AutoReplyWorkerLoop(AppState* state) {
    if (state == nullptr) {
        return;
    }

    for (;;) {
        PendingAutoReplyJob job;
        {
            std::unique_lock<std::mutex> lock(state->auto_reply_mutex);
            for (;;) {
                if (state->auto_reply_stop_requested) {
                    return;
                }

                if (state->auto_reply_queue.empty()) {
                    state->auto_reply_cv.wait(lock, [state] {
                        return state->auto_reply_stop_requested || !state->auto_reply_queue.empty();
                    });
                    continue;
                }

                const ULONGLONG now_tick = GetTickCount64();
                const ULONGLONG due_tick = state->auto_reply_queue.front().due_tick;
                if (due_tick > now_tick) {
                    state->auto_reply_cv.wait_for(
                        lock,
                        std::chrono::milliseconds(static_cast<long long>(due_tick - now_tick)));
                    continue;
                }

                job = std::move(state->auto_reply_queue.front());
                state->auto_reply_queue.pop_front();
                state->auto_reply_status = L"\u6b63\u5728\u53d1\u9001\u81ea\u52a8\u56de\u590d";
                break;
            }
        }

        RequestAutoReplySnapshotRefresh(state);

        std::wstring target_title;
        std::wstring error_text;
        const bool send_ok = SendAutoReplyText(state, job.reply_text, &target_title, &error_text);
        {
            std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
            if (!target_title.empty()) {
                state->auto_reply_target_title = target_title;
            }
            if (send_ok) {
                RememberRecentAutoReplyEchoLocked(state, job.reply_text, GetTickCount64());
            }
            state->auto_reply_status =
                send_ok
                    ? L"\u81ea\u52a8\u56de\u590d\u5df2\u53d1\u9001"
                    : (std::wstring(L"\u81ea\u52a8\u56de\u590d\u53d1\u9001\u5931\u8d25\uff1a") +
                       (error_text.empty() ? L"\u672a\u77e5\u539f\u56e0" : error_text));
        }

        if (send_ok) {
            AppendAutoReplyRecentLog(
                state,
                std::wstring(L"\u5df2\u53d1\u9001\u5230\u300a") +
                    (!target_title.empty() ? target_title : L"\u76ee\u6807\u7a97\u53e3") +
                    L"\u300b\uff1a" +
                    job.reply_text);
        } else {
            AppendAutoReplyRecentLog(
                state,
                std::wstring(L"\u53d1\u9001\u5931\u8d25\uff1a") +
                    (error_text.empty() ? L"\u672a\u77e5\u539f\u56e0" : error_text));
        }
    }
}

void StopAutoReplyWorker(AppState* state) {
    if (state == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        state->auto_reply_stop_requested = true;
        state->auto_reply_match_queue.clear();
        state->auto_reply_queue.clear();
        state->auto_reply_recent_echoes.clear();
    }
    state->auto_reply_match_cv.notify_all();
    state->auto_reply_cv.notify_all();
    if (state->auto_reply_match_thread.joinable()) {
        state->auto_reply_match_thread.join();
    }
    if (state->auto_reply_thread.joinable()) {
        state->auto_reply_thread.join();
    }
}

void ExecuteEmbeddedWebUiAction(
    AppState* state,
    const std::wstring& action,
    const std::wstring& value,
    const std::wstring& extra);
std::string BuildEmbeddedWebUiSnapshotJson(AppState* state);
void RequestKeyframe(AppState* state);
bool FileExists(const std::wstring& path);
std::string WideToUtf8(const std::wstring& value);
std::wstring WideFromUtf8(const std::string& value);
std::string ReadUtf8File(const std::wstring& path);
void WriteUtf8File(const std::wstring& path, const std::string& utf8_text);
std::string QuoteJsonWide(const std::wstring& value);
std::wstring ExtractJsonStringField(const std::string& json_text, const char* field_name);
std::wstring NormalizeFullPath(const std::wstring& path);

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

COLORREF BlendColor(COLORREF from, COLORREF to, int to_percent) {
    const int clamped = std::clamp(to_percent, 0, 100);
    const int from_percent = 100 - clamped;
    return RGB(
        (GetRValue(from) * from_percent + GetRValue(to) * clamped) / 100,
        (GetGValue(from) * from_percent + GetGValue(to) * clamped) / 100,
        (GetBValue(from) * from_percent + GetBValue(to) * clamped) / 100);
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

std::wstring BuildAudioProfileText(const protocol::StreamProfile* profile, bool has_profile) {
    if (!has_profile || profile == nullptr) {
        return L"\u7B49\u5F85\u534F\u5546";
    }
    if (!profile->audio_enabled || profile->audio_port <= 0 || profile->audio_sample_rate <= 0 || profile->audio_channels <= 0) {
        return L"\u5173\u95ED";
    }

    std::wostringstream stream;
    stream << profile->audio_sample_rate
           << L"Hz / "
           << profile->audio_channels
           << L"ch / UDP/"
           << profile->audio_port;
    return stream.str();
}

std::wstring BuildAudioRuntimeText(
    const protocol::StreamProfile* profile,
    bool has_profile,
    const AudioPlaybackStats& stats) {
    if (!has_profile || profile == nullptr) {
        return L"\u7B49\u5F85\u534F\u5546";
    }
    if (!profile->audio_enabled || profile->audio_port <= 0 || profile->audio_sample_rate <= 0 || profile->audio_channels <= 0) {
        return L"\u5173\u95ED";
    }

    std::wostringstream stream;
    stream << profile->audio_sample_rate
           << L"Hz/"
           << profile->audio_channels
           << L"ch"
           << L"\uFF0C\u7F13\u51B2 "
           << stats.buffered_frames
           << L" \u5E27 / "
           << stats.buffered_ms
           << L" ms"
           << L"\uFF0C\u4E22\u5F03 "
           << stats.dropped_frames;
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

std::wstring TrimWhitespace(const std::wstring& text) {
    const size_t start = text.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) {
        return {};
    }
    const size_t end = text.find_last_not_of(L" \t\r\n");
    return text.substr(start, end - start + 1);
}

SenderIdentity ParseSenderIdentity(const std::wstring& raw_name) {
    SenderIdentity identity{};
    const std::wstring trimmed = TrimWhitespace(raw_name);
    if (trimmed.empty()) {
        return identity;
    }

    if (!trimmed.empty() && trimmed.back() == L']') {
        const size_t open_bracket = trimmed.find_last_of(L'[');
        if (open_bracket != std::wstring::npos && open_bracket + 1 < trimmed.size() - 1) {
            identity.display_name = TrimWhitespace(trimmed.substr(0, open_bracket));
            identity.version_label = TrimWhitespace(trimmed.substr(open_bracket + 1, trimmed.size() - open_bracket - 2));
            if (!identity.display_name.empty()) {
                return identity;
            }
        }
    }

    identity.display_name = trimmed;
    return identity;
}

const wchar_t* StatusPageLabel(StatusPage page) {
    switch (page) {
    case StatusPage::kConnect:
        return kTextConnectPage;
    case StatusPage::kDisplay:
        return kTextDisplayPage;
    case StatusPage::kVoiceControl:
        return kTextVoicePage;
    case StatusPage::kDiagnostics:
        return kTextDiagnosticsPage;
    case StatusPage::kLogs:
        return kTextLogsPage;
    case StatusPage::kOverview:
    default:
        return kTextOverviewPage;
    }
}

const wchar_t* StatusPageSummary(StatusPage page) {
    switch (page) {
    case StatusPage::kConnect:
        return L"\u8FDE\u63A5\u3001\u5730\u5740\u548C\u534F\u5546\u72B6\u6001\u5206\u9875\u67E5\u770B";
    case StatusPage::kDisplay:
        return L"\u6E90\u753B\u8D28\u3001client area \u548C\u50CF\u7D20\u6620\u5C04\u5206\u5F00\u663E\u793A";
    case StatusPage::kVoiceControl:
        return L"\u53EA\u4FDD\u7559\u63A5\u6536\u7AEF\u81EA\u5E26\u7684\u79BB\u7EBF\u8BED\u97F3\u63A7\u5236";
    case StatusPage::kDiagnostics:
        return L"\u5148\u5224\u65AD\u662F\u8FDE\u63A5\u3001\u89E3\u7801\u8FD8\u662F\u6E32\u67D3\u5361\u4F4F";
    case StatusPage::kLogs:
        return L"\u65E5\u5FD7\u3001\u5FEB\u7167\u548C\u8F93\u51FA\u76EE\u5F55\u5728\u8FD9\u91CC";
    case StatusPage::kOverview:
    default:
        return L"\u4F1A\u8BDD\u3001\u5E27\u7387\u3001\u5EF6\u8FDF\u548C\u64CD\u4F5C\u5165\u53E3\u5DF2\u62C6\u5F00";
    }
}

int NavigationButtonIdFromPage(StatusPage page) {
    switch (page) {
    case StatusPage::kConnect:
        return kNavConnectButtonId;
    case StatusPage::kDisplay:
        return kNavDisplayButtonId;
    case StatusPage::kVoiceControl:
        return kNavVoiceButtonId;
    case StatusPage::kDiagnostics:
        return kNavDiagnosticsButtonId;
    case StatusPage::kLogs:
        return kNavLogsButtonId;
    case StatusPage::kOverview:
    default:
        return kNavOverviewButtonId;
    }
}

StatusPage PageFromNavigationButtonId(int button_id) {
    switch (button_id) {
    case kNavConnectButtonId:
        return StatusPage::kConnect;
    case kNavDisplayButtonId:
        return StatusPage::kDisplay;
    case kNavVoiceButtonId:
        return StatusPage::kVoiceControl;
    case kNavDiagnosticsButtonId:
        return StatusPage::kDiagnostics;
    case kNavLogsButtonId:
        return StatusPage::kLogs;
    case kNavOverviewButtonId:
    default:
        return StatusPage::kOverview;
    }
}

bool IsNavigationButtonId(int button_id) {
    return button_id >= kNavOverviewButtonId && button_id <= kNavVoiceButtonId;
}

bool ShouldShowLogView(StatusPage page) {
    return page == StatusPage::kDiagnostics || page == StatusPage::kLogs;
}

bool ShouldUseWideLogView(StatusPage page) {
    return page == StatusPage::kLogs;
}

std::wstring FormatDimensions(int width, int height) {
    if (width <= 0 || height <= 0) {
        return kTextMeasurePending;
    }

    std::wostringstream stream;
    stream << width << L"x" << height;
    return stream.str();
}

std::wstring FormatScaleValue(double scale) {
    if (scale <= 0.0) {
        return kTextMeasurePending;
    }

    std::wostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(2);
    stream << scale << L"x";
    return stream.str();
}

std::wstring BuildProfileDetailBlock(const protocol::StreamProfile* profile, bool has_profile, const std::wstring& mode_label) {
    if (!has_profile || profile == nullptr) {
        return L"\u5206\u8FA8\u7387\uFF1A\u7B49\u5F85\u534F\u5546"
               L"\r\n\u7F16\u7801\uFF1A--"
               L"\r\n\u5237\u65B0\uFF1A--"
               L"\r\n\u7801\u7387\uFF1A--"
               L"\r\n\u97F3\u9891\uFF1A\u7B49\u5F85\u534F\u5546"
               L"\r\n\u6A21\u5F0F\uFF1A" + mode_label;
    }

    return std::wstring(L"\u5206\u8FA8\u7387\uFF1A") + FormatDimensions(profile->width, profile->height) +
           L"\r\n\u7F16\u7801\uFF1A" + CodecName(profile->codec) +
           L"\r\n\u5237\u65B0\uFF1A" + FormatProfileFrameRate(*profile) +
           L"\r\n\u7801\u7387\uFF1A" + FormatBitrateValue(profile->bitrate) +
           L"\r\n\u97F3\u9891\uFF1A" + BuildAudioProfileText(profile, true) +
           L"\r\n\u6A21\u5F0F\uFF1A" + mode_label;
}

std::wstring BuildRateDetailBlock(double receive_fps, double decode_fps, double display_fps) {
    return std::wstring(L"\u91CD\u7EC4\uFF1A") + FormatOptionalFps(receive_fps) +
           L"\r\n\u89E3\u7801\uFF1A" + FormatOptionalFps(decode_fps) +
           L"\r\n\u663E\u793A\uFF1A" + FormatOptionalFps(display_fps);
}

std::wstring BuildFrameCounterBlock(const VideoStats& stats, uint64_t displayed_frame_count, uint64_t decoded_frame_count) {
    const uint64_t pending_frames = decoded_frame_count > displayed_frame_count ? decoded_frame_count - displayed_frame_count : 0;
    return std::wstring(L"\u5F85\u663E\uFF1A") + std::to_wstring(pending_frames) +
           L"\r\n\u5DF2\u6536\uFF1A" + std::to_wstring(stats.completed_frames) +
           L"\r\n\u5DF2\u89E3\uFF1A" + std::to_wstring(decoded_frame_count) +
           L"\r\n\u5DF2\u663E\uFF1A" + std::to_wstring(displayed_frame_count);
}

std::wstring FormatRelativeMoment(int64_t now_us, int64_t event_us, const wchar_t* fallback) {
    if (event_us <= 0 || now_us <= event_us) {
        return fallback;
    }

    const double seconds = static_cast<double>(now_us - event_us) / 1'000'000.0;
    std::wostringstream stream;
    stream.setf(std::ios::fixed);
    if (seconds < 1.0) {
        stream.precision(1);
        stream << seconds << L" \u79D2\u524D";
        return stream.str();
    }
    if (seconds < 60.0) {
        stream.precision(0);
        stream << seconds << L" \u79D2\u524D";
        return stream.str();
    }

    const double minutes = seconds / 60.0;
    if (minutes < 60.0) {
        stream.precision(0);
        stream << minutes << L" \u5206\u949F\u524D";
        return stream.str();
    }

    const double hours = minutes / 60.0;
    stream.precision(1);
    stream << hours << L" \u5C0F\u65F6\u524D";
    return stream.str();
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

LRESULT HitTestFramelessStatusWindow(HWND hwnd, LPARAM l_param) {
    if (hwnd == nullptr || IsZoomed(hwnd)) {
        return HTCLIENT;
    }

    RECT window_rect{};
    if (!GetWindowRect(hwnd, &window_rect)) {
        return HTCLIENT;
    }

    const int border = ScaleByDpi(hwnd, kStatusWindowResizeBorder);
    const int x = GET_X_LPARAM(l_param);
    const int y = GET_Y_LPARAM(l_param);

    const bool left = x >= window_rect.left && x < window_rect.left + border;
    const bool right = x < window_rect.right && x >= window_rect.right - border;
    const bool top = y >= window_rect.top && y < window_rect.top + border;
    const bool bottom = y < window_rect.bottom && y >= window_rect.bottom - border;

    if (top && left) {
        return HTTOPLEFT;
    }
    if (top && right) {
        return HTTOPRIGHT;
    }
    if (bottom && left) {
        return HTBOTTOMLEFT;
    }
    if (bottom && right) {
        return HTBOTTOMRIGHT;
    }
    if (left) {
        return HTLEFT;
    }
    if (right) {
        return HTRIGHT;
    }
    if (top) {
        return HTTOP;
    }
    if (bottom) {
        return HTBOTTOM;
    }
    return HTCLIENT;
}

HFONT CreateUiFont(HWND hwnd, int pixel_size, int weight, const wchar_t* face_name) {
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
        face_name);
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

template <typename T>
void ReleaseComHandle(T*& handle) {
    if (handle != nullptr) {
        handle->Release();
        handle = nullptr;
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
    DeleteFontHandle(state->spotlight_value_font);
    DeleteFontHandle(state->body_font);
    DeleteFontHandle(state->button_font);

    state->title_font = CreateUiFont(state->status_window, 34, 600, kUiDisplayFontFamily);
    state->subtitle_font = CreateUiFont(state->status_window, 12, FW_NORMAL, kUiTextFontFamily);
    state->section_font = CreateUiFont(state->status_window, 12, 600, kUiTextFontFamily);
    state->value_font = CreateUiFont(state->status_window, 22, 600, kUiDisplayFontFamily);
    state->spotlight_value_font = CreateUiFont(state->status_window, 28, 600, kUiDisplayFontFamily);
    state->body_font = CreateUiFont(state->status_window, 15, FW_NORMAL, kUiTextFontFamily);
    state->button_font = CreateUiFont(state->status_window, 13, FW_NORMAL, kUiTextFontFamily);
}

void ApplyUiToControls(AppState* state) {
    if (state == nullptr) {
        return;
    }

    for (HWND nav_button : state->nav_buttons) {
        if (nav_button != nullptr && state->button_font != nullptr) {
            SendMessageW(nav_button, WM_SETFONT, reinterpret_cast<WPARAM>(state->button_font), TRUE);
        }
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

SessionSnapshot CopySessionSnapshot(const AppState* state) {
    SessionSnapshot snapshot;
    if (state == nullptr) {
        return snapshot;
    }

    std::lock_guard<std::mutex> lock(state->session_mutex);
    snapshot = state->session;
    return snapshot;
}

bool HasConnectedSender(const AppState* state) {
    if (state == nullptr) {
        return false;
    }

    const SessionSnapshot snapshot = CopySessionSnapshot(state);
    return snapshot.control_connected;
}

std::wstring PrimaryActionButtonText(const AppState* state) {
    if (state == nullptr) {
        return L"";
    }

    switch (state->current_page) {
    case StatusPage::kDiagnostics:
        return L"\u8BF7\u6C42\u5173\u952E\u5E27";
    case StatusPage::kVoiceControl:
        return state->voice_control_enabled ? L"\u5173\u95ED\u8BED\u97F3\u63A7\u5236" : L"\u5F00\u542F\u8BED\u97F3\u63A7\u5236";
    case StatusPage::kLogs:
        return L"\u6253\u5F00\u65E5\u5FD7";
    case StatusPage::kOverview:
    case StatusPage::kConnect:
    case StatusPage::kDisplay:
    default:
        return L"\u6253\u5F00\u6295\u5C4F\u7A97\u53E3";
    }
}

std::wstring SecondaryActionButtonText(const AppState* state) {
    if (state == nullptr) {
        return L"";
    }

    switch (state->current_page) {
    case StatusPage::kConnect:
        if (state->airplay_controller.running()) {
            return L"\u5173\u95ED\u82F9\u679C\u6295\u5C4F";
        }
        if (state->airplay_controller.installed()) {
            return L"\u542F\u52A8\u82F9\u679C\u6295\u5C4F";
        }
        return L"\u82F9\u679C\u7EC4\u4EF6\u7F3A\u5931";
    case StatusPage::kLogs:
        return L"\u6253\u5F00\u72B6\u6001\u5FEB\u7167";
    case StatusPage::kVoiceControl:
        return L"\u6253\u5F00\u65E5\u5FD7";
    case StatusPage::kDisplay:
        if (state->virtual_camera_controller.starting()) {
            return L"\u542F\u52A8\u4E2D";
        }
        if (state->virtual_camera_controller.running()) {
            return L"\u5173\u95ED\u865A\u62DF\u6444\u50CF\u5934";
        }
        if (state->virtual_camera_controller.installed()) {
            return L"\u542F\u52A8\u865A\u62DF\u6444\u50CF\u5934";
        }
        return L"\u5B89\u88C5\u865A\u62DF\u6444\u50CF\u5934";
    case StatusPage::kDiagnostics:
        return L"\u6253\u5F00\u6295\u5C4F\u7A97\u53E3";
    case StatusPage::kOverview:
    default:
        return L"\u6253\u5F00\u8BED\u97F3\u9875";
    }
}

std::wstring TertiaryActionButtonText(const AppState* state) {
    if (state == nullptr) {
        return L"";
    }

    switch (state->current_page) {
    case StatusPage::kConnect:
        return L"\u6253\u5F00\u72B6\u6001\u5FEB\u7167";
    case StatusPage::kDisplay:
        return L"\u6309\u6E90\u753B\u8D28\u663E\u793A";
    case StatusPage::kVoiceControl:
        return L"\u6253\u5F00\u76EE\u5F55";
    case StatusPage::kDiagnostics:
        return L"\u6253\u5F00\u72B6\u6001\u5FEB\u7167";
    case StatusPage::kLogs:
        return L"\u6253\u5F00\u76EE\u5F55";
    case StatusPage::kOverview:
    default:
        return L"\u6253\u5F00\u65E5\u5FD7";
    }
}

bool SecondaryActionEnabled(const AppState* state) {
    if (state == nullptr) {
        return false;
    }

    switch (state->current_page) {
    case StatusPage::kConnect:
        return state->airplay_controller.installed() || state->airplay_controller.running();
    case StatusPage::kDisplay:
        return !state->virtual_camera_tool_path.empty() && !state->virtual_camera_controller.starting();
    case StatusPage::kVoiceControl:
        return !state->log_file_path.empty();
    case StatusPage::kLogs:
        return !state->status_file_path.empty();
    case StatusPage::kDiagnostics:
        return state->video_window != nullptr;
    case StatusPage::kOverview:
    default:
        return true;
    }
}

bool TertiaryActionEnabled(const AppState* state) {
    if (state == nullptr) {
        return false;
    }

    switch (state->current_page) {
    case StatusPage::kConnect:
        return !state->status_file_path.empty();
    case StatusPage::kDisplay:
        return state->video_window != nullptr && state->has_selected_profile;
    case StatusPage::kVoiceControl:
        return true;
    case StatusPage::kDiagnostics:
        return !state->status_file_path.empty();
    case StatusPage::kLogs:
        return true;
    case StatusPage::kOverview:
    default:
        return !state->log_file_path.empty();
    }
}

void UpdateActionButtons(AppState* state) {
    if (state == nullptr) {
        return;
    }

    if (state->focus_video_button != nullptr) {
        const std::wstring text = PrimaryActionButtonText(state);
        SetWindowTextW(state->focus_video_button, text.c_str());
        EnableWindow(state->focus_video_button, TRUE);
    }
    if (state->presentation_mode_button != nullptr) {
        const std::wstring text = SecondaryActionButtonText(state);
        SetWindowTextW(state->presentation_mode_button, text.c_str());
        EnableWindow(state->presentation_mode_button, SecondaryActionEnabled(state));
    }
    if (state->open_log_button != nullptr) {
        const std::wstring text = TertiaryActionButtonText(state);
        SetWindowTextW(state->open_log_button, text.c_str());
        EnableWindow(state->open_log_button, TertiaryActionEnabled(state));
    }
}

void ApplyPresentationMode(AppState* state, VideoRenderer::PresentationMode mode, bool log_change) {
    if (state == nullptr) {
        return;
    }

    mode = VideoRenderer::PresentationMode::kLowLatency;

    if (state->presentation_mode == mode) {
        UpdateActionButtons(state);
        return;
    }

    state->presentation_mode = mode;
    state->renderer.SetPresentationMode(mode);
    if (state->decoder != nullptr) {
        state->decoder->SetSmoothMode(false);
    }
    UpdateActionButtons(state);

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

void UpdateSessionSnapshot(
    AppState* state,
    const std::wstring& sender_host,
    const std::wstring& device_name,
    bool connected) {
    if (state == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state->session_mutex);
        state->session.host = sender_host;
        state->session.device_name = device_name;
        state->session.control_connected = connected;
        state->session.last_session_change_us = NowSteadyUs();
        if (connected) {
            state->session.session_started_us = state->session.last_session_change_us;
        }
    }

    if (!connected && state->audio_player != nullptr) {
        state->audio_player->Stop();
    }

    if (state->status_window != nullptr) {
        PostMessageW(state->status_window, kStatsMessage, 0, 0);
    }
}

protocol::StreamProfile CapStreamProfileTo60Fps(const protocol::StreamProfile& profile) {
    protocol::StreamProfile normalized = profile;
    if (normalized.fps > kMaxStreamFps) {
        normalized.fps = kMaxStreamFps;
    }
    return normalized;
}

void PostApplyStreamProfileMessage(
    AppState* state,
    const protocol::StreamProfile& profile,
    bool auto_resumed,
    bool submit_cached_codec_config,
    uint32_t cached_codec_config_frame_id,
    std::vector<uint8_t> cached_codec_config,
    bool log_auto_resume_notice) {
    if (state == nullptr || state->status_window == nullptr) {
        if (state != nullptr && auto_resumed) {
            state->auto_resume_profile_message_pending.store(false);
        }
        return;
    }

    const protocol::StreamProfile capped_profile = CapStreamProfileTo60Fps(profile);

    auto* message = new PendingStreamProfileMessage{
        capped_profile,
        auto_resumed,
        submit_cached_codec_config,
        cached_codec_config_frame_id,
        std::move(cached_codec_config),
        log_auto_resume_notice};
    if (!PostMessageW(
            state->status_window,
            kApplyStreamProfileMessage,
            0,
            reinterpret_cast<LPARAM>(message))) {
        if (auto_resumed) {
            state->auto_resume_profile_message_pending.store(false);
        }
        delete message;
        QueueLog(state, L"\u63A5\u6536\u7AEF: \u6295\u5C4F\u914D\u7F6E\u5207\u6362\u6D88\u606F\u6295\u9012\u5931\u8D25\u3002");
    }
}

void PostSessionUpdateMessage(
    AppState* state,
    const std::wstring& sender_host,
    const std::wstring& device_name,
    bool connected) {
    if (state == nullptr || state->status_window == nullptr) {
        return;
    }

    auto* message = new PendingSessionUpdateMessage{sender_host, device_name, connected};
    if (!PostMessageW(
            state->status_window,
            kSessionUpdateMessage,
            0,
            reinterpret_cast<LPARAM>(message))) {
        delete message;
        QueueLog(state, L"\u63A5\u6536\u7AEF: \u4F1A\u8BDD\u72B6\u6001\u6D88\u606F\u6295\u9012\u5931\u8D25\u3002");
    }
}

void NavigateToPage(AppState* state, StatusPage page) {
    if (state == nullptr || state->current_page == page) {
        return;
    }

    state->current_page = page;
    UpdateActionButtons(state);
    if (state->status_window != nullptr) {
        RedrawWindow(
            state->status_window,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

void SetEmbeddedWebUiMode(AppState* state, bool enabled) {
    if (state == nullptr) {
        return;
    }

    state->web_ui_ready = enabled;
    for (HWND button : state->nav_buttons) {
        if (button != nullptr) {
            ShowWindow(button, enabled ? SW_HIDE : SW_SHOW);
        }
    }
    if (state->focus_video_button != nullptr) {
        ShowWindow(state->focus_video_button, enabled ? SW_HIDE : SW_SHOW);
    }
    if (state->presentation_mode_button != nullptr) {
        ShowWindow(state->presentation_mode_button, enabled ? SW_HIDE : SW_SHOW);
    }
    if (state->open_log_button != nullptr) {
        ShowWindow(state->open_log_button, enabled ? SW_HIDE : SW_SHOW);
    }
    if (state->log_view != nullptr) {
        ShowWindow(state->log_view, enabled ? SW_HIDE : (ShouldShowLogView(state->current_page) ? SW_SHOW : SW_HIDE));
    }

    InvalidateRect(state->status_window, nullptr, TRUE);
}

void DestroyNativeStatusControls(AppState* state) {
    if (state == nullptr) {
        return;
    }

    for (HWND& button : state->nav_buttons) {
        if (button != nullptr && IsWindow(button)) {
            DestroyWindow(button);
        }
        button = nullptr;
    }
    if (state->focus_video_button != nullptr && IsWindow(state->focus_video_button)) {
        DestroyWindow(state->focus_video_button);
    }
    state->focus_video_button = nullptr;
    if (state->presentation_mode_button != nullptr && IsWindow(state->presentation_mode_button)) {
        DestroyWindow(state->presentation_mode_button);
    }
    state->presentation_mode_button = nullptr;
    if (state->open_log_button != nullptr && IsWindow(state->open_log_button)) {
        DestroyWindow(state->open_log_button);
    }
    state->open_log_button = nullptr;
    if (state->log_view != nullptr && IsWindow(state->log_view)) {
        DestroyWindow(state->log_view);
    }
    state->log_view = nullptr;
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

void OpenStatusSnapshotFile(AppState* state) {
    if (state == nullptr || state->status_file_path.empty()) {
        return;
    }
    ShellExecuteW(state->status_window, L"open", state->status_file_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenOutputDirectory(AppState* state) {
    if (state == nullptr) {
        return;
    }
    const std::wstring directory = GetExecutableDirectory();
    if (directory.empty()) {
        return;
    }
    ShellExecuteW(state->status_window, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenDirectoryPath(AppState* state, const std::wstring& directory) {
    if (state == nullptr || directory.empty()) {
        return;
    }
    ShellExecuteW(state->status_window, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenVoiceMusicDirectory(AppState* state) {
    if (state == nullptr) {
        return;
    }
    if (state->voice_music_root_path.empty()) {
        state->voice_control_status = L"点歌目录不可用";
        QueueLog(state, L"语音点歌：打开点歌目录失败，目录为空。");
        return;
    }
    EnsureDirectoryPath(state->voice_music_root_path);
    OpenDirectoryPath(state, state->voice_music_root_path);
    state->voice_control_status = L"已打开点歌目录";
    QueueLog(state, std::wstring(L"语音点歌：已打开点歌目录：") + state->voice_music_root_path);
}

void OpenVoiceModelDirectory(AppState* state) {
    if (state == nullptr) {
        return;
    }
    if (state->voice_model_path.empty()) {
        state->voice_control_status = L"语音模型目录不可用";
        QueueLog(state, L"语音控制：打开模型目录失败，模型目录为空。");
        return;
    }
    OpenDirectoryPath(state, state->voice_model_path);
    state->voice_control_status = L"已打开语音模型目录";
    QueueLog(state, std::wstring(L"语音控制：已打开模型目录：") + state->voice_model_path);
}

void RefreshVoiceMusicLibrary(AppState* state) {
    if (state == nullptr) {
        return;
    }
    EnsureVoiceMusicScaffold(state);
    if (state->voice_music_library == nullptr) {
        state->voice_control_status = L"刷新点歌目录失败";
        QueueLog(state, L"语音点歌：刷新失败，曲库未初始化。");
        return;
    }
    if (!state->voice_music_library->RefreshIndex()) {
        state->voice_control_status = L"刷新点歌目录失败";
        QueueLog(state, L"语音点歌：刷新点歌目录失败。");
        return;
    }
    state->voice_control_status = L"已刷新点歌目录";
    QueueLog(state, std::wstring(L"语音点歌：已刷新点歌目录：") + state->voice_music_root_path);
    RefreshVoiceRecognitionContext(state, true, true);
}

void StopLocalMusicPlayback(AppState* state) {
    if (state == nullptr || state->local_music_player == nullptr) {
        return;
    }
    if (!state->local_music_player->running()) {
        state->voice_control_status = L"当前没有正在播放的点歌";
        QueueLog(state, L"语音点歌：停止播放已忽略，当前没有本地点歌在播放。");
        return;
    }

    const std::wstring current_file = state->local_music_player->current_file();
    state->local_music_player->Stop();
    if (current_file.empty()) {
        state->voice_control_status = L"已停止点歌播放";
        QueueLog(state, L"语音点歌：已停止当前播放。");
        return;
    }

    const std::wstring current_name = ExtractFileName(current_file);
    state->voice_control_status = std::wstring(L"已停止播放：") + current_name;
    QueueLog(state, std::wstring(L"语音点歌：已停止播放“") + current_name + L"”。");
}

void ExecuteEmbeddedWebUiAction(
    AppState* state,
    const std::wstring& action,
    const std::wstring& value,
    const std::wstring& extra) {
    if (state == nullptr || action.empty()) {
        return;
    }

    if (action == L"dragWindow") {
        if (state->status_window != nullptr) {
            ReleaseCapture();
            SendMessageW(state->status_window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return;
    }
    if (action == L"minimizeWindow") {
        if (state->status_window != nullptr) {
            ShowWindow(state->status_window, SW_MINIMIZE);
        }
        return;
    }
    if (action == L"closeWindow") {
        if (state->status_window != nullptr) {
            PostMessageW(state->status_window, WM_CLOSE, 0, 0);
        }
        return;
    }
    if (action == L"focusVideoWindow") {
        FocusVideoWindow(state);
        QueueLog(state, L"内嵌前端：已执行“聚焦投屏窗口”。");
        return;
    }
    if (action == L"toggleVoiceControl") {
        if (state->voice_control_enabled) {
            StopVoiceControl(state, true);
        } else {
            StartVoiceControl(state);
        }
        QueueLog(state, L"内嵌前端：已执行“切换语音控制”。");
        return;
    }
    if (action == L"toggleAirPlay") {
        if (state->airplay_controller.running()) {
            StopAirPlayReceiver(state, true);
        } else {
            StartAirPlayReceiver(state, false);
        }
        QueueLog(state, L"内嵌前端：已执行“切换苹果投屏”。");
        return;
    }
    if (action == L"toggleVirtualCamera") {
        if (state->virtual_camera_controller.running()) {
            StopVirtualCamera(state, true);
        } else if (!state->virtual_camera_controller.installed()) {
            InstallVirtualCamera(state);
        } else {
            StartVirtualCamera(state);
        }
        QueueLog(state, L"内嵌前端：已执行“切换虚拟摄像头”。");
        return;
    }
    if (action == L"openLogFile") {
        OpenLogFile(state);
        QueueLog(state, L"内嵌前端：已执行“打开日志文件”。");
        return;
    }
    if (action == L"openOutputDirectory") {
        OpenOutputDirectory(state);
        QueueLog(state, L"内嵌前端：已执行“打开接收端目录”。");
        return;
    }
    if (action == L"openVoiceMusicDirectory") {
        OpenVoiceMusicDirectory(state);
        QueueLog(state, L"内嵌前端：已执行“打开点歌目录”。");
        return;
    }
    if (action == L"openVoiceModelDirectory") {
        OpenVoiceModelDirectory(state);
        QueueLog(state, L"内嵌前端：已执行“打开语音模型目录”。");
        return;
    }
    if (action == L"refreshVoiceMusicLibrary") {
        RefreshVoiceMusicLibrary(state);
        QueueLog(state, L"内嵌前端：已执行“刷新点歌目录”。");
        return;
    }
    if (action == L"stopLocalMusicPlayback") {
        StopLocalMusicPlayback(state);
        QueueLog(state, L"内嵌前端：已执行“停止当前点歌”。");
        return;
    }
    if (action == L"createVoiceMusicProject") {
        ExecuteVoiceMusicProjectCreate(state, value, extra);
        return;
    }
    if (action == L"selectDanmakuRegion") {
        if (state->danmaku_controller != nullptr) {
            state->danmaku_controller->SelectRegion();
            UpdateStatusLabel(state);
        }
        return;
    }
    if (action == L"toggleDanmakuRecognition") {
        if (state->danmaku_controller != nullptr) {
            const auto snapshot = state->danmaku_controller->GetSnapshot();
            if (snapshot.running) {
                state->danmaku_controller->Stop();
            } else {
                state->danmaku_controller->Start();
            }
            UpdateStatusLabel(state);
        }
        return;
    }
    if (action == L"toggleDanmakuReminderBundle") {
        if (state->danmaku_controller != nullptr) {
            auto snapshot = state->danmaku_controller->GetSnapshot();
            const bool reminder_bundle_enabled = snapshot.running && snapshot.reminder_enabled;
            if (reminder_bundle_enabled) {
                if (snapshot.speech_enabled) {
                    state->danmaku_controller->ToggleSpeech();
                    snapshot = state->danmaku_controller->GetSnapshot();
                }
                if (snapshot.reminder_enabled) {
                    state->danmaku_controller->ToggleReminder();
                    snapshot = state->danmaku_controller->GetSnapshot();
                }
                if (snapshot.running && !snapshot.gift_reminder_enabled) {
                    state->danmaku_controller->Stop();
                }
            } else {
                state->danmaku_controller->StartUiAutomationProbe();
                if (!snapshot.running) {
                    state->danmaku_controller->Start();
                    snapshot = state->danmaku_controller->GetSnapshot();
                }
                if (!snapshot.reminder_enabled) {
                    state->danmaku_controller->ToggleReminder();
                }
            }
            UpdateStatusLabel(state);
        }
        return;
    }
    if (action == L"toggleDanmakuGiftReminderBundle") {
        if (state->danmaku_controller != nullptr) {
            auto snapshot = state->danmaku_controller->GetSnapshot();
            const bool gift_reminder_bundle_enabled =
                snapshot.running && snapshot.gift_reminder_enabled;
            if (gift_reminder_bundle_enabled) {
                state->danmaku_controller->ToggleGiftReminder();
                snapshot = state->danmaku_controller->GetSnapshot();
                if (snapshot.running && !snapshot.reminder_enabled && !snapshot.speech_enabled) {
                    state->danmaku_controller->Stop();
                }
            } else {
                state->danmaku_controller->StartUiAutomationProbe();
                if (!snapshot.running) {
                    state->danmaku_controller->Start();
                    snapshot = state->danmaku_controller->GetSnapshot();
                }
                if (!snapshot.gift_reminder_enabled) {
                    state->danmaku_controller->ToggleGiftReminder();
                }
            }
            UpdateStatusLabel(state);
        }
        return;
    }
    if (action == L"toggleDanmakuSpeechBundle") {
        if (state->danmaku_controller != nullptr) {
            auto snapshot = state->danmaku_controller->GetSnapshot();
            const bool speech_bundle_enabled =
                snapshot.running && snapshot.reminder_enabled && snapshot.speech_enabled;
            if (speech_bundle_enabled) {
                state->danmaku_controller->ToggleSpeech();
            } else {
                state->danmaku_controller->StartUiAutomationProbe();
                if (!snapshot.running) {
                    state->danmaku_controller->Start();
                    snapshot = state->danmaku_controller->GetSnapshot();
                }
                if (!snapshot.reminder_enabled) {
                    state->danmaku_controller->ToggleReminder();
                    snapshot = state->danmaku_controller->GetSnapshot();
                }
                if (!snapshot.speech_enabled) {
                    state->danmaku_controller->ToggleSpeech();
                }
            }
            UpdateStatusLabel(state);
        }
        return;
    }
    if (action == L"toggleDanmakuReminder") {
        if (state->danmaku_controller != nullptr) {
            state->danmaku_controller->ToggleReminder();
            UpdateStatusLabel(state);
        }
        return;
    }
    if (action == L"toggleDanmakuSpeech") {
        if (state->danmaku_controller != nullptr) {
            state->danmaku_controller->ToggleSpeech();
            UpdateStatusLabel(state);
        }
        return;
    }
    if (action == L"cycleDanmakuSpeechVoice") {
        if (state->danmaku_controller != nullptr) {
            state->danmaku_controller->CycleSpeechVoice();
            UpdateStatusLabel(state);
        }
        return;
    }
    if (action == L"probeDanmakuUi") {
        if (state->danmaku_controller != nullptr) {
            state->danmaku_controller->StartUiAutomationProbe();
            UpdateStatusLabel(state);
        }
        return;
    }
    if (action == L"testDanmakuFrame") {
        if (state->danmaku_controller != nullptr) {
            state->danmaku_controller->TestRecognizeFrame();
            UpdateStatusLabel(state);
        }
        return;
    }
    if (action == L"clearDanmakuResults") {
        if (state->danmaku_controller != nullptr) {
            state->danmaku_controller->ClearRecentEvents();
            UpdateStatusLabel(state);
        }
        return;
    }
    if (action == L"openDanmakuCaptureFolder") {
        QueueLog(state, L"\u5185\u5d4c\u524d\u7aef\uff1a\u5f39\u5e55\u56fe\u50cf\u8bc6\u522b\u5df2\u79fb\u9664\uff0c\u622a\u56fe\u76ee\u5f55\u5165\u53e3\u5df2\u505c\u7528\u3002");
        return;
    }
    if (action == L"toggleAutoReplyEnabled") {
        ToggleAutoReplyEnabled(state);
        UpdateStatusLabel(state);
        return;
    }
    if (action == L"toggleAutoReplySendEnabled") {
        ToggleAutoReplySendEnabled(state);
        UpdateStatusLabel(state);
        return;
    }
    if (action == L"saveAutoReplyTextConfig") {
        SaveAutoReplyTextConfig(state, value, extra);
        UpdateStatusLabel(state);
        return;
    }
    if (action == L"saveAutoReplyTiming") {
        SaveAutoReplyTimingConfig(state, value);
        UpdateStatusLabel(state);
        return;
    }
    if (action == L"captureAutoReplyInputPoint") {
        StartAutoReplyInputPointCapture(state);
        UpdateStatusLabel(state);
        return;
    }
    if (action == L"captureAutoReplySendPoint") {
        StartAutoReplySendPointCapture(state);
        UpdateStatusLabel(state);
        return;
    }
    if (action == L"reloadAutoReplyConfig") {
        LoadAutoReplyConfig(state);
        UpdateStatusLabel(state);
        return;
    }
    if (action == L"openAutoReplyDirectory") {
        OpenAutoReplyDirectory(state);
        UpdateStatusLabel(state);
        return;
    }
    if (action == L"requestKeyframe") {
        RequestKeyframe(state);
        QueueLog(state, L"内嵌前端：已执行“请求关键帧”。");
        return;
    }
}

void RequestKeyframe(AppState* state) {
    if (state == nullptr || state->control_server == nullptr) {
        return;
    }

    if (!state->control_server->RequestIdr()) {
        QueueLog(state, L"\u63A7\u5236\u670D\u52A1: \u5F53\u524D\u6CA1\u6709\u6D3B\u8DC3\u4F1A\u8BDD\uFF0C\u65E0\u6CD5\u8BF7\u6C42\u5173\u952E\u5E27\u3002");
    }
}

void ApplyWindowBackdrop(HWND hwnd) {
    if (hwnd == nullptr) {
        return;
    }

    constexpr DWORD kDwmUseImmersiveDarkMode = 20;
    constexpr DWORD kDwmUseImmersiveDarkModeLegacy = 19;
    constexpr DWORD kDwmWindowCornerPreference = 33;
    constexpr DWORD kDwmBorderColor = 34;
    constexpr DWORD kDwmSystemBackdropType = 38;
    constexpr int kWindowCornerRound = 2;
    constexpr int kBackdropTransientWindow = 3;
    constexpr DWORD kDwmColorNone = 0xFFFFFFFEu;

    const HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll");
    if (dwmapi == nullptr) {
        return;
    }

    struct DwmMargins {
        int cxLeftWidth;
        int cxRightWidth;
        int cyTopHeight;
        int cyBottomHeight;
    };

    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    using DwmExtendFrameIntoClientAreaFn = HRESULT(WINAPI*)(HWND, const DwmMargins*);
    const auto set_window_attribute =
        reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(dwmapi, "DwmSetWindowAttribute"));
    const auto extend_frame =
        reinterpret_cast<DwmExtendFrameIntoClientAreaFn>(GetProcAddress(dwmapi, "DwmExtendFrameIntoClientArea"));
    if (set_window_attribute != nullptr) {
        const BOOL dark_mode = TRUE;
        set_window_attribute(hwnd, kDwmUseImmersiveDarkMode, &dark_mode, sizeof(dark_mode));
        set_window_attribute(hwnd, kDwmUseImmersiveDarkModeLegacy, &dark_mode, sizeof(dark_mode));
        set_window_attribute(hwnd, kDwmWindowCornerPreference, &kWindowCornerRound, sizeof(kWindowCornerRound));
        set_window_attribute(hwnd, kDwmBorderColor, &kDwmColorNone, sizeof(kDwmColorNone));
        set_window_attribute(hwnd, kDwmSystemBackdropType, &kBackdropTransientWindow, sizeof(kBackdropTransientWindow));
    }
    if (extend_frame != nullptr) {
        const DwmMargins margins{-1, -1, -1, -1};
        extend_frame(hwnd, &margins);
    }
    FreeLibrary(dwmapi);
}

void FillRoundedRect(HDC dc, const RECT& rect, int radius, COLORREF fill_color, COLORREF border_color) {
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, fill_color);
    // 加粗边框，让卡片更明显
    SetDCPenColor(dc, border_color);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    
    // 添加内边框，增强视觉效果
    HPEN inner_pen = CreatePen(PS_SOLID, 2, border_color);
    HGDIOBJ old_inner_pen = SelectObject(dc, inner_pen);
    SetDCBrushColor(dc, RGB(0, 0, 0)); // 透明填充
    RoundRect(dc, rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1, radius - 1, radius - 1);
    SelectObject(dc, old_inner_pen);
    DeleteObject(inner_pen);
    
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
}

void StrokeRoundedRect(HDC dc, const RECT& rect, int radius, COLORREF border_color) {
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(DC_PEN));
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
    OffsetRect(&far_shadow, 0, ScaleByDpi(state->status_window, std::max(3, base_offset)));
    InflateRect(&far_shadow, ScaleByDpi(state->status_window, 6), ScaleByDpi(state->status_window, 7));
    FillRoundedRect(
        dc,
        far_shadow,
        radius + ScaleByDpi(state->status_window, 12),
        kColorShadowFar,
        kColorShadowFar);

    RECT near_shadow = rect;
    OffsetRect(&near_shadow, 0, ScaleByDpi(state->status_window, std::max(1, base_offset / 2)));
    InflateRect(&near_shadow, ScaleByDpi(state->status_window, 2), ScaleByDpi(state->status_window, 3));
    FillRoundedRect(
        dc,
        near_shadow,
        radius + ScaleByDpi(state->status_window, 5),
        kColorShadowNear,
        kColorShadowNear);
}

void DrawGlassCard(AppState* state, HDC dc, const RECT& rect, int radius, COLORREF fill_color, COLORREF border_color, int shadow_offset) {
    // 增强阴影效果，制造悬浮感
    if (shadow_offset > 0) {
        // 绘制多层阴影，增强深度感
        DrawSoftShadow(state, dc, rect, radius, shadow_offset + 2);
        DrawSoftShadow(state, dc, rect, radius, shadow_offset);
    }
    
    // 绘制卡片背景和边框
    FillRoundedRect(dc, rect, radius, fill_color, border_color);
}

void DrawGlassBackdrop(AppState* state, HDC dc, const RECT& client_rect) {
    if (state == nullptr) {
        return;
    }

    if (state->web_ui_ready) {
        RECT left_glow = MakeRect(
            client_rect.left - ScaleByDpi(state->status_window, 220),
            client_rect.top - ScaleByDpi(state->status_window, 120),
            client_rect.left + ScaleByDpi(state->status_window, 380),
            client_rect.bottom + ScaleByDpi(state->status_window, 180));
        RECT left_edge = MakeRect(
            client_rect.left - ScaleByDpi(state->status_window, 80),
            client_rect.top - ScaleByDpi(state->status_window, 90),
            client_rect.left + ScaleByDpi(state->status_window, 260),
            client_rect.top + ScaleByDpi(state->status_window, 220));
        RECT grounding = MakeRect(
            client_rect.left - ScaleByDpi(state->status_window, 120),
            client_rect.bottom - ScaleByDpi(state->status_window, 180),
            client_rect.left + ScaleByDpi(state->status_window, 320),
            client_rect.bottom + ScaleByDpi(state->status_window, 90));

        FillEllipseColor(dc, left_glow, kColorEmbeddedGlassGlowLeft);
        FillEllipseColor(dc, left_edge, kColorEmbeddedGlassGlowEdge);
        FillEllipseColor(dc, grounding, kColorEmbeddedGlassGlowGround);
        return;
    }

    RECT highlight = MakeRect(
        client_rect.right - ScaleByDpi(state->status_window, 480),
        client_rect.top - ScaleByDpi(state->status_window, 120),
        client_rect.right + ScaleByDpi(state->status_window, 120),
        client_rect.top + ScaleByDpi(state->status_window, 300));
    RECT grounding = MakeRect(
        client_rect.left - ScaleByDpi(state->status_window, 160),
        client_rect.bottom - ScaleByDpi(state->status_window, 240),
        client_rect.left + ScaleByDpi(state->status_window, 360),
        client_rect.bottom + ScaleByDpi(state->status_window, 60));

    FillEllipseColor(dc, highlight, kColorGlassGlowBlue);
    FillEllipseColor(dc, grounding, kColorGlassGlowMint);
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

int MeasureTextBlockHeight(HDC dc, HFONT font, const std::wstring& text, int width, UINT format) {
    if (dc == nullptr || text.empty()) {
        return 0;
    }

    RECT rect{0, 0, std::max(1, width), 0};
    HGDIOBJ old_font = nullptr;
    if (font != nullptr) {
        old_font = SelectObject(dc, font);
    }
    DrawTextW(dc, text.c_str(), -1, &rect, format | DT_CALCRECT);
    if (old_font != nullptr) {
        SelectObject(dc, old_font);
    }
    return std::max(0L, rect.bottom - rect.top);
}

int MeasureSingleLineTextWidth(HDC dc, HFONT font, const std::wstring& text) {
    if (dc == nullptr || text.empty()) {
        return 0;
    }

    RECT rect{0, 0, 0, 0};
    HGDIOBJ old_font = nullptr;
    if (font != nullptr) {
        old_font = SelectObject(dc, font);
    }
    DrawTextW(dc, text.c_str(), -1, &rect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_CALCRECT);
    if (old_font != nullptr) {
        SelectObject(dc, old_font);
    }
    return std::max(0L, rect.right - rect.left);
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

    const int padding = ScaleByDpi(state->status_window, 16);
    const int gap = ScaleByDpi(state->status_window, 16);
    const int topbar_height = ScaleByDpi(state->status_window, 84);
    const int sidebar_width = ScaleByDpi(state->status_window, 228);
    const int hero_height = ScaleByDpi(state->status_window, 224);
    const int summary_height = ScaleByDpi(state->status_window, 164);
    const int hero_inner_padding = ScaleByDpi(state->status_window, 24);
    const int button_width = ScaleByDpi(state->status_window, 146);
    const int button_height = ScaleByDpi(state->status_window, 42);
    const int button_gap = ScaleByDpi(state->status_window, 12);
    const int nav_height = ScaleByDpi(state->status_window, 46);

    layout.shell = MakeRect(padding, padding, width - padding, height - padding);
    layout.topbar = MakeRect(
        layout.shell.left + padding,
        layout.shell.top + padding,
        layout.shell.right - padding,
        layout.shell.top + padding + topbar_height);
    layout.sidebar = MakeRect(
        layout.shell.left + padding,
        layout.topbar.bottom + gap,
        layout.shell.left + padding + sidebar_width,
        layout.shell.bottom - padding);
    layout.content = MakeRect(
        layout.sidebar.right + gap,
        layout.topbar.bottom + gap,
        layout.shell.right - padding,
        layout.shell.bottom - padding);

    const int nav_padding = ScaleByDpi(state->status_window, 14);
    const int nav_gap = ScaleByDpi(state->status_window, 8);
    int nav_top = layout.sidebar.top + ScaleByDpi(state->status_window, 68);
    for (size_t index = 0; index < layout.nav_buttons.size(); ++index) {
        layout.nav_buttons[index] = MakeRect(
            layout.sidebar.left + nav_padding,
            nav_top,
            layout.sidebar.right - nav_padding,
            nav_top + nav_height);
        nav_top += nav_height + nav_gap;
    }

    const int content_width = RectWidth(layout.content);
    const int hero_main_width = std::max(ScaleByDpi(state->status_window, 380), content_width * 56 / 100);
    layout.hero_main = MakeRect(
        layout.content.left,
        layout.content.top,
        layout.content.left + hero_main_width,
        layout.content.top + hero_height);
    layout.hero_side = MakeRect(
        layout.hero_main.right + gap,
        layout.content.top,
        layout.content.right,
        layout.content.top + hero_height);

    const int button_top = layout.hero_main.bottom - hero_inner_padding - button_height;
    layout.open_log_button = MakeRect(
        layout.hero_main.right - hero_inner_padding - button_width,
        button_top,
        layout.hero_main.right - hero_inner_padding,
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

    const int summary_top = layout.hero_main.bottom + gap;
    const int summary_width =
        std::max(ScaleByDpi(state->status_window, 150), (RectWidth(layout.content) - gap * 3) / 4);
    for (size_t index = 0; index < layout.summary_cards.size(); ++index) {
        const int left = layout.content.left + static_cast<int>(index) * (summary_width + gap);
        layout.summary_cards[index] = MakeRect(left, summary_top, left + summary_width, summary_top + summary_height);
    }

    const int detail_top = summary_top + summary_height + gap;
    layout.detail_full = MakeRect(layout.content.left, detail_top, layout.content.right, layout.content.bottom);
    const int detail_left_width = std::max(ScaleByDpi(state->status_window, 360), (RectWidth(layout.content) - gap) * 58 / 100);
    layout.detail_left = MakeRect(
        layout.content.left,
        detail_top,
        layout.content.left + detail_left_width,
        layout.content.bottom);
    layout.detail_right = MakeRect(
        layout.detail_left.right + gap,
        detail_top,
        layout.content.right,
        layout.content.bottom);

    const int card_inner_padding = ScaleByDpi(state->status_window, 20);
    const int log_title_height = ScaleByDpi(state->status_window, 44);
    const RECT log_host = ShouldUseWideLogView(state->current_page) ? layout.detail_full : layout.detail_left;
    layout.log_view = MakeRect(
        log_host.left + card_inner_padding,
        log_host.top + card_inner_padding + log_title_height + ScaleByDpi(state->status_window, 10),
        log_host.right - card_inner_padding,
        log_host.bottom - card_inner_padding);
    return layout;
}

void DrawPill(HDC dc, HFONT font, const RECT& rect, const std::wstring& text, COLORREF fill_color, COLORREF text_color) {
    FillRoundedRect(dc, rect, RectHeight(rect), fill_color, fill_color);
    RECT text_rect = rect;
    text_rect.left += 14;
    text_rect.right -= 14;
    DrawTextBlock(dc, font, text, text_rect, text_color, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void DrawBrandGlyph(HDC dc, const RECT& rect) {
    const HMODULE instance = GetModuleHandleW(nullptr);
    if (instance != nullptr) {
        HICON icon = LoadAppIconHandle(instance, std::max(16, std::min(RectWidth(rect), RectHeight(rect))));
        if (icon != nullptr) {
            DrawIconEx(
                dc,
                rect.left,
                rect.top,
                icon,
                RectWidth(rect),
                RectHeight(rect),
                0,
                nullptr,
                DI_NORMAL);
            DestroyIcon(icon);
            return;
        }
    }

    const int radius = std::max(14, RectHeight(rect) / 4);
    FillRoundedRect(dc, rect, radius, kColorWhite, kColorTopbarBorder);
    RECT fallback = InsetRectCopy(rect, std::max(6, RectWidth(rect) / 5), std::max(6, RectHeight(rect) / 5));
    FillRoundedRect(dc, fallback, std::max(8, RectHeight(fallback) / 4), kColorAccent, kColorAccent);
}

void DrawSummaryCard(AppState* state, HDC dc, const RECT& rect, const DashboardCard& card, bool accent_style) {
    const int radius = ScaleByDpi(state->status_window, 16);
    const int inner_padding = ScaleByDpi(state->status_window, 22);
    const COLORREF fill_color = accent_style ? RGB(250, 252, 255) : kColorCardBackground;
    const COLORREF border_color = accent_style ? BlendColor(kColorAccent, kColorWhite, 92) : kColorBorder;
    const COLORREF value_color = accent_style ? kColorAccent : kColorTextPrimary;

    DrawGlassCard(state, dc, rect, radius, fill_color, border_color, 6);

    RECT title_rect = MakeRect(
        rect.left + inner_padding,
        rect.top + inner_padding,
        rect.right - inner_padding,
        rect.top + inner_padding + ScaleByDpi(state->status_window, 22));
    DrawTextBlock(dc, state->section_font, card.title, title_rect, kColorTextSecondary, DT_LEFT | DT_TOP | DT_SINGLELINE);

    const int content_width = std::max(1, RectWidth(rect) - inner_padding * 2);
    const int min_note_height = ScaleByDpi(state->status_window, 48);
    const int available_value_space =
        static_cast<int>(rect.bottom) - inner_padding - static_cast<int>(title_rect.bottom) - ScaleByDpi(state->status_window, 12) - min_note_height;
    const int available_value_height = std::max(
        ScaleByDpi(state->status_window, 28),
        available_value_space);
    int value_height = MeasureTextBlockHeight(dc, state->value_font, card.value, content_width, DT_LEFT | DT_TOP | DT_WORDBREAK);
    value_height = std::max(value_height, ScaleByDpi(state->status_window, 28));
    value_height = std::min(value_height, available_value_height);
    RECT value_rect = MakeRect(
        rect.left + inner_padding,
        title_rect.bottom + ScaleByDpi(state->status_window, 10),
        rect.right - inner_padding,
        title_rect.bottom + ScaleByDpi(state->status_window, 10) + value_height);
    DrawTextBlock(dc, state->value_font, card.value, value_rect, value_color, DT_LEFT | DT_TOP | DT_WORDBREAK);

    RECT note_rect = MakeRect(
        rect.left + inner_padding,
        value_rect.bottom + ScaleByDpi(state->status_window, 8),
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
    const int radius = ScaleByDpi(state->status_window, 16);
    const int inner_padding = ScaleByDpi(state->status_window, 22);
    DrawGlassCard(state, dc, rect, radius, kColorCardBackground, kColorBorder, 5);

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

void DrawSpotlightCard(
    AppState* state,
    HDC dc,
    const RECT& rect,
    const std::wstring& label,
    const std::wstring& value,
    const std::wstring& body,
    bool accent_style) {
    const int radius = ScaleByDpi(state->status_window, 20);
    const int inner_padding = ScaleByDpi(state->status_window, 22);
    const COLORREF fill_color = accent_style ? RGB(250, 252, 255) : kColorCardBackground;
    const COLORREF border_color = accent_style ? BlendColor(kColorAccent, kColorWhite, 92) : kColorBorder;
    const COLORREF value_color = accent_style ? kColorAccent : kColorTextPrimary;
    DrawGlassCard(state, dc, rect, radius, fill_color, border_color, 7);

    const int content_width = std::max(1, RectWidth(rect) - inner_padding * 2);
    RECT label_rect = MakeRect(
        rect.left + inner_padding,
        rect.top + inner_padding,
        rect.right - inner_padding,
        rect.top + inner_padding + ScaleByDpi(state->status_window, 20));
    DrawTextBlock(dc, state->section_font, label, label_rect, kColorTextSecondary, DT_LEFT | DT_TOP | DT_SINGLELINE);

    const HFONT value_font = state->spotlight_value_font != nullptr ? state->spotlight_value_font : state->value_font;
    const int value_top = label_rect.bottom + ScaleByDpi(state->status_window, 12);
    const int body_gap = ScaleByDpi(state->status_window, 14);
    const int min_body_height = ScaleByDpi(state->status_window, 60);
    const int available_value_height = static_cast<int>(rect.bottom) - inner_padding - value_top - body_gap - min_body_height;
    const int max_value_height = std::max(
        ScaleByDpi(state->status_window, 28),
        available_value_height);
    int value_height = MeasureTextBlockHeight(dc, value_font, value, content_width, DT_LEFT | DT_TOP | DT_WORDBREAK);
    value_height = std::max(value_height, ScaleByDpi(state->status_window, 28));
    value_height = std::min(value_height, max_value_height);
    RECT value_rect = MakeRect(
        rect.left + inner_padding,
        value_top,
        rect.right - inner_padding,
        value_top + value_height);
    DrawTextBlock(dc, value_font, value, value_rect, value_color, DT_LEFT | DT_TOP | DT_WORDBREAK);

    const int body_top = std::min(
        value_rect.bottom + body_gap,
        rect.bottom - inner_padding - ScaleByDpi(state->status_window, 44));
    RECT body_rect = MakeRect(
        rect.left + inner_padding,
        body_top,
        rect.right - inner_padding,
        rect.bottom - inner_padding);
    DrawTextBlock(dc, state->body_font, body, body_rect, kColorTextSecondary, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

std::wstring BuildRecentLogPreview(AppState* state, size_t max_lines) {
    if (state == nullptr) {
        return {};
    }

    std::lock_guard<std::mutex> lock(state->log_mutex);
    if (state->recent_logs.empty()) {
        return L"\u6700\u8FD1\u8FD8\u6CA1\u6709\u65B0\u7684\u8FD0\u884C\u65E5\u5FD7\u3002";
    }

    const size_t start_index = state->recent_logs.size() > max_lines ? state->recent_logs.size() - max_lines : 0;
    std::wstring combined;
    for (size_t index = start_index; index < state->recent_logs.size(); ++index) {
        if (!combined.empty()) {
            combined += L"\r\n";
        }
        combined += state->recent_logs[index];
    }
    return combined;
}

void GetVideoClientSize(AppState* state, int* width, int* height) {
    if (width != nullptr) {
        *width = 0;
    }
    if (height != nullptr) {
        *height = 0;
    }
    if (state == nullptr || state->video_window == nullptr) {
        return;
    }

    RECT client_rect{};
    GetClientRect(state->video_window, &client_rect);
    if (width != nullptr) {
        *width = std::max(0L, client_rect.right - client_rect.left);
    }
    if (height != nullptr) {
        *height = std::max(0L, client_rect.bottom - client_rect.top);
    }
}

void DrawLogCard(
    AppState* state,
    HDC dc,
    const RECT& rect,
    const std::wstring& title,
    const std::wstring& hint) {
    DrawGlassCard(state, dc, rect, ScaleByDpi(state->status_window, 18), kColorCardBackground, kColorBorder, 6);
    RECT title_rect = MakeRect(
        rect.left + ScaleByDpi(state->status_window, 20),
        rect.top + ScaleByDpi(state->status_window, 20),
        rect.right - ScaleByDpi(state->status_window, 20),
        rect.top + ScaleByDpi(state->status_window, 42));
    DrawTextBlock(dc, state->section_font, title, title_rect, kColorTextPrimary, DT_LEFT | DT_TOP | DT_SINGLELINE);

    RECT hint_rect = MakeRect(
        rect.left + ScaleByDpi(state->status_window, 20),
        title_rect.bottom + ScaleByDpi(state->status_window, 4),
        rect.right - ScaleByDpi(state->status_window, 20),
        rect.top + ScaleByDpi(state->status_window, 78));
    DrawTextBlock(dc, state->subtitle_font, hint, hint_rect, kColorTextSecondary, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void DrawActionButton(AppState* state, const DRAWITEMSTRUCT* draw_item) {
    if (draw_item == nullptr) {
        return;
    }

    const bool navigation_button = IsNavigationButtonId(static_cast<int>(draw_item->CtlID));
    const bool nav_active =
        navigation_button &&
        state != nullptr &&
        PageFromNavigationButtonId(static_cast<int>(draw_item->CtlID)) == state->current_page;
    const bool accent_style = navigation_button ? nav_active : draw_item->CtlID == kFocusVideoButtonId;
    const bool pressed = (draw_item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (draw_item->itemState & ODS_DISABLED) != 0;
    const bool hot = (draw_item->itemState & ODS_HOTLIGHT) != 0;
    
    COLORREF fill_color = navigation_button
        ? (nav_active ? kColorSidebarActive : kColorSidebarSurface)
        : (accent_style ? (pressed ? kColorAccentDark : kColorAccent) : kColorWhite);
    COLORREF border_color = navigation_button
        ? (nav_active ? kColorSidebarActiveBorder : kColorSidebarSurface)
        : (accent_style ? fill_color : kColorBorder);
    COLORREF text_color = navigation_button
        ? (nav_active ? kColorSidebarTextActive : kColorSidebarText)
        : (accent_style ? kColorWhite : kColorTextPrimary);

    // 禁用状态
    if (disabled) {
        fill_color = navigation_button ? RGB(246, 246, 248) : RGB(248, 248, 250);
        border_color = navigation_button ? RGB(241, 241, 244) : RGB(236, 236, 240);
        text_color = navigation_button ? RGB(165, 165, 170) : RGB(160, 160, 165);
    } 
    // 按下状态 - 变暗
    else if (pressed && !nav_active) {
        if (navigation_button) {
            fill_color = RGB(243, 243, 246);
        } else if (!accent_style) {
            fill_color = RGB(247, 247, 249);
        } else {
            fill_color = RGB(0, 80, 180);
        }
    }
    // Hover 状态 - 变亮
    else if (hot && !pressed && !nav_active) {
        if (navigation_button) {
            fill_color = RGB(45, 45, 47);
        } else if (accent_style) {
            fill_color = RGB(30, 150, 255);
        } else {
            fill_color = RGB(255, 255, 255);
        }
    }

    // 绘制阴影（非按下、非禁用状态）
    if (!pressed && !disabled && (!navigation_button || nav_active)) {
        DrawSoftShadow(
            state,
            draw_item->hDC,
            draw_item->rcItem,
            std::max(14, RectHeight(draw_item->rcItem) / 2),
            navigation_button ? 3 : 4);
    }
    
    const int radius = navigation_button ? std::max(16, RectHeight(draw_item->rcItem) / 2) :
                                           std::max(18, RectHeight(draw_item->rcItem) / 2);
    FillRoundedRect(draw_item->hDC, draw_item->rcItem, radius, fill_color, border_color);

    // 添加左侧光晕效果（hover 或 active 状态）- 适用于所有按钮类型
    if ((hot || nav_active || (accent_style && hot)) && !disabled) {
        RECT glow_rect = draw_item->rcItem;
        glow_rect.right = glow_rect.left + ScaleByDpi(state->status_window, 60);
        InflateRect(&glow_rect, -4, -4);
        
        // 创建渐变光晕区域
        HRGN button_rgn = CreateRoundRectRgn(
            draw_item->rcItem.left, draw_item->rcItem.top,
            draw_item->rcItem.right, draw_item->rcItem.bottom, radius, radius);
        HRGN glow_rgn = CreateRoundRectRgn(
            glow_rect.left, glow_rect.top,
            glow_rect.right, glow_rect.bottom, radius - 2, radius - 2);
        HRGN intersect_rgn = CreateRectRgn(0, 0, 1, 1);
        CombineRgn(intersect_rgn, button_rgn, glow_rgn, RGN_AND);
        
        // 绘制半透明蓝色光晕
        HBRUSH glow_brush = CreateSolidBrush(RGB(30, 60, 120));
        SetBkColor(draw_item->hDC, RGB(30, 60, 120));
        FillRgn(draw_item->hDC, intersect_rgn, glow_brush);
        
        DeleteObject(glow_brush);
        DeleteObject(intersect_rgn);
        DeleteObject(glow_rgn);
        DeleteObject(button_rgn);
    }

    wchar_t text_buffer[96]{};
    GetWindowTextW(draw_item->hwndItem, text_buffer, static_cast<int>(_countof(text_buffer)));
    RECT text_rect = draw_item->rcItem;
    if (navigation_button) {
        text_rect.left += ScaleByDpi(state != nullptr ? state->status_window : nullptr, 18);
        text_rect.right -= ScaleByDpi(state != nullptr ? state->status_window : nullptr, 14);
    }
    DrawTextBlock(
        draw_item->hDC,
        state != nullptr ? state->button_font : nullptr,
        text_buffer,
        text_rect,
        text_color,
        navigation_button ? (DT_LEFT | DT_VCENTER | DT_SINGLELINE) : (DT_CENTER | DT_VCENTER | DT_SINGLELINE));

    if ((draw_item->itemState & ODS_FOCUS) != 0) {
        RECT focus_rect = draw_item->rcItem;
        InflateRect(&focus_rect, -3, -3);
        StrokeRoundedRect(draw_item->hDC, focus_rect, std::max(12, radius - 4), kColorFocusRing);
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

    HBRUSH temporary_background_brush = nullptr;
    HBRUSH background_brush = state != nullptr ? state->window_background_brush : nullptr;
    if (state != nullptr && state->web_ui_ready) {
        temporary_background_brush = CreateSolidBrush(kColorEmbeddedWindowBackground);
        background_brush = temporary_background_brush;
    }
    if (background_brush == nullptr) {
        background_brush = CreateSolidBrush(kColorWindowBackground);
    }
    FillRect(memory_dc, &client_rect, background_brush);
    DrawGlassBackdrop(state, memory_dc, client_rect);

    if (state != nullptr && state->web_ui_bridge == nullptr) {
        const StatusWindowLayout layout = CalculateStatusWindowLayout(state);
        const SessionSnapshot session = CopySessionSnapshot(state);
        const VideoStats stats = state->udp_receiver != nullptr ? state->udp_receiver->GetStats() : VideoStats{};
        const size_t decoder_queue_depth =
            state->decoder != nullptr ? state->decoder->GetPendingAccessUnitCount() : 0;

        bool has_clock_sync = false;
        int64_t sender_clock_rtt_us = 0;
        int64_t latest_latency_us = 0;
        double content_fps = 0.0;
        double receive_fps = 0.0;
        double decode_fps = 0.0;
        double display_fps = 0.0;
        uint64_t decoded_frame_count = 0;
        uint64_t displayed_frame_count = 0;
        {
            std::lock_guard<std::mutex> lock(state->metrics_mutex);
            has_clock_sync = state->has_clock_sync;
            sender_clock_rtt_us = state->sender_clock_rtt_us;
            latest_latency_us = state->latest_latency_us;
            content_fps = state->content_fps;
            receive_fps = state->receive_fps;
            decode_fps = state->decode_fps;
            display_fps = state->display_fps;
            decoded_frame_count = state->decoded_frame_count;
            displayed_frame_count = state->displayed_frame_count;
        }
        const uint64_t display_dropped_frames =
            decoded_frame_count > displayed_frame_count ? decoded_frame_count - displayed_frame_count : 0;
        const int64_t now_us = NowSteadyUs();
        const SenderIdentity sender_identity = ParseSenderIdentity(session.device_name);
        const std::wstring current_sender_name =
            !sender_identity.display_name.empty() ? sender_identity.display_name : L"\u7B49\u5F85\u5B89\u5353\u53D1\u9001\u7AEF";
        const std::wstring sender_version_label = sender_identity.version_label;
        const std::wstring sender_version_line =
            sender_version_label.empty()
                ? std::wstring()
                : (std::wstring(L"\r\n\u53D1\u9001\u7AEF\u7248\u672C\uFF1A") + sender_version_label);
        const std::wstring current_host = !session.host.empty() ? session.host : L"--";
        const std::wstring last_frame_text =
            FormatRelativeMoment(now_us, session.last_present_us, L"\u7B49\u5F85\u9996\u5E27");
        const std::wstring sync_text =
            has_clock_sync ? FormatLatencyMs(sender_clock_rtt_us) : L"\u7B49\u5F85\u6821\u65F6";

        int video_client_width = 0;
        int video_client_height = 0;
        GetVideoClientSize(state, &video_client_width, &video_client_height);
        double pixel_scale = 0.0;
        if (state->has_selected_profile && state->selected_profile.width > 0 && state->selected_profile.height > 0 &&
            video_client_width > 0 && video_client_height > 0) {
            pixel_scale = std::min(
                static_cast<double>(video_client_width) / static_cast<double>(state->selected_profile.width),
                static_cast<double>(video_client_height) / static_cast<double>(state->selected_profile.height));
        }

        const std::wstring current_profile_summary =
            state->has_selected_profile
                ? (FormatDimensions(state->selected_profile.width, state->selected_profile.height) +
                    L" / " + CodecName(state->selected_profile.codec) +
                    L" / " + FormatProfileFrameRate(state->selected_profile))
                : std::wstring(L"\u7B49\u5F85\u534F\u8BAE\u534F\u5546");
        const std::wstring current_profile_details = BuildProfileDetailBlock(
            state->has_selected_profile ? &state->selected_profile : nullptr,
            state->has_selected_profile,
            PresentationModeLabel(state->presentation_mode));
        const std::wstring airplay_status = BuildAirPlayStatusText(state);
        const std::wstring voice_control_status = BuildVoiceControlStatusText(state);
        const std::wstring airplay_error = state->airplay_controller.last_error();
        const std::wstring airplay_log_name =
            state->airplay_log_path.empty() ? std::wstring(L"--") : ExtractFileName(state->airplay_log_path);

        std::wstring hero_title;
        std::wstring hero_body;
        std::wstring side_label;
        std::wstring side_value;
        std::wstring side_body;
        std::array<DashboardCard, 4> summary_cards = state->dashboard.summary_cards;
        std::wstring left_title = kTextTrafficTitle;
        std::wstring left_body = state->dashboard.traffic_body;
        std::wstring right_title = kTextRuntimeTitle;
        std::wstring right_body = state->dashboard.runtime_body;
        std::wstring log_title = kTextLogTitle;
        std::wstring log_hint = std::wstring(kTextFilePrefix) + ExtractFileName(state->log_file_path);

        switch (state->current_page) {
        case StatusPage::kConnect:
            hero_title = session.control_connected
                ? (std::wstring(L"\u5F53\u524D\u6B63\u5728\u63A5\u6536 ") + current_sender_name)
                : L"\u5B89\u5353\u548C iPhone \u90FD\u53EF\u4EE5\u4ECE\u8FD9\u91CC\u627E\u5230\u5165\u53E3";
            hero_body =
                L"\u8FD9\u4E00\u9875\u4F1A\u540C\u65F6\u544A\u8BC9\u4F60 Android \u63A7\u5236\u901A\u9053\u3001\u89C6\u9891\u901A\u9053\u4EE5\u53CA Apple AirPlay \u670D\u52A1\u7684\u72B6\u6001\u3002"
                L"\r\n\u6240\u4EE5\u73B0\u5728\u4E0D\u7528\u518D\u5230\u65E5\u5FD7\u91CC\u53CD\u590D\u627E\u201C\u662F\u4E0D\u662F\u6CA1\u8D77\u6765\u201D\u4E86\u3002";
            side_label = L"\u5F53\u524D\u4F1A\u8BDD";
            side_value = session.control_connected ? current_sender_name : L"\u7B49\u5F85\u8BBE\u5907";
            side_body =
                std::wstring(L"\u5730\u5740\uFF1A") + current_host +
                L"\r\n\u63A7\u5236\u901A\u9053\uFF1ATCP/" + std::to_wstring(kControlPort) +
                L"\r\n\u89C6\u9891\u901A\u9053\uFF1AUDP/" + std::to_wstring(kVideoPort) +
                L"\r\n\u82F9\u679C\u6295\u5C4F\uFF1A" + airplay_status +
                L"\r\n\u63E1\u624B\u72B6\u6001\uFF1A" + (session.control_connected ? L"\u5DF2\u8FDE\u901A" : L"\u7B49\u5F85\u8FDE\u63A5") +
                L"\r\n\u6700\u8FD1\u753B\u9762\uFF1A" + last_frame_text +
                sender_version_line;
            summary_cards = {{
                DashboardCard{L"\u5F53\u524D\u8BBE\u5907", current_sender_name, std::wstring(L"\u5730\u5740\uFF1A") + current_host + sender_version_line},
                DashboardCard{L"\u5B89\u5353\u901A\u9053", session.control_connected ? L"\u5DF2\u8FDE\u901A" : L"\u7B49\u5F85 HELLO", L"TCP/5500\r\nTCP/55000"},
                DashboardCard{L"\u82F9\u679C\u6295\u5C4F", airplay_status, state->airplay_controller.running() ? L"iPhone / iPad \u53EF\u4ECE\u201C\u5C4F\u5E55\u955C\u50CF\u201D\u76F4\u63A5\u9009\u62E9\u201C\u76F4\u64AD\u6295\u5C4F\u52A9\u624B\u201D" : (airplay_error.empty() ? L"\u53EF\u901A\u8FC7\u4E0B\u65B9\u6309\u94AE\u542F\u52A8 Apple AirPlay \u670D\u52A1" : airplay_error)},
                DashboardCard{L"\u534F\u8BAE\u72B6\u6001", state->has_selected_profile ? L"\u5DF2\u534F\u5546" : L"\u5F85\u534F\u5546", current_profile_details},
            }};
            left_title = L"\u8FDE\u63A5\u4F53\u68C0";
            left_body =
                std::wstring(L"\u63A7\u5236\u7AEF\u53E3\uFF1ATCP/") + std::to_wstring(kControlPort) +
                L"\r\n\u89C6\u9891\u7AEF\u53E3\uFF1AUDP/" + std::to_wstring(kVideoPort) +
                L"\r\nAirPlay \u72B6\u6001\uFF1A" + airplay_status +
                L"\r\n\u65F6\u949F\u6821\u65F6\uFF1A" + sync_text +
                L"\r\n\u5F53\u524D\u914D\u7F6E\uFF1A" + current_profile_summary;
            right_title = L"\u63A5\u4E0B\u6765\u600E\u4E48\u505A";
            right_body =
                session.control_connected
                    ? L"\u5982\u679C\u5DF2\u7ECF\u6709\u753B\u9762\u7A97\u53E3\uFF0C\u4F18\u5148\u76F4\u63A5\u6253\u5F00\u6295\u5C4F\u7A97\u53E3\u770B\u753B\u9762\u3002"
                      L"\r\n\u82E5\u8FDE\u4E0A\u4E86\u4F46\u753B\u9762\u4E0D\u65B0\uFF0C\u53EF\u4EE5\u8BF7\u6C42\u5173\u952E\u5E27\u6216\u6253\u5F00\u72B6\u6001\u5FEB\u7167\u7ED9\u6211\u3002"
                    : (state->airplay_controller.running()
                        ? (std::wstring(L"Android \u8FD8\u6CA1\u8D77\u6765\u4E5F\u6CA1\u5173\u7CFB\u3002")
                            + L"\r\n\u73B0\u5728 iPhone / iPad \u53EF\u4EE5\u76F4\u63A5\u5728\u201C\u5C4F\u5E55\u955C\u50CF\u201D\u91CC\u9009\u62E9\u201C\u76F4\u64AD\u6295\u5C4F\u52A9\u624B\u201D\uFF0CApple \u753B\u9762\u4F1A\u7528\u72EC\u7ACB\u7A97\u53E3\u663E\u793A\u3002")
                        : (std::wstring(L"\u5F53\u524D\u8FD8\u6CA1\u6709 Android \u53D1\u9001\u7AEF\u8FDE\u4E0A\u3002")
                            + L"\r\n\u4F60\u53EF\u4EE5\u5728\u624B\u673A\u7AEF\u542F\u52A8 Android \u6295\u5C4F\uFF0C\u6216\u8005\u5148\u542F\u52A8 Apple AirPlay \u670D\u52A1\uFF0C\u8BA9 iPhone / iPad \u76F4\u63A5\u955C\u50CF\u5230\u8FD9\u53F0 PC\u3002"
                            + L"\r\nAirPlay \u65E5\u5FD7\u6587\u4EF6\uFF1A" + airplay_log_name));
            break;
        case StatusPage::kDisplay:
            hero_title = L"\u663E\u793A\u6A21\u5F0F\u4E0D\u518D\u85CF\u5728\u6295\u5C4F\u7A97\u53E3\u91CC";
            hero_body =
                L"\u8FD9\u91CC\u4F1A\u628A\u6E90\u5206\u8FA8\u7387\u3001\u6295\u5C4F\u7A97\u53E3 client area\u3001\u50CF\u7D20\u6620\u5C04\u548C GPU \u6E32\u67D3\u94FE\u8DEF\u62C6\u5F00\u663E\u793A\u3002"
                L"\r\n\u8FD9\u6837\u5C31\u4E0D\u4F1A\u518D\u628A\u7A97\u53E3\u7F29\u653E\u8BEF\u5224\u6210\u7801\u7387\u6216\u89E3\u7801\u95EE\u9898\u3002";
            side_label = L"\u5F53\u524D\u7A97\u53E3";
            side_value = FormatDimensions(video_client_width, video_client_height);
            side_body =
                std::wstring(L"\u6E90\u753B\u8D28\uFF1A") +
                (state->has_selected_profile ? FormatDimensions(state->selected_profile.width, state->selected_profile.height) : kTextMeasurePending) +
                L"\r\n\u50CF\u7D20\u6620\u5C04\uFF1A" + FormatScaleValue(pixel_scale) +
                L"\r\n\u865A\u62DF\u6444\u50CF\u5934\uFF1A" + BuildVirtualCameraStatusText(state) +
                L"\r\n\u6E32\u67D3\u663E\u5361\uFF1A" + (state->renderer.gpu_name().empty() ? L"\u7B49\u5F85 GPU" : state->renderer.gpu_name()) +
                L"\r\n\u9ED8\u8BA4\u52A8\u4F5C\uFF1A\u6309\u6E90\u753B\u8D28\u663E\u793A";
            summary_cards = {{
                DashboardCard{L"\u6E90\u5206\u8FA8\u7387", state->has_selected_profile ? FormatDimensions(state->selected_profile.width, state->selected_profile.height) : kTextMeasurePending, current_profile_details},
                DashboardCard{L"\u5F53\u524D\u663E\u793A\u533A\u57DF", FormatDimensions(video_client_width, video_client_height), L"client area\r\n\u4F1A\u6309\u6E90\u753B\u8D28\u91CD\u8BBE"},
                DashboardCard{L"\u50CF\u7D20\u6620\u5C04", FormatScaleValue(pixel_scale), L"\u5C0F\u4E8E 1.00x \u65F6\u4E3A\u7B49\u6BD4\u7F29\u5C0F\r\n\u5927\u4E8E 1.00x \u65F6\u4E3A\u7A97\u53E3\u653E\u5927"},
                DashboardCard{L"\u865A\u62DF\u6444\u50CF\u5934", BuildVirtualCameraStatusText(state), state->virtual_camera_controller.running() ? L"\u53EF\u5728 OBS \u3001\u4F1A\u8BAE\u8F6F\u4EF6\u6216\u5176\u4ED6\u652F\u6301\u6444\u50CF\u5934\u7684\u5E94\u7528\u4E2D\u9009\u62E9\u201C\u76F4\u64AD\u6295\u5C4F\u52A9\u624B\u865A\u62DF\u6444\u50CF\u5934\u201D" : L"\u5148\u5B89\u88C5\u518D\u542F\u52A8\uFF0C\u8F93\u51FA\u5F53\u524D\u6295\u5C4F\u753B\u9762"},
            }};
            left_title = L"\u663E\u793A\u68C0\u67E5";
            left_body =
                std::wstring(L"\u6E90\u753B\u8D28\uFF1A") +
                (state->has_selected_profile ? FormatDimensions(state->selected_profile.width, state->selected_profile.height) : kTextMeasurePending) +
                L"\r\n\u6295\u5C4F\u7A97\u53E3 client area\uFF1A" + FormatDimensions(video_client_width, video_client_height) +
                L"\r\n\u50CF\u7D20\u6620\u5C04\uFF1A" + FormatScaleValue(pixel_scale) +
                L"\r\n\u865A\u62DF\u6444\u50CF\u5934\uFF1A" + BuildVirtualCameraStatusText(state) +
                L"\r\n\u5F53\u524D\u663E\u793A\u901F\u7387\uFF1A" + FormatOptionalFps(display_fps) +
                L"\r\n\u7AEF\u5230\u7AEF\u5EF6\u8FDF\uFF1A" + (latest_latency_us > 0 ? FormatLatencyMs(latest_latency_us) : std::wstring(kTextWaitingSync));
            right_title = L"\u663E\u793A\u8BF4\u660E";
            right_body =
                L"\u5982\u679C\u4F60\u89C9\u5F97\u6587\u5B57\u53D1\u7CCA\uFF0C\u5148\u770B\u201C\u50CF\u7D20\u6620\u5C04\u201D\u662F\u4E0D\u662F\u5C0F\u4E8E 1.00x\u3002"
                L"\r\n\u5982\u679C\u753B\u9762\u7A97\u53E3\u6BD4\u6E90\u753B\u8D28\u5C0F\uFF0C\u5B83\u4ECD\u7136\u662F\u9AD8\u6E05\u6E90\uFF0C\u53EA\u662F\u88AB\u7B49\u6BD4\u7F29\u5C0F\u663E\u793A\u3002"
                L"\r\n\u5982\u679C\u9700\u8981\u7ED9 OBS \u6216\u5176\u4ED6\u8F6F\u4EF6\u91C7\u96C6\u753B\u9762\uFF0C\u5148\u70B9\u51FB\u201C\u5B89\u88C5 / \u542F\u52A8\u865A\u62DF\u6444\u50CF\u5934\u201D\u3002"
                L"\r\n\u201C\u6309\u6E90\u753B\u8D28\u663E\u793A\u201D\u4F1A\u91CD\u8BBE\u6295\u5C4F\u7A97\u53E3 client area\u3002";
            break;
        case StatusPage::kVoiceControl:
            hero_title = L"\u53EA\u4FDD\u7559\u63A5\u6536\u7AEF\u81EA\u5E26\u7684\u79BB\u7EBF\u8BED\u97F3\u63A7\u5236";
            hero_body =
                L"\u8FD9\u4E00\u9875\u53EA\u5904\u7406 PC \u7AEF\u7684\u8BED\u97F3\u64CD\u4F5C\u3002"
                L"\r\n\u73B0\u5728\u5DF2\u5B8C\u5168\u79FB\u9664 Windows \u7CFB\u7EDF\u8BED\u97F3\u8BBF\u95EE\u5165\u53E3\uFF0C\u53EA\u7559\u4E0B\u63A5\u6536\u7AEF\u81EA\u5DF1\u7684\u79BB\u7EBF\u8BC6\u522B\u3002"
                L"\r\n\u4E3A\u4E86\u517C\u987E\u54CD\u5E94\u901F\u5EA6\u548C\u9632\u8BEF\u89E6\u53D1\uFF0C\u73B0\u5728\u4F1A\u5BF9\u7A33\u5B9A\u547D\u4E2D\u7684\u4E2D\u9014\u8BC6\u522B\u7ED3\u679C\u63D0\u524D\u89E6\u53D1\uFF0C\u540C\u65F6\u4FDD\u7559\u6700\u7EC8\u8BC6\u522B\u786E\u8BA4\u548C\u6267\u884C\u540E\u7684\u77ED\u6682\u56DE\u58F0\u6291\u5236\u3002"
                L"\r\n\u9664\u4E86\u201C\u97F3\u4E50 / \u64AD\u653E / \u6682\u505C\u201D\uFF0C\u73B0\u5728\u4E5F\u53EF\u4EE5\u76F4\u63A5\u8BF4\u201C\u70ED\u8840\u201D\u3001\u201C\u7EAF\u97F3\u4E50\u201D\u6216\u201C123\u201D\u8FD9\u79CD\u76EE\u5F55\u540D\u6765\u968F\u673A\u64AD\u653E\u672C\u5730\u97F3\u4E50\u3002";
            side_label = L"\u5F53\u524D\u8BED\u97F3";
            side_value = voice_control_status;
            side_body =
                std::wstring(L"\u5E94\u7528\u8BED\u97F3\uFF1A") + voice_control_status +
                L"\r\n\u5173\u952E\u8BCD\uFF1A\u5A92\u4F53\u6307\u4EE4 + \u8BED\u97F3\u70B9\u6B4C\u76EE\u5F55\u540D" +
                L"\r\n\u89E6\u53D1\u89C4\u5219\uFF1A\u7A33\u5B9A\u4E2D\u9014\u7ED3\u679C\u53EF\u63D0\u524D\u89E6\u53D1\uFF0C\u6700\u7EC8\u8BC6\u522B\u4ECD\u4F1A\u5160\u5E95" +
                L"\r\n\u52A8\u4F5C\uFF1A\u5A92\u4F53\u63A7\u5236 / \u968F\u673A\u64AD\u653E\u8BED\u97F3\u70B9\u6B4C";
            summary_cards = {{
                DashboardCard{L"\u5E94\u7528\u8BED\u97F3\u72B6\u6001", state->voice_control_enabled ? L"\u5DF2\u5F00\u542F" : L"\u672A\u5F00\u542F", voice_control_status},
                DashboardCard{L"\u7CFB\u7EDF\u8BED\u97F3", L"\u5DF2\u79FB\u9664", L"\u4E0D\u518D\u63A5\u5165 Windows Voice Access \u6216\u7CFB\u7EDF\u542C\u5199"},
                DashboardCard{L"\u8BC6\u522B\u5173\u952E\u8BCD", L"\u5A92\u4F53\u6307\u4EE4 + \u76EE\u5F55\u540D", L"\u7A33\u5B9A\u4E2D\u9014\u7ED3\u679C\u4E5F\u53EF\u63D0\u524D\u6267\u884C"},
                DashboardCard{L"\u8BED\u97F3\u70B9\u6B4C\u76EE\u5F55", L"\u8BED\u97F3\u70B9\u6B4C", L"\u628A\u4E0D\u540C\u98CE\u683C\u7684\u97F3\u4E50\u5206\u5B50\u76EE\u5F55\u653E\u5230\u6210\u54C1\u4E3B\u76EE\u5F55\u4E0B"},
            }};
            left_title = L"\u4F7F\u7528\u8BF4\u660E";
            left_body =
                L"1. \u5148\u70B9\u51FB\u201C\u5F00\u542F\u8BED\u97F3\u63A7\u5236\u201D\uFF0C\u8BA9\u63A5\u6536\u7AEF\u5F00\u59CB\u76D1\u542C\u9EA6\u514B\u98CE\u3002"
                L"\r\n2. \u5F53\u4E2D\u9014\u8BC6\u522B\u7ED3\u679C\u5DF2\u7ECF\u7A33\u5B9A\u547D\u4E2D\u201C\u97F3\u4E50\u201D\u3001\u201C\u64AD\u653E\u201D\u3001\u201C\u6682\u505C\u201D\u6216\u5305\u542B\u201C\u64AD\u653E\u97F3\u4E50 / \u6682\u505C\u97F3\u4E50\u201D\u65F6\uFF0C\u5E94\u7528\u4F1A\u63D0\u524D\u6267\u884C\u3002"
                L"\r\n3. \u628A\u4E0D\u540C\u98CE\u683C\u7684\u97F3\u4E50\u653E\u5230\u6210\u54C1\u4E3B\u76EE\u5F55\u4E0B\u7684\u201C\u8BED\u97F3\u70B9\u6B4C\u201D\u5B50\u76EE\u5F55\u91CC\uFF0C\u7136\u540E\u5BF9\u7740 PC \u8BF4\u76EE\u5F55\u540D\u5C31\u4F1A\u968F\u673A\u64AD\u653E\u3002"
                L"\r\n4. \u7B2C\u4E8C\u4E2A\u6309\u94AE\u53EF\u4EE5\u76F4\u63A5\u6253\u5F00\u672C\u6B21\u8FD0\u884C\u65E5\u5FD7\uFF0C\u7B2C\u4E09\u4E2A\u6309\u94AE\u6253\u5F00\u6210\u54C1\u76EE\u5F55\u3002";
            right_title = L"\u8BBE\u7F6E\u4E0E\u6392\u67E5";
            right_body =
                L"\u5982\u679C\u73AF\u5883\u58F0\u8FD8\u662F\u4F1A\u8BEF\u89E6\u53D1\uFF0C\u5148\u67E5\u770B\u65E5\u5FD7\u91CC\u7684\u201C\u8BC6\u522B\u5230\u201D\u6587\u5B57\uFF0C\u786E\u8BA4\u7A76\u7ADF\u88AB\u542C\u6210\u4E86\u54EA\u4E2A\u53E3\u4EE4\u3002"
                L"\r\n\u5F53\u524D\u7248\u672C\u5DF2\u7ECF\u5B8C\u5168\u79FB\u9664 Windows \u7CFB\u7EDF\u8BED\u97F3\u8BBF\u95EE\u7EC4\u4EF6\uFF0C\u4E0D\u4F1A\u518D\u53BB\u6253\u5F00\u6216\u5173\u95ED\u5B83\u3002"
                L"\r\n\u5982\u679C\u4F60\u7A0D\u540E\u8FD8\u60F3\u628A\u8BEF\u89E6\u53D1\u538B\u5F97\u66F4\u4F4E\uFF0C\u6211\u53EF\u4EE5\u518D\u7ED9\u5B83\u52A0\u5524\u9192\u8BCD\u6216\u80FD\u91CF\u95E8\u9650\u3002";
            break;
        case StatusPage::kDiagnostics:
            hero_title = L"\u8BCA\u65AD\u6062\u590D\u96C6\u4E2D\u5230\u4E00\u5C4F";
            hero_body =
                L"\u8FD9\u4E00\u9875\u4F18\u5148\u56DE\u7B54\u201C\u662F\u54EA\u4E00\u6BB5\u5361\u4F4F\u4E86\u201D\u3002"
                L"\r\n\u5982\u679C\u63A7\u5236\u901A\u9053\u8FD8\u5728\uFF0C\u4F46\u753B\u9762\u5361\u4F4F\uFF0C\u5C31\u5148\u770B\u961F\u5217\u548C\u6700\u8FD1\u65E5\u5FD7\u3002";
            side_label = L"\u5F53\u524D\u5224\u65AD";
            side_value =
                !session.control_connected ? L"\u7B49\u5F85\u4F1A\u8BDD" :
                (decoder_queue_depth > 0 ? L"\u5173\u6CE8\u89E3\u7801\u961F\u5217" : L"\u94FE\u8DEF\u57FA\u672C\u7A33\u5B9A");
            side_body =
                std::wstring(L"\u63A7\u5236\u8FDE\u63A5\uFF1A") + (session.control_connected ? L"\u5DF2\u8FDE\u901A" : L"\u672A\u8FDE\u901A") +
                L"\r\n\u961F\u5217\u79EF\u538B\uFF1A" + std::to_wstring(decoder_queue_depth) +
                L"\r\n\u6700\u65B0\u65E5\u5FD7\u4F1A\u76F4\u63A5\u663E\u793A\u5728\u4E0B\u65B9";
            summary_cards = {{
                DashboardCard{L"\u63A7\u5236\u72B6\u6001", session.control_connected ? L"\u5DF2\u8FDE\u901A" : L"\u7B49\u5F85", L"\u4E0A\u6B21\u753B\u9762 " + last_frame_text},
                DashboardCard{L"\u5F85\u89E3\u961F\u5217", std::to_wstring(decoder_queue_depth), L"\u89E3\u7801\u961F\u5217 / \u5DF2\u663E " + std::to_wstring(displayed_frame_count)},
                DashboardCard{L"\u7AEF\u5230\u7AEF\u5EF6\u8FDF", latest_latency_us > 0 ? FormatLatencyMs(latest_latency_us) : kTextWaitingSync, L"\u6821\u65F6 RTT " + sync_text},
                DashboardCard{L"NVDEC", BuildNvdecStatusText(state), state->renderer.gpu_name().empty() ? L"\u7B49\u5F85 GPU \u521D\u59CB\u5316" : state->renderer.gpu_name()},
            }};
            right_title = L"\u6062\u590D\u52A8\u4F5C";
            right_body =
                L"1. \u63A7\u5236\u901A\u9053\u8FD8\u5728\u4F46\u753B\u9762\u7A81\u7136\u4E0D\u65B0\uFF0C\u5148\u8BF7\u6C42\u5173\u952E\u5E27\u3002"
                L"\r\n2. \u6709\u5E27\u7387\u4F46\u7A97\u53E3\u9ED1\u5C4F\uFF0C\u4F18\u5148\u5BF9\u7167 GPU/NVDEC \u72B6\u6001\u3002"
                L"\r\n3. \u8981\u53D1\u6211\u6392\u67E5\u65F6\uFF0C\u76F4\u63A5\u6253\u5F00\u72B6\u6001\u5FEB\u7167\u548C\u65E5\u5FD7\u6587\u4EF6\u3002";
            log_title = L"\u5B9E\u65F6\u65E5\u5FD7";
            break;
        case StatusPage::kLogs:
            hero_title = L"\u65E5\u5FD7\u548C\u72B6\u6001\u5FEB\u7167\u5355\u72EC\u6536\u53E3";
            hero_body =
                L"\u8FD9\u4E00\u9875\u4FDD\u7559\u5B9E\u65F6\u65E5\u5FD7\u67E5\u770B\uFF0C\u540C\u65F6\u7ED9\u4F60\u65E5\u5FD7\u6587\u4EF6\u3001\u72B6\u6001\u5FEB\u7167\u548C\u8F93\u51FA\u76EE\u5F55\u5165\u53E3\u3002"
                L"\r\n\u9700\u8981\u53D1\u6211\u6392\u67E5\u65F6\uFF0C\u76F4\u63A5\u4ECE\u8FD9\u91CC\u6253\u5F00\u5C31\u884C\u3002";
            side_label = L"\u5BFC\u51FA\u4FE1\u606F";
            side_value = ExtractFileName(state->log_file_path);
            side_body =
                std::wstring(L"\u72B6\u6001\u5FEB\u7167\uFF1A") + ExtractFileName(state->status_file_path) +
                L"\r\n\u6700\u8FD1\u753B\u9762\uFF1A" + last_frame_text +
                L"\r\n\u603B\u6570\u636E\u5305\uFF1A" + std::to_wstring(stats.total_packets);
            summary_cards = {{
                DashboardCard{L"\u603B\u6570\u636E\u5305", std::to_wstring(stats.total_packets), FormatDataSize(stats.total_bytes)},
                DashboardCard{L"\u5B8C\u6574\u5E27", std::to_wstring(stats.completed_frames), L"\u5173\u952E\u5E27 " + std::to_wstring(stats.keyframes)},
                DashboardCard{L"\u4E22\u5E27", std::to_wstring(stats.dropped_frames), L"\u663E\u793A\u4E22\u5E27 " + std::to_wstring(display_dropped_frames)},
                DashboardCard{L"\u6700\u540E\u5E27", std::to_wstring(stats.last_frame_id), last_frame_text},
            }};
            log_hint = std::wstring(kTextFilePrefix) + state->log_file_path;
            break;
        case StatusPage::kOverview:
        default:
            hero_title = state->has_selected_profile ? L"\u4E00\u4F53\u5316\u63A5\u6536\u524D\u53F0\u5DF2\u5C31\u7EEA" : L"\u63A5\u6536\u7AEF\u524D\u53F0\u5DF2\u6253\u5F00\uFF0C\u7B49\u5F85\u624B\u673A\u63A5\u5165";
            hero_body =
                std::wstring(L"\u53D1\u9001\u7AEF\uFF1A") + current_sender_name +
                L"\r\n\u5730\u5740\uFF1A" + current_host +
                L"\r\n\u5F53\u524D\u914D\u7F6E\uFF1A" + current_profile_summary +
                L"\r\n\u8FD9\u91CC\u5C31\u662F\u63A5\u6536\u3001\u663E\u793A\u3001\u8BED\u97F3\u548C\u8BCA\u65AD\u5171\u7528\u7684\u540C\u4E00\u4E2A\u524D\u53F0\u7A0B\u5E8F\uFF0C\u4E0D\u518D\u62C6\u5206\u72EC\u7ACB\u63A7\u5236\u7AEF\u3002";
            side_label = L"\u5F53\u524D\u4F1A\u8BDD";
            side_value = session.control_connected ? current_sender_name : L"\u7B49\u5F85\u8FDE\u63A5";
            side_body =
                std::wstring(L"\u5730\u5740\uFF1A") + current_host +
                L"\r\n\u5F53\u524D\u914D\u7F6E\uFF1A" + current_profile_summary +
                L"\r\n\u6700\u8FD1\u753B\u9762\uFF1A" + last_frame_text +
                L"\r\n\u6E32\u67D3\u663E\u5361\uFF1A" +
                (state->renderer.gpu_name().empty() ? L"\u7B49\u5F85\u521D\u59CB\u5316" : state->renderer.gpu_name()) +
                sender_version_line;
            summary_cards = {{
                DashboardCard{L"\u5F53\u524D\u914D\u7F6E", state->has_selected_profile ? FormatDimensions(state->selected_profile.width, state->selected_profile.height) : L"\u7B49\u5F85\u8FDE\u63A5", current_profile_details},
                DashboardCard{L"\u5185\u5BB9\u6E90\u5E27\u7387", FormatOptionalFps(content_fps), BuildRateDetailBlock(receive_fps, decode_fps, display_fps)},
                DashboardCard{L"\u4F30\u7B97\u7AEF\u5230\u7AEF\u5EF6\u8FDF", latest_latency_us > 0 ? FormatLatencyMs(latest_latency_us) : kTextWaitingSync, std::wstring(L"\u6821\u65F6\u5F80\u8FD4\uFF1A") + sync_text + L"\r\n\u6700\u8FD1\u753B\u9762\uFF1A" + last_frame_text},
                DashboardCard{L"\u663E\u793A\u4E22\u5E27", std::to_wstring(display_dropped_frames), BuildFrameCounterBlock(stats, displayed_frame_count, decoded_frame_count)},
            }};
            break;
        }

        const COLORREF shell_fill = kColorWindowBackground;
        const COLORREF shell_border = kColorWindowBackground;
        DrawGlassCard(state, memory_dc, layout.shell, ScaleByDpi(hwnd, 30), shell_fill, shell_border, 0);
        DrawGlassCard(
            state,
            memory_dc,
            layout.topbar,
            ScaleByDpi(hwnd, 24),
            kColorTopbarBackground,
            kColorTopbarBorder,
            5);
        DrawGlassCard(
            state,
            memory_dc,
            layout.sidebar,
            ScaleByDpi(hwnd, 24),
            kColorSidebarBackground,
            kColorSidebarBorder,
            4);

        const int icon_size = ScaleByDpi(hwnd, 58);
        const RECT icon_rect = MakeRect(
            layout.topbar.left + ScaleByDpi(hwnd, 18),
            layout.topbar.top + ScaleByDpi(hwnd, 14),
            layout.topbar.left + ScaleByDpi(hwnd, 18) + icon_size,
            layout.topbar.top + ScaleByDpi(hwnd, 14) + icon_size);
        DrawBrandGlyph(memory_dc, icon_rect);

        const int title_left = icon_rect.right + ScaleByDpi(hwnd, 16);
        const int pill_top = layout.topbar.top + ScaleByDpi(hwnd, 18);
        const int pill_height = ScaleByDpi(hwnd, 28);
        const int pill_gap = ScaleByDpi(hwnd, 10);
        const std::wstring version_text = std::wstring(kTextVersionPrefix) + kAppBuildLabel;
        const std::wstring page_text = StatusPageLabel(state->current_page);
        const int version_pill_width = std::max(
            ScaleByDpi(hwnd, 220),
            MeasureSingleLineTextWidth(memory_dc, state->subtitle_font, version_text) + ScaleByDpi(hwnd, 28));
        const int page_pill_width = std::max(
            ScaleByDpi(hwnd, 96),
            MeasureSingleLineTextWidth(memory_dc, state->subtitle_font, page_text) + ScaleByDpi(hwnd, 28));
        const RECT version_pill_rect = MakeRect(
            layout.topbar.right - ScaleByDpi(hwnd, 18) - version_pill_width,
            pill_top,
            layout.topbar.right - ScaleByDpi(hwnd, 18),
            pill_top + pill_height);
        const RECT page_pill_rect = MakeRect(
            version_pill_rect.left - pill_gap - page_pill_width,
            pill_top,
            version_pill_rect.left - pill_gap,
            pill_top + pill_height);
        DrawPill(
            memory_dc,
            state->subtitle_font,
            page_pill_rect,
            page_text,
            RGB(237, 245, 255),
            kColorAccentDark);
        DrawPill(
            memory_dc,
            state->subtitle_font,
            version_pill_rect,
            version_text,
            RGB(249, 249, 251),
            kColorTextSecondary);

        const int title_right = std::max(title_left + ScaleByDpi(hwnd, 220), static_cast<int>(page_pill_rect.left) - ScaleByDpi(hwnd, 18));
        RECT title_rect = MakeRect(
            title_left,
            layout.topbar.top + ScaleByDpi(hwnd, 16),
            title_right,
            layout.topbar.top + ScaleByDpi(hwnd, 42));
        DrawTextBlock(
            memory_dc,
            state->value_font,
            kTextAppDisplayTitle,
            title_rect,
            kColorTextPrimary,
            DT_LEFT | DT_TOP | DT_SINGLELINE);
        RECT subtitle_rect = MakeRect(
            title_left,
            title_rect.bottom + ScaleByDpi(hwnd, 6),
            title_right,
            layout.topbar.bottom - ScaleByDpi(hwnd, 14));
        DrawTextBlock(
            memory_dc,
            state->subtitle_font,
            StatusPageSummary(state->current_page),
            subtitle_rect,
            kColorTextSecondary,
            DT_LEFT | DT_TOP | DT_WORDBREAK);

        RECT nav_label_rect = MakeRect(
            layout.sidebar.left + ScaleByDpi(hwnd, 18),
            layout.sidebar.top + ScaleByDpi(hwnd, 18),
            layout.sidebar.right - ScaleByDpi(hwnd, 18),
            layout.sidebar.top + ScaleByDpi(hwnd, 38));
        DrawTextBlock(memory_dc, state->subtitle_font, L"\u63A7\u5236\u53F0", nav_label_rect, kColorSidebarMuted, DT_LEFT | DT_TOP | DT_SINGLELINE);
        RECT nav_subtitle_rect = MakeRect(
            layout.sidebar.left + ScaleByDpi(hwnd, 18),
            nav_label_rect.bottom + ScaleByDpi(hwnd, 4),
            layout.sidebar.right - ScaleByDpi(hwnd, 18),
            layout.sidebar.top + ScaleByDpi(hwnd, 58));
        DrawTextBlock(memory_dc, state->section_font, StatusPageLabel(state->current_page), nav_subtitle_rect, kColorSidebarText, DT_LEFT | DT_TOP | DT_SINGLELINE);
        RECT sidebar_hint_rect = MakeRect(
            layout.sidebar.left + ScaleByDpi(hwnd, 18),
            layout.sidebar.bottom - ScaleByDpi(hwnd, 210),
            layout.sidebar.right - ScaleByDpi(hwnd, 18),
            layout.sidebar.bottom - ScaleByDpi(hwnd, 18));
        DrawTextBlock(
            memory_dc,
            state->subtitle_font,
            std::wstring(kTextFeatureDescription) +
                L"\r\n" + kTextFeatureGroups +
                L"\r\n" + kTextAuthor +
                L"\r\n\r\n\u53D1\u9001\u7AEF\uFF1A" + current_sender_name +
                (sender_version_label.empty() ? std::wstring() : (std::wstring(L"\r\n\u7248\u672C\uFF1A") + sender_version_label)) +
                L"\r\n\u5730\u5740\uFF1A" + current_host +
                L"\r\nTCP/" + std::to_wstring(kControlPort) + L"    UDP/" + std::to_wstring(kVideoPort),
            sidebar_hint_rect,
            kColorSidebarMuted,
            DT_LEFT | DT_TOP | DT_WORDBREAK);

        DrawGlassCard(
            state,
            memory_dc,
            layout.hero_main,
            ScaleByDpi(hwnd, 24),
            kColorHeroBackground,
            kColorBorder,
            7);
        const int hero_inner_padding = ScaleByDpi(hwnd, 24);
        const int hero_title_top = layout.hero_main.top + ScaleByDpi(hwnd, 22);
        const int hero_content_width = std::max(1, RectWidth(layout.hero_main) - hero_inner_padding * 2);
        const int hero_title_height = std::max(
            ScaleByDpi(hwnd, 34),
            MeasureTextBlockHeight(memory_dc, state->title_font, hero_title, hero_content_width, DT_LEFT | DT_TOP | DT_WORDBREAK));
        RECT hero_title_rect = MakeRect(
            layout.hero_main.left + hero_inner_padding,
            hero_title_top,
            layout.hero_main.right - hero_inner_padding,
            hero_title_top + hero_title_height);
        DrawTextBlock(memory_dc, state->title_font, hero_title, hero_title_rect, kColorTextPrimary, DT_LEFT | DT_TOP | DT_WORDBREAK);
        RECT hero_body_rect = MakeRect(
            layout.hero_main.left + hero_inner_padding,
            hero_title_rect.bottom + ScaleByDpi(hwnd, 10),
            layout.hero_main.right - hero_inner_padding,
            layout.focus_button.top - ScaleByDpi(hwnd, 14));
        DrawTextBlock(memory_dc, state->body_font, hero_body, hero_body_rect, kColorTextSecondary, DT_LEFT | DT_TOP | DT_WORDBREAK);

        DrawSpotlightCard(state, memory_dc, layout.hero_side, side_label, side_value, side_body, false);

        for (size_t index = 0; index < layout.summary_cards.size(); ++index) {
            DrawSummaryCard(state, memory_dc, layout.summary_cards[index], summary_cards[index], index == 0);
        }

        if (state->current_page == StatusPage::kDisplay) {
            DrawSectionCard(state, memory_dc, layout.detail_left, left_title, left_body);
            DrawSectionCard(state, memory_dc, layout.detail_right, right_title, right_body);
        } else if (state->current_page == StatusPage::kDiagnostics) {
            DrawLogCard(state, memory_dc, layout.detail_left, log_title, log_hint);
            DrawSectionCard(state, memory_dc, layout.detail_right, right_title, right_body + L"\r\n\r\n" + BuildRecentLogPreview(state, 4));
        } else if (state->current_page == StatusPage::kLogs) {
            DrawLogCard(state, memory_dc, layout.detail_full, log_title, log_hint);
        } else {
            DrawSectionCard(state, memory_dc, layout.detail_left, left_title, left_body);
            DrawSectionCard(state, memory_dc, layout.detail_right, right_title, right_body);
        }
    }

    BitBlt(target_dc, 0, 0, width, height, memory_dc, 0, 0, SRCCOPY);

    SelectObject(memory_dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);

    if (temporary_background_brush != nullptr) {
        DeleteObject(temporary_background_brush);
        return;
    }
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

bool FileExists(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }

    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }

    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring GetLocalAppDataPath() {
    DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required == 0) {
        return L"";
    }

    std::wstring path(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", path.data(), required);
    if (written == 0) {
        return L"";
    }
    if (!path.empty() && path.back() == L'\0') {
        path.pop_back();
    } else {
        path.resize(written);
    }
    return path;
}

bool EnsureDirectoryPath(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path), error);
    return !error;
}

bool ShouldRefreshVoiceAsset(const std::wstring& source_probe_path, const std::wstring& dest_probe_path) {
    if (!FileExists(source_probe_path)) {
        return false;
    }
    if (!FileExists(dest_probe_path)) {
        return true;
    }

    std::error_code error;
    const auto source_time = std::filesystem::last_write_time(std::filesystem::path(source_probe_path), error);
    if (error) {
        return false;
    }
    error.clear();
    const auto dest_time = std::filesystem::last_write_time(std::filesystem::path(dest_probe_path), error);
    if (error) {
        return true;
    }
    return source_time > dest_time;
}

bool CopyDirectoryContents(const std::wstring& source_path, const std::wstring& destination_path) {
    if (!DirectoryExists(source_path)) {
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(destination_path), error);
    if (error) {
        return false;
    }

    std::filesystem::copy(
        std::filesystem::path(source_path),
        std::filesystem::path(destination_path),
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
        error);
    return !error;
}

std::wstring NormalizeFullPath(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }

    DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        return path;
    }

    std::wstring normalized(required, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), required, normalized.data(), nullptr);
    if (written == 0) {
        return path;
    }

    if (!normalized.empty() && normalized.back() == L'\0') {
        normalized.pop_back();
    } else {
        normalized.resize(written);
    }
    return normalized;
}

std::wstring BuildLogFilePath() {
    return GetExecutableDirectory() + L"\\receiver-log.txt";
}

std::wstring BuildStatusFilePath() {
    return GetExecutableDirectory() + L"\\receiver-status.json";
}

std::wstring BuildWebUiFolderPath() {
    return GetExecutableDirectory() + L"\\web-ui";
}

std::wstring BuildWebUiUserDataPath() {
    return GetExecutableDirectory() + L"\\webview-data";
}

std::wstring BuildProfileCachePath() {
    return GetExecutableDirectory() + L"\\receiver-profile-cache.json";
}

std::wstring BuildCodecConfigCachePath() {
    return GetExecutableDirectory() + L"\\receiver-codec-config.bin";
}

std::wstring BuildDanmakuCaptureRootPath() {
    return NormalizeFullPath(GetExecutableDirectory() + L"\\\u5f39\u5e55\u8bc6\u522b");
}

std::wstring BuildDanmakuRegionFilePath() {
    return NormalizeFullPath(BuildDanmakuCaptureRootPath() + L"\\\u5f39\u5e55\u533a\u57df.json");
}

std::wstring BuildAutoReplyRootPath() {
    return NormalizeFullPath(GetExecutableDirectory() + L"\\\u5f39\u5e55\u81ea\u52a8\u56de\u590d");
}

std::wstring BuildAutoReplyConfigPath() {
    return NormalizeFullPath(BuildAutoReplyRootPath() + L"\\\u81ea\u52a8\u56de\u590d\u914d\u7f6e.json");
}

std::wstring BuildVirtualCameraToolPath() {
    return GetExecutableDirectory() + L"\\" + virtual_camera::kToolFileName;
}

std::wstring BuildVirtualCameraMediaSourcePath() {
    return GetExecutableDirectory() + L"\\" + virtual_camera::kMediaSourceFileName;
}

std::wstring BuildVirtualCameraPlaceholderPath() {
    const std::wstring packaged = GetExecutableDirectory() + L"\\" + virtual_camera::kPlaceholderImageFileName;
    if (FileExists(packaged)) {
        return packaged;
    }

    const std::wstring dev_fallback =
        NormalizeFullPath(GetExecutableDirectory() + L"\\..\\assets\\" + virtual_camera::kPlaceholderImageFileName);
    if (FileExists(dev_fallback)) {
        return dev_fallback;
    }

    return packaged;
}

std::wstring BuildAirPlayRuntimeRootPath() {
    const std::wstring packaged = GetExecutableDirectory() + L"\\apple-airplay";
    if (FileExists(packaged + L"\\_internal\\bin\\uxplay.exe")) {
        return packaged;
    }

    const std::wstring dev_fallback = NormalizeFullPath(GetExecutableDirectory() + L"\\..\\..\\..\\tools\\uxplay-windows\\installed");
    if (FileExists(dev_fallback + L"\\_internal\\bin\\uxplay.exe")) {
        return dev_fallback;
    }

    return packaged;
}

std::wstring BuildAirPlayLogPath() {
    return GetExecutableDirectory() + L"\\airplay-receiver.log";
}

std::wstring BuildVoiceRuntimeRootPath() {
    const std::wstring packaged_source = GetExecutableDirectory() + L"\\voice-runtime";
    std::wstring source = packaged_source;
    if (!FileExists(source + L"\\sherpa-onnx-c-api.dll")) {
        source = NormalizeFullPath(
            GetExecutableDirectory() + L"\\..\\..\\third_party\\sherpa-onnx\\" +
            std::wstring(kVoiceRuntimePackageFolder) + L"\\lib");
    }
    if (!FileExists(source + L"\\sherpa-onnx-c-api.dll")) {
        return packaged_source;
    }

    const std::wstring local_app_data = GetLocalAppDataPath();
    if (local_app_data.empty()) {
        return source;
    }

    const std::wstring cache_root = NormalizeFullPath(local_app_data + L"\\LiveCastAssistant\\offline-voice\\voice-runtime");
    if (ShouldRefreshVoiceAsset(source + L"\\sherpa-onnx-c-api.dll", cache_root + L"\\sherpa-onnx-c-api.dll") ||
        !FileExists(cache_root + L"\\sherpa-onnx-c-api.dll")) {
        if (CopyDirectoryContents(source, cache_root)) {
            return cache_root;
        }
    }
    return FileExists(cache_root + L"\\sherpa-onnx-c-api.dll") ? cache_root : source;
}

std::wstring BuildVoiceModelPath() {
    const std::wstring packaged_target =
        NormalizeFullPath(BuildVoiceMusicRootPath() + L"\\voice-model\\" + std::wstring(kVoiceModelFolderName));
    if (FileExists(packaged_target + L"\\encoder.int8.onnx") &&
        FileExists(packaged_target + L"\\decoder.onnx") &&
        FileExists(packaged_target + L"\\joiner.int8.onnx") &&
        FileExists(packaged_target + L"\\tokens.txt")) {
        return packaged_target;
    }

    const std::wstring packaged_source = GetExecutableDirectory() + L"\\voice-model\\" + std::wstring(kVoiceModelFolderName);
    std::wstring source = packaged_source;
    if (!(FileExists(source + L"\\encoder.int8.onnx") &&
          FileExists(source + L"\\decoder.onnx") &&
          FileExists(source + L"\\joiner.int8.onnx") &&
          FileExists(source + L"\\tokens.txt"))) {
        source = NormalizeFullPath(
            GetExecutableDirectory() + L"\\..\\..\\assets\\voice-model-sherpa\\" +
            std::wstring(kVoiceModelFolderName));
    }
    if (!(FileExists(source + L"\\encoder.int8.onnx") &&
          FileExists(source + L"\\decoder.onnx") &&
          FileExists(source + L"\\joiner.int8.onnx") &&
          FileExists(source + L"\\tokens.txt"))) {
        return packaged_target;
    }

    if (ShouldRefreshVoiceAsset(source + L"\\encoder.int8.onnx", packaged_target + L"\\encoder.int8.onnx") ||
        !(FileExists(packaged_target + L"\\encoder.int8.onnx") &&
          FileExists(packaged_target + L"\\decoder.onnx") &&
          FileExists(packaged_target + L"\\joiner.int8.onnx") &&
          FileExists(packaged_target + L"\\tokens.txt"))) {
        if (CopyDirectoryContents(source, packaged_target)) {
            return packaged_target;
        }
    }
    return (FileExists(packaged_target + L"\\encoder.int8.onnx") &&
            FileExists(packaged_target + L"\\decoder.onnx") &&
            FileExists(packaged_target + L"\\joiner.int8.onnx") &&
            FileExists(packaged_target + L"\\tokens.txt"))
               ? packaged_target
               : source;
}

std::wstring BuildVoiceModelRuntimePath(const std::wstring& model_source_path) {
    if (!(FileExists(model_source_path + L"\\encoder.int8.onnx") &&
          FileExists(model_source_path + L"\\decoder.onnx") &&
          FileExists(model_source_path + L"\\joiner.int8.onnx") &&
          FileExists(model_source_path + L"\\tokens.txt"))) {
        return model_source_path;
    }

    std::error_code error;
    const std::filesystem::path exe_dir(GetExecutableDirectory());
    const std::wstring drive_root = exe_dir.root_path().wstring();
    if (drive_root.empty()) {
        return model_source_path;
    }

    const std::wstring runtime_root =
        NormalizeFullPath(drive_root + L"LiveCastAssistantRuntime\\voice-model\\" + std::wstring(kVoiceModelFolderName));
    if (ShouldRefreshVoiceAsset(model_source_path + L"\\encoder.int8.onnx", runtime_root + L"\\encoder.int8.onnx") ||
        !(FileExists(runtime_root + L"\\encoder.int8.onnx") &&
          FileExists(runtime_root + L"\\decoder.onnx") &&
          FileExists(runtime_root + L"\\joiner.int8.onnx") &&
          FileExists(runtime_root + L"\\tokens.txt"))) {
        std::filesystem::create_directories(std::filesystem::path(runtime_root).parent_path(), error);
        error.clear();
        if (CopyDirectoryContents(model_source_path, runtime_root)) {
            return runtime_root;
        }
    }

    return (FileExists(runtime_root + L"\\encoder.int8.onnx") &&
            FileExists(runtime_root + L"\\decoder.onnx") &&
            FileExists(runtime_root + L"\\joiner.int8.onnx") &&
            FileExists(runtime_root + L"\\tokens.txt"))
               ? runtime_root
               : model_source_path;
}

std::wstring BuildVoiceMusicRootPath() {
    return GetExecutableDirectory() + L"\\\u8BED\u97F3\u70B9\u6B4C";
}

std::wstring BuildAirPlayStatusText(const AppState* state) {
    if (state == nullptr) {
        return L"\u672A\u5C31\u7EEA";
    }

    if (state->airplay_controller.running()) {
        return L"\u5DF2\u542F\u52A8";
    }
    if (!state->airplay_controller.installed()) {
        return L"\u7EC4\u4EF6\u7F3A\u5931";
    }
    if (!state->airplay_controller.bonjour_installed()) {
        return L"Bonjour \u7F3A\u5931";
    }
    if (!state->airplay_controller.bonjour_running()) {
        return L"Bonjour \u672A\u8FD0\u884C";
    }
    if (!state->airplay_controller.last_error().empty()) {
        return L"\u542F\u52A8\u5931\u8D25";
    }
    return L"\u5DF2\u5C31\u7EEA\u672A\u542F\u52A8";
}

void RefreshAirPlayState(AppState* state) {
    if (state == nullptr) {
        return;
    }

    state->airplay_controller.RefreshInstallState();
    UpdateActionButtons(state);
    if (state->status_window != nullptr) {
        PostMessageW(state->status_window, kStatsMessage, 0, 0);
    }
}

void StartAirPlayReceiver(AppState* state, bool automatic) {
    if (state == nullptr) {
        return;
    }

    RefreshAirPlayState(state);
    if (!state->airplay_controller.installed()) {
        if (!automatic) {
            QueueLog(state, L"\u82F9\u679C\u6295\u5C4F: \u672A\u68C0\u6D4B\u5230 apple-airplay \u8FD0\u884C\u65F6\u76EE\u5F55\uFF0C\u8BF7\u4F7F\u7528\u968F\u6210\u54C1\u4E00\u8D77\u6253\u5305\u7684 PC \u7248\u672C\u3002");
        }
        return;
    }

    if (state->airplay_controller.running()) {
        if (!automatic) {
            QueueLog(state, L"\u82F9\u679C\u6295\u5C4F: AirPlay \u670D\u52A1\u5DF2\u5728\u8FD0\u884C\u3002");
        }
        return;
    }

    state->airplay_controller.Start(kAirPlayServerName);
    RefreshAirPlayState(state);
}

void StopAirPlayReceiver(AppState* state, bool write_log) {
    if (state == nullptr) {
        return;
    }

    (void)write_log;
    state->airplay_controller.Stop();
    RefreshAirPlayState(state);
}

std::wstring BuildVirtualCameraStatusText(const AppState* state) {
    if (state == nullptr) {
        return L"\u672A\u5C31\u7EEA";
    }

    if (state->virtual_camera_controller.starting()) {
        return L"\u542F\u52A8\u4E2D";
    }
    if (state->virtual_camera_controller.running()) {
        return L"\u5DF2\u542F\u52A8";
    }
    if (state->virtual_camera_controller.installed()) {
        if (!state->virtual_camera_controller.last_error().empty()) {
            return L"\u542F\u52A8\u5931\u8D25";
        }
        if (!state->virtual_camera_controller.install_warning().empty()) {
            return L"\u5DF2\u5B89\u88C5\uFF0C\u4F46\u76EE\u5F55\u4E0D\u4E00\u81F4";
        }
        return L"\u5DF2\u5B89\u88C5\u672A\u542F\u52A8";
    }
    if (FileExists(state->virtual_camera_tool_path) && FileExists(state->virtual_camera_media_source_path)) {
        return L"\u5F85\u5B89\u88C5";
    }
    return L"\u7EC4\u4EF6\u7F3A\u5931";
}

void NotifyVirtualCameraStateChanged(AppState* state) {
    if (state == nullptr) {
        return;
    }

    UpdateActionButtons(state);
    if (state->status_window != nullptr) {
        PostMessageW(state->status_window, kStatsMessage, 0, 0);
    }
}

void HandleVirtualCameraControllerStateChanged(AppState* state) {
    if (state == nullptr) {
        return;
    }

    if (!state->virtual_camera_controller.running() && !state->virtual_camera_controller.starting()) {
        if (state->virtual_camera_bridge.running()) {
            state->virtual_camera_bridge.Stop();
        }
        if (state->decoder != nullptr) {
            state->decoder->SetPreferBgraOutput(false);
        }
    }

    NotifyVirtualCameraStateChanged(state);
}

void RefreshVirtualCameraState(AppState* state) {
    if (state == nullptr) {
        return;
    }

    state->virtual_camera_controller.RefreshInstallState(state->virtual_camera_media_source_path);
    NotifyVirtualCameraStateChanged(state);
}

std::vector<std::wstring> BuildVoiceCommandPhraseList() {
    return VoiceIntentResolver::CollectMediaCommandPhrases();
}

std::vector<std::wstring> BuildVoiceRecognitionGrammar(AppState* state) {
    std::unordered_set<std::wstring> dedup;
    std::vector<std::wstring> grammar;

    auto append = [&](const std::wstring& phrase) {
        const std::wstring trimmed = TrimWhitespace(phrase);
        if (trimmed.empty()) {
            return;
        }
        if (dedup.insert(trimmed).second) {
            grammar.push_back(trimmed);
        }
    };

    for (const auto& phrase : BuildVoiceCommandPhraseList()) {
        append(phrase);
    }

    if (state != nullptr && state->voice_music_library != nullptr) {
        for (const auto& alias : state->voice_music_library->CollectAliases()) {
            append(alias);
        }
    }

    if (state != nullptr) {
        state->voice_exact_phrases = dedup;
    }
    return grammar;
}

std::vector<std::wstring> BuildVoiceRecognitionHotwords(AppState* state) {
    std::unordered_set<std::wstring> dedup;
    std::vector<std::wstring> hotwords;

    auto append = [&](const std::wstring& phrase) {
        const std::wstring trimmed = TrimWhitespace(phrase);
        if (trimmed.empty()) {
            return;
        }

        const std::wstring normalized = VoiceIntentResolver::NormalizePhrase(trimmed);
        if (normalized.size() < 2) {
            return;
        }

        if (dedup.insert(normalized).second) {
            hotwords.push_back(trimmed);
        }
    };

    for (const auto& phrase : BuildVoiceCommandPhraseList()) {
        append(phrase);
    }

    if (state != nullptr && state->voice_music_library != nullptr) {
        for (const auto& alias : state->voice_music_library->CollectAliases()) {
            append(alias);
        }
    }

    return hotwords;
}

void RefreshVoiceRecognitionContext(AppState* state, bool restart_if_enabled, bool write_log) {
    if (state == nullptr) {
        return;
    }

    const std::vector<std::wstring> grammar = BuildVoiceRecognitionGrammar(state);
    const std::vector<std::wstring> hotwords = BuildVoiceRecognitionHotwords(state);
    if (write_log) {
        std::wostringstream stream;
        stream << L"语音控制：已刷新识别词表，短语数=" << grammar.size() << L"。";
        stream.str(L"");
        stream.clear();
        stream << L"\u8BED\u97F3\u63A7\u5236: \u5DF2\u5237\u65B0\u8BC6\u522B\u8BCD\u8868\uFF0C\u77ED\u8BED\u6570="
               << grammar.size()
               << L"\uFF0C\u70ED\u8BCD\u6570="
               << hotwords.size()
               << L"\u3002";
        QueueLog(state, stream.str());
    }

    if (!restart_if_enabled || !state->voice_control_enabled) {
        return;
    }

    StopVoiceControl(state, false);
    if (StartVoiceControl(state)) {
        if (write_log) {
            QueueLog(state, L"语音控制：已重新加载识别词表。");
        }
    } else if (write_log) {
        QueueLog(state, L"语音控制：重新加载识别词表失败。");
    }
}

std::wstring BuildVoiceControlStatusText(const AppState* state) {
    if (state == nullptr) {
        return kTextVoiceControlUnavailable;
    }
    if (state->voice_control_enabled) {
        return state->voice_control_status.empty() ? std::wstring(kTextVoiceControlListening) : state->voice_control_status;
    }
    return state->voice_control_status.empty() ? std::wstring(kTextVoiceControlDisabled) : state->voice_control_status;
}

bool ShouldQueueVoicePhrase(AppState* state, const std::wstring& phrase, bool is_final) {
    static_cast<void>(state);
    static_cast<void>(phrase);
    return is_final;
}

std::wstring FormatVoiceIntentScore(float score) {
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(2) << score;
    return stream.str();
}

std::wstring VoiceMediaCommandIntentText(VoiceMediaCommandIntent intent) {
    return VoiceIntentResolver::IntentDisplayText(intent);
}

bool TryInvokeSessionMediaCommand(
    const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession& session,
    VoiceMediaCommandIntent intent) {
    if (!session) {
        return false;
    }

    const auto playback_info = session.GetPlaybackInfo();
    const auto controls = playback_info.Controls();
    switch (intent) {
    case VoiceMediaCommandIntent::kPlay:
        return controls.IsPlayEnabled() && session.TryPlayAsync().get();
    case VoiceMediaCommandIntent::kPause:
        return controls.IsPauseEnabled() && session.TryPauseAsync().get();
    case VoiceMediaCommandIntent::kPrevious:
        return controls.IsPreviousEnabled() && session.TrySkipPreviousAsync().get();
    case VoiceMediaCommandIntent::kNext:
        return controls.IsNextEnabled() && session.TrySkipNextAsync().get();
    case VoiceMediaCommandIntent::kNone:
    default:
        if (controls.IsPlayPauseToggleEnabled()) {
            return session.TryTogglePlayPauseAsync().get();
        }

        constexpr int kPlaybackStatusPlaying = 4;
        const int playback_status = static_cast<int>(playback_info.PlaybackStatus());
        if (playback_status == kPlaybackStatusPlaying && controls.IsPauseEnabled()) {
            return session.TryPauseAsync().get();
        }
        if (controls.IsPlayEnabled()) {
            return session.TryPlayAsync().get();
        }
        return controls.IsPauseEnabled() && session.TryPauseAsync().get();
    }
}

bool TryDispatchSystemMediaSession(VoiceMediaCommandIntent intent, std::wstring* failure_detail) {
    try {
        const auto manager =
            winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();

        const auto current_session = manager.GetCurrentSession();
        if (TryInvokeSessionMediaCommand(current_session, intent)) {
            return true;
        }

        const auto sessions = manager.GetSessions();
        const uint32_t session_count = sessions.Size();
        for (uint32_t index = 0; index < session_count; ++index) {
            if (TryInvokeSessionMediaCommand(sessions.GetAt(index), intent)) {
                return true;
            }
        }

        if (failure_detail != nullptr) {
            *failure_detail = L"\u5F53\u524D\u6CA1\u6709\u627E\u5230\u53EF\u63A7\u5236\u7684\u7CFB\u7EDF\u5A92\u4F53\u4F1A\u8BDD";
        }
    } catch (const winrt::hresult_error& error) {
        if (failure_detail != nullptr) {
            *failure_detail = std::wstring(L"\u7CFB\u7EDF\u5A92\u4F53\u4F1A\u8BDD\u63A5\u53E3\u5931\u8D25\uFF1A") + error.message().c_str();
        }
    } catch (...) {
        if (failure_detail != nullptr) {
            *failure_detail = L"\u7CFB\u7EDF\u5A92\u4F53\u4F1A\u8BDD\u63A5\u53E3\u5F02\u5E38";
        }
    }
    return false;
}

bool SendMediaAppCommand(VoiceMediaCommandIntent intent) {
    WORD app_command = APPCOMMAND_MEDIA_PLAY_PAUSE;
    switch (intent) {
    case VoiceMediaCommandIntent::kPlay:
        app_command = APPCOMMAND_MEDIA_PLAY;
        break;
    case VoiceMediaCommandIntent::kPause:
        app_command = APPCOMMAND_MEDIA_PAUSE;
        break;
    case VoiceMediaCommandIntent::kPrevious:
        app_command = APPCOMMAND_MEDIA_PREVIOUSTRACK;
        break;
    case VoiceMediaCommandIntent::kNext:
        app_command = APPCOMMAND_MEDIA_NEXTTRACK;
        break;
    case VoiceMediaCommandIntent::kNone:
    default:
        app_command = APPCOMMAND_MEDIA_PLAY_PAUSE;
        break;
    }

    const LPARAM command_param = static_cast<LPARAM>(static_cast<DWORD>(app_command) << 16);
    DWORD_PTR result = 0;
    bool sent = false;

    const HWND foreground_window = GetForegroundWindow();
    if (foreground_window != nullptr) {
        sent = SendMessageTimeoutW(
                   foreground_window,
                   WM_APPCOMMAND,
                   reinterpret_cast<WPARAM>(foreground_window),
                   command_param,
                   SMTO_ABORTIFHUNG,
                   250,
                   &result) != 0 ||
               sent;
    }

    const HWND shell_window = GetShellWindow();
    if (shell_window != nullptr && shell_window != foreground_window) {
        sent = SendMessageTimeoutW(
                   shell_window,
                   WM_APPCOMMAND,
                   reinterpret_cast<WPARAM>(shell_window),
                   command_param,
                   SMTO_ABORTIFHUNG,
                   250,
                   &result) != 0 ||
               sent;
    }

    sent = SendMessageTimeoutW(
               HWND_BROADCAST,
               WM_APPCOMMAND,
               0,
               command_param,
               SMTO_ABORTIFHUNG,
               250,
               &result) != 0 ||
           sent;
    return sent;
}

bool SendMediaKey(AppState* state, WORD virtual_key) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = virtual_key;
    inputs[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = virtual_key;
    inputs[1].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    const UINT sent = SendInput(static_cast<UINT>(_countof(inputs)), inputs, sizeof(INPUT));
    if (sent != _countof(inputs)) {
        if (state != nullptr) {
            state->voice_control_status = L"\u5A92\u4F53\u952E\u53D1\u9001\u5931\u8D25";
        }
        return false;
    }
    return true;
}

bool DispatchVoiceMediaCommand(AppState* state, VoiceMediaCommandIntent intent, std::wstring* result_text) {
    std::wstring failure_detail;
    if (TryDispatchSystemMediaSession(intent, &failure_detail)) {
        if (result_text != nullptr) {
            *result_text = VoiceMediaCommandIntentText(intent) + L"\uFF08\u7CFB\u7EDF\u5A92\u4F53\u4F1A\u8BDD\uFF09";
        }
        return true;
    }

    if (SendMediaAppCommand(intent)) {
        if (result_text != nullptr) {
            *result_text = VoiceMediaCommandIntentText(intent) + L"\uFF08\u7CFB\u7EDF\u5A92\u4F53\u547D\u4EE4\uFF09";
        }
        return true;
    }

    WORD fallback_key = VK_MEDIA_PLAY_PAUSE;
    switch (intent) {
    case VoiceMediaCommandIntent::kPrevious:
        fallback_key = VK_MEDIA_PREV_TRACK;
        break;
    case VoiceMediaCommandIntent::kNext:
        fallback_key = VK_MEDIA_NEXT_TRACK;
        break;
    case VoiceMediaCommandIntent::kPause:
    case VoiceMediaCommandIntent::kPlay:
    case VoiceMediaCommandIntent::kNone:
    default:
        fallback_key = VK_MEDIA_PLAY_PAUSE;
        break;
    }

    if (SendMediaKey(state, fallback_key)) {
        if (result_text != nullptr) {
            *result_text = VoiceMediaCommandIntentText(intent) + L"\uFF08\u5A92\u4F53\u952E\u517C\u5BB9\u515C\u5E95\uFF09";
        }
        return true;
    }

    if (result_text != nullptr) {
        *result_text = failure_detail.empty() ? L"\u672A\u627E\u5230\u53EF\u63A7\u5236\u7684\u5A92\u4F53" : failure_detail;
    }
    return false;
}

bool DispatchVoiceMusicCommand(AppState* state, const std::wstring& phrase, std::wstring* result_text) {
    if (state == nullptr || state->voice_music_library == nullptr || state->local_music_player == nullptr) {
        return false;
    }

    VoiceMusicSelection selection;
    std::wstring detail;
    const VoiceMusicResolveStatus resolve_status =
        state->voice_music_library->ResolveRandomTrack(phrase, &selection, &detail);
    if (resolve_status == VoiceMusicResolveStatus::kNoMatch) {
        return false;
    }

    if (resolve_status == VoiceMusicResolveStatus::kNoFiles) {
        if (result_text != nullptr) {
            *result_text = std::wstring(L"\u547D\u4E2D\u201C") + selection.folder_name +
                           L"\u201D\u76EE\u5F55\uFF0C\u4F46\u91CC\u9762\u8FD8\u6CA1\u6709\u53EF\u64AD\u653E\u7684\u97F3\u9891";
        }
        return true;
    }

    if (resolve_status == VoiceMusicResolveStatus::kError) {
        if (result_text != nullptr) {
            *result_text = detail.empty() ? L"\u8BED\u97F3\u70B9\u6B4C\u76EE\u5F55\u521D\u59CB\u5316\u5931\u8D25" : detail;
        }
        return true;
    }

    std::wstring play_detail;
    if (!state->local_music_player->PlayFile(selection.file_path, &play_detail)) {
        if (result_text != nullptr) {
            *result_text = play_detail.empty() ? L"\u672C\u5730\u97F3\u4E50\u64AD\u653E\u542F\u52A8\u5931\u8D25" : play_detail;
        }
        return true;
    }

    if (result_text != nullptr) {
        *result_text = std::wstring(L"\u5DF2\u5728\u201C") + selection.folder_name + L"\u201D\u91CC\u968F\u673A\u64AD\u653E\u300C" +
                       selection.file_name + L"\u300D";
    }
    return true;
}

bool DispatchStopLocalMusicCommand(AppState* state, const std::wstring& phrase, std::wstring* result_text) {
    if (state == nullptr || state->local_music_player == nullptr) {
        return false;
    }

    if (!state->local_music_player->running()) {
        if (result_text != nullptr) {
            *result_text = L"\u5F53\u524D\u6CA1\u6709\u6B63\u5728\u64AD\u653E\u7684\u70B9\u6B4C\u97F3\u4E50";
        }
        return true;
    }

    const std::wstring current_file = state->local_music_player->current_file();
    state->local_music_player->Stop();

    if (result_text != nullptr) {
        if (current_file.empty()) {
            *result_text = L"\u5DF2\u505C\u6B62\u5F53\u524D\u70B9\u6B4C\u64AD\u653E";
        } else {
            const size_t slash = current_file.find_last_of(L"\\/");
            const std::wstring file_name =
                slash == std::wstring::npos ? current_file : current_file.substr(slash + 1);
            *result_text = std::wstring(L"\u5DF2\u505C\u6B62\u64AD\u653E\u300C") + file_name + L"\u300D";
        }
    }
    return true;
}

bool StartVoiceControl(AppState* state) {
    if (state == nullptr || state->status_window == nullptr) {
        return false;
    }
    if (state->voice_control_enabled) {
        return true;
    }

    StopVoiceControl(state, false);

    if (state->voice_controller == nullptr) {
        state->voice_controller = std::make_unique<VoiceCommandController>(
            [state](const std::wstring& line) { QueueLog(state, line); });
    }

    VoiceCommandController::Config config;
    config.runtime_directory = state->voice_runtime_root_path;
    config.model_directory = BuildVoiceModelRuntimePath(state->voice_model_path);
    config.grammar_phrases = BuildVoiceRecognitionGrammar(state);
    config.hotwords_phrases = BuildVoiceRecognitionHotwords(state);
    config.hotwords_score = 1.5f;
    if (config.model_directory != state->voice_model_path) {
        QueueLog(state, std::wstring(L"语音控制：模型源目录=") + state->voice_model_path);
        QueueLog(state, std::wstring(L"语音控制：模型运行目录=") + config.model_directory);
    }

    if (!FileExists(config.runtime_directory + L"\\sherpa-onnx-c-api.dll")) {
        state->voice_control_status = L"\u79BB\u7EBF\u8BED\u97F3\u7EC4\u4EF6\u7F3A\u5931";
        QueueLog(state, L"\u8BED\u97F3\u63A7\u5236: \u672A\u627E\u5230 sherpa-onnx-c-api.dll\uFF0C\u8BF7\u786E\u8BA4 voice-runtime \u76EE\u5F55\u5B58\u5728\u3002");
        UpdateActionButtons(state);
        return false;
    }
    if (!FileExists(config.model_directory + L"\\encoder.int8.onnx") ||
        !FileExists(config.model_directory + L"\\decoder.onnx") ||
        !FileExists(config.model_directory + L"\\joiner.int8.onnx") ||
        !FileExists(config.model_directory + L"\\tokens.txt")) {
        state->voice_control_status = L"\u79BB\u7EBF\u8BED\u97F3\u6A21\u578B\u7F3A\u5931";
        QueueLog(state, L"\u8BED\u97F3\u63A7\u5236: \u672A\u627E\u5230 sherpa-onnx \u4E2D\u6587\u6A21\u578B\u6587\u4EF6\uFF0C\u8BF7\u786E\u8BA4 voice-model \u76EE\u5F55\u5B58\u5728\u3002");
        UpdateActionButtons(state);
        return false;
    }

    const bool started = state->voice_controller->Start(
        config,
        [state](const std::wstring& phrase, bool is_final) {
            if (!ShouldQueueVoicePhrase(state, phrase, is_final)) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(state->voice_phrase_mutex);
                state->pending_voice_phrases.push_back(phrase);
            }
            if (state->status_window != nullptr) {
                PostMessageW(state->status_window, kVoiceCommandMessage, 0, 0);
            }
        });
    if (!started) {
        state->voice_control_status = kTextVoiceControlUnavailable;
        UpdateActionButtons(state);
        if (state->status_window != nullptr) {
            PostMessageW(state->status_window, kStatsMessage, 0, 0);
        }
        return false;
    }

    state->voice_control_enabled = true;
    state->voice_control_status = L"\u79BB\u7EBF\u8BED\u97F3\u5DF2\u5F00\u542F\uFF0C\u53EA\u5904\u7406\u660E\u786E\u53E3\u4EE4";
    QueueLog(state, L"\u8BED\u97F3\u63A7\u5236: \u5DF2\u5207\u6362\u4E3A\u63A5\u6536\u7AEF\u81EA\u5E26\u7684\u79BB\u7EBF\u8BC6\u522B\u3002\u73B0\u5728\u53EA\u6709\u201C\u64AD\u653E\u97F3\u4E50\u201D\u3001\u201C\u6682\u505C\u97F3\u4E50\u201D\u3001\u201C\u4E0A\u4E00\u66F2\u201D\u3001\u201C\u4E0B\u4E00\u66F2\u201D\u8FD9\u56DB\u7C7B\u5A92\u4F53\u53E3\u4EE4\u4F1A\u8D70\u7CFB\u7EDF\u5A92\u4F53\u952E\u63A7\u5236\u3002\u9664\u6B64\u4E4B\u5916\u7684\u8BED\u97F3\u7ED3\u679C\uFF0C\u53EA\u8981\u547D\u4E2D\u201C\u8BED\u97F3\u70B9\u6B4C\u201D\u66F2\u5E93\u76EE\u5F55\u6216\u522B\u540D\uFF0C\u5C31\u4E00\u5F8B\u4EA4\u7ED9\u5185\u7F6E\u64AD\u653E\u5668\u5904\u7406\u3002\u6267\u884C\u540E\u4F1A\u77ED\u6682\u5FFD\u7565\u9EA6\u514B\u98CE\u56DE\u58F0\u3002");
    UpdateActionButtons(state);
    PostMessageW(state->status_window, kStatsMessage, 0, 0);
    return true;
}

void StopVoiceControl(AppState* state, bool write_log) {
    if (state == nullptr) {
        return;
    }

    const bool was_enabled = state->voice_control_enabled;
    if (state->voice_controller != nullptr) {
        state->voice_controller->Stop();
    }
    {
        std::lock_guard<std::mutex> lock(state->voice_phrase_mutex);
        state->pending_voice_phrases.clear();
    }
    state->voice_control_enabled = false;
    state->voice_control_status = kTextVoiceControlDisabled;
    if (write_log && was_enabled) {
        QueueLog(state, L"\u8BED\u97F3\u63A7\u5236: \u5DF2\u5173\u95ED\u3002");
    }
    UpdateActionButtons(state);
    if (state->status_window != nullptr) {
        PostMessageW(state->status_window, kStatsMessage, 0, 0);
    }
}

void HandleVoiceCommandMessage(AppState* state) {
    if (state == nullptr || !state->voice_control_enabled) {
        return;
    }

    std::deque<std::wstring> phrases;
    {
        std::lock_guard<std::mutex> lock(state->voice_phrase_mutex);
        phrases.swap(state->pending_voice_phrases);
    }
    if (phrases.empty()) {
        return;
    }

    for (const std::wstring& phrase : phrases) {
        const std::wstring normalized_phrase = VoiceIntentResolver::NormalizePhrase(phrase);
        const VoiceMediaIntentMatch media_match = VoiceIntentResolver::ResolveMediaCommand(phrase);
        state->voice_control_status = std::wstring(L"\u5DF2\u542C\u5230\uFF1A") + phrase;
        const ULONGLONG now = GetTickCount64();

        std::wstring dispatch_result;
        bool handled = false;
        if (media_match.accepted && media_match.intent != VoiceMediaCommandIntent::kNone) {
            if (now - state->last_voice_media_command_tick < kVoiceMediaCommandDebounceMs) {
                continue;
            }
            handled = true;
            if (DispatchVoiceMediaCommand(state, media_match.intent, &dispatch_result)) {
                state->last_voice_media_command_tick = now;
                if (state->voice_controller != nullptr) {
                    state->voice_controller->SuppressFor(kVoiceCommandRecoverySuppressMs);
                }
                state->voice_control_status = std::wstring(L"\u5DF2\u542C\u5230\uFF1A") + phrase + L"\uFF0C\u5DF2\u6267\u884C" + dispatch_result;
                std::wostringstream stream;
                stream << L"\u8BED\u97F3\u63A7\u5236: \u8BC6\u522B\u5230\u201C" << phrase
                       << L"\u201D\uFF0C\u5F52\u4E00\u5316\u201C" << normalized_phrase
                       << L"\u201D\uFF0C\u610F\u56FE=" << VoiceMediaCommandIntentText(media_match.intent)
                       << L"\uFF0C\u5339\u914D\u201C" << media_match.matched_phrase
                       << L"\u201D\uFF0C\u5F97\u5206=" << FormatVoiceIntentScore(media_match.score)
                       << L"\uFF0C\u5DF2\u6267\u884C" << dispatch_result << L"\u3002";
                QueueLog(state, stream.str());
            } else {
                state->voice_control_status = std::wstring(L"\u5DF2\u542C\u5230\uFF1A") + phrase + L"\uFF0C\u4F46" + dispatch_result;
                std::wostringstream stream;
                stream << L"\u8BED\u97F3\u63A7\u5236: \u8BC6\u522B\u5230\u201C" << phrase
                       << L"\u201D\uFF0C\u5F52\u4E00\u5316\u201C" << normalized_phrase
                       << L"\u201D\uFF0C\u610F\u56FE=" << VoiceMediaCommandIntentText(media_match.intent)
                       << L"\uFF0C\u5339\u914D\u201C" << media_match.matched_phrase
                       << L"\u201D\uFF0C\u5F97\u5206=" << FormatVoiceIntentScore(media_match.score)
                       << L"\uFF0C\u4F46" << dispatch_result << L"\u3002";
                QueueLog(state, stream.str());
            }
            continue;
        }

        if (now - state->last_voice_music_command_tick < kVoiceMusicCommandDebounceMs) {
            continue;
        }
        if (DispatchVoiceMusicCommand(state, phrase, &dispatch_result)) {
            handled = true;
            state->last_voice_music_command_tick = now;
            if (state->voice_controller != nullptr) {
                state->voice_controller->SuppressFor(kVoiceCommandRecoverySuppressMs);
            }
            state->voice_control_status = std::wstring(L"\u5DF2\u542C\u5230\uFF1A") + phrase + L"\uFF0C" + dispatch_result;
            std::wostringstream stream;
            stream << L"\u8BED\u97F3\u70B9\u6B4C: \u8BC6\u522B\u5230\u201C" << phrase
                   << L"\u201D\uFF0C\u5F52\u4E00\u5316\u201C" << normalized_phrase
                   << L"\u201D\uFF0C\u4EA4\u7ED9\u5185\u7F6E\u64AD\u653E\u5668\u5904\u7406\uFF0C"
                   << dispatch_result << L"\u3002";
            QueueLog(state, stream.str());
        }

        if (handled) {
            continue;
        }

        state->voice_control_status = std::wstring(L"\u5DF2\u542C\u5230\uFF1A") + phrase + L"\uFF0C\u672A\u547D\u4E2D\u6307\u4EE4";
        std::wostringstream stream;
        stream << L"\u8BED\u97F3\u63A7\u5236: \u8BC6\u522B\u5230\u201C" << phrase
               << L"\u201D\uFF0C\u5F52\u4E00\u5316\u201C" << normalized_phrase
               << L"\u201D\uFF0C\u672A\u547D\u4E2D\u5A92\u4F53\u6307\u4EE4\u6216\u70B9\u6B4C\u76EE\u5F55\u3002";
        QueueLog(state, stream.str());
    }
    if (state->status_window != nullptr) {
        PostMessageW(state->status_window, kStatsMessage, 0, 0);
    }
}

bool RunVirtualCameraTool(AppState* state, const wchar_t* arguments, bool elevated) {
    if (state == nullptr) {
        return false;
    }
    if (!FileExists(state->virtual_camera_tool_path)) {
        QueueLog(state, L"\u865A\u62DF\u6444\u50CF\u5934: \u627E\u4E0D\u5230\u5B89\u88C5\u5DE5\u5177\uFF0C\u8BF7\u786E\u8BA4 live-cast-virtual-camera-tool.exe \u4E0E\u63A5\u6536\u7AEF\u5728\u540C\u76EE\u5F55\u3002");
        return false;
    }

    const std::wstring working_directory = GetExecutableDirectory();
    SHELLEXECUTEINFOW execute_info{};
    execute_info.cbSize = sizeof(execute_info);
    execute_info.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute_info.hwnd = state->status_window;
    execute_info.lpVerb = elevated ? L"runas" : L"open";
    execute_info.lpFile = state->virtual_camera_tool_path.c_str();
    execute_info.lpParameters = arguments;
    execute_info.lpDirectory = working_directory.empty() ? nullptr : working_directory.c_str();
    execute_info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&execute_info)) {
        const DWORD last_error = GetLastError();
        if (last_error == ERROR_CANCELLED) {
            QueueLog(state, L"\u865A\u62DF\u6444\u50CF\u5934: \u5DF2\u53D6\u6D88\u7BA1\u7406\u5458\u6388\u6743\uFF0C\u672C\u6B21\u5B89\u88C5\u672A\u6267\u884C\u3002");
        } else {
            std::wostringstream stream;
            stream << L"\u865A\u62DF\u6444\u50CF\u5934: \u542F\u52A8\u5DE5\u5177\u5931\u8D25\uFF0CWin32 \u9519\u8BEF " << last_error;
            QueueLog(state, stream.str());
        }
        return false;
    }

    DWORD exit_code = ERROR_GEN_FAILURE;
    WaitForSingleObject(execute_info.hProcess, INFINITE);
    GetExitCodeProcess(execute_info.hProcess, &exit_code);
    CloseHandle(execute_info.hProcess);

    if (exit_code != 0) {
        std::wostringstream stream;
        stream << L"\u865A\u62DF\u6444\u50CF\u5934: \u5DE5\u5177\u6267\u884C\u5931\u8D25\uFF0C\u9000\u51FA\u7801 " << exit_code;
        QueueLog(state, stream.str());
        return false;
    }

    return true;
}

void StopVirtualCamera(AppState* state, bool write_log) {
    if (state == nullptr) {
        return;
    }

    const bool was_running =
        state->virtual_camera_controller.starting() ||
        state->virtual_camera_controller.running() ||
        state->virtual_camera_bridge.running();
    state->virtual_camera_controller.Stop();
    state->virtual_camera_bridge.Stop();
    if (state->decoder != nullptr) {
        state->decoder->SetPreferBgraOutput(false);
    }
    NotifyVirtualCameraStateChanged(state);

    if (write_log && was_running) {
        QueueLog(state, L"\u865A\u62DF\u6444\u50CF\u5934: \u5DF2\u5173\u95ED\u865A\u62DF\u6444\u50CF\u5934\u8F93\u51FA\u3002");
    }
}

void InstallVirtualCamera(AppState* state) {
    if (state == nullptr) {
        return;
    }
    if (!FileExists(state->virtual_camera_media_source_path)) {
        QueueLog(state, L"\u865A\u62DF\u6444\u50CF\u5934: \u627E\u4E0D\u5230 media source DLL\uFF0C\u8BF7\u786E\u8BA4 live-cast-virtual-camera-media-source.dll \u4E0E\u63A5\u6536\u7AEF\u5728\u540C\u76EE\u5F55\u3002");
        RefreshVirtualCameraState(state);
        return;
    }

    QueueLog(state, L"\u865A\u62DF\u6444\u50CF\u5934: \u6B63\u5728\u4EE5\u7BA1\u7406\u5458\u6743\u9650\u5B89\u88C5\u865A\u62DF\u6444\u50CF\u5934\u7EC4\u4EF6\u3002");
    if (RunVirtualCameraTool(state, L"install", true)) {
        QueueLog(state, L"\u865A\u62DF\u6444\u50CF\u5934: \u5B89\u88C5\u5B8C\u6210\uFF0C\u73B0\u5728\u53EF\u4EE5\u70B9\u51FB\u201C\u542F\u52A8\u865A\u62DF\u6444\u50CF\u5934\u201D\u3002");
    }
    RefreshVirtualCameraState(state);
}

void StartVirtualCamera(AppState* state) {
    if (state == nullptr) {
        return;
    }
    if (state->virtual_camera_controller.running() || state->virtual_camera_controller.starting()) {
        return;
    }

    RefreshVirtualCameraState(state);
    if (!state->virtual_camera_controller.installed()) {
        const std::wstring install_warning = state->virtual_camera_controller.install_warning();
        if (!install_warning.empty()) {
            QueueLog(state, install_warning);
        } else {
            QueueLog(state, L"\u865A\u62DF\u6444\u50CF\u5934: \u5C1A\u672A\u5B89\u88C5\uFF0C\u8BF7\u5148\u70B9\u51FB\u201C\u5B89\u88C5\u865A\u62DF\u6444\u50CF\u5934\u201D\u3002");
        }
        return;
    }

    if (!state->virtual_camera_bridge.Start(state->renderer.d3d_device())) {
        RefreshVirtualCameraState(state);
        return;
    }

    if (state->decoder != nullptr) {
        state->decoder->SetPreferBgraOutput(true);
    }

    QueueLog(state, L"\u865A\u62DF\u6444\u50CF\u5934: \u6B63\u5728\u542F\u52A8\uFF0C\u8BF7\u7A0D\u5019\u3002");
    if (!state->virtual_camera_controller.Start()) {
        const std::wstring error_text = state->virtual_camera_controller.last_error();
        if (!error_text.empty()) {
            QueueLog(state, std::wstring(L"\u865A\u62DF\u6444\u50CF\u5934: ") + error_text);
        }
        state->virtual_camera_bridge.Stop();
        if (state->decoder != nullptr) {
            state->decoder->SetPreferBgraOutput(false);
        }
        NotifyVirtualCameraStateChanged(state);
        return;
    }

    NotifyVirtualCameraStateChanged(state);
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

std::wstring WideFromUtf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }

    std::wstring wide(required, L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        wide.data(),
        static_cast<int>(wide.size()));
    if (written <= 0) {
        return {};
    }

    if (written < static_cast<int>(wide.size())) {
        wide.resize(static_cast<size_t>(written));
    }
    return wide;
}

std::wstring ExtractJsonStringField(const std::string& json_text, const char* field_name) {
    if (field_name == nullptr || *field_name == '\0' || json_text.empty()) {
        return {};
    }

    const std::regex field_regex(
        std::string("\"") + field_name + "\"\\s*:\\s*\"([^\"]*)\"",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_search(json_text, match, field_regex) || match.size() < 2) {
        return {};
    }
    return WideFromUtf8(match[1].str());
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
        R"json((?:"selectedProfile"\s*:\s*)?\{\s*"codec"\s*:\s*"([^"]+)"\s*,\s*"width"\s*:\s*(\d+)\s*,\s*"height"\s*:\s*(\d+)\s*,\s*"fps"\s*:\s*(\d+)\s*,\s*"adaptiveFps"\s*:\s*(true|false)\s*,\s*"bitrate"\s*:\s*(\d+)(?:\s*,\s*"audioEnabled"\s*:\s*(true|false)\s*,\s*"audioPort"\s*:\s*(\d+)\s*,\s*"audioSampleRate"\s*:\s*(\d+)\s*,\s*"audioChannels"\s*:\s*(\d+))?\s*\})json");
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
    if (match[7].matched) {
        profile.audio_enabled = match[7].str() == "true";
        profile.audio_port = std::stoi(match[8].str());
        profile.audio_sample_rate = std::stoi(match[9].str());
        profile.audio_channels = std::stoi(match[10].str());
    }
    if (profile.codec == protocol::Codec::kUnknown ||
        profile.width <= 0 ||
        profile.height <= 0 ||
        profile.fps <= 0 ||
        profile.bitrate <= 0) {
        return false;
    }

    *out = CapStreamProfileTo60Fps(profile);
    return true;
}

std::string BuildProfileCacheJson(const protocol::StreamProfile& profile) {
    const protocol::StreamProfile capped_profile = CapStreamProfileTo60Fps(profile);
    std::ostringstream json;
    json << "{"
         << "\"codec\":\"" << protocol::CodecToWireName(capped_profile.codec) << "\","
         << "\"width\":" << capped_profile.width << ","
         << "\"height\":" << capped_profile.height << ","
         << "\"fps\":" << capped_profile.fps << ","
         << "\"adaptiveFps\":" << (capped_profile.adaptive_fps ? "true" : "false") << ","
         << "\"bitrate\":" << capped_profile.bitrate << ","
         << "\"audioEnabled\":" << (capped_profile.audio_enabled ? "true" : "false") << ","
         << "\"audioPort\":" << capped_profile.audio_port << ","
         << "\"audioSampleRate\":" << capped_profile.audio_sample_rate << ","
         << "\"audioChannels\":" << capped_profile.audio_channels
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

void EnsureVoiceMusicScaffold(AppState* state) {
    if (state == nullptr || state->voice_music_root_path.empty()) {
        return;
    }

    if (!EnsureDirectoryPath(state->voice_music_root_path)) {
        return;
    }

    const std::array<const wchar_t*, 2> default_music_folders = {
        L"\u6218\u6B4C",
        L"\u51FA\u8D27",
    };
    for (const wchar_t* folder_name : default_music_folders) {
        if (folder_name == nullptr || *folder_name == L'\0') {
            continue;
        }
        EnsureDirectoryPath(state->voice_music_root_path + L"\\" + folder_name);
    }

    const std::wstring note_path = state->voice_music_root_path + L"\\\u8BED\u97F3\u70B9\u6B4C\u8BF4\u660E.txt";
    if (FileExists(note_path)) {
        return;
    }

    const std::string note_text = WideToUtf8(
        L"\u8BED\u97F3\u70B9\u6B4C\u4F7F\u7528\u8BF4\u660E\r\n"
        L"========================\r\n"
        L"1. \u7A0B\u5E8F\u4F1A\u9ED8\u8BA4\u521B\u5EFA\u201C\u6218\u6B4C\u201D\u548C\u201C\u51FA\u8D27\u201D\u4E24\u4E2A\u793A\u4F8B\u5206\u7C7B\u76EE\u5F55\u3002\r\n"
        L"2. \u4F60\u4E5F\u53EF\u4EE5\u5728\u201C\u8BED\u97F3\u70B9\u6B4C\u201D\u76EE\u5F55\u4E0B\u7EE7\u7EED\u4E3A\u6BCF\u79CD\u98CE\u683C\u65B0\u5EFA\u5B50\u76EE\u5F55\u3002\r\n"
        L"3. \u5B50\u76EE\u5F55\u540D\u5C31\u662F\u53EF\u4EE5\u5BF9\u7740 PC \u8BF4\u7684\u53E3\u4EE4\u3002\u6BD4\u5982\u201C\u6218\u6B4C\u201D\u3001\u201C\u51FA\u8D27\u201D\u3001\u201C\u7EAF\u97F3\u4E50\u201D\u3002\r\n"
        L"4. \u5982\u679C\u5B50\u76EE\u5F55\u540D\u662F\u7EAF\u6570\u5B57\uFF0C\u4F8B\u5982 123\uFF0C\u7A0B\u5E8F\u4E5F\u4F1A\u81EA\u52A8\u8BC6\u522B\u201C\u4E00\u4E8C\u4E09\u201D\u8FD9\u79CD\u8BFB\u6CD5\u3002\r\n"
        L"5. \u6BCF\u4E2A\u5B50\u76EE\u5F55\u53EF\u4EE5\u76F4\u63A5\u653E mp3 / wav / m4a / aac / flac / wma \u7B49\u6587\u4EF6\u3002\r\n"
        L"6. \u5BF9\u7740 PC \u8BF4\u201C\u64AD\u653E\u6218\u6B4C\u201D\u3001\u201C\u6765\u70B9\u51FA\u8D27\u201D\u3001\u201C123\u201D\u90FD\u53EF\u4EE5\u89E6\u53D1\u968F\u673A\u64AD\u653E\u3002\r\n");
    WriteUtf8File(note_path, note_text);
}

std::wstring SanitizeVoiceMusicProjectName(const std::wstring& raw_name) {
    std::wstring sanitized = TrimWhitespace(raw_name);
    for (wchar_t& ch : sanitized) {
        switch (ch) {
        case L'\\':
        case L'/':
        case L':':
        case L'*':
        case L'?':
        case L'"':
        case L'<':
        case L'>':
        case L'|':
            ch = L'_';
            break;
        default:
            break;
        }
    }

    while (!sanitized.empty() && (sanitized.back() == L'.' || std::iswspace(sanitized.back()))) {
        sanitized.pop_back();
    }
    return TrimWhitespace(sanitized);
}

std::vector<std::wstring> ParseVoiceAliasLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    std::wstring current;
    current.reserve(text.size());

    auto flush = [&]() {
        const std::wstring trimmed = TrimWhitespace(current);
        if (!trimmed.empty() && trimmed[0] != L'#' && trimmed[0] != L';') {
            lines.push_back(trimmed);
        }
        current.clear();
    };

    for (wchar_t ch : text) {
        if (ch == L'|' || ch == L'\r' || ch == L'\n') {
            flush();
            continue;
        }
        current.push_back(ch);
    }
    flush();
    return lines;
}

void ExecuteVoiceMusicProjectCreate(
    AppState* state,
    const std::wstring& project_name,
    const std::wstring& alias_payload) {
    if (state == nullptr) {
        return;
    }

    const std::wstring sanitized_name = SanitizeVoiceMusicProjectName(project_name);
    if (sanitized_name.empty()) {
        state->voice_control_status = L"新增项目失败：目录名称不能为空";
        QueueLog(state, L"语音点歌：新增项目失败，目录名称为空。");
        return;
    }
    if (state->voice_music_root_path.empty()) {
        state->voice_control_status = L"新增项目失败：点歌目录不可用";
        QueueLog(state, L"语音点歌：新增项目失败，点歌根目录为空。");
        return;
    }

    if (!EnsureDirectoryPath(state->voice_music_root_path)) {
        state->voice_control_status = L"新增项目失败：无法创建点歌目录";
        QueueLog(state, L"语音点歌：新增项目失败，无法创建点歌根目录。");
        return;
    }

    const std::wstring project_path = state->voice_music_root_path + L"\\" + sanitized_name;
    if (!EnsureDirectoryPath(project_path)) {
        state->voice_control_status = std::wstring(L"新增项目失败：无法创建目录“") + sanitized_name + L"”";
        QueueLog(state, std::wstring(L"语音点歌：新增项目失败，无法创建目录“") + sanitized_name + L"”。");
        return;
    }

    std::vector<std::wstring> alias_lines = ParseVoiceAliasLines(alias_payload);
    alias_lines.push_back(sanitized_name);

    const std::wstring alias_file_path = project_path + L"\\别名.txt";
    const std::string existing_alias_text = ReadUtf8File(alias_file_path);
    if (!existing_alias_text.empty()) {
        const std::vector<std::wstring> existing_lines = ParseVoiceAliasLines(WideFromUtf8(existing_alias_text));
        alias_lines.insert(alias_lines.end(), existing_lines.begin(), existing_lines.end());
    }

    std::unordered_set<std::wstring> dedup;
    std::vector<std::wstring> merged_aliases;
    merged_aliases.reserve(alias_lines.size());
    for (const std::wstring& alias : alias_lines) {
        const std::wstring trimmed = TrimWhitespace(alias);
        if (trimmed.empty()) {
            continue;
        }
        if (dedup.insert(trimmed).second) {
            merged_aliases.push_back(trimmed);
        }
    }

    std::wstring content =
        L"# 自动生成的语音点歌别名，一行一个。\r\n"
        L"# 可以继续手动追加常见口语、同音字、外号。\r\n";
    for (const std::wstring& alias : merged_aliases) {
        content += alias;
        content += L"\r\n";
    }
    WriteUtf8File(alias_file_path, WideToUtf8(content));

    if (state->voice_music_library != nullptr) {
        state->voice_music_library->RefreshIndex();
    }
    RefreshVoiceRecognitionContext(state, true, true);

    state->voice_control_status = std::wstring(L"已新增点歌项目：") + sanitized_name;
    std::wostringstream stream;
    stream << L"语音点歌：已新增项目“" << sanitized_name << L"”，目录=" << project_path
           << L"，别名数=" << merged_aliases.size() << L"。";
    QueueLog(state, stream.str());
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
        state->recent_logs.push_back(message);
        while (state->recent_logs.size() > 120) {
            state->recent_logs.pop_front();
        }
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
        std::lock_guard<std::mutex> session_lock(state->session_mutex);
        state->session.last_present_us = receiver_present_us;
    }
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
    state->last_stale_video_drop_request_us = 0;
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
           << FormatBitrateValue(profile.bitrate)
           << L" / \u97F3\u9891 "
           << BuildAudioProfileText(&profile, true);
    return stream.str();
}

void ApplyStreamProfile(AppState* state, const protocol::StreamProfile& profile, bool auto_resumed) {
    if (state == nullptr) {
        return;
    }

    const protocol::StreamProfile capped_profile = CapStreamProfileTo60Fps(profile);
    state->selected_profile = capped_profile;
    state->has_selected_profile = true;
    state->auto_resumed_profile = auto_resumed;
    state->suppress_video_window_auto_show = false;
    ResetRuntimeMetricsForStream(state);

    state->renderer.SetNominalFrameRate(capped_profile.fps, capped_profile.adaptive_fps);

    if (state->decoder != nullptr) {
        state->decoder->Configure(capped_profile);
    }

    if (state->audio_player != nullptr) {
        if (capped_profile.audio_enabled &&
            capped_profile.audio_sample_rate > 0 &&
            capped_profile.audio_channels > 0) {
            if (!state->audio_player->Start(capped_profile.audio_sample_rate, capped_profile.audio_channels)) {
                QueueLog(state, L"\u97F3\u9891\u64AD\u653E\u521D\u59CB\u5316\u5931\u8D25\uFF0C\u672C\u6B21\u4ECD\u4FDD\u6301\u89C6\u9891\u94FE\u8DEF\u7EE7\u7EED\u5DE5\u4F5C\u3002");
            }
        } else {
            state->audio_player->Stop();
        }
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

void SubmitCachedCodecConfig(AppState* state, const std::vector<uint8_t>& codec_config, uint32_t frame_id) {
    if (state == nullptr ||
        state->decoder == nullptr ||
        codec_config.empty()) {
        return;
    }

    AccessUnit cached_unit;
    cached_unit.bytes = codec_config;
    cached_unit.frame_id = frame_id;
    cached_unit.pts_us = 0;
    cached_unit.flags = static_cast<uint8_t>(protocol::kFlagCodecConfig | protocol::kFlagKeyframe);
    state->decoder->SubmitAccessUnit(cached_unit);
}

void SubmitCachedCodecConfig(AppState* state, uint32_t frame_id) {
    if (state == nullptr ||
        !state->has_cached_codec_config ||
        state->cached_codec_config.empty()) {
        return;
    }

    SubmitCachedCodecConfig(state, state->cached_codec_config, frame_id);
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

    const AudioStats audio_stats = state->audio_receiver != nullptr ? state->audio_receiver->GetStats() : AudioStats{};
    const AudioPlaybackStats audio_playback_stats =
        state->audio_player != nullptr ? state->audio_player->GetStats() : AudioPlaybackStats{};
    const DanmakuController::Snapshot danmaku_snapshot =
        state->danmaku_controller != nullptr ? state->danmaku_controller->GetSnapshot() : DanmakuController::Snapshot{};
    bool auto_reply_enabled = false;
    bool auto_reply_send_enabled = false;
    int auto_reply_cooldown_seconds = 0;
    int auto_reply_delay_ms = 0;
    size_t auto_reply_pending_count = 0;
    std::wstring auto_reply_status;
    std::wstring auto_reply_target_title;
    std::wstring auto_reply_last_match_text;
    std::wstring auto_reply_last_reply_text;
    bool auto_reply_input_point_configured = false;
    int auto_reply_input_point_x_permille = 329;
    int auto_reply_input_point_y_permille = 927;
    bool auto_reply_send_point_configured = false;
    bool auto_reply_capture_running = false;
    AutoReplyCaptureTarget auto_reply_capture_target = AutoReplyCaptureTarget::kNone;
    int auto_reply_send_point_x_permille = kAutoReplySendPointXPermilleDefault;
    int auto_reply_send_point_y_permille = kAutoReplySendPointYPermilleDefault;
    std::wstring auto_reply_input_point_text;
    std::wstring auto_reply_send_point_text;
    std::wstring auto_reply_exact_rules_text;
    std::wstring auto_reply_keyword_rules_text;
    std::wstring auto_reply_fallback_text;
    std::deque<std::wstring> auto_reply_recent_logs;
    {
        std::lock_guard<std::mutex> lock(state->auto_reply_mutex);
        auto_reply_enabled = state->auto_reply_enabled;
        auto_reply_send_enabled = state->auto_reply_send_enabled;
        auto_reply_cooldown_seconds = state->auto_reply_cooldown_seconds;
        auto_reply_delay_ms = state->auto_reply_delay_ms;
        auto_reply_pending_count = state->auto_reply_match_queue.size() + state->auto_reply_queue.size();
        auto_reply_status = state->auto_reply_status;
        auto_reply_target_title = state->auto_reply_target_title;
        auto_reply_last_match_text = state->auto_reply_last_match_text;
        auto_reply_last_reply_text = state->auto_reply_last_reply_text;
        auto_reply_input_point_configured = state->auto_reply_input_point_configured;
        auto_reply_input_point_x_permille = state->auto_reply_input_point_x_permille;
        auto_reply_input_point_y_permille = state->auto_reply_input_point_y_permille;
        auto_reply_send_point_configured = state->auto_reply_send_point_configured;
        auto_reply_capture_running = state->auto_reply_capture_running;
        auto_reply_capture_target = state->auto_reply_capture_target;
        auto_reply_send_point_x_permille = state->auto_reply_send_point_x_permille;
        auto_reply_send_point_y_permille = state->auto_reply_send_point_y_permille;
        auto_reply_exact_rules_text = state->auto_reply_exact_rules_text;
        auto_reply_keyword_rules_text = state->auto_reply_keyword_rules_text;
        auto_reply_fallback_text = state->auto_reply_fallback_text;
        auto_reply_recent_logs = state->auto_reply_recent_logs;
    }
    auto_reply_input_point_text = BuildAutoReplyPointText(
        AutoReplyCaptureTarget::kInput,
        auto_reply_input_point_configured,
        auto_reply_input_point_x_permille,
        auto_reply_input_point_y_permille,
        auto_reply_capture_running,
        auto_reply_capture_target);
    auto_reply_send_point_text = BuildAutoReplyPointText(
        AutoReplyCaptureTarget::kSend,
        auto_reply_send_point_configured,
        auto_reply_send_point_x_permille,
        auto_reply_send_point_y_permille,
        auto_reply_capture_running,
        auto_reply_capture_target);
    if (auto_reply_target_title.empty()) {
        auto_reply_target_title = danmaku_snapshot.ui_probe_target_title;
    }

    std::ostringstream json;
    json << "{\n";
    json << "  \"buildLabel\": " << QuoteJsonWide(kAppBuildLabel) << ",\n";
    json << "  \"updatedAtUs\": " << NowSteadyUs() << ",\n";
    json << "  \"controlPort\": " << kControlPort << ",\n";
    json << "  \"videoPort\": " << kVideoPort << ",\n";
    json << "  \"audioPort\": " << kAudioPort << ",\n";
    json << "  \"totalPackets\": " << stats.total_packets << ",\n";
    json << "  \"totalBytes\": " << stats.total_bytes << ",\n";
    json << "  \"audioPackets\": " << audio_stats.total_packets << ",\n";
    json << "  \"audioBytes\": " << audio_stats.total_bytes << ",\n";
    json << "  \"audioFrames\": " << audio_stats.completed_frames << ",\n";
    json << "  \"audioDroppedFrames\": " << audio_stats.dropped_frames << ",\n";
    json << "  \"audioPlaybackSubmitted\": " << audio_playback_stats.submitted_frames << ",\n";
    json << "  \"audioPlaybackPlayed\": " << audio_playback_stats.played_frames << ",\n";
    json << "  \"audioPlaybackDropped\": " << audio_playback_stats.dropped_frames << ",\n";
    json << "  \"audioPlaybackBuffered\": " << audio_playback_stats.buffered_frames << ",\n";
    json << "  \"audioPlaybackBufferedMs\": " << audio_playback_stats.buffered_ms << ",\n";
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
    json << "  \"airPlayInstalled\": " << (state->airplay_controller.installed() ? "true" : "false") << ",\n";
    json << "  \"airPlayRunning\": " << (state->airplay_controller.running() ? "true" : "false") << ",\n";
    json << "  \"airPlayBonjourInstalled\": " << (state->airplay_controller.bonjour_installed() ? "true" : "false") << ",\n";
    json << "  \"airPlayBonjourRunning\": " << (state->airplay_controller.bonjour_running() ? "true" : "false") << ",\n";
    json << "  \"airPlayStatus\": " << QuoteJsonWide(BuildAirPlayStatusText(state)) << ",\n";
    json << "  \"airPlayRuntimeRoot\": " << QuoteJsonWide(state->airplay_runtime_root_path) << ",\n";
    json << "  \"airPlayBinaryPath\": " << QuoteJsonWide(state->airplay_controller.binary_path()) << ",\n";
    json << "  \"airPlayLogPath\": " << QuoteJsonWide(state->airplay_log_path) << ",\n";
    json << "  \"airPlayLastError\": " << QuoteJsonWide(state->airplay_controller.last_error()) << ",\n";
    json << "  \"airPlayLastExitCode\": " << state->airplay_controller.last_exit_code() << ",\n";
    json << "  \"virtualCameraInstalled\": " << (state->virtual_camera_controller.installed() ? "true" : "false") << ",\n";
    json << "  \"virtualCameraStarting\": " << (state->virtual_camera_controller.starting() ? "true" : "false") << ",\n";
    json << "  \"virtualCameraRunning\": " << (state->virtual_camera_controller.running() ? "true" : "false") << ",\n";
    json << "  \"virtualCameraStatus\": " << QuoteJsonWide(BuildVirtualCameraStatusText(state)) << ",\n";
    json << "  \"virtualCameraToolPath\": " << QuoteJsonWide(state->virtual_camera_tool_path) << ",\n";
    json << "  \"virtualCameraMediaSourcePath\": " << QuoteJsonWide(state->virtual_camera_media_source_path) << ",\n";
    json << "  \"virtualCameraInstalledPath\": " << QuoteJsonWide(state->virtual_camera_controller.installed_path()) << ",\n";
    json << "  \"videoWindowReady\": " << (state->video_window != nullptr ? "true" : "false") << ",\n";
    json << "  \"voiceControlEnabled\": " << (state->voice_control_enabled ? "true" : "false") << ",\n";
    json << "  \"voiceControlStatus\": " << QuoteJsonWide(BuildVoiceControlStatusText(state)) << ",\n";
    json << "  \"voiceModelPath\": " << QuoteJsonWide(state->voice_model_path) << ",\n";
    json << "  \"voiceMusicRootPath\": " << QuoteJsonWide(state->voice_music_root_path) << ",\n";
    json << "  \"danmakuRegionReady\": " << (danmaku_snapshot.region_ready ? "true" : "false") << ",\n";
    json << "  \"danmakuRunning\": " << (danmaku_snapshot.running ? "true" : "false") << ",\n";
    json << "  \"danmakuUiProbeRunning\": " << (danmaku_snapshot.ui_probe_running ? "true" : "false") << ",\n";
    json << "  \"danmakuReminderEnabled\": " << (danmaku_snapshot.reminder_enabled ? "true" : "false") << ",\n";
    json << "  \"danmakuGiftReminderEnabled\": " << (danmaku_snapshot.gift_reminder_enabled ? "true" : "false") << ",\n";
    json << "  \"danmakuSpeechEnabled\": " << (danmaku_snapshot.speech_enabled ? "true" : "false") << ",\n";
    json << "  \"danmakuSpeechVoiceCount\": " << danmaku_snapshot.speech_voice_count << ",\n";
    json << "  \"danmakuStatus\": " << QuoteJsonWide(danmaku_snapshot.status_text) << ",\n";
    json << "  \"danmakuRegionLabel\": " << QuoteJsonWide(danmaku_snapshot.region_label) << ",\n";
    json << "  \"danmakuUiProbeStatus\": " << QuoteJsonWide(danmaku_snapshot.ui_probe_status) << ",\n";
    json << "  \"danmakuUiProbeTargetTitle\": " << QuoteJsonWide(danmaku_snapshot.ui_probe_target_title) << ",\n";
    json << "  \"danmakuSpeechVoiceName\": " << QuoteJsonWide(danmaku_snapshot.speech_voice_name) << ",\n";
    json << "  \"danmakuLastText\": " << QuoteJsonWide(danmaku_snapshot.last_text) << ",\n";
    json << "  \"danmakuLastCapturePath\": " << QuoteJsonWide(danmaku_snapshot.last_capture_path) << ",\n";
    json << "  \"danmakuCaptureRootPath\": " << QuoteJsonWide(state->danmaku_capture_root_path) << ",\n";
    json << "  \"autoReplyEnabled\": " << (auto_reply_enabled ? "true" : "false") << ",\n";
    json << "  \"autoReplySendEnabled\": " << (auto_reply_send_enabled ? "true" : "false") << ",\n";
    json << "  \"autoReplyCooldownSeconds\": " << auto_reply_cooldown_seconds << ",\n";
    json << "  \"autoReplyDelayMs\": " << auto_reply_delay_ms << ",\n";
    json << "  \"autoReplyPendingCount\": " << auto_reply_pending_count << ",\n";
    json << "  \"autoReplyStatus\": " << QuoteJsonWide(auto_reply_status) << ",\n";
    json << "  \"autoReplyConfigPath\": " << QuoteJsonWide(state->auto_reply_config_path) << ",\n";
    json << "  \"autoReplyTargetTitle\": " << QuoteJsonWide(auto_reply_target_title) << ",\n";
    json << "  \"autoReplyLastMatchText\": " << QuoteJsonWide(auto_reply_last_match_text) << ",\n";
    json << "  \"autoReplyLastReplyText\": " << QuoteJsonWide(auto_reply_last_reply_text) << ",\n";
    json << "  \"autoReplyInputPointConfigured\": " << (auto_reply_input_point_configured ? "true" : "false") << ",\n";
    json << "  \"autoReplySendPointConfigured\": " << (auto_reply_send_point_configured ? "true" : "false") << ",\n";
    json << "  \"autoReplyCaptureRunning\": " << (auto_reply_capture_running ? "true" : "false") << ",\n";
    json << "  \"autoReplyCaptureTarget\": "
         << QuoteJsonWide(std::wstring(AutoReplyCaptureTargetToJson(auto_reply_capture_target))) << ",\n";
    json << "  \"autoReplyInputPointText\": " << QuoteJsonWide(auto_reply_input_point_text) << ",\n";
    json << "  \"autoReplySendPointText\": " << QuoteJsonWide(auto_reply_send_point_text) << ",\n";
    json << "  \"autoReplyExactRulesText\": " << QuoteJsonWide(auto_reply_exact_rules_text) << ",\n";
    json << "  \"autoReplyKeywordRulesText\": " << QuoteJsonWide(auto_reply_keyword_rules_text) << ",\n";
    json << "  \"autoReplyRulesText\": " << QuoteJsonWide(auto_reply_keyword_rules_text) << ",\n";
    json << "  \"autoReplyFallbackRepliesText\": " << QuoteJsonWide(auto_reply_fallback_text) << ",\n";
    json << "  \"localMusicPlaying\": "
         << ((state->local_music_player != nullptr && state->local_music_player->running()) ? "true" : "false")
         << ",\n";
    json << "  \"localMusicCurrentFile\": "
         << QuoteJsonWide(state->local_music_player != nullptr ? state->local_music_player->current_file() : std::wstring{})
         << ",\n";
    json << "  \"localMusicCurrentName\": "
         << QuoteJsonWide(
                state->local_music_player != nullptr
                    ? ExtractFileName(state->local_music_player->current_file())
                    : std::wstring{})
         << ",\n";
    json << "  \"danmakuRecentEvents\": [";
    for (size_t index = 0; index < danmaku_snapshot.recent_events.size(); ++index) {
        if (index > 0) {
            json << ", ";
        }
        json << QuoteJsonWide(danmaku_snapshot.recent_events[index]);
    }
    json << "],\n";
    json << "  \"danmakuUiProbeLines\": [";
    for (size_t index = 0; index < danmaku_snapshot.recent_probe_lines.size(); ++index) {
        if (index > 0) {
            json << ", ";
        }
        json << QuoteJsonWide(danmaku_snapshot.recent_probe_lines[index]);
    }
    json << "],\n";
    json << "  \"autoReplyRecentLogs\": [";
    for (size_t index = 0; index < auto_reply_recent_logs.size(); ++index) {
        if (index > 0) {
            json << ", ";
        }
        json << QuoteJsonWide(auto_reply_recent_logs[index]);
    }
    json << "],\n";
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
             << "\"bitrate\": " << state->selected_profile.bitrate << ", "
             << "\"audioEnabled\": " << (state->selected_profile.audio_enabled ? "true" : "false") << ", "
             << "\"audioPort\": " << state->selected_profile.audio_port << ", "
             << "\"audioSampleRate\": " << state->selected_profile.audio_sample_rate << ", "
             << "\"audioChannels\": " << state->selected_profile.audio_channels
             << "}";
    } else {
        json << "null";
    }
    json << "\n}\n";

    WriteUtf8File(state->status_file_path, json.str());
}

std::string BuildEmbeddedWebUiSnapshotJson(AppState* state) {
    if (state == nullptr) {
        return {};
    }

    const std::string status_json = ReadUtf8File(state->status_file_path);

    std::deque<std::wstring> recent_logs;
    {
        std::lock_guard<std::mutex> lock(state->log_mutex);
        recent_logs = state->recent_logs;
    }

    std::ostringstream json;
    json << "{";
    json << "\"status\": " << (status_json.empty() ? "null" : status_json) << ", ";
    json << "\"logs\": [";
    bool first = true;
    for (const auto& line : recent_logs) {
        if (!first) {
            json << ", ";
        }
        first = false;
        json << QuoteJsonWide(line);
    }
    json << "]";
    json << "}";
    return json.str();
}

void UpdateStatusLabel(AppState* state) {
    if (state == nullptr || state->udp_receiver == nullptr) {
        return;
    }

    const VideoStats stats = state->udp_receiver->GetStats();
    const AudioStats audio_stats = state->audio_receiver != nullptr ? state->audio_receiver->GetStats() : AudioStats{};
    const AudioPlaybackStats audio_playback_stats =
        state->audio_player != nullptr ? state->audio_player->GetStats() : AudioPlaybackStats{};
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
        state->dashboard.summary_cards[0] = {
            kTextCurrentConfig,
            config_value.str(),
            BuildProfileDetailBlock(&state->selected_profile, true, PresentationModeLabel(state->presentation_mode))};
    } else {
        state->dashboard.subtitle = kTextDashboardWaiting;
        state->dashboard.summary_cards[0] = {kTextCurrentConfig, kTextWaitingConnect, kTextProfilePending};
    }

    state->dashboard.summary_cards[1] = {kTextRealtimeFps, FormatOptionalFps(content_fps), BuildRateDetailBlock(receive_fps, decode_fps, display_fps)};

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

    state->dashboard.summary_cards[3] = {
        kTextDecoderQueue,
        std::to_wstring(display_dropped_frames),
        BuildFrameCounterBlock(stats, displayed_frame_count, decoded_frame_count)};

    std::wostringstream traffic_stream;
    traffic_stream << L"\u63A7\u5236\u901A\u9053\uFF1ATCP/" << kControlPort
                   << L"\r\n\u89C6\u9891\u901A\u9053\uFF1AUDP/" << kVideoPort
                   << L"\r\n\u97F3\u9891\u901A\u9053\uFF1AUDP/" << kAudioPort
                   << L"\r\n\u6570\u636E\u5305\uFF1A" << stats.total_packets << L"    \u5B57\u8282\uFF1A" << FormatDataSize(stats.total_bytes)
                   << L"\r\nPCM \u5E27\uFF1A" << audio_stats.completed_frames << L"    \u97F3\u9891\u5305\uFF1A" << audio_stats.total_packets
                   << L"\r\n\u5B8C\u6574\u5E27\uFF1A" << stats.completed_frames << L"    \u5173\u952E\u5E27\uFF1A" << stats.keyframes
                   << L"\r\n\u4E22\u5E27\uFF1A" << stats.dropped_frames << L"    \u6700\u540E\u5E27 ID\uFF1A" << stats.last_frame_id;
    state->dashboard.traffic_body = traffic_stream.str();

    const bool video_visible = state->video_window != nullptr && IsWindowVisible(state->video_window);
    const std::wstring airplay_status = BuildAirPlayStatusText(state);

    std::wostringstream runtime_stream;
    runtime_stream << L"\u7F51\u7EDC\u91CD\u7EC4\u901F\u7387\uFF1A" << FormatOptionalFps(receive_fps)
                   << L"\r\n\u89E3\u7801\u8F93\u51FA\u901F\u7387\uFF1A" << FormatOptionalFps(decode_fps)
                   << L"\r\n\u6700\u7EC8\u663E\u793A\u901F\u7387\uFF1A" << FormatOptionalFps(display_fps)
                   << L"\r\n\u663E\u793A\u6A21\u5F0F\uFF1A" << PresentationModeLabel(state->presentation_mode)
                   << L"\r\n\u97F3\u9891\u72B6\u6001\uFF1A" << BuildAudioRuntimeText(
                       state->has_selected_profile ? &state->selected_profile : nullptr,
                       state->has_selected_profile,
                       audio_playback_stats)
                   << L"\r\n\u82F9\u679C\u6295\u5C4F\uFF1A" << airplay_status
                   << L"\r\n\u8BED\u97F3\u63A7\u5236\uFF1A" << BuildVoiceControlStatusText(state)
                   << L"\r\n\u865A\u62DF\u6444\u50CF\u5934\uFF1A" << BuildVirtualCameraStatusText(state)
                   << L"\r\n\u6E32\u67D3\u663E\u5361\uFF1A"
                   << (state->renderer.gpu_name().empty() ? L"\u672A\u8BC6\u522B" : state->renderer.gpu_name())
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

    if (state->web_ui_bridge != nullptr) {
        state->web_ui_bridge->PushSnapshot(false);
    }

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
        UpdateActionButtons(state);
        SetWindowTextW(state->status_window, status_title.c_str());
        InvalidateRect(state->status_window, nullptr, FALSE);
    }

    if (state->video_window != nullptr) {
        std::wostringstream title_stream;
        title_stream << kTextVideoDisplayTitle << L" v" << kAppBuildLabel;
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
    for (size_t index = 0; index < state->nav_buttons.size(); ++index) {
        if (state->nav_buttons[index] != nullptr) {
            MoveWindow(
                state->nav_buttons[index],
                layout.nav_buttons[index].left,
                layout.nav_buttons[index].top,
                RectWidth(layout.nav_buttons[index]),
                RectHeight(layout.nav_buttons[index]),
                TRUE);
        }
    }
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
        ShowWindow(state->log_view, ShouldShowLogView(state->current_page) ? SW_SHOWNA : SW_HIDE);
        MoveWindow(
            state->log_view,
            layout.log_view.left,
            layout.log_view.top,
            RectWidth(layout.log_view),
            RectHeight(layout.log_view),
            TRUE);
    }
    if (state->web_ui_bridge != nullptr) {
        state->web_ui_bridge->ResizeToClient();
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
            if (state->danmaku_controller != nullptr) {
                state->danmaku_controller->SetWindows(state->status_window, hwnd);
            }
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
            if (state->danmaku_controller != nullptr) {
                state->danmaku_controller->SetWindows(state->status_window, nullptr);
            }
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
        new_state->web_ui_folder_path = BuildWebUiFolderPath();
        new_state->web_ui_user_data_path = BuildWebUiUserDataPath();
        new_state->profile_cache_path = BuildProfileCachePath();
        new_state->codec_config_cache_path = BuildCodecConfigCachePath();
        new_state->virtual_camera_tool_path = BuildVirtualCameraToolPath();
        new_state->virtual_camera_media_source_path = BuildVirtualCameraMediaSourcePath();
        new_state->virtual_camera_placeholder_path = BuildVirtualCameraPlaceholderPath();
        new_state->airplay_runtime_root_path = BuildAirPlayRuntimeRootPath();
        new_state->airplay_log_path = BuildAirPlayLogPath();
        new_state->voice_runtime_root_path = BuildVoiceRuntimeRootPath();
        new_state->voice_model_path = BuildVoiceModelPath();
        new_state->voice_music_root_path = BuildVoiceMusicRootPath();
        new_state->danmaku_capture_root_path = BuildDanmakuCaptureRootPath();
        new_state->danmaku_region_file_path = BuildDanmakuRegionFilePath();
        new_state->auto_reply_root_path = BuildAutoReplyRootPath();
        new_state->auto_reply_config_path = BuildAutoReplyConfigPath();
        ResetLogFile(new_state->log_file_path);
        ResetDashboardSnapshot(new_state);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new_state));
        EnsureVoiceMusicScaffold(new_state);
        EnsureAutoReplyScaffold(new_state);
        LoadAutoReplyConfig(new_state);

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

        const std::array<std::pair<int, const wchar_t*>, 6> nav_definitions = {{
            {kNavOverviewButtonId, kTextOverviewPage},
            {kNavConnectButtonId, kTextConnectPage},
            {kNavDisplayButtonId, kTextDisplayPage},
            {kNavVoiceButtonId, kTextVoicePage},
            {kNavDiagnosticsButtonId, kTextDiagnosticsPage},
            {kNavLogsButtonId, kTextLogsPage},
        }};
        for (size_t index = 0; index < nav_definitions.size(); ++index) {
            new_state->nav_buttons[index] = CreateWindowExW(
                0,
                L"BUTTON",
                nav_definitions[index].second,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0,
                0,
                100,
                40,
                hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(nav_definitions[index].first)),
                create_struct->hInstance,
                nullptr);
        }
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
        new_state->airplay_controller.SetLogFn([new_state](const std::wstring& line) { QueueLog(new_state, line); });
        new_state->airplay_controller.SetRuntimeRoot(new_state->airplay_runtime_root_path);
        new_state->airplay_controller.SetLogFilePath(new_state->airplay_log_path);
        new_state->virtual_camera_controller.SetLogFn([new_state](const std::wstring& line) { QueueLog(new_state, line); });
        new_state->virtual_camera_controller.SetStateChangedFn([new_state]() {
            if (new_state->status_window != nullptr) {
                PostMessageW(new_state->status_window, kVirtualCameraStateMessage, 0, 0);
            }
        });
        new_state->virtual_camera_bridge.SetLogFn([new_state](const std::wstring& line) { QueueLog(new_state, line); });
        new_state->virtual_camera_bridge.SetPlaceholderImagePath(new_state->virtual_camera_placeholder_path);
        ApplyUiToControls(new_state);
        RefreshAirPlayState(new_state);
        RefreshVirtualCameraState(new_state);
        UpdateActionButtons(new_state);
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
            kTextVideoDisplayTitle,
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
            DeleteFontHandle(new_state->spotlight_value_font);
            DeleteFontHandle(new_state->body_font);
            DeleteFontHandle(new_state->button_font);
            delete new_state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return -1;
        }

        new_state->danmaku_controller = std::make_unique<DanmakuController>(
            new_state->danmaku_region_file_path,
            new_state->danmaku_capture_root_path,
            [new_state](const std::wstring& line) { QueueLog(new_state, line); });
        new_state->danmaku_controller->SetWindows(new_state->status_window, new_state->video_window);
        new_state->danmaku_controller->SetEventFn([new_state](bool is_gift, const std::wstring& text) {
            HandleAutoReplyDanmakuEvent(new_state, is_gift, text);
        });
        new_state->auto_reply_match_thread = std::thread(AutoReplyMatchWorkerLoop, new_state);
        new_state->auto_reply_thread = std::thread(AutoReplyWorkerLoop, new_state);
        if (new_state->auto_reply_enabled || new_state->auto_reply_send_enabled) {
            EnsureDanmakuReadyForAutoReply(new_state);
        }

        new_state->decoder = std::make_unique<VideoDecoder>(
            [new_state](const std::wstring& line) { QueueLog(new_state, line); },
            [new_state](DecodedFrame frame) {
                {
                    std::lock_guard<std::mutex> lock(new_state->metrics_mutex);
                    new_state->decoded_frame_count += 1;
                }
                bool drop_stale_frame = false;
                if (frame.pts_us > 0) {
                    const int64_t receiver_now_us = NowSteadyUs();
                    std::lock_guard<std::mutex> lock(new_state->metrics_mutex);
                    if (new_state->has_clock_sync) {
                        const int64_t estimated_latency_us =
                            receiver_now_us + new_state->sender_clock_offset_us - static_cast<int64_t>(frame.pts_us);
                        if (estimated_latency_us > kVideoLateDropUs) {
                            drop_stale_frame = true;
                        }
                    }
                }
                if (drop_stale_frame) {
                    return;
                }
                if (new_state->virtual_camera_bridge.running() &&
                    frame.width > 0 &&
                    frame.height > 0 &&
                    frame.format == DecodedFrameFormat::kBgra) {
                    DecodedFrame virtual_camera_frame;
                    virtual_camera_frame.width = frame.width;
                    virtual_camera_frame.height = frame.height;
                    virtual_camera_frame.pts_us = frame.pts_us;
                    virtual_camera_frame.format = frame.format;
                    virtual_camera_frame.stride0 = frame.stride0;
                    virtual_camera_frame.stride1 = frame.stride1;
                    virtual_camera_frame.plane1_offset = frame.plane1_offset;
                    virtual_camera_frame.bytes = frame.bytes;
                    virtual_camera_frame.d3d_subresource = frame.d3d_subresource;
                    virtual_camera_frame.gpu_backed = frame.gpu_backed;
                    virtual_camera_frame.direct_sample_safe = frame.direct_sample_safe;
                    virtual_camera_frame.separate_textures = frame.separate_textures;
                    if (frame.d3d_texture != nullptr) {
                        virtual_camera_frame.d3d_texture = frame.d3d_texture;
                        virtual_camera_frame.d3d_texture->AddRef();
                    }
                    if (frame.d3d_texture_plane1 != nullptr) {
                        virtual_camera_frame.d3d_texture_plane1 = frame.d3d_texture_plane1;
                        virtual_camera_frame.d3d_texture_plane1->AddRef();
                    }
                    new_state->virtual_camera_bridge.PublishFrame(virtual_camera_frame);
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
        new_state->audio_player = std::make_unique<AudioPlayer>(
            [new_state](const std::wstring& line) { QueueLog(new_state, line); });
        new_state->local_music_player = std::make_unique<LocalMusicPlayer>(
            [new_state](const std::wstring& line) { QueueLog(new_state, line); },
            [new_state](bool running, const std::wstring& file_path) {
                if (new_state == nullptr) {
                    return;
                }
                if (running) {
                    if (new_state->status_window != nullptr) {
                        PostMessageW(new_state->status_window, kStatsMessage, 0, 0);
                    }
                    return;
                }

                if (!file_path.empty()) {
                    new_state->voice_control_status =
                        std::wstring(L"点歌已播放结束：") + ExtractFileName(file_path);
                }
                if (new_state->status_window != nullptr) {
                    PostMessageW(new_state->status_window, kStatsMessage, 0, 0);
                }
            });
        new_state->voice_music_library = std::make_unique<VoiceMusicLibrary>(new_state->voice_music_root_path);
        new_state->voice_music_library->RefreshIndex();
        RefreshVoiceRecognitionContext(new_state, false, false);

        new_state->control_server = std::make_unique<ControlServer>(
            [new_state](const std::wstring& line) { QueueLog(new_state, line); },
            [new_state](const protocol::StreamProfile& profile) {
                PostApplyStreamProfileMessage(new_state, profile, false);
            },
            [new_state](int64_t offset_us, int64_t rtt_us) {
                UpdateClockSync(new_state, offset_us, rtt_us);
            },
            [new_state](const std::wstring& sender_host, const std::wstring& device_name, bool connected) {
                PostSessionUpdateMessage(new_state, sender_host, device_name, connected);
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
                    bool expected_pending = false;
                    if (new_state->auto_resume_profile_message_pending.compare_exchange_strong(expected_pending, true)) {
                        PostApplyStreamProfileMessage(
                            new_state,
                            new_state->cached_startup_profile,
                            true,
                            !is_codec_config,
                            unit.frame_id,
                            !is_codec_config ? new_state->cached_codec_config : std::vector<uint8_t>{},
                            !new_state->auto_resume_notice_logged);
                    }
                }
                if (new_state->decoder != nullptr) {
                    new_state->decoder->SubmitAccessUnit(unit);
                }
            });
        new_state->audio_receiver = std::make_unique<UdpAudioReceiver>(
            [new_state](const std::wstring& line) { QueueLog(new_state, line); },
            [new_state](const AccessUnit& unit) {
                if (unit.bytes.empty() || new_state->audio_player == nullptr) {
                    return;
                }

                bool has_clock_sync = false;
                int64_t sender_clock_offset_us = 0;
                uint64_t last_present_sender_pts_us = 0;
                {
                    std::lock_guard<std::mutex> lock(new_state->metrics_mutex);
                    has_clock_sync = new_state->has_clock_sync;
                    sender_clock_offset_us = new_state->sender_clock_offset_us;
                    last_present_sender_pts_us = new_state->last_present_sender_pts_us;
                }

                if (has_clock_sync &&
                    unit.pts_us > 0 &&
                    last_present_sender_pts_us > 0 &&
                    last_present_sender_pts_us > unit.pts_us &&
                    (last_present_sender_pts_us - unit.pts_us) > static_cast<uint64_t>(kAudioVideoLateDropUs)) {
                    return;
                }

                new_state->audio_player->SubmitPcmFrame(
                    unit.bytes.data(),
                    unit.bytes.size(),
                    unit.pts_us,
                    has_clock_sync,
                    sender_clock_offset_us);
            });

        new_state->control_server->Start(kControlPort, kVideoPort, kAudioPort);
        new_state->udp_receiver->Start(kVideoPort);
        new_state->audio_receiver->Start(kAudioPort);
        SetTimer(hwnd, kStatsTimerId, kStatsIntervalMs, nullptr);
        StartAirPlayReceiver(new_state, true);
        LayoutStatusWindow(new_state);
        UpdateStatusLabel(new_state);
        new_state->web_ui_bridge = std::make_unique<EmbeddedWebUiBridge>(
            [new_state]() { return BuildEmbeddedWebUiSnapshotJson(new_state); },
            [new_state](const std::wstring& action, const std::wstring& value, const std::wstring& extra) {
                ExecuteEmbeddedWebUiAction(new_state, action, value, extra);
            },
            [new_state](const std::wstring& line) { QueueLog(new_state, line); },
            [new_state](bool ready) {
                SetEmbeddedWebUiMode(new_state, ready);
                LayoutStatusWindow(new_state);
                if (new_state->status_window != nullptr) {
                    PostMessageW(new_state->status_window, kStatsMessage, 0, 0);
                }
            });
        if (!new_state->web_ui_bridge->Initialize(
                hwnd,
                new_state->web_ui_folder_path,
                new_state->web_ui_user_data_path)) {
            new_state->web_ui_bridge.reset();
        } else {
            DestroyNativeStatusControls(new_state);
            SetEmbeddedWebUiMode(new_state, true);
            LayoutStatusWindow(new_state);
        }
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
        QueueLog(new_state, std::wstring(L"\u82F9\u679C\u6295\u5C4F\u65E5\u5FD7\uFF1A") + new_state->airplay_log_path);
        QueueLog(new_state, std::wstring(L"\u8BED\u97F3\u70B9\u6B4C\u76EE\u5F55\uFF1A") + new_state->voice_music_root_path);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* minmax = reinterpret_cast<MINMAXINFO*>(l_param);
        minmax->ptMinTrackSize.x = ScaleByDpi(hwnd, kStatusWindowMinWidth);
        minmax->ptMinTrackSize.y = ScaleByDpi(hwnd, kStatusWindowMinHeight);
        return 0;
    }
    case WM_NCHITTEST:
        return HitTestFramelessStatusWindow(hwnd, l_param);
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
    case kApplyStreamProfileMessage: {
        auto* message = reinterpret_cast<PendingStreamProfileMessage*>(l_param);
        if (state != nullptr && message != nullptr) {
            if (message->auto_resumed && state->has_selected_profile) {
                state->auto_resume_profile_message_pending.store(false);
            } else if (!message->auto_resumed) {
                state->cached_startup_profile = message->profile;
                state->has_cached_startup_profile = true;
                state->auto_resume_notice_logged = false;
                WriteUtf8File(state->profile_cache_path, BuildProfileCacheJson(message->profile));
                ApplyStreamProfile(state, message->profile, message->auto_resumed);
            } else {
                ApplyStreamProfile(state, message->profile, message->auto_resumed);
                if (message->submit_cached_codec_config) {
                    SubmitCachedCodecConfig(state, message->cached_codec_config, message->cached_codec_config_frame_id);
                }
                if (message->log_auto_resume_notice && !state->auto_resume_notice_logged) {
                    QueueLog(
                        state,
                        L"\u81EA\u52A8\u63A5\u7BA1: \u68C0\u6D4B\u5230\u624B\u673A\u7AEF\u5DF2\u5728\u6301\u7EED\u9001\u5E27\uFF0C\u5DF2\u4F7F\u7528\u4E0A\u6B21\u914D\u7F6E\u4E0E\u7F13\u5B58\u7684\u7801\u6D41\u53C2\u6570\u76F4\u63A5\u6062\u590D\u89E3\u7801\u3002");
                    state->auto_resume_notice_logged = true;
                }
                state->auto_resume_profile_message_pending.store(false);
            }
        }
        delete message;
        return 0;
    }
    case kSessionUpdateMessage: {
        auto* message = reinterpret_cast<PendingSessionUpdateMessage*>(l_param);
        if (state != nullptr && message != nullptr) {
            UpdateSessionSnapshot(state, message->sender_host, message->device_name, message->connected);
        }
        delete message;
        return 0;
    }
    case kVirtualCameraStateMessage:
        HandleVirtualCameraControllerStateChanged(state);
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
            case kNavOverviewButtonId:
            case kNavConnectButtonId:
            case kNavDisplayButtonId:
            case kNavVoiceButtonId:
            case kNavDiagnosticsButtonId:
            case kNavLogsButtonId:
                NavigateToPage(state, PageFromNavigationButtonId(LOWORD(w_param)));
                LayoutStatusWindow(state);
                return 0;
            case kFocusVideoButtonId:
                if (state != nullptr) {
                    switch (state->current_page) {
                    case StatusPage::kDiagnostics:
                        RequestKeyframe(state);
                        break;
                    case StatusPage::kVoiceControl:
                        if (state->voice_control_enabled) {
                            StopVoiceControl(state, true);
                        } else {
                            StartVoiceControl(state);
                        }
                        break;
                    case StatusPage::kLogs:
                        OpenLogFile(state);
                        break;
                    case StatusPage::kOverview:
                    case StatusPage::kConnect:
                    case StatusPage::kDisplay:
                    default:
                        FocusVideoWindow(state);
                        break;
                    }
                }
                return 0;
            case kPresentationModeButtonId:
                if (state != nullptr) {
                    switch (state->current_page) {
                    case StatusPage::kConnect:
                        if (state->airplay_controller.running()) {
                            StopAirPlayReceiver(state, true);
                        } else {
                            StartAirPlayReceiver(state, false);
                        }
                        break;
                    case StatusPage::kLogs:
                        OpenStatusSnapshotFile(state);
                        break;
                    case StatusPage::kDisplay:
                        if (state->virtual_camera_controller.running()) {
                            StopVirtualCamera(state, true);
                        } else if (!state->virtual_camera_controller.installed()) {
                            InstallVirtualCamera(state);
                        } else {
                            StartVirtualCamera(state);
                        }
                        break;
                    case StatusPage::kDiagnostics:
                        FocusVideoWindow(state);
                        break;
                    case StatusPage::kVoiceControl:
                        OpenLogFile(state);
                        break;
                    case StatusPage::kOverview:
                    default:
                        NavigateToPage(state, StatusPage::kVoiceControl);
                        LayoutStatusWindow(state);
                        break;
                    }
                }
                return 0;
            case kOpenLogButtonId:
                if (state != nullptr) {
                    switch (state->current_page) {
                    case StatusPage::kDisplay:
                        ResizeVideoWindowToSelectedProfile(state);
                        FocusVideoWindow(state);
                        break;
                    case StatusPage::kVoiceControl:
                        OpenOutputDirectory(state);
                        break;
                    case StatusPage::kDiagnostics:
                        OpenStatusSnapshotFile(state);
                        break;
                    case StatusPage::kLogs:
                        OpenOutputDirectory(state);
                        break;
                    case StatusPage::kConnect:
                        OpenStatusSnapshotFile(state);
                        break;
                    case StatusPage::kOverview:
                    default:
                        OpenLogFile(state);
                        break;
                    }
                }
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
    case kVoiceCommandMessage:
        HandleVoiceCommandMessage(state);
        return 0;
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
            StopVoiceControl(state, false);
            if (state->danmaku_controller != nullptr) {
                state->danmaku_controller->Stop();
            }
            StopAutoReplySendPointCapture(state);
            StopAutoReplyWorker(state);
            if (state->local_music_player != nullptr) {
                state->local_music_player->Stop();
            }
            state->web_ui_bridge.reset();
            state->danmaku_controller.reset();
            state->airplay_controller.Stop();
            state->virtual_camera_controller.SetStateChangedFn({});
            state->virtual_camera_controller.SetLogFn({});
            state->virtual_camera_controller.Stop();
            state->virtual_camera_bridge.Stop();
            state->control_server.reset();
            state->udp_receiver.reset();
            state->audio_receiver.reset();
            state->decoder.reset();
            state->audio_player.reset();
            DeleteFontHandle(state->title_font);
            DeleteFontHandle(state->subtitle_font);
            DeleteFontHandle(state->section_font);
            DeleteFontHandle(state->value_font);
            DeleteFontHandle(state->spotlight_value_font);
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    HANDLE instance_mutex = CreateMutexW(nullptr, TRUE, kInstanceMutexName);
    if (instance_mutex == nullptr) {
        MessageBoxW(nullptr, L"\u521B\u5EFA\u5355\u4F8B\u4E92\u65A5\u9501\u5931\u8D25\u3002", kStatusWindowTitle, MB_ICONERROR | MB_OK);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing_window = FindWindowW(kStatusWindowClassName, nullptr); existing_window != nullptr) {
            ShowWindow(existing_window, IsIconic(existing_window) ? SW_RESTORE : SW_SHOW);
            BringWindowToTop(existing_window);
            SetForegroundWindow(existing_window);
        } else {
            MessageBoxW(nullptr, L"\u76F4\u64AD\u6295\u5C4F\u52A9\u624B\u5DF2\u7ECF\u5728\u8FD0\u884C\u3002", kStatusWindowTitle, MB_ICONINFORMATION | MB_OK);
        }
        CloseHandle(instance_mutex);
        return 0;
    }

    const HRESULT com_initialize_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_initialize_result)) {
        CloseHandle(instance_mutex);
        MessageBoxW(nullptr, L"\u63A5\u6536\u7AEF COM \u73AF\u5883\u521D\u59CB\u5316\u5931\u8D25\u3002", kStatusWindowTitle, MB_ICONERROR | MB_OK);
        return 1;
    }
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
        WS_POPUP | WS_CLIPCHILDREN | WS_VISIBLE,
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

    ShowWindow(status_window, show_command == SW_HIDE ? SW_SHOWNORMAL : show_command);
    UpdateWindow(status_window);

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
