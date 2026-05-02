#include "AudioPlayer.h"

#include <avrt.h>

#include <algorithm>
#include <chrono>
#include <sstream>

#pragma comment(lib, "Avrt.lib")

namespace {

// Tune for 10ms PCM frames. The current waveOut path is more stable if it
// behaves like a short buffered PCM queue instead of trying to schedule every
// frame against the sender clock. We still keep the buffer compact, but give it
// enough room to absorb normal wireless and thread jitter without audible cuts.
constexpr bool kUseClockSyncedPcmScheduling = false;
constexpr size_t kMaxInFlightBuffers = 10;
constexpr size_t kMaxPendingBufferedMs = 120;
constexpr size_t kStartupMaxSubmittedBufferedMs = 50;
constexpr size_t kStartupTargetTotalBufferedMs = 60;
constexpr size_t kStartupSoftTotalBufferedMs = 80;
constexpr size_t kSteadyMaxSubmittedBufferedMs = 40;
constexpr size_t kSteadyTargetTotalBufferedMs = 50;
constexpr size_t kSteadySoftTotalBufferedMs = 70;
constexpr size_t kRecoveryMaxSubmittedBufferedMs = 60;
constexpr size_t kRecoveryTargetTotalBufferedMs = 70;
constexpr size_t kRecoverySoftTotalBufferedMs = 90;
constexpr size_t kHardTotalBufferedMs = 130;
constexpr size_t kStartupMinSubmitFrameCount = 1;
constexpr size_t kSteadyMinSubmitFrameCount = 1;
constexpr size_t kRecoveryMinSubmitFrameCount = 1;
constexpr uint64_t kSteadyStatePlayedFramesThreshold = 14;
constexpr int64_t kSteadyStateWarmupUs = 200'000;
constexpr int64_t kRecoveryHoldUs = 1'500'000;
constexpr int64_t kSoftOverloadWindowUs = 800'000;
constexpr uint64_t kSoftOverloadEventsBeforeRecovery = 2;
constexpr uint64_t kSoftOverloadEventsBeforeResync = 5;
constexpr int64_t kResyncCooldownUs = 1'500'000;

constexpr int64_t kAudioSubmitLeadUs = 8'000;
constexpr int64_t kAudioLateDropUs = 90'000;
constexpr int64_t kAudioWakeMarginUs = 500;

int64_t NowSteadyUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::wstring WaveErrorText(MMRESULT result) {
    wchar_t buffer[MAXERRORLENGTH] = {};
    if (waveOutGetErrorTextW(result, buffer, MAXERRORLENGTH) == MMSYSERR_NOERROR) {
        return buffer;
    }

    std::wostringstream stream;
    stream << L"MMRESULT=" << result;
    return stream.str();
}

}  // namespace

AudioPlayer::AudioPlayer(LogFn log_fn)
    : log_fn_(std::move(log_fn)),
      worker_thread_([this] { WorkerMain(); }) {}

AudioPlayer::~AudioPlayer() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        desired_running_ = false;
        desired_sample_rate_ = 0;
        desired_channels_ = 0;
        pending_pcm_frames_.clear();
        worker_running_ = false;
        ++request_generation_;
    }
    cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

bool AudioPlayer::Start(int sample_rate, int channels) {
    if (sample_rate <= 0 || channels <= 0) {
        Stop();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool same_format =
            desired_running_ &&
            desired_sample_rate_ == sample_rate &&
            desired_channels_ == channels;
        desired_running_ = true;
        desired_sample_rate_ = sample_rate;
        desired_channels_ = channels;
        if (!same_format) {
            pending_pcm_frames_.clear();
            ResetSessionStateLocked();
            ++request_generation_;
        } else if (wave_out_ != nullptr) {
            return true;
        } else {
            ++request_generation_;
        }
    }

    cv_.notify_all();
    return true;
}

void AudioPlayer::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!desired_running_ && wave_out_ == nullptr && pending_pcm_frames_.empty()) {
            return;
        }
        desired_running_ = false;
        desired_sample_rate_ = 0;
        desired_channels_ = 0;
        pending_pcm_frames_.clear();
        ++request_generation_;
    }
    cv_.notify_all();
}

