#include "VoiceCommandController.h"

#include <algorithm>
#include <cwctype>
#include <sstream>
#include <unordered_set>

#include "sherpa-onnx/c-api/c-api.h"

namespace {

constexpr uint32_t kCaptureSampleRate = 16000;
constexpr uint16_t kCaptureBitsPerSample = 16;
constexpr uint16_t kCaptureChannels = 1;
constexpr size_t kCaptureBufferBytes = 2048;
constexpr size_t kCaptureBufferCount = 4;
constexpr ULONGLONG kRepeatedPhraseSuppressMs = 900;
constexpr int32_t kRecognizerNumThreads = 2;
constexpr float kEndpointRule1TrailingSilenceSeconds = 0.35f;
constexpr float kEndpointRule2TrailingSilenceSeconds = 0.60f;
constexpr float kEndpointRule3MinUtteranceSeconds = 12.0f;

std::wstring WaveInErrorText(MMRESULT result) {
    wchar_t buffer[MAXERRORLENGTH] = {};
    if (waveInGetErrorTextW(result, buffer, MAXERRORLENGTH) == MMSYSERR_NOERROR) {
        return buffer;
    }

    std::wostringstream stream;
    stream << L"MMRESULT=" << result;
    return stream.str();
}

std::wstring WideFromUtf8(const std::string& value) {
    if (value.empty()) {
        return L"";
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return L"";
    }

    std::wstring converted(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), converted.data(), length);
    return converted;
}

std::wstring WideFromUtf8(const char* value) {
    return value == nullptr ? L"" : WideFromUtf8(std::string(value));
}

std::string Utf8FromWide(const std::wstring& value) {
    if (value.empty()) {
        return "";
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return "";
    }

    std::string converted(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), converted.data(), length, nullptr, nullptr);
    return converted;
}

std::wstring NormalizePhrase(const std::wstring& phrase) {
    std::wstring normalized;
    normalized.reserve(phrase.size());
    for (wchar_t ch : phrase) {
        if (std::iswspace(ch) || ch == L',' || ch == L'.' || ch == L'!' || ch == L'?' || ch == L'"' || ch == L'\'') {
            continue;
        }
        normalized.push_back(ch);
    }
    return normalized;
}

std::wstring BuildModelFilePath(const std::wstring& model_directory, const wchar_t* file_name) {
    if (model_directory.empty() || file_name == nullptr || *file_name == L'\0') {
        return L"";
    }

    return model_directory + L"\\" + file_name;
}

void AppendNormalizedPhrase(
    const std::wstring& phrase,
    std::unordered_set<std::wstring>* dedup,
    std::vector<std::wstring>* output) {
    if (dedup == nullptr || output == nullptr) {
        return;
    }

    const std::wstring normalized = NormalizePhrase(phrase);
    if (normalized.empty()) {
        return;
    }
    if (dedup->insert(normalized).second) {
        output->push_back(normalized);
    }
}

std::string BuildHotwordsBuffer(const std::vector<std::wstring>& phrases) {
    std::string buffer;
    bool first = true;
    for (const auto& phrase : phrases) {
        const std::string phrase_utf8 = Utf8FromWide(phrase);
        if (phrase_utf8.empty()) {
            continue;
        }
        if (!first) {
            buffer.push_back('\n');
        }
        buffer += phrase_utf8;
        first = false;
    }
    return buffer;
}

}  // namespace

