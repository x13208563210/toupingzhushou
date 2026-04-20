#pragma once

#include <Windows.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct IMFVirtualCamera;

class VirtualCameraController {
public:
    using LogFn = std::function<void(const std::wstring&)>;
    using StateChangedFn = std::function<void()>;

    VirtualCameraController();
    ~VirtualCameraController();

    void SetLogFn(LogFn log_fn);
    void SetStateChangedFn(StateChangedFn state_changed_fn);

    bool RefreshInstallState(const std::wstring& media_source_path);
    bool Start();
    void Stop();

    bool installed() const;
    bool starting() const;
    bool running() const;
    std::wstring installed_path() const;
    std::wstring install_warning() const;
    std::wstring last_error() const;

private:
    void NotifyStateChanged() const;
    void Log(const std::wstring& message) const;
    void WorkerMain();
    std::wstring FormatHr(HRESULT hr) const;

    mutable std::mutex state_mutex_;
    mutable std::mutex callback_mutex_;
    std::condition_variable worker_cv_;
    std::thread worker_thread_;
    bool worker_started_ = false;
    bool shutdown_requested_ = false;
    bool desired_running_ = false;
    bool installed_ = false;
    bool starting_ = false;
    bool running_ = false;
    std::wstring installed_path_;
    std::wstring install_warning_;
    std::wstring last_error_;
    LogFn log_fn_;
    StateChangedFn state_changed_fn_;
};