bool AudioPlayer::SubmitPcmFrame(const uint8_t* data, size_t size) {
    return SubmitPcmFrame(data, size, 0, false, 0);
}

bool AudioPlayer::SubmitPcmFrame(
    const uint8_t* data,
    size_t size,
    uint64_t sender_pts_us,
    bool has_clock_sync,
    int64_t sender_clock_offset_us) {
    if (data == nullptr || size == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!desired_running_) {
        return false;
    }

    PendingPcmFrame frame;
    frame.bytes.assign(data, data + size);
    frame.sender_pts_us = sender_pts_us;
    if (kUseClockSyncedPcmScheduling && has_clock_sync && sender_pts_us > 0) {
        frame.target_submit_us =
            static_cast<int64_t>(sender_pts_us) - sender_clock_offset_us - kAudioSubmitLeadUs;
    }

    pending_pcm_frames_.push_back(std::move(frame));
    if (PendingBufferedMsLocked() > kMaxPendingBufferedMs) {
        TrimPendingBufferedMsLocked(
            kMaxPendingBufferedMs,
            L"PCM \u5f85\u63d0\u4ea4\u7f13\u51b2\u65f6\u957f\u79ef\u538b\uff0c\u4e22\u5f03\u65e7\u97f3\u9891\u5e27\u8ffd\u8d76\u5b9e\u65f6\u64ad\u653e\u3002");
    }
    if (TotalBufferedMsLocked() > kHardTotalBufferedMs) {
        TrimTotalBufferedMsLocked(
            kHardTotalBufferedMs,
            L"\u97f3\u9891\u603b\u7f13\u51b2\u79ef\u538b\u8fc7\u591a\uff0c\u4e22\u5f03\u8fc7\u671f PCM \u5e27\u907f\u514d\u58F0\u97f3\u6ed1\u5230\u753b\u9762\u540e\u9762\u3002");
    }
    cv_.notify_all();
    return true;
}

AudioPlaybackStats AudioPlayer::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    AudioPlaybackStats stats;
    stats.submitted_frames = submitted_frames_;
    stats.played_frames = played_frames_;
    stats.dropped_frames = dropped_frames_;
    stats.resync_count = resync_count_;
    stats.recovery_mode = recovery_mode_until_us_ > NowSteadyUs();
    stats.buffered_frames = buffers_.size() + pending_pcm_frames_.size();
    stats.buffered_ms = TotalBufferedMsLocked();
    stats.pending_buffered_ms = PendingBufferedMsLocked();
    stats.submitted_buffered_ms = SubmittedBufferedMsLocked();
    return stats;
}

void CALLBACK AudioPlayer::WaveOutCallback(
    HWAVEOUT,
    UINT message,
    DWORD_PTR instance,
    DWORD_PTR param1,
    DWORD_PTR) {
    auto* player = reinterpret_cast<AudioPlayer*>(instance);
    if (player == nullptr) {
        return;
    }

    player->HandleWaveOutMessage(message, reinterpret_cast<WAVEHDR*>(param1));
}

