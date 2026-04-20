#include "VirtualCameraController.h"

#include "VirtualCameraShared.h"

#include <Windows.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>

#include <sstream>

namespace {

std::wstring QueryInstalledMediaSourcePath() {
    HKEY key = nullptr;
    const std::wstring registry_path =
        std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + virtual_camera::kMediaSourceClsidString + L"\\InprocServer32";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, registry_path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }

    DWORD value_type = 0;
    DWORD byte_count = 0;
    if (RegQueryValueExW(key, nullptr, nullptr, &value_type, nullptr, &byte_count) != ERROR_SUCCESS ||
        value_type != REG_SZ ||
        byte_count == 0) {
        RegCloseKey(key);
        return {};
    }

    std::wstring value(byte_count / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(
            key,
            nullptr,
            nullptr,
            &value_type,
            reinterpret_cast<LPBYTE>(value.data()),
            &byte_count) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return {};
    }
    RegCloseKey(key);

    const size_t end = value.find(L'\0');
    if (end != std::wstring::npos) {
        value.resize(end);
    }
    return value;
}

void DestroyVirtualCamera(IMFVirtualCamera*& camera) {
    if (camera != nullptr) {
        camera->Stop();
        camera->Shutdown();
        camera->Release();
        camera = nullptr;
    }
}

}  // namespace

VirtualCameraController::VirtualCameraController() = default;

VirtualCameraController::~VirtualCameraController() {
    Stop();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        shutdown_requested_ = true;
    }
    worker_cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void VirtualCameraController::SetLogFn(LogFn log_fn) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    log_fn_ = std::move(log_fn);
}

void VirtualCameraController::SetStateChangedFn(StateChangedFn state_changed_fn) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    state_changed_fn_ = std::move(state_changed_fn);
}

bool VirtualCameraController::installed() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return installed_;
}

bool VirtualCameraController::starting() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return starting_;
}

bool VirtualCameraController::running() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return running_;
}

std::wstring VirtualCameraController::installed_path() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return installed_path_;
}

std::wstring VirtualCameraController::install_warning() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return install_warning_;
}

std::wstring VirtualCameraController::last_error() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_error_;
}

bool VirtualCameraController::RefreshInstallState(const std::wstring& media_source_path) {
    std::wstring installed_path = QueryInstalledMediaSourcePath();
    bool installed = !installed_path.empty();
    std::wstring install_warning;

    if (!installed) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        installed_path_ = std::move(installed_path);
        installed_ = false;
        install_warning_.clear();
        return false;
    }

    const DWORD attributes = GetFileAttributesW(installed_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        installed = false;
        install_warning =
            std::wstring(L"\u865A\u62DF\u6444\u50CF\u5934 DLL \u5DF2\u6CE8\u518C\uFF0C\u4F46\u6587\u4EF6\u4E0D\u5B58\u5728\uFF1A") + installed_path;
        std::lock_guard<std::mutex> lock(state_mutex_);
        installed_path_ = std::move(installed_path);
        installed_ = false;
        install_warning_ = std::move(install_warning);
        return false;
    }

    if (!media_source_path.empty() && _wcsicmp(installed_path.c_str(), media_source_path.c_str()) != 0) {
        install_warning =
            std::wstring(L"\u5F53\u524D\u6CE8\u518C\u7684\u865A\u62DF\u6444\u50CF\u5934 DLL \u4E0D\u5728\u8FD0\u884C\u76EE\u5F55\u4E2D\uFF1A") + installed_path;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        installed_path_ = std::move(installed_path);
        installed_ = installed;
        install_warning_ = std::move(install_warning);
    }
    return installed;
}

bool VirtualCameraController::Start() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (running_ || starting_) {
            return true;
        }
        if (!installed_) {
            last_error_ = L"\u865A\u62DF\u6444\u50CF\u5934\u7EC4\u4EF6\u5C1A\u672A\u5B89\u88C5\u3002";
            return false;
        }

        last_error_.clear();
        desired_running_ = true;
        starting_ = true;

        if (!worker_started_) {
            try {
                worker_thread_ = std::thread(&VirtualCameraController::WorkerMain, this);
                worker_started_ = true;
            } catch (...) {
                desired_running_ = false;
                starting_ = false;
                last_error_ = L"\u865A\u62DF\u6444\u50CF\u5934\u542F\u52A8\u7EBF\u7A0B\u521B\u5EFA\u5931\u8D25\u3002";
                return false;
            }
        }
    }

    worker_cv_.notify_all();
    return true;
}

void VirtualCameraController::Stop() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        desired_running_ = false;
        if (starting_) {
            starting_ = false;
        }
    }
    worker_cv_.notify_all();
}

void VirtualCameraController::NotifyStateChanged() const {
    StateChangedFn state_changed_fn;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        state_changed_fn = state_changed_fn_;
    }
    if (state_changed_fn) {
        state_changed_fn();
    }
}

void VirtualCameraController::Log(const std::wstring& message) const {
    LogFn log_fn;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        log_fn = log_fn_;
    }
    if (log_fn) {
        log_fn(message);
    }
}