struct VoiceCommandController::RuntimeApi {
    using GetVersionStrFn = const char* (*)();
    using CreateOnlineRecognizerFn =
        const SherpaOnnxOnlineRecognizer* (*)(const SherpaOnnxOnlineRecognizerConfig*);
    using DestroyOnlineRecognizerFn = void (*)(const SherpaOnnxOnlineRecognizer*);
    using CreateOnlineStreamFn = const SherpaOnnxOnlineStream* (*)(const SherpaOnnxOnlineRecognizer*);
    using DestroyOnlineStreamFn = void (*)(const SherpaOnnxOnlineStream*);
    using OnlineStreamAcceptWaveformFn =
        void (*)(const SherpaOnnxOnlineStream*, int32_t, const float*, int32_t);
    using IsOnlineStreamReadyFn =
        int32_t (*)(const SherpaOnnxOnlineRecognizer*, const SherpaOnnxOnlineStream*);
    using DecodeOnlineStreamFn =
        void (*)(const SherpaOnnxOnlineRecognizer*, const SherpaOnnxOnlineStream*);
    using GetOnlineStreamResultFn =
        const SherpaOnnxOnlineRecognizerResult* (*)(
            const SherpaOnnxOnlineRecognizer*,
            const SherpaOnnxOnlineStream*);
    using DestroyOnlineRecognizerResultFn = void (*)(const SherpaOnnxOnlineRecognizerResult*);
    using OnlineStreamResetFn =
        void (*)(const SherpaOnnxOnlineRecognizer*, const SherpaOnnxOnlineStream*);
    using OnlineStreamIsEndpointFn =
        int32_t (*)(const SherpaOnnxOnlineRecognizer*, const SherpaOnnxOnlineStream*);

    GetVersionStrFn get_version_str = nullptr;
    CreateOnlineRecognizerFn create_online_recognizer = nullptr;
    DestroyOnlineRecognizerFn destroy_online_recognizer = nullptr;
    CreateOnlineStreamFn create_online_stream = nullptr;
    DestroyOnlineStreamFn destroy_online_stream = nullptr;
    OnlineStreamAcceptWaveformFn online_stream_accept_waveform = nullptr;
    IsOnlineStreamReadyFn is_online_stream_ready = nullptr;
    DecodeOnlineStreamFn decode_online_stream = nullptr;
    GetOnlineStreamResultFn get_online_stream_result = nullptr;
    DestroyOnlineRecognizerResultFn destroy_online_recognizer_result = nullptr;
    OnlineStreamResetFn online_stream_reset = nullptr;
    OnlineStreamIsEndpointFn online_stream_is_endpoint = nullptr;
};

VoiceCommandController::VoiceCommandController(LogFn log_fn)
    : log_fn_(std::move(log_fn)) {}

VoiceCommandController::~VoiceCommandController() {
    Stop();
}

bool VoiceCommandController::Start(const Config& config, PhraseCallback phrase_callback) {
    if (config.runtime_directory.empty() || config.model_directory.empty()) {
        Stop();
        return false;
    }

    Stop();

    phrase_callback_ = std::move(phrase_callback);
    if (!LoadRuntime(config.runtime_directory)) {
        return false;
    }
    if (!CreateRecognizer(config)) {
        UnloadRuntime();
        return false;
    }
    if (!OpenCaptureDevice()) {
        DestroyRecognizer();
        UnloadRuntime();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = false;
        running_ = true;
        last_emitted_phrase_.clear();
        last_emitted_tick_ = 0;
        suppress_until_tick_ = 0;
    }

    worker_thread_ = std::thread(&VoiceCommandController::WorkerLoop, this);
    const MMRESULT start_result = waveInStart(wave_in_);
    if (start_result != MMSYSERR_NOERROR) {
        if (log_fn_) {
            log_fn_(L"\u8BED\u97F3\u63A7\u5236: \u542F\u52A8\u9EA6\u514B\u98CE\u5931\u8D25\uFF0C" + WaveInErrorText(start_result));
        }
        Stop();
        return false;
    }

    if (log_fn_) {
        log_fn_(L"\u8BED\u97F3\u63A7\u5236: \u5DF2\u542F\u52A8 sherpa-onnx \u79BB\u7EBF\u8BED\u97F3\u8BC6\u522B\uFF0C\u7531\u63A5\u6536\u7AEF\u81EA\u5DF1\u76D1\u542C\u9EA6\u514B\u98CE\u3002");
    }
    return true;
}