void AudioPlayer::WorkerMain() {
    DWORD mmcss_task_index = 0;
    HANDLE mmcss_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task_index);
    if (mmcss_handle != nullptr) {
        AvSetMmThreadPriority(mmcss_handle, AVRT_PRIORITY_HIGH);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    uint64_t observed_generation = 0;

    while (true) {
        bool should_run = false;
        int desired_sample_rate = 0;
        int desired_channels = 0;
        bool needs_close = false;
        bool needs_open = false;
        bool needs_resync = false;
        std::vector<uint8_t> frame_to_submit;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            while (true) {
                if (!worker_running_) {
                    break;
                }
                if (request_generation_ != observed_generation) {
                    break;
                }
                if (HasCompletedBuffersLocked()) {
                    break;
                }

                const BufferProfile active_profile = ActiveProfileLocked();
                LogProfileTransitionIfNeededLocked(active_profile);
                const bool can_submit =
                    desired_running_ &&
                    wave_out_ != nullptr &&
                    !pending_pcm_frames_.empty() &&
                    buffers_.size() < kMaxInFlightBuffers &&
                    SubmittedBufferedMsLocked() < active_profile.max_submitted_buffered_ms;
                if (!can_submit) {
                    cv_.wait(lock);
                    continue;
                }

                const PendingPcmFrame& pending_frame = pending_pcm_frames_.front();
                if (pending_frame.target_submit_us <= 0) {
                    break;
                }

                const int64_t now_us = NowSteadyUs();
                const int64_t delta_us = pending_frame.target_submit_us - now_us;
                if (delta_us <= kAudioWakeMarginUs) {
                    break;
                }

                sync_wait_count_ += 1;
                cv_.wait_for(lock, std::chrono::microseconds(delta_us));
            }

            if (!worker_running_ && wave_out_ == nullptr && pending_pcm_frames_.empty()) {
                break;
            }

            observed_generation = request_generation_;
            should_run = desired_running_;
            desired_sample_rate = desired_sample_rate_;
            desired_channels = desired_channels_;

            if (wave_out_ != nullptr) {
                ReapCompletedBuffersLocked(wave_out_, false);
            }

            needs_close =
                wave_out_ != nullptr &&
                (!should_run ||
                    sample_rate_ != desired_sample_rate ||
                    channels_ != desired_channels);
            needs_open = should_run && wave_out_ == nullptr;
            needs_resync =
                should_run &&
                wave_out_ != nullptr &&
                !needs_close &&
                playback_resync_requested_;
            if (needs_resync) {
                playback_resync_requested_ = false;
            }

            if (should_run && !needs_close && !needs_open) {
                const BufferProfile active_profile = ActiveProfileLocked();
                LogProfileTransitionIfNeededLocked(active_profile);
                if (TotalBufferedMsLocked() > active_profile.soft_total_buffered_ms) {
                    HandleSoftOverloadLocked(active_profile);
                    if (!needs_resync &&
                        wave_out_ != nullptr &&
                        playback_resync_requested_) {
                        needs_resync = true;
                        playback_resync_requested_ = false;
                    }
                }
            }

            const BufferProfile active_profile = ActiveProfileLocked();
            LogProfileTransitionIfNeededLocked(active_profile);
            if (should_run &&
                !needs_close &&
                !needs_open &&
                !pending_pcm_frames_.empty() &&
                buffers_.size() < kMaxInFlightBuffers &&
                SubmittedBufferedMsLocked() < active_profile.max_submitted_buffered_ms) {
                // 累积足够的帧再提交，避免撕裂
                while (!pending_pcm_frames_.empty()) {
                    const PendingPcmFrame& pending_frame = pending_pcm_frames_.front();
                    if (pending_frame.target_submit_us <= 0) {
                        break;
                    }

                    const int64_t now_us = NowSteadyUs();
                    if (now_us - pending_frame.target_submit_us <= kAudioLateDropUs) {
                        break;
                    }

                    pending_pcm_frames_.pop_front();
                    ++dropped_frames_;
                    ++sync_drop_count_;
                    if (log_fn_ != nullptr &&
                        (sync_drop_count_ <= 5 || (sync_drop_count_ % 20) == 0)) {
                        std::wostringstream stream;
                        stream << L"\u97f3\u9891\u64ad\u653e: \u65F6\u95F4\u6233\u5DF2\u843D\u540E\u753B\u9762\u8FC7\u591A\uFF0C\u4E22\u5F03\u8FC7\u671F PCM \u5E27"
                               << L" lateUs=" << (now_us - pending_frame.target_submit_us)
                               << L", senderPtsUs=" << pending_frame.sender_pts_us
                               << L", dropped=" << dropped_frames_;
                        log_fn_(stream.str());
                    }
                }

                if (pending_pcm_frames_.empty()) {
                    continue;
                }

                const size_t pending_ms = PendingBufferedMsLocked();
                const size_t submitted_ms = SubmittedBufferedMsLocked();
                if (pending_ms + submitted_ms < active_profile.target_total_buffered_ms &&
                    pending_pcm_frames_.size() < active_profile.min_submit_frame_count) {
                    continue;
                }
/*
                if (pending_ms + submitted_ms < active_profile.target_total_buffered_ms &&
                
                // 如果缓冲太少，等待更多数据
                if (pending_ms + submitted_ms < active_profile.target_total_buffered_ms &&
                    pending_pcm_frames_.size() < active_profile.min_submit_frame_count) {
                    continue;
                }
                
*/
                frame_to_submit = std::move(pending_pcm_frames_.front().bytes);
                pending_pcm_frames_.pop_front();
            }
        }

        if (needs_close) {
            CloseWaveOutDevice();
            continue;
        }

        if (needs_resync) {
            CloseWaveOutDevice();
            continue;
        }

        if (needs_open) {
            OpenWaveOutDevice(desired_sample_rate, desired_channels);
            continue;
        }

        if (!frame_to_submit.empty()) {
            SubmitQueuedFrame(std::move(frame_to_submit));
        }
    }

    CloseWaveOutDevice();
    if (mmcss_handle != nullptr) {
        AvRevertMmThreadCharacteristics(mmcss_handle);
    }
}

