#pragma once

#include <Windows.h>

#include <functional>
#include <mutex>
#include <string>

class AirPlayReceiverController {
public:
    using LogFn = std::function<void(const std::wstring&)>;

    explicit AirPlayReceiverController(LogFn log_fn = {});
    ~AirPlayReceiverController();

    void SetLogFn(LogFn log_fn);
    void SetRuntimeRoot(const std::wstring& runtime_root);
    void SetLogFilePath(const std::wstring& log_file_path);
    void RefreshInstallState();
    bool Start(const std::wstring& server_name);
    void Stop();
    void Poll();

    bool installed() const;
    bool running() const;
    bool bonjour_installed() const;
    bool bonjour_running() const;
    std::wstring runtime_root() const;
    std::wstring binary_path() const;
    std::wstring log_file_path() const;
    std::wstring last_error() const;
    DWORD last_exit_code() const;

private:
    void RefreshBonjourStateLocked() const;
    void PollLocked() const;
    void ClearProcessHandlesLocked() const;
    bool EnsureBonjourServiceRunningLocked(std::wstring* error_text);
    std::wstring BuildBinaryPathLocked() const;

    mutable std::mutex mutex_;
    LogFn log_fn_;
    std::wstring runtime_root_;
    std::wstring log_file_path_;
    mutable std::wstring last_error_;
    mutable DWORD last_exit_code_ = 0;
    mutable bool installed_ = false;
    mutable bool running_ = false;
    mutable bool bonjour_installed_ = false;
    mutable bool bonjour_running_ = false;
    mutable HANDLE process_handle_ = nullptr;
    mutable HANDLE log_handle_ = nullptr;
    mutable DWORD process_id_ = 0;
};
