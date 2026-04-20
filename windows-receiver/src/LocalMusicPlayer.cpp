#include "LocalMusicPlayer.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <xaudio2.h>

#include <chrono>
#include <sstream>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

std::wstring HrToText(HRESULT hr) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return stream.str();
}

}  // namespace

LocalMusicPlayer::LocalMusicPlayer(LogFn log_fn, StateChangedFn state_changed_fn)
    : log_fn_(std::move(log_fn)),
      state_changed_fn_(std::move(state_changed_fn)) {}

LocalMusicPlayer::~LocalMusicPlayer() {
    Stop();
}

bool LocalMusicPlayer::PlayFile(const std::wstring& file_path, std::wstring* detail) {
    if (detail != nullptr) {
        detail->clear();
    }
    if (file_path.empty()) {
        if (detail != nullptr) {
            *detail = L"\u97F3\u9891\u6587\u4EF6\u8DEF\u5F84\u4E3A\u7A7A";
        }
        return false;
    }

    const DWORD attributes = GetFileAttributesW(file_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        if (detail != nullptr) {
            *detail = L"\u627E\u4E0D\u5230\u8981\u64AD\u653E\u7684\u97F3\u9891\u6587\u4EF6";
        }
        return false;
    }

    Stop();

    uint64_t play_token = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = false;
        running_ = true;
        current_file_ = file_path;
        ++play_token_;
        play_token = play_token_;
    }

    NotifyStateChanged(true, file_path);

    worker_thread_ = std::thread(&LocalMusicPlayer::WorkerMain, this, file_path, play_token);
    return true;
}

void LocalMusicPlayer::Stop() {
    std::wstring current_file;
    bool was_running = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        was_running = running_;
        current_file = current_file_;
        stop_requested_ = true;
        ++play_token_;
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        current_file_.clear();
        stop_requested_ = false;
    }
    if (was_running || !current_file.empty()) {
        NotifyStateChanged(false, current_file);
    }
}

bool LocalMusicPlayer::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

std::wstring LocalMusicPlayer::current_file() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_file_;
}

void LocalMusicPlayer::WorkerMain(std::wstring file_path, uint64_t play_token) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (log_fn_) {
        log_fn_(std::wstring(L"\u8BED\u97F3\u70B9\u6B4C: \u5F00\u59CB\u64AD\u653E\u300C") + FileNameFromPath(file_path) + L"\u300D\u3002");
    }

    std::wstring detail;
    bool success = PlayFileWithCustomPlayer(file_path, play_token, &detail);
    if (!success && !IsStopRequested(play_token) && log_fn_) {
        const std::wstring reason = detail.empty() ? L"\u672A\u77E5\u9519\u8BEF" : detail;
        log_fn_(std::wstring(L"\u8BED\u97F3\u70B9\u6B4C: \u64AD\u653E\u5931\u8D25\uFF0C") + reason);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (play_token == play_token_) {
            running_ = false;
            current_file_.clear();
        }
    }

    NotifyStateChanged(false, file_path);

    CoUninitialize();
}