bool AudioPlayer::OpenWaveOutDevice(int sample_rate, int channels) {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(channels);
    format.nSamplesPerSec = static_cast<DWORD>(sample_rate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;

    HWAVEOUT wave_out = nullptr;
    const MMRESULT open_result = waveOutOpen(
        &wave_out,
        WAVE_MAPPER,
        &format,
        reinterpret_cast<DWORD_PTR>(&AudioPlayer::WaveOutCallback),
        reinterpret_cast<DWORD_PTR>(this),
        CALLBACK_FUNCTION);
    if (open_result != MMSYSERR_NOERROR) {
        if (log_fn_) {
            log_fn_(L"\u97f3\u9891\u64ad\u653e: \u6253\u5f00\u64ad\u653e\u8bbe\u5907\u5931\u8d25\uff0c" + WaveErrorText(open_result));
        }
        return false;
    }

    bool keep_open = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (worker_running_ &&
            desired_running_ &&
            desired_sample_rate_ == sample_rate &&
            desired_channels_ == channels) {
            wave_out_ = wave_out;
            sample_rate_ = sample_rate;
            channels_ = channels;
            device_played_frames_ = 0;
            sync_wait_count_ = 0;
            sync_drop_count_ = 0;
            playback_started_at_us_ = NowSteadyUs();
            steady_state_profile_logged_ = false;
            buffers_.clear();
            keep_open = true;
        }
    }

    if (!keep_open) {
        waveOutClose(wave_out);
        return false;
    }

    if (log_fn_) {
        std::wostringstream stream;
        stream << L"\u97f3\u9891\u64ad\u653e: \u5df2\u542f\u52a8 " << sample_rate << L"Hz/" << channels << L"ch PCM";
        log_fn_(stream.str());
    }
    return true;
}

bool AudioPlayer::SubmitQueuedFrame(std::vector<uint8_t> bytes) {
    if (bytes.empty()) {
        return false;
    }

    HWAVEOUT wave_out = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wave_out_ == nullptr || !desired_running_) {
            return false;
        }
        wave_out = wave_out_;
    }

    auto buffer = std::make_unique<Buffer>();
    buffer->bytes = std::move(bytes);
    buffer->header.lpData = reinterpret_cast<LPSTR>(buffer->bytes.data());
    buffer->header.dwBufferLength = static_cast<DWORD>(buffer->bytes.size());

    const MMRESULT prepare_result = waveOutPrepareHeader(wave_out, &buffer->header, sizeof(WAVEHDR));
    if (prepare_result != MMSYSERR_NOERROR) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++dropped_frames_;
        if (log_fn_) {
            log_fn_(L"\u97f3\u9891\u64ad\u653e: \u9884\u5904\u7406\u97f3\u9891\u7f13\u51b2\u5931\u8d25\uff0c" + WaveErrorText(prepare_result));
        }
        return false;
    }

    const MMRESULT write_result = waveOutWrite(wave_out, &buffer->header, sizeof(WAVEHDR));
    if (write_result != MMSYSERR_NOERROR) {
        waveOutUnprepareHeader(wave_out, &buffer->header, sizeof(WAVEHDR));
        std::lock_guard<std::mutex> lock(mutex_);
        ++dropped_frames_;
        if (log_fn_) {
            log_fn_(L"\u97f3\u9891\u64ad\u653e: \u63d0\u4ea4 PCM \u7f13\u51b2\u5931\u8d25\uff0c" + WaveErrorText(write_result));
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        buffers_.push_back(std::move(buffer));
        ++submitted_frames_;
    }
    return true;
}