void VoiceCommandController::Stop() {
    HWAVEIN wave_in = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wave_in = wave_in_;
        stop_requested_ = true;
        running_ = false;
    }

    if (wave_in != nullptr) {
        waveInStop(wave_in);
        waveInReset(wave_in);
    }

    queue_cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    CloseCaptureDevice();
    DestroyRecognizer();
    UnloadRuntime();

    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        pending_audio_.clear();
    }

    phrase_callback_ = nullptr;
}

bool VoiceCommandController::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

void VoiceCommandController::SuppressFor(DWORD duration_ms) {
    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        suppress_until_tick_ = std::max(suppress_until_tick_, now + duration_ms);
        last_emitted_phrase_.clear();
        last_emitted_tick_ = 0;
        ResetRecognizerLocked();
    }
    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        pending_audio_.clear();
    }
}

void CALLBACK VoiceCommandController::WaveInCallback(
    HWAVEIN,
    UINT message,
    DWORD_PTR instance,
    DWORD_PTR param1,
    DWORD_PTR) {
    auto* controller = reinterpret_cast<VoiceCommandController*>(instance);
    if (controller == nullptr) {
        return;
    }

    if (message == WIM_DATA) {
        controller->HandleWaveInData(reinterpret_cast<WAVEHDR*>(param1));
    }
}

bool VoiceCommandController::LoadRuntime(const std::wstring& runtime_directory) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::wstring dll_path = runtime_directory + L"\\sherpa-onnx-c-api.dll";
    runtime_module_ = LoadLibraryExW(dll_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (runtime_module_ == nullptr) {
        if (log_fn_) {
            log_fn_(L"\u8BED\u97F3\u63A7\u5236: \u52A0\u8F7D sherpa-onnx-c-api.dll \u5931\u8D25\u3002");
        }
        return false;
    }

    auto api = std::make_unique<RuntimeApi>();
    api->get_version_str =
        reinterpret_cast<RuntimeApi::GetVersionStrFn>(GetProcAddress(runtime_module_, "SherpaOnnxGetVersionStr"));
    api->create_online_recognizer = reinterpret_cast<RuntimeApi::CreateOnlineRecognizerFn>(
        GetProcAddress(runtime_module_, "SherpaOnnxCreateOnlineRecognizer"));
    api->destroy_online_recognizer = reinterpret_cast<RuntimeApi::DestroyOnlineRecognizerFn>(
        GetProcAddress(runtime_module_, "SherpaOnnxDestroyOnlineRecognizer"));
    api->create_online_stream = reinterpret_cast<RuntimeApi::CreateOnlineStreamFn>(
        GetProcAddress(runtime_module_, "SherpaOnnxCreateOnlineStream"));
    api->destroy_online_stream = reinterpret_cast<RuntimeApi::DestroyOnlineStreamFn>(
        GetProcAddress(runtime_module_, "SherpaOnnxDestroyOnlineStream"));
    api->online_stream_accept_waveform = reinterpret_cast<RuntimeApi::OnlineStreamAcceptWaveformFn>(
        GetProcAddress(runtime_module_, "SherpaOnnxOnlineStreamAcceptWaveform"));
    api->is_online_stream_ready = reinterpret_cast<RuntimeApi::IsOnlineStreamReadyFn>(
        GetProcAddress(runtime_module_, "SherpaOnnxIsOnlineStreamReady"));
    api->decode_online_stream = reinterpret_cast<RuntimeApi::DecodeOnlineStreamFn>(
        GetProcAddress(runtime_module_, "SherpaOnnxDecodeOnlineStream"));
    api->get_online_stream_result = reinterpret_cast<RuntimeApi::GetOnlineStreamResultFn>(
        GetProcAddress(runtime_module_, "SherpaOnnxGetOnlineStreamResult"));
    api->destroy_online_recognizer_result = reinterpret_cast<RuntimeApi::DestroyOnlineRecognizerResultFn>(
        GetProcAddress(runtime_module_, "SherpaOnnxDestroyOnlineRecognizerResult"));
    api->online_stream_reset = reinterpret_cast<RuntimeApi::OnlineStreamResetFn>(
        GetProcAddress(runtime_module_, "SherpaOnnxOnlineStreamReset"));
    api->online_stream_is_endpoint = reinterpret_cast<RuntimeApi::OnlineStreamIsEndpointFn>(
        GetProcAddress(runtime_module_, "SherpaOnnxOnlineStreamIsEndpoint"));

    if (api->create_online_recognizer == nullptr ||
        api->destroy_online_recognizer == nullptr ||
        api->create_online_stream == nullptr ||
        api->destroy_online_stream == nullptr ||
        api->online_stream_accept_waveform == nullptr ||
        api->is_online_stream_ready == nullptr ||
        api->decode_online_stream == nullptr ||
        api->get_online_stream_result == nullptr ||
        api->destroy_online_recognizer_result == nullptr ||
        api->online_stream_reset == nullptr ||
        api->online_stream_is_endpoint == nullptr) {
        if (log_fn_) {
            log_fn_(L"\u8BED\u97F3\u63A7\u5236: sherpa-onnx-c-api.dll \u7F3A\u5C11\u5FC5\u8981\u63A5\u53E3\u3002");
        }
        FreeLibrary(runtime_module_);
        runtime_module_ = nullptr;
        return false;
    }

    if (log_fn_ && api->get_version_str != nullptr) {
        log_fn_(L"\u8BED\u97F3\u63A7\u5236: \u5DF2\u52A0\u8F7D sherpa-onnx " +
                WideFromUtf8(api->get_version_str()) + L"\u3002");
    }

    runtime_api_ = std::move(api);
    return true;
}

