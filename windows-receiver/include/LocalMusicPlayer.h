#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <thread>

class LocalMusicPlayer {
public:
    using LogFn = std::function<void(const std::wstring&)>;
    using StateChangedFn = std::function<void(bool, const std::wstring&)>;

    explicit LocalMusicPlayer(LogFn log_fn, StateChangedFn state_changed_fn = {});
    ~LocalMusicPlayer();

    bool PlayFile(const std::wstring& file_path, std::wstring* detail);
    void Stop();
    bool running() const;
    std::wstring current_file() const;

private:
    void WorkerMain(std::wstring file_path, uint64_t play_token);
    bool PlayFileWithCustomPlayer(const std::wstring& file_path, uint64_t play_token, std::wstring* detail);
    bool IsStopRequested(uint64_t play_token) const;
    void NotifyStateChanged(bool running, const std::wstring& file_path) const;
    static std::wstring FileNameFromPath(const std::wstring& file_path);

    LogFn log_fn_;
    StateChangedFn state_changed_fn_;
    mutable std::mutex mutex_;
    std::thread worker_thread_;
    bool running_ = false;
    bool stop_requested_ = false;
    uint64_t play_token_ = 0;
    std::wstring current_file_;
};