bool LocalMusicPlayer::PlayFileWithCustomPlayer(const std::wstring& file_path, uint64_t play_token, std::wstring* detail) {
    if (detail != nullptr) {
        detail->clear();
    }

    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(file_path.c_str(), nullptr, &reader);
    if (FAILED(hr)) {
        if (detail != nullptr) {
            *detail = std::wstring(L"\u81EA\u7814\u64AD\u653E\u5668\u6253\u5F00\u97F3\u9891\u6587\u4EF6\u5931\u8D25\uFF1A") + HrToText(hr);
        }
        return false;
    }

    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    ComPtr<IMFMediaType> output_type;
    hr = MFCreateMediaType(&output_type);
    if (FAILED(hr)) {
        if (detail != nullptr) {
            *detail = std::wstring(L"\u81EA\u7814\u64AD\u653E\u5668\u521B\u5EFA PCM \u8F93\u51FA\u7C7B\u578B\u5931\u8D25\uFF1A") + HrToText(hr);
        }
        return false;
    }

    output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    output_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    output_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);

    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, output_type.Get());
    if (FAILED(hr)) {
        if (detail != nullptr) {
            *detail = std::wstring(L"\u81EA\u7814\u64AD\u653E\u5668\u8F6C PCM \u5931\u8D25\uFF1A") + HrToText(hr);
        }
        return false;
    }

    ComPtr<IMFMediaType> current_type;
    hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &current_type);
    if (FAILED(hr)) {
        if (detail != nullptr) {
            *detail = std::wstring(L"\u81EA\u7814\u64AD\u653E\u5668\u8BFB\u53D6 PCM \u683C\u5F0F\u5931\u8D25\uFF1A") + HrToText(hr);
            return false;
        }
        return false;
    }

    UINT32 sample_rate = 0;
    UINT32 channels = 0;
    hr = current_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sample_rate);
    if (FAILED(hr)) {
        sample_rate = 44100;
    }
    hr = current_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    if (FAILED(hr)) {
        channels = 2;
    }

    std::vector<uint8_t> pcm_bytes;
    pcm_bytes.reserve(1 << 20);

    for (;;) {
        if (IsStopRequested(play_token)) {
            return false;
        }

        DWORD stream_flags = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            0,
            nullptr,
            &stream_flags,
            nullptr,
            &sample);
        if (FAILED(hr)) {
            if (detail != nullptr) {
                *detail = std::wstring(L"\u81EA\u7814\u64AD\u653E\u5668\u8BFB\u53D6\u97F3\u9891\u6D41\u5931\u8D25\uFF1A") + HrToText(hr);
            }
            return false;
        }

        if ((stream_flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            break;
        }
        if (sample == nullptr) {
            continue;
        }

        ComPtr<IMFMediaBuffer> media_buffer;
        hr = sample->ConvertToContiguousBuffer(&media_buffer);
        if (FAILED(hr) || media_buffer == nullptr) {
            continue;
        }

        BYTE* buffer_data = nullptr;
        DWORD current_length = 0;
        hr = media_buffer->Lock(&buffer_data, nullptr, &current_length);
        if (FAILED(hr) || buffer_data == nullptr || current_length == 0) {
            if (SUCCEEDED(hr)) {
                media_buffer->Unlock();
            }
            continue;
        }

        const size_t previous_size = pcm_bytes.size();
        pcm_bytes.resize(previous_size + current_length);
        memcpy(pcm_bytes.data() + previous_size, buffer_data, current_length);
        media_buffer->Unlock();
    }

    if (pcm_bytes.empty()) {
        if (detail != nullptr) {
            *detail = L"\u81EA\u7814\u64AD\u653E\u5668\u672A\u89E3\u7801\u5230\u53EF\u64AD\u653E\u7684 PCM \u6570\u636E";
        }
        return false;
    }

    ComPtr<IXAudio2> xaudio;
    hr = XAudio2Create(xaudio.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        if (detail != nullptr) {
            *detail = std::wstring(L"\u81EA\u7814 XAudio2 \u5F15\u64CE\u521D\u59CB\u5316\u5931\u8D25\uFF1A") + HrToText(hr);
        }
        return false;
    }

    IXAudio2MasteringVoice* mastering_voice = nullptr;
    hr = xaudio->CreateMasteringVoice(&mastering_voice);
    if (FAILED(hr) || mastering_voice == nullptr) {
        if (detail != nullptr) {
            *detail = std::wstring(L"\u81EA\u7814\u64AD\u653E\u5668\u521B\u5EFA\u4E3B\u8F93\u51FA\u5931\u8D25\uFF1A") + HrToText(hr);
        }
        return false;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(channels);
    format.nSamplesPerSec = sample_rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    IXAudio2SourceVoice* source_voice = nullptr;
    hr = xaudio->CreateSourceVoice(&source_voice, &format);
    if (FAILED(hr) || source_voice == nullptr) {
        mastering_voice->DestroyVoice();
        if (detail != nullptr) {
            *detail = std::wstring(L"\u81EA\u7814\u64AD\u653E\u5668\u521B\u5EFA\u97F3\u9891\u58F0\u9053\u5931\u8D25\uFF1A") + HrToText(hr);
        }
        return false;
    }

    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(pcm_bytes.size());
    buffer.pAudioData = pcm_bytes.data();
    buffer.Flags = XAUDIO2_END_OF_STREAM;

    hr = source_voice->SubmitSourceBuffer(&buffer);
    if (FAILED(hr)) {
        source_voice->DestroyVoice();
        mastering_voice->DestroyVoice();
        if (detail != nullptr) {
            *detail = std::wstring(L"\u81EA\u7814\u64AD\u653E\u5668\u63D0\u4EA4 PCM \u7F13\u51B2\u5931\u8D25\uFF1A") + HrToText(hr);
        }
        return false;
    }

    hr = source_voice->Start(0);
    if (FAILED(hr)) {
        source_voice->DestroyVoice();
        mastering_voice->DestroyVoice();
        if (detail != nullptr) {
            *detail = std::wstring(L"\u81EA\u7814\u64AD\u653E\u5668\u542F\u52A8\u58F0\u9053\u5931\u8D25\uFF1A") + HrToText(hr);
        }
        return false;
    }

    if (log_fn_) {
        std::wostringstream stream;
        stream << L"\u8BED\u97F3\u70B9\u6B4C: \u5DF2\u7531\u81EA\u7814 XAudio2 \u64AD\u653E\u5668\u63A5\u7BA1\u300C"
               << FileNameFromPath(file_path) << L"\u300D"
               << L" (" << sample_rate << L"Hz/" << channels << L"ch, "
               << pcm_bytes.size() << L" bytes PCM)\u3002";
        log_fn_(stream.str());
    }

    for (;;) {
        if (IsStopRequested(play_token)) {
            source_voice->Stop(0);
            source_voice->FlushSourceBuffers();
            source_voice->DestroyVoice();
            mastering_voice->DestroyVoice();
            return false;
        }

        XAUDIO2_VOICE_STATE state{};
        source_voice->GetState(&state);
        if (state.BuffersQueued == 0) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    source_voice->Stop(0);
    source_voice->DestroyVoice();
    mastering_voice->DestroyVoice();
    return true;
}

void LocalMusicPlayer::NotifyStateChanged(bool running, const std::wstring& file_path) const {
    if (state_changed_fn_) {
        state_changed_fn_(running, file_path);
    }
}

bool LocalMusicPlayer::IsStopRequested(uint64_t play_token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_requested_ || play_token != play_token_;
}

std::wstring LocalMusicPlayer::FileNameFromPath(const std::wstring& file_path) {
    const size_t slash = file_path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? file_path : file_path.substr(slash + 1);
}