void AudioPlayer::CloseWaveOutDevice() {
    HWAVEOUT wave_out = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wave_out = wave_out_;
        wave_out_ = nullptr;
        sample_rate_ = 0;
        channels_ = 0;
        playback_started_at_us_ = 0;
        steady_state_profile_logged_ = false;
    }

    if (wave_out == nullptr) {
        return;
    }

    waveOutReset(wave_out);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ReapCompletedBuffersLocked(wave_out, true);
        buffers_.clear();
    }
    waveOutClose(wave_out);

    if (log_fn_) {
        log_fn_(L"\u97f3\u9891\u64ad\u653e: \u5df2\u505c\u6b62");
    }
}

void AudioPlayer::HandleWaveOutMessage(UINT message, WAVEHDR* header) {
    if (message != WOM_DONE || header == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& buffer : buffers_) {
            if (&buffer->header == header) {
                buffer->done = true;
                ++played_frames_;
                ++device_played_frames_;
                break;
            }
        }
    }
    cv_.notify_all();
}

bool AudioPlayer::HasCompletedBuffersLocked() const {
    return std::any_of(
        buffers_.begin(),
        buffers_.end(),
        [](const std::unique_ptr<Buffer>& buffer) {
            return buffer->done || ((buffer->header.dwFlags & WHDR_DONE) != 0);
        });
}

AudioPlayer::BufferProfile AudioPlayer::ActiveProfileLocked() const {
    const int64_t now_us = NowSteadyUs();
    if (recovery_mode_until_us_ > now_us) {
        return BufferProfile{
            kRecoveryMaxSubmittedBufferedMs,
            kRecoveryTargetTotalBufferedMs,
            kRecoverySoftTotalBufferedMs,
            kRecoveryMinSubmitFrameCount,
            false,
            true};
    }

    const bool warmed_up_by_time =
        playback_started_at_us_ > 0 &&
        now_us > playback_started_at_us_ &&
        (now_us - playback_started_at_us_) >= kSteadyStateWarmupUs;
    const bool warmed_up_by_playback = device_played_frames_ >= kSteadyStatePlayedFramesThreshold;

    if (warmed_up_by_time && warmed_up_by_playback) {
        return BufferProfile{
            kSteadyMaxSubmittedBufferedMs,
            kSteadyTargetTotalBufferedMs,
            kSteadySoftTotalBufferedMs,
            kSteadyMinSubmitFrameCount,
            true,
            false};
    }

    return BufferProfile{
        kStartupMaxSubmittedBufferedMs,
        kStartupTargetTotalBufferedMs,
        kStartupSoftTotalBufferedMs,
        kStartupMinSubmitFrameCount,
        false,
        false};
}

void AudioPlayer::LogProfileTransitionIfNeededLocked(const BufferProfile& profile) {
    if (!profile.steady_state || steady_state_profile_logged_ || log_fn_ == nullptr) {
        return;
    }

    steady_state_profile_logged_ = true;
    std::wostringstream stream;
    stream << L"\u97f3\u9891\u64ad\u653e: \u5DF2\u5207\u5230\u7A33\u5B9A\u4F4E\u5EF6\u8FDF\u7A33\u6001"
           << L"\uFF0C\u76EE\u6807\u603B\u7F13\u51B2 " << profile.target_total_buffered_ms
           << L"ms\uFF0C\u63D0\u4EA4\u7F13\u51B2\u4E0A\u9650 " << profile.max_submitted_buffered_ms
           << L"ms\u3002";
    log_fn_(stream.str());
}

void AudioPlayer::ResetSessionStateLocked() {
    submitted_frames_ = 0;
    played_frames_ = 0;
    dropped_frames_ = 0;
    device_played_frames_ = 0;
    resync_count_ = 0;
    sync_wait_count_ = 0;
    sync_drop_count_ = 0;
    soft_overload_event_count_ = 0;
    playback_started_at_us_ = 0;
    recovery_mode_until_us_ = 0;
    last_soft_overload_at_us_ = 0;
    last_resync_at_us_ = 0;
    steady_state_profile_logged_ = false;
    playback_resync_requested_ = false;
}