void VoiceCommandController::UnloadRuntime() {
    std::lock_guard<std::mutex> lock(mutex_);
    runtime_api_.reset();
    if (runtime_module_ != nullptr) {
        FreeLibrary(runtime_module_);
        runtime_module_ = nullptr;
    }
}

bool VoiceCommandController::CreateRecognizer(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (runtime_api_ == nullptr) {
        return false;
    }

    std::unordered_set<std::wstring> dedup;
    grammar_phrases_.clear();
    grammar_phrases_.reserve(config.grammar_phrases.size());
    for (const auto& phrase : config.grammar_phrases) {
        AppendNormalizedPhrase(phrase, &dedup, &grammar_phrases_);
    }

    dedup.clear();
    hotwords_phrases_.clear();
    hotwords_phrases_.reserve(config.hotwords_phrases.size());
    for (const auto& phrase : config.hotwords_phrases) {
        AppendNormalizedPhrase(phrase, &dedup, &hotwords_phrases_);
    }
    hotwords_buffer_utf8_ = BuildHotwordsBuffer(hotwords_phrases_);

    const std::wstring encoder_path = BuildModelFilePath(config.model_directory, L"encoder.int8.onnx");
    const std::wstring decoder_path = BuildModelFilePath(config.model_directory, L"decoder.onnx");
    const std::wstring joiner_path = BuildModelFilePath(config.model_directory, L"joiner.int8.onnx");
    const std::wstring tokens_path = BuildModelFilePath(config.model_directory, L"tokens.txt");

    const std::string encoder_utf8 = Utf8FromWide(encoder_path);
    const std::string decoder_utf8 = Utf8FromWide(decoder_path);
    const std::string joiner_utf8 = Utf8FromWide(joiner_path);
    const std::string tokens_utf8 = Utf8FromWide(tokens_path);

    SherpaOnnxOnlineRecognizerConfig recognizer_config{};
    recognizer_config.feat_config.sample_rate = static_cast<int32_t>(kCaptureSampleRate);
    recognizer_config.feat_config.feature_dim = 80;
    recognizer_config.model_config.transducer.encoder = encoder_utf8.c_str();
    recognizer_config.model_config.transducer.decoder = decoder_utf8.c_str();
    recognizer_config.model_config.transducer.joiner = joiner_utf8.c_str();
    recognizer_config.model_config.tokens = tokens_utf8.c_str();
    recognizer_config.model_config.num_threads = kRecognizerNumThreads;
    recognizer_config.model_config.provider = "cpu";
    recognizer_config.model_config.debug = 0;
    recognizer_config.decoding_method =
        hotwords_buffer_utf8_.empty() ? "greedy_search" : "modified_beam_search";
    recognizer_config.max_active_paths = 4;
    recognizer_config.enable_endpoint = 1;
    recognizer_config.rule1_min_trailing_silence = kEndpointRule1TrailingSilenceSeconds;
    recognizer_config.rule2_min_trailing_silence = kEndpointRule2TrailingSilenceSeconds;
    recognizer_config.rule3_min_utterance_length = kEndpointRule3MinUtteranceSeconds;
    recognizer_config.hotwords_score = config.hotwords_score;
    if (!hotwords_buffer_utf8_.empty()) {
        recognizer_config.hotwords_buf = hotwords_buffer_utf8_.c_str();
        recognizer_config.hotwords_buf_size = static_cast<int32_t>(hotwords_buffer_utf8_.size());
    }

    recognizer_handle_ = const_cast<SherpaOnnxOnlineRecognizer*>(
        runtime_api_->create_online_recognizer(&recognizer_config));
    if (recognizer_handle_ == nullptr) {
        if (log_fn_) {
            log_fn_(L"\u8BED\u97F3\u63A7\u5236: \u521B\u5EFA sherpa-onnx \u8BC6\u522B\u5668\u5931\u8D25\u3002");
        }
        return false;
    }

    stream_handle_ = const_cast<SherpaOnnxOnlineStream*>(
        runtime_api_->create_online_stream(reinterpret_cast<const SherpaOnnxOnlineRecognizer*>(recognizer_handle_)));
    if (stream_handle_ == nullptr) {
        if (log_fn_) {
            log_fn_(L"\u8BED\u97F3\u63A7\u5236: \u521B\u5EFA sherpa-onnx \u8BED\u97F3\u6D41\u5931\u8D25\u3002");
        }
        runtime_api_->destroy_online_recognizer(
            reinterpret_cast<const SherpaOnnxOnlineRecognizer*>(recognizer_handle_));
        recognizer_handle_ = nullptr;
        return false;
    }

    if (log_fn_) {
        std::wostringstream stream;
        stream << L"\u8BED\u97F3\u63A7\u5236: \u8BC6\u522B\u5668\u5DF2\u521B\u5EFA\uFF0C\u89E3\u7801="
               << WideFromUtf8(recognizer_config.decoding_method)
               << L"\uFF0C\u77ED\u8BED\u6570=" << grammar_phrases_.size()
               << L"\uFF0C\u70ED\u8BCD\u6570=" << hotwords_phrases_.size();
        if (!hotwords_phrases_.empty()) {
            stream << L"\uFF0C\u70ED\u8BCD\u5206\u6570=" << config.hotwords_score;
        }
        log_fn_(stream.str());
    }

    ResetRecognizerLocked();
    return true;
}

