#pragma once

#include <Windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class DanmakuController {
public:
    struct Snapshot {
        bool region_ready = false;
        bool running = false;
        bool ui_probe_running = false;
        bool reminder_enabled = true;
        bool gift_reminder_enabled = true;
        bool speech_enabled = true;
        int speech_voice_count = 0;
        std::wstring status_text;
        std::wstring region_label;
        std::wstring ui_probe_status;
        std::wstring ui_probe_target_title;
        std::wstring speech_voice_name;
        std::wstring last_text;
        std::wstring last_capture_path;
        std::vector<std::wstring> recent_events;
        std::vector<std::wstring> recent_probe_lines;
    };

    struct NormalizedRegion {
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
        bool valid = false;
    };

    using LogFn = std::function<void(const std::wstring&)>;
    using EventFn = std::function<void(bool is_gift, const std::wstring& text)>;

    DanmakuController(std::wstring region_file_path, std::wstring capture_root_path, LogFn log_fn);
    ~DanmakuController();

    void SetWindows(HWND owner_window, HWND video_window);
    void SetEventFn(EventFn event_fn);
    bool SelectRegion();
    bool TestRecognizeFrame();
    bool Start();
    void Stop();
    bool StartUiAutomationProbe();
    bool ToggleReminder();
    bool ToggleGiftReminder();
    bool ToggleSpeech();
    bool CycleSpeechVoice();
    void ClearRecentEvents();
    Snapshot GetSnapshot() const;

    const std::wstring& capture_root_path() const { return capture_root_path_; }

private:
    enum class EventKind {
        kText,
        kImageGift,
    };

    struct FrameCapture {
        int width = 0;
        int height = 0;
        RECT screen_rect{};
        std::vector<uint8_t> pixels;
    };

    struct DedupeEntry {
        std::wstring dedupe_key;
        ULONGLONG tick = 0;
    };

    struct VisualCandidate {
        bool valid = false;
        uint64_t hash = 0;
        RECT bounds{};
        double pixel_ratio = 0.0;
        double bounds_ratio = 0.0;
        int occupied_blocks = 0;
    };

    struct UiProbeResult {
        std::wstring target_title;
        std::wstring status_text;
        std::vector<std::wstring> lines;
    };

    void WorkerLoop();
    void SpeechLoop();
    void UiProbeLoop();
    bool PollUiDanmakuWindow();
    bool CaptureAndRecognizeFrame(bool manual_test);
    bool CaptureScreenRect(const RECT& capture_rect, FrameCapture* frame, std::wstring* error) const;
    bool CaptureCurrentRegion(FrameCapture* frame, std::wstring* error) const;
    bool CaptureWindowRegion(HWND target_window, FrameCapture* frame, std::wstring* error) const;
    std::vector<std::wstring> RecognizeTextLines(const FrameCapture& frame, std::wstring* error) const;
    std::wstring RecognizeText(const FrameCapture& frame, std::wstring* error) const;
    bool SaveBitmap(const std::wstring& path, const FrameCapture& frame) const;
    void SetStatus(const std::wstring& status);
    std::wstring BuildRegionLabelLocked() const;
    bool LoadRegionLocked();
    bool SaveRegionLocked() const;
    bool HasUsableVideoWindow() const;
    std::wstring BuildCaptureFilePath(bool manual_test) const;
    bool IsDuplicateLocked(const std::wstring& dedupe_key, ULONGLONG now_tick);
    void AppendEventLocked(
        EventKind kind,
        const std::wstring& text,
        const std::wstring& capture_path,
        const std::wstring& dedupe_key,
        ULONGLONG now_tick);
    void ResetGiftCandidateLocked();
    bool UpdateGiftCandidateLocked(const VisualCandidate& candidate, ULONGLONG now_tick);
    void NotifyAcceptedEvent(EventKind kind, const std::wstring& text, bool manual_test);
    void QueueSpeech(const std::wstring& text);
    void ApplyUiProbeResult(UiProbeResult result);
    bool IsOwnWindowLocked(HWND target) const;

    static std::wstring NormalizeText(const std::wstring& text);
    static std::wstring TrimWhitespace(const std::wstring& text);
    static std::wstring TruncateText(const std::wstring& text, size_t max_chars);
    static std::wstring ClassifyPrefix(const std::wstring& text);
    static VisualCandidate AnalyzeVisualCandidate(const FrameCapture& frame);
    static int HammingDistance64(uint64_t left, uint64_t right);
    static bool AreRectsSimilar(const RECT& left, const RECT& right, int tolerance);
    static std::wstring BuildSpeechText(EventKind kind, const std::wstring& text);
    static std::vector<std::wstring> ExtractOcrDanmakuLines(const std::vector<std::wstring>& lines);
    UiProbeResult ProbeWindowByVisibleContent(HWND target_window) const;
    static std::wstring FormatClockNow();

    mutable std::mutex mutex_;
    LogFn log_fn_;
    EventFn event_fn_;
    HWND owner_window_ = nullptr;
    HWND video_window_ = nullptr;
    std::wstring region_file_path_;
    std::wstring capture_root_path_;
    NormalizedRegion region_{};
    std::wstring status_text_ = L"\u7b49\u5f85\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97";
    std::wstring ui_probe_status_ = L"\u7b49\u5f85\u63a2\u6d4b\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97";
    std::wstring ui_probe_target_title_;
    std::wstring ui_live_target_title_;
    std::wstring last_text_;
    std::wstring last_capture_path_;
    std::deque<std::wstring> recent_events_;
    std::deque<std::wstring> recent_probe_lines_;
    std::deque<std::wstring> ui_live_visible_lines_;
    std::deque<std::wstring> ui_live_visible_keys_;
    std::deque<DedupeEntry> dedupe_entries_;
    std::vector<std::wstring> speech_voice_names_;
    size_t speech_voice_index_ = 0;
    bool speech_voice_index_loaded_ = false;
    bool reminder_enabled_ = true;
    bool gift_reminder_enabled_ = true;
    bool speech_enabled_ = true;
    bool visual_baseline_ready_ = false;
    bool visual_candidate_active_ = false;
    uint64_t visual_candidate_hash_ = 0;
    RECT visual_candidate_bounds_{};
    int visual_candidate_stable_frames_ = 0;
    ULONGLONG last_gift_tick_ = 0;
    std::thread worker_thread_;
    std::thread ui_probe_thread_;
    std::mutex speech_mutex_;
    std::condition_variable speech_cv_;
    std::deque<std::wstring> speech_queue_;
    std::thread speech_thread_;
    bool running_ = false;
    bool ui_probe_running_ = false;
    bool stop_requested_ = false;
    bool speech_stop_requested_ = false;
};