void AudioPlayer::EnterRecoveryModeLocked(int64_t now_us, const wchar_t* reason) {
    const bool already_active = recovery_mode_until_us_ > now_us;
    recovery_mode_until_us_ = std::max(recovery_mode_until_us_, now_us + kRecoveryHoldUs);
    if (already_active || log_fn_ == nullptr) {
        return;
    }

    std::wostringstream stream;
    stream << L"\u97f3\u9891\u64ad\u653e: " << reason
           << L"\uFF0C\u4E34\u65F6\u5207\u5230\u6062\u590D\u7F13\u51B2\u6A21\u5F0F"
           << L"\uFF0C\u76EE\u6807\u603B\u7F13\u51B2 " << kRecoveryTargetTotalBufferedMs
           << L"ms\uFF0C\u63D0\u4EA4\u4E0A\u9650 " << kRecoveryMaxSubmittedBufferedMs
           << L"ms\u3002";
    log_fn_(stream.str());
}

void AudioPlayer::HandleSoftOverloadLocked(const BufferProfile& profile) {
    const int64_t now_us = NowSteadyUs();
    if (last_soft_overload_at_us_ <= 0 ||
        (now_us - last_soft_overload_at_us_) > kSoftOverloadWindowUs) {
        soft_overload_event_count_ = 0;
    }
    last_soft_overload_at_us_ = now_us;
    ++soft_overload_event_count_;

    if (soft_overload_event_count_ >= kSoftOverloadEventsBeforeRecovery) {
        EnterRecoveryModeLocked(
            now_us,
            L"\u68C0\u6D4B\u5230\u63A5\u6536\u7AEF\u64AD\u653E\u79EF\u538B\u53CD\u590D\u51FA\u73B0");
    }

    if (soft_overload_event_count_ >= kSoftOverloadEventsBeforeResync &&
        (last_resync_at_us_ <= 0 || (now_us - last_resync_at_us_) >= kResyncCooldownUs)) {
        TrimTotalBufferedMsLocked(
            kRecoveryTargetTotalBufferedMs,
            L"\u97f3\u9891\u961F\u5217\u79EF\u538B\u6301\u7EED\u53CD\u590D\u51FA\u73B0\uFF0C\u5148\u4E22\u5F03\u90E8\u5206\u65E7 PCM \u5E27\u51C6\u5907\u91CD\u540C\u6B65\u3002");
        playback_resync_requested_ = true;
        last_resync_at_us_ = now_us;
        soft_overload_event_count_ = 0;
        ++resync_count_;
        if (log_fn_ != nullptr) {
            std::wostringstream stream;
            stream << L"\u97f3\u9891\u64ad\u653e: \u5DF2\u89E6\u53D1\u81EA\u52A8\u91CD\u540C\u6B65"
                   << L"\uFF0Cresyncs=" << resync_count_
                   << L", totalMs=" << TotalBufferedMsLocked()
                   << L", pendingMs=" << PendingBufferedMsLocked()
                   << L", submittedMs=" << SubmittedBufferedMsLocked();
            log_fn_(stream.str());
        }
        return;
    }

    TrimTotalBufferedMsLocked(
        profile.recovery_mode ? kRecoveryTargetTotalBufferedMs : profile.target_total_buffered_ms,
        L"\u97f3\u9891\u64ad\u653e\u79EF\u538B\u8D85\u51FA\u5F53\u524D\u7A33\u5B9A\u533A\u95F4\uFF0C\u4E22\u5F03\u6700\u65E7 PCM \u5E27\u907F\u514D\u58F0\u97F3\u8FDE\u7EED\u88C2\u97F3\u3002");
}

size_t AudioPlayer::FrameDurationMsLocked(size_t bytes) const {
    const int sample_rate = sample_rate_ > 0 ? sample_rate_ : desired_sample_rate_;
    const int channels = channels_ > 0 ? channels_ : desired_channels_;
    if (sample_rate <= 0 || channels <= 0 || bytes == 0) {
        return 0;
    }

    const size_t bytes_per_second =
        static_cast<size_t>(sample_rate) * static_cast<size_t>(channels) * sizeof(int16_t);
    const size_t rounded_up = bytes * 1000 + (bytes_per_second - 1);
    return std::max<size_t>(1, rounded_up / bytes_per_second);
}

size_t AudioPlayer::FrameDurationMsLocked(const PendingPcmFrame& frame) const {
    return FrameDurationMsLocked(frame.bytes.size());
}