void VoiceCommandController::DestroyRecognizer() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (runtime_api_ != nullptr && stream_handle_ != nullptr) {
        runtime_api_->destroy_online_stream(reinterpret_cast<const SherpaOnnxOnlineStream*>(stream_handle_));
        stream_handle_ = nullptr;
    }
    if (runtime_api_ != nullptr && recognizer_handle_ != nullptr) {
        runtime_api_->destroy_online_recognizer(
            reinterpret_cast<const SherpaOnnxOnlineRecognizer*>(recognizer_handle_));
        recognizer_handle_ = nullptr;
    }
    grammar_phrases_.clear();
    hotwords_phrases_.clear();
    hotwords_buffer_utf8_.clear();
}

void VoiceCommandController::ResetRecognizerLocked() {
    if (runtime_api_ != nullptr && recognizer_handle_ != nullptr && stream_handle_ != nullptr) {
        runtime_api_->online_stream_reset(
            reinterpret_cast<const SherpaOnnxOnlineRecognizer*>(recognizer_handle_),
            reinterpret_cast<const SherpaOnnxOnlineStream*>(stream_handle_));
    }
}

bool VoiceCommandController::OpenCaptureDevice() {
    std::lock_guard<std::mutex> lock(mutex_);

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kCaptureChannels;
    format.nSamplesPerSec = kCaptureSampleRate;
    format.wBitsPerSample = kCaptureBitsPerSample;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;

    HWAVEIN wave_in = nullptr;
    const MMRESULT open_result = waveInOpen(
        &wave_in,
        WAVE_MAPPER,
        &format,
        reinterpret_cast<DWORD_PTR>(&VoiceCommandController::WaveInCallback),
        reinterpret_cast<DWORD_PTR>(this),
        CALLBACK_FUNCTION);
    if (open_result != MMSYSERR_NOERROR) {
        if (log_fn_) {
            log_fn_(L"\u8BED\u97F3\u63A7\u5236: \u6253\u5F00\u9EA6\u514B\u98CE\u5931\u8D25\uFF0C" + WaveInErrorText(open_result));
        }
        return false;
    }

    buffers_.clear();
    buffers_.reserve(kCaptureBufferCount);
    for (size_t index = 0; index < kCaptureBufferCount; ++index) {
        auto buffer = std::make_unique<Buffer>();
        buffer->bytes.resize(kCaptureBufferBytes);
        buffer->header.lpData = reinterpret_cast<LPSTR>(buffer->bytes.data());
        buffer->header.dwBufferLength = static_cast<DWORD>(buffer->bytes.size());

        const MMRESULT prepare_result = waveInPrepareHeader(wave_in, &buffer->header, sizeof(WAVEHDR));
        if (prepare_result != MMSYSERR_NOERROR) {
            if (log_fn_) {
                log_fn_(L"\u8BED\u97F3\u63A7\u5236: \u9884\u5904\u7406\u9EA6\u514B\u98CE\u7F13\u51B2\u5931\u8D25\uFF0C" +
                        WaveInErrorText(prepare_result));
            }
            waveInClose(wave_in);
            return false;
        }

        const MMRESULT add_result = waveInAddBuffer(wave_in, &buffer->header, sizeof(WAVEHDR));
        if (add_result != MMSYSERR_NOERROR) {
            if (log_fn_) {
                log_fn_(L"\u8BED\u97F3\u63A7\u5236: \u63D0\u4EA4\u9EA6\u514B\u98CE\u7F13\u51B2\u5931\u8D25\uFF0C" +
                        WaveInErrorText(add_result));
            }
            waveInUnprepareHeader(wave_in, &buffer->header, sizeof(WAVEHDR));
            waveInClose(wave_in);
            return false;
        }

        buffers_.push_back(std::move(buffer));
    }

    wave_in_ = wave_in;
    return true;
}

