#include "AirPlayReceiverController.h"

#include <WinSock2.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <map>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kBonjourServiceName[] = L"Bonjour Service";

bool FileExists(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }

    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring DirectoryName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    return path.substr(0, slash);
}

std::wstring QuoteArg(const std::wstring& value) {
    std::wstring quoted = L"\"";
    for (const wchar_t ch : value) {
        if (ch == L'"') {
            quoted += L"\\\"";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted += L"\"";
    return quoted;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    UINT code_page = CP_UTF8;
    if (size <= 0) {
        code_page = CP_ACP;
        size = MultiByteToWideChar(code_page, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    }
    if (size <= 0) {
        return std::wstring(value.begin(), value.end());
    }

    std::wstring result(size, L'\0');
    MultiByteToWideChar(code_page, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

struct CaseInsensitiveLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const {
        return std::lexicographical_compare(
            left.begin(),
            left.end(),
            right.begin(),
            right.end(),
            [](wchar_t lhs, wchar_t rhs) {
                return std::towlower(lhs) < std::towlower(rhs);
            });
    }
};

std::vector<wchar_t> BuildEnvironmentBlock(const std::wstring& runtime_root) {
    std::map<std::wstring, std::wstring, CaseInsensitiveLess> variables;
    LPWCH raw_environment = GetEnvironmentStringsW();
    if (raw_environment != nullptr) {
        for (LPWCH cursor = raw_environment; *cursor != L'\0'; cursor += wcslen(cursor) + 1) {
            std::wstring entry(cursor);
            const size_t equals = entry.find(L'=');
            if (equals == std::wstring::npos || equals == 0) {
                continue;
            }
            variables[entry.substr(0, equals)] = entry.substr(equals + 1);
        }
        FreeEnvironmentStringsW(raw_environment);
    }

    const std::wstring bin_dir = runtime_root + L"\\_internal\\bin";
    const std::wstring internal_dir = runtime_root + L"\\_internal";
    const std::wstring plugin_dir = runtime_root + L"\\_internal\\lib\\gstreamer-1.0";
    const std::wstring current_path = variables.count(L"PATH") > 0 ? variables[L"PATH"] : std::wstring();

    variables[L"PATH"] = bin_dir + L";" + internal_dir + (current_path.empty() ? L"" : L";" + current_path);
    variables[L"GST_PLUGIN_PATH_1_0"] = plugin_dir;
    variables[L"GST_PLUGIN_SYSTEM_PATH_1_0"] = plugin_dir;
    variables[L"GST_REGISTRY_1_0"] = runtime_root + L"\\_internal\\gst-registry.bin";

    std::vector<wchar_t> block;
    for (const auto& [name, value] : variables) {
        block.insert(block.end(), name.begin(), name.end());
        block.push_back(L'=');
        block.insert(block.end(), value.begin(), value.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

}  // namespace

AirPlayReceiverController::AirPlayReceiverController(LogFn log_fn)
    : log_fn_(std::move(log_fn)) {}

AirPlayReceiverController::~AirPlayReceiverController() {
    Stop();
}

void AirPlayReceiverController::SetLogFn(LogFn log_fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_fn_ = std::move(log_fn);
}

void AirPlayReceiverController::SetRuntimeRoot(const std::wstring& runtime_root) {
    std::lock_guard<std::mutex> lock(mutex_);
    runtime_root_ = runtime_root;
    installed_ = false;
}

void AirPlayReceiverController::SetLogFilePath(const std::wstring& log_file_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_file_path_ = log_file_path;
}

void AirPlayReceiverController::RefreshInstallState() {
    std::lock_guard<std::mutex> lock(mutex_);
    installed_ = FileExists(BuildBinaryPathLocked());
    RefreshBonjourStateLocked();
}

bool AirPlayReceiverController::Start(const std::wstring& server_name) {
    LogFn log_fn;
    std::wstring success_text;
    std::wstring error_text;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        PollLocked();
        log_fn = log_fn_;

        if (running_) {
            return true;
        }

        installed_ = FileExists(BuildBinaryPathLocked());
        if (!installed_) {
            last_error_ = L"\u82F9\u679C\u6295\u5C4F\u7EC4\u4EF6\u7F3A\u5931\uFF0C\u8BF7\u786E\u8BA4 apple-airplay \u76EE\u5F55\u5DF2\u968F PC \u6210\u54C1\u4E00\u8D77\u53D1\u8D27\u3002";
            error_text = last_error_;
        } else if (!EnsureBonjourServiceRunningLocked(&error_text)) {
            last_error_ = error_text;
        } else if (log_file_path_.empty()) {
            last_error_ = L"\u82F9\u679C\u6295\u5C4F\u65E5\u5FD7\u8DEF\u5F84\u672A\u914D\u7F6E\u3002";
            error_text = last_error_;
        } else {
            SECURITY_ATTRIBUTES attributes{};
            attributes.nLength = sizeof(attributes);
            attributes.bInheritHandle = TRUE;

            log_handle_ = CreateFileW(
                log_file_path_.c_str(),
                FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                &attributes,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (log_handle_ == INVALID_HANDLE_VALUE) {
                log_handle_ = nullptr;
                last_error_ = L"\u65E0\u6CD5\u521B\u5EFA\u82F9\u679C\u6295\u5C4F\u65E5\u5FD7\u6587\u4EF6\u3002";
                error_text = last_error_;
            } else {
                SetFilePointer(log_handle_, 0, nullptr, FILE_END);

                const std::wstring binary_path = BuildBinaryPathLocked();
                std::wstring command_line =
                    QuoteArg(binary_path) +
                    L" -n " + QuoteArg(server_name) +
                    L" -nh -vs d3d11videosink -as wasapisink -fps 60 -nc";
                std::vector<wchar_t> command_line_buffer(command_line.begin(), command_line.end());
                command_line_buffer.push_back(L'\0');
                std::vector<wchar_t> environment_block = BuildEnvironmentBlock(runtime_root_);

                STARTUPINFOW startup_info{};
                startup_info.cb = sizeof(startup_info);
                startup_info.dwFlags = STARTF_USESTDHANDLES;
                startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
                startup_info.hStdOutput = log_handle_;
                startup_info.hStdError = log_handle_;

                PROCESS_INFORMATION process_info{};
                const std::wstring working_directory = DirectoryName(binary_path);
                const BOOL created = CreateProcessW(
                    nullptr,
                    command_line_buffer.data(),
                    nullptr,
                    nullptr,
                    TRUE,
                    CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                    environment_block.data(),
                    working_directory.empty() ? nullptr : working_directory.c_str(),
                    &startup_info,
                    &process_info);
                if (!created) {
                    std::wostringstream stream;
                    stream << L"\u542F\u52A8\u82F9\u679C\u6295\u5C4F\u670D\u52A1\u5931\u8D25\uFF0CWin32 \u9519\u8BEF " << GetLastError();
                    last_error_ = stream.str();
                    error_text = last_error_;
                    CloseHandle(log_handle_);
                    log_handle_ = nullptr;
                } else {
                    process_handle_ = process_info.hProcess;
                    CloseHandle(process_info.hThread);
                    process_id_ = process_info.dwProcessId;
                    running_ = true;
                    last_error_.clear();
                    last_exit_code_ = STILL_ACTIVE;
                    success_text =
                        std::wstring(L"\u82F9\u679C\u6295\u5C4F: \u5DF2\u542F\u52A8 AirPlay \u670D\u52A1\uFF0CiPhone / iPad \u53EF\u5728\u201C\u5C4F\u5E55\u955C\u50CF\u201D\u4E2D\u5BFB\u627E\u201C") +
                        server_name +
                        L"\u201D\u3002";
                }
            }
        }
    }

    if (!success_text.empty() && log_fn) {
        log_fn(success_text);
    }
    if (!error_text.empty() && log_fn) {
        log_fn(std::wstring(L"\u82F9\u679C\u6295\u5C4F: ") + error_text);
    }
    return error_text.empty();
}

void AirPlayReceiverController::Stop() {
    LogFn log_fn;
    bool was_running = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        PollLocked();
        log_fn = log_fn_;
        if (process_handle_ == nullptr) {
            return;
        }

        was_running = running_;
        if (WaitForSingleObject(process_handle_, 0) != WAIT_OBJECT_0) {
            TerminateProcess(process_handle_, 0);
            WaitForSingleObject(process_handle_, 3000);
        }
        PollLocked();
        last_error_.clear();
    }

    if (was_running && log_fn) {
        log_fn(L"\u82F9\u679C\u6295\u5C4F: \u5DF2\u5173\u95ED AirPlay \u670D\u52A1\u3002");
    }
}

void AirPlayReceiverController::Poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    PollLocked();
}

bool AirPlayReceiverController::installed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return installed_;
}

bool AirPlayReceiverController::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    PollLocked();
    return running_;
}

bool AirPlayReceiverController::bonjour_installed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RefreshBonjourStateLocked();
    return bonjour_installed_;
}

bool AirPlayReceiverController::bonjour_running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RefreshBonjourStateLocked();
    return bonjour_running_;
}

