#include "AudioPlayer.h"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace {

// 低延迟配置 - 平衡延迟和音质
constexpr size_t kMaxInFlightBuffers = 4;         // 增加并发缓冲数，避免欠载
constexpr size_t kMaxPendingBufferedMs = 15;      // 待提交缓冲 15ms (原 40ms)
constexpr size_t kMaxSubmittedBufferedMs = 12;    // 已提交缓冲 12ms (原 30ms)
constexpr size_t kTargetTotalBufferedMs = 15;     // 目标总缓冲 15ms (原 28ms)
constexpr size_t kHardTotalBufferedMs = 25;       // 硬限制 25ms (原 40ms)
// Sender audio is delivered in ~10ms PCM frames. Keep startup low-latency by allowing
// the first frame to submit immediately instead of waiting for a second frame that may
// never coexist under the 15ms pending-buffer cap.
constexpr size_t kMinSubmitFrameCount = 1;

constexpr int64_t kAudioSubmitLeadUs = 8'000;
constexpr int64_t kAudioLateDropUs = 25'000;
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
    if (has_clock_sync && sender_pts_us > 0) {
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
    stats.buffered_frames = buffers_.size() + pending_pcm_frames_.size();
    stats.buffered_ms = TotalBufferedMsLocked();
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
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    uint64_t observed_generation = 0;

    while (true) {
        bool should_run = false;
        int desired_sample_rate = 0;
        int desired_channels = 0;
        bool needs_close = false;
        bool needs_open = false;
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

                const bool can_submit =
                    desired_running_ &&
                    wave_out_ != nullptr &&
                    !pending_pcm_frames_.empty() &&
                    buffers_.size() < kMaxInFlightBuffers &&
                    SubmittedBufferedMsLocked() < kMaxSubmittedBufferedMs;
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

            if (should_run &&
                !needs_close &&
                !needs_open &&
                !pending_pcm_frames_.empty()) {
                if (TotalBufferedMsLocked() > kTargetTotalBufferedMs) {
                    TrimTotalBufferedMsLocked(
                        kTargetTotalBufferedMs,
                        L"\u97f3\u9891\u64ad\u653e\u79ef\u538b\u8fc7\u6df1\uff0c\u4e22\u5f03\u8fc7\u671f PCM \u5e27\u4fdd\u6301\u4f4e\u5ef6\u8fdf\u3002");
                }
            }

            if (should_run &&
                !needs_close &&
                !needs_open &&
                !pending_pcm_frames_.empty() &&
                buffers_.size() < kMaxInFlightBuffers &&
                SubmittedBufferedMsLocked() < kMaxSubmittedBufferedMs) {
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
                
                // 如果缓冲太少，等待更多数据
                if (pending_ms + submitted_ms < kTargetTotalBufferedMs &&
                    pending_pcm_frames_.size() < kMinSubmitFrameCount) {
                    continue;
                }
                
                frame_to_submit = std::move(pending_pcm_frames_.front().bytes);
                pending_pcm_frames_.pop_front();
            }
        }

        if (needs_close) {
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
            submitted_frames_ = 0;
            played_frames_ = 0;
            dropped_frames_ = 0;
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
