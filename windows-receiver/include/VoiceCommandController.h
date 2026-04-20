#pragma once

#include <Windows.h>
#include <mmsystem.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class VoiceCommandController {
public:
    struct Config {
        std::wstring runtime_directory;
        std::wstring model_directory;
        std::vector<std::wstring> grammar_phrases;
        std::vector<std::wstring> hotwords_phrases;
        float hotwords_score = 1.5f;
    };

    using LogFn = std::function<void(const std::wstring&)>;
    using PhraseCallback = std::function<void(const std::wstring&, bool is_final)>;

    explicit VoiceCommandController(LogFn log_fn);
    ~VoiceCommandController();

    bool Start(const Config& config, PhraseCallback phrase_callback);
    void Stop();
    bool running() const;
    void SuppressFor(DWORD duration_ms);

private:
    struct Buffer {
        WAVEHDR header{};
        std::vector<uint8_t> bytes;
    };

    struct RuntimeApi;

    static void CALLBACK WaveInCallback(
        HWAVEIN wave_in,
        UINT message,
        DWORD_PTR instance,
        DWORD_PTR param1,
        DWORD_PTR param2);

    bool LoadRuntime(const std::wstring& runtime_directory);
    void UnloadRuntime();
    bool CreateRecognizer(const Config& config);
    void DestroyRecognizer();
    bool OpenCaptureDevice();
    void CloseCaptureDevice();
    void WorkerLoop();
    void HandleWaveInData(WAVEHDR* header);
    void ProcessAudioChunk(const std::vector<uint8_t>& chunk);
    void EmitPhraseIfNeeded(const std::wstring& phrase, bool is_final);
    void ResetRecognizerLocked();

    LogFn log_fn_;
    PhraseCallback phrase_callback_;
    mutable std::mutex mutex_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::vector<uint8_t>> pending_audio_;
    std::thread worker_thread_;
    std::unique_ptr<RuntimeApi> runtime_api_;
    std::vector<std::unique_ptr<Buffer>> buffers_;
    HMODULE runtime_module_ = nullptr;
    void* recognizer_handle_ = nullptr;
    void* stream_handle_ = nullptr;
    HWAVEIN wave_in_ = nullptr;
    bool running_ = false;
    std::atomic<bool> stop_requested_{false};
    std::wstring last_emitted_phrase_;
    ULONGLONG last_emitted_tick_ = 0;
    ULONGLONG suppress_until_tick_ = 0;
    std::vector<std::wstring> grammar_phrases_;
    std::vector<std::wstring> hotwords_phrases_;
    std::string hotwords_buffer_utf8_;
};