void VoiceCommandController::CloseCaptureDevice() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (wave_in_ == nullptr) {
        buffers_.clear();
        return;
    }

    for (auto& buffer : buffers_) {
        if (buffer != nullptr) {
            waveInUnprepareHeader(wave_in_, &buffer->header, sizeof(WAVEHDR));
        }
    }
    waveInClose(wave_in_);
    wave_in_ = nullptr;
    buffers_.clear();
}

void VoiceCommandController::WorkerLoop() {
    for (;;) {
        std::vector<uint8_t> chunk;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() { return stop_requested_ || !pending_audio_.empty(); });
            if (stop_requested_ && pending_audio_.empty()) {
                break;
            }
            chunk = std::move(pending_audio_.front());
            pending_audio_.pop_front();
        }

        if (!chunk.empty()) {
            ProcessAudioChunk(chunk);
        }
    }
}

void VoiceCommandController::HandleWaveInData(WAVEHDR* header) {
    if (header == nullptr) {
        return;
    }

    bool requeue = false;
    HWAVEIN wave_in = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stop_requested_ && wave_in_ != nullptr) {
            requeue = true;
            wave_in = wave_in_;
        }
    }

    if (header->dwBytesRecorded > 0) {
        std::vector<uint8_t> chunk(
            reinterpret_cast<uint8_t*>(header->lpData),
            reinterpret_cast<uint8_t*>(header->lpData) + header->dwBytesRecorded);
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            pending_audio_.push_back(std::move(chunk));
        }
        queue_cv_.notify_one();
    }

    if (requeue) {
        header->dwBytesRecorded = 0;
        waveInAddBuffer(wave_in, header, sizeof(WAVEHDR));
    }
}

