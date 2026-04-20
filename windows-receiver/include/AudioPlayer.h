#pragma once

#include <Windows.h>
#include <mmsystem.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AudioPlaybackStats {
    uint64_t submitted_frames = 0;
    uint64_t played_frames = 0;
    uint64_t dropped_frames = 0;
    size_t buffered_frames = 0;
    size_t buffered_ms = 0;
};

class AudioPlayer {
public:
    using LogFn = std::function<void(const std::wstring&)>;

    explicit AudioPlayer(LogFn log_fn);
    ~AudioPlayer();

    bool Start(int sample_rate, int channels);
    void Stop();
    bool SubmitPcmFrame(const uint8_t* data, size_t size);
    bool SubmitPcmFrame(
        const uint8_t* data,
        size_t size,
        uint64_t sender_pts_us,
        bool has_clock_sync,
        int64_t sender_clock_offset_us);
    AudioPlaybackStats GetStats() const;

private:
    struct Buffer {
        WAVEHDR header{};
        std::vector<uint8_t> bytes;
        bool done = false;
    };

    struct PendingPcmFrame {
        std::vector<uint8_t> bytes;
        uint64_t sender_pts_us = 0;
        int64_t target_submit_us = 0;
    };

    static void CALLBACK WaveOutCallback(
        HWAVEOUT wave_out,
        UINT message,
        DWORD_PTR instance,
        DWORD_PTR param1,
        DWORD_PTR param2);

    void WorkerMain();
    void HandleWaveOutMessage(UINT message, WAVEHDR* header);
    void ReapCompletedBuffersLocked(HWAVEOUT wave_out, bool force);
    void CloseWaveOutDevice();
    bool OpenWaveOutDevice(int sample_rate, int channels);
    bool SubmitQueuedFrame(std::vector<uint8_t> bytes);
    bool HasCompletedBuffersLocked() const;
    void DropOldestPendingFramesLocked(size_t frames_to_drop, const wchar_t* reason);
    size_t FrameDurationMsLocked(size_t bytes) const;
    size_t FrameDurationMsLocked(const PendingPcmFrame& frame) const;
    size_t PendingBufferedMsLocked() const;
    size_t SubmittedBufferedMsLocked() const;
    size_t TotalBufferedMsLocked() const;
    void TrimPendingBufferedMsLocked(size_t max_pending_ms, const wchar_t* reason);
    void TrimTotalBufferedMsLocked(size_t max_total_ms, const wchar_t* reason);

    LogFn log_fn_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_thread_;
    bool worker_running_ = true;
    bool desired_running_ = false;
    int desired_sample_rate_ = 0;
    int desired_channels_ = 0;
    uint64_t request_generation_ = 0;
    HWAVEOUT wave_out_ = nullptr;
    int sample_rate_ = 0;
    int channels_ = 0;
    uint64_t submitted_frames_ = 0;
    uint64_t played_frames_ = 0;
    uint64_t dropped_frames_ = 0;
    uint64_t sync_wait_count_ = 0;
    uint64_t sync_drop_count_ = 0;
    std::deque<PendingPcmFrame> pending_pcm_frames_;
    std::deque<std::unique_ptr<Buffer>> buffers_;
};