std::wstring AirPlayReceiverController::runtime_root() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return runtime_root_;
}

std::wstring AirPlayReceiverController::binary_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return BuildBinaryPathLocked();
}

std::wstring AirPlayReceiverController::log_file_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return log_file_path_;
}

std::wstring AirPlayReceiverController::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    PollLocked();
    return last_error_;
}

DWORD AirPlayReceiverController::last_exit_code() const {
    std::lock_guard<std::mutex> lock(mutex_);
    PollLocked();
    return last_exit_code_;
}

void AirPlayReceiverController::RefreshBonjourStateLocked() const {
    bonjour_installed_ = false;
    bonjour_running_ = false;

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return;
    }

    SC_HANDLE service = OpenServiceW(manager, kBonjourServiceName, SERVICE_QUERY_STATUS);
    if (service != nullptr) {
        bonjour_installed_ = true;
        SERVICE_STATUS_PROCESS status{};
        DWORD bytes_needed = 0;
        if (QueryServiceStatusEx(
                service,
                SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&status),
                sizeof(status),
                &bytes_needed)) {
            bonjour_running_ = status.dwCurrentState == SERVICE_RUNNING;
        }
        CloseServiceHandle(service);
    }

    CloseServiceHandle(manager);
}

void AirPlayReceiverController::PollLocked() const {
    if (process_handle_ == nullptr) {
        running_ = false;
        return;
    }

    const DWORD wait_result = WaitForSingleObject(process_handle_, 0);
    if (wait_result == WAIT_TIMEOUT) {
        running_ = true;
        return;
    }

    running_ = false;
    if (wait_result == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(process_handle_, &exit_code)) {
            last_exit_code_ = exit_code;
            if (exit_code != 0 && last_error_.empty()) {
                std::wostringstream stream;
                stream << L"AirPlay \u670D\u52A1\u5DF2\u9000\u51FA\uFF0C\u9000\u51FA\u7801 " << exit_code;
                last_error_ = stream.str();
            }
        }
    }
    ClearProcessHandlesLocked();
}