void VirtualCameraController::WorkerMain() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_initialized = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_error_ = std::wstring(L"\u865A\u62DF\u6444\u50CF\u5934 COM \u521D\u59CB\u5316\u5931\u8D25\uFF0C\u9519\u8BEF\u7801 ") + FormatHr(hr);
            desired_running_ = false;
            starting_ = false;
            running_ = false;
        }
        Log(std::wstring(L"\u865A\u62DF\u6444\u50CF\u5934: ") + last_error());
        NotifyStateChanged();
        return;
    }

    hr = MFStartup(MF_VERSION);
    const bool mf_started = SUCCEEDED(hr);
    if (!mf_started) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_error_ = std::wstring(L"\u865A\u62DF\u6444\u50CF\u5934 Media Foundation \u521D\u59CB\u5316\u5931\u8D25\uFF0C\u9519\u8BEF\u7801 ") + FormatHr(hr);
            desired_running_ = false;
            starting_ = false;
            running_ = false;
        }
        Log(std::wstring(L"\u865A\u62DF\u6444\u50CF\u5934: ") + last_error());
        NotifyStateChanged();
        if (co_initialized) {
            CoUninitialize();
        }
        return;
    }

    IMFVirtualCamera* camera = nullptr;
    std::unique_lock<std::mutex> lock(state_mutex_);
    while (!shutdown_requested_) {
        worker_cv_.wait(lock, [this]() { return shutdown_requested_ || desired_running_ != running_; });
        if (shutdown_requested_) {
            break;
        }

        const bool should_run = desired_running_;
        lock.unlock();

        if (should_run) {
            IMFVirtualCamera* new_camera = nullptr;
            HRESULT start_hr = MFCreateVirtualCamera(
                MFVirtualCameraType_SoftwareCameraSource,
                MFVirtualCameraLifetime_Session,
                MFVirtualCameraAccess_CurrentUser,
                virtual_camera::kFriendlyName,
                virtual_camera::kMediaSourceClsidString,
                nullptr,
                0,
                &new_camera);
            if (SUCCEEDED(start_hr)) {
                start_hr = new_camera->Start(nullptr);
            }

            std::wstring error_text;
            if (FAILED(start_hr)) {
                error_text = std::wstring(L"\u542F\u52A8\u865A\u62DF\u6444\u50CF\u5934\u5931\u8D25\uFF0C\u9519\u8BEF\u7801 ") + FormatHr(start_hr);
                if (new_camera != nullptr) {
                    new_camera->Shutdown();
                    new_camera->Release();
                    new_camera = nullptr;
                }
            }

            bool notify_state_changed = false;
            bool log_started = false;
            bool log_error = false;
            lock.lock();
            if (shutdown_requested_ || !desired_running_) {
                if (new_camera != nullptr) {
                    lock.unlock();
                    DestroyVirtualCamera(new_camera);
                    lock.lock();
                }
                starting_ = false;
                running_ = false;
                notify_state_changed = true;
            } else if (FAILED(start_hr)) {
                last_error_ = error_text;
                desired_running_ = false;
                starting_ = false;
                running_ = false;
                notify_state_changed = true;
                log_error = true;
            } else {
                camera = new_camera;
                last_error_.clear();
                starting_ = false;
                running_ = true;
                notify_state_changed = true;
                log_started = true;
            }
            lock.unlock();

            if (log_error) {
                Log(std::wstring(L"\u865A\u62DF\u6444\u50CF\u5934: ") + error_text);
            } else if (log_started) {
                Log(
                    L"\u865A\u62DF\u6444\u50CF\u5934: \u5DF2\u542F\u52A8\uFF0COBS\u3001\u4F1A\u8BAE\u8F6F\u4EF6\u6216\u5176\u4ED6\u652F\u6301\u6444\u50CF\u5934\u7684\u5E94\u7528\u53EF\u4EE5\u9009\u62E9\u201C\u76F4\u64AD\u6295\u5C4F\u52A9\u624B\u865A\u62DF\u6444\u50CF\u5934\u201D\u3002");
            }
            if (notify_state_changed) {
                NotifyStateChanged();
            }
            lock.lock();
            continue;
        }

        const bool was_running = running_;
        lock.unlock();
        DestroyVirtualCamera(camera);
        lock.lock();
        running_ = false;
        starting_ = false;
        if (was_running) {
            lock.unlock();
            NotifyStateChanged();
            lock.lock();
        }
    }

    lock.unlock();
    DestroyVirtualCamera(camera);
    lock.lock();
    const bool notify_state_changed = running_ || starting_;
    running_ = false;
    starting_ = false;
    desired_running_ = false;
    lock.unlock();

    if (notify_state_changed) {
        NotifyStateChanged();
    }
    if (mf_started) {
        MFShutdown();
    }
    if (co_initialized) {
        CoUninitialize();
    }
}

std::wstring VirtualCameraController::FormatHr(HRESULT hr) const {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase << hr;
    return stream.str();
}