void VoiceCommandController::ProcessAudioChunk(const std::vector<uint8_t>& chunk) {
    RuntimeApi* api = nullptr;
    void* recognizer = nullptr;
    void* stream = nullptr;
    ULONGLONG suppress_until_tick = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        api = runtime_api_.get();
        recognizer = recognizer_handle_;
        stream = stream_handle_;
        suppress_until_tick = suppress_until_tick_;
    }
    if (api == nullptr || recognizer == nullptr || stream == nullptr || chunk.size() < sizeof(int16_t)) {
        return;
    }
    if (GetTickCount64() < suppress_until_tick) {
        return;
    }

    const size_t sample_count = chunk.size() / sizeof(int16_t);
    std::vector<float> samples(sample_count);
    const auto* pcm = reinterpret_cast<const int16_t*>(chunk.data());
    for (size_t index = 0; index < sample_count; ++index) {
        samples[index] = static_cast<float>(pcm[index]) / 32768.0f;
    }

    api->online_stream_accept_waveform(
        reinterpret_cast<const SherpaOnnxOnlineStream*>(stream),
        static_cast<int32_t>(kCaptureSampleRate),
        samples.data(),
        static_cast<int32_t>(samples.size()));

    while (api->is_online_stream_ready(
               reinterpret_cast<const SherpaOnnxOnlineRecognizer*>(recognizer),
               reinterpret_cast<const SherpaOnnxOnlineStream*>(stream))) {
        api->decode_online_stream(
            reinterpret_cast<const SherpaOnnxOnlineRecognizer*>(recognizer),
            reinterpret_cast<const SherpaOnnxOnlineStream*>(stream));
    }

    if (!api->online_stream_is_endpoint(
            reinterpret_cast<const SherpaOnnxOnlineRecognizer*>(recognizer),
            reinterpret_cast<const SherpaOnnxOnlineStream*>(stream))) {
        return;
    }

    std::wstring final_phrase;
    const SherpaOnnxOnlineRecognizerResult* result = api->get_online_stream_result(
        reinterpret_cast<const SherpaOnnxOnlineRecognizer*>(recognizer),
        reinterpret_cast<const SherpaOnnxOnlineStream*>(stream));
    if (result != nullptr) {
        if (result->text != nullptr) {
            final_phrase = NormalizePhrase(WideFromUtf8(result->text));
        }
        api->destroy_online_recognizer_result(result);
    }

    api->online_stream_reset(
        reinterpret_cast<const SherpaOnnxOnlineRecognizer*>(recognizer),
        reinterpret_cast<const SherpaOnnxOnlineStream*>(stream));

    if (!final_phrase.empty()) {
        EmitPhraseIfNeeded(final_phrase, true);
    }
}

void VoiceCommandController::EmitPhraseIfNeeded(const std::wstring& phrase, bool is_final) {
    if (phrase.empty() || phrase_callback_ == nullptr) {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!is_final && phrase == last_emitted_phrase_ && now - last_emitted_tick_ < kRepeatedPhraseSuppressMs) {
            return;
        }
        last_emitted_phrase_ = phrase;
        last_emitted_tick_ = now;
    }

    phrase_callback_(phrase, is_final);
}