void AirPlayReceiverController::ClearProcessHandlesLocked() const {
    if (process_handle_ != nullptr) {
        CloseHandle(process_handle_);
        process_handle_ = nullptr;
    }
    if (log_handle_ != nullptr) {
        CloseHandle(log_handle_);
        log_handle_ = nullptr;
    }
    process_id_ = 0;
}

bool AirPlayReceiverController::EnsureBonjourServiceRunningLocked(std::wstring* error_text) {
    RefreshBonjourStateLocked();
    if (!bonjour_installed_) {
        if (error_text != nullptr) {
            *error_text = L"\u7CFB\u7EDF\u4E2D\u672A\u627E\u5230 Bonjour \u670D\u52A1\uFF0C\u65E0\u6CD5\u5B8C\u6210 AirPlay \u5E7F\u64AD\u6CE8\u518C\u3002";
        }
        return false;
    }
    if (bonjour_running_) {
        return true;
    }

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        if (error_text != nullptr) {
            *error_text = L"\u65E0\u6CD5\u6253\u5F00\u670D\u52A1\u63A7\u5236\u7BA1\u7406\u5668\u3002";
        }
        return false;
    }

    SC_HANDLE service = OpenServiceW(manager, kBonjourServiceName, SERVICE_QUERY_STATUS | SERVICE_START);
    if (service == nullptr) {
        CloseServiceHandle(manager);
        if (error_text != nullptr) {
            *error_text = L"\u65E0\u6CD5\u6253\u5F00 Bonjour \u670D\u52A1\u3002";
        }
        return false;
    }

    if (!StartServiceW(service, 0, nullptr)) {
        const DWORD last_error = GetLastError();
        if (last_error != ERROR_SERVICE_ALREADY_RUNNING) {
            std::wostringstream stream;
            stream << L"\u542F\u52A8 Bonjour \u670D\u52A1\u5931\u8D25\uFF0CWin32 \u9519\u8BEF " << last_error;
            if (error_text != nullptr) {
                *error_text = stream.str();
            }
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return false;
        }
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD bytes_needed = 0;
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (!QueryServiceStatusEx(
                service,
                SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&status),
                sizeof(status),
                &bytes_needed)) {
            break;
        }
        if (status.dwCurrentState == SERVICE_RUNNING) {
            bonjour_running_ = true;
            bonjour_installed_ = true;
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return true;
        }
        Sleep(200);
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (error_text != nullptr) {
        *error_text = L"Bonjour \u670D\u52A1\u5728\u9884\u671F\u65F6\u95F4\u5185\u672A\u6210\u529F\u542F\u52A8\u3002";
    }
    return false;
}

std::wstring AirPlayReceiverController::BuildBinaryPathLocked() const {
    if (runtime_root_.empty()) {
        return {};
    }
    return runtime_root_ + L"\\_internal\\bin\\uxplay.exe";
}