size_t AudioPlayer::PendingBufferedMsLocked() const {
    size_t total_ms = 0;
    for (const auto& frame : pending_pcm_frames_) {
        total_ms += FrameDurationMsLocked(frame.bytes.size());
    }
    return total_ms;
}

size_t AudioPlayer::SubmittedBufferedMsLocked() const {
    size_t total_ms = 0;
    for (const auto& buffer : buffers_) {
        const bool done = buffer->done || ((buffer->header.dwFlags & WHDR_DONE) != 0);
        if (!done) {
            total_ms += FrameDurationMsLocked(buffer->bytes.size());
        }
    }
    return total_ms;
}

size_t AudioPlayer::TotalBufferedMsLocked() const {
    return PendingBufferedMsLocked() + SubmittedBufferedMsLocked();
}

void AudioPlayer::DropOldestPendingFramesLocked(size_t frames_to_drop, const wchar_t* reason) {
    const size_t removable = std::min(frames_to_drop, pending_pcm_frames_.size());
    if (removable == 0) {
        return;
    }

    for (size_t index = 0; index < removable; ++index) {
        pending_pcm_frames_.pop_front();
    }
    dropped_frames_ += removable;

    if (log_fn_ == nullptr) {
        return;
    }

    if (dropped_frames_ <= 5 || removable > 1 || (dropped_frames_ % 20) == 0) {
        std::wostringstream stream;
        stream << L"\u97f3\u9891\u64ad\u653e: " << reason
               << L" droppedNow=" << removable
               << L", pending=" << pending_pcm_frames_.size()
               << L", buffered=" << buffers_.size()
               << L", dropped=" << dropped_frames_;
        log_fn_(stream.str());
    }
}

void AudioPlayer::TrimPendingBufferedMsLocked(size_t max_pending_ms, const wchar_t* reason) {
    size_t dropped_now = 0;
    while (!pending_pcm_frames_.empty() && PendingBufferedMsLocked() > max_pending_ms) {
        pending_pcm_frames_.pop_front();
        ++dropped_now;
    }

    if (dropped_now == 0) {
        return;
    }

    dropped_frames_ += dropped_now;
    if (log_fn_ && (dropped_frames_ <= 5 || dropped_now > 1 || (dropped_frames_ % 20) == 0)) {
        std::wostringstream stream;
        stream << L"\u97f3\u9891\u64ad\u653e: " << reason
               << L" droppedNow=" << dropped_now
               << L", pendingMs=" << PendingBufferedMsLocked()
               << L", submittedMs=" << SubmittedBufferedMsLocked()
               << L", dropped=" << dropped_frames_;
        log_fn_(stream.str());
    }
}

void AudioPlayer::TrimTotalBufferedMsLocked(size_t max_total_ms, const wchar_t* reason) {
    size_t dropped_now = 0;
    while (!pending_pcm_frames_.empty() && TotalBufferedMsLocked() > max_total_ms) {
        pending_pcm_frames_.pop_front();
        ++dropped_now;
    }

    if (dropped_now == 0) {
        return;
    }

    dropped_frames_ += dropped_now;
    if (log_fn_ && (dropped_frames_ <= 5 || dropped_now > 1 || (dropped_frames_ % 20) == 0)) {
        std::wostringstream stream;
        stream << L"\u97f3\u9891\u64ad\u653e: " << reason
               << L" droppedNow=" << dropped_now
               << L", totalMs=" << TotalBufferedMsLocked()
               << L", pendingMs=" << PendingBufferedMsLocked()
               << L", submittedMs=" << SubmittedBufferedMsLocked()
               << L", dropped=" << dropped_frames_;
        log_fn_(stream.str());
    }
}

void AudioPlayer::ReapCompletedBuffersLocked(HWAVEOUT wave_out, bool force) {
    for (auto it = buffers_.begin(); it != buffers_.end();) {
        Buffer* buffer = it->get();
        const bool done = force || buffer->done || ((buffer->header.dwFlags & WHDR_DONE) != 0);
        if (!done) {
            ++it;
            continue;
        }

        waveOutUnprepareHeader(wave_out, &buffer->header, sizeof(WAVEHDR));
        it = buffers_.erase(it);
    }
}
