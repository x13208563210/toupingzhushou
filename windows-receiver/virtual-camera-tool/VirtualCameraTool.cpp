#include "VirtualCameraShared.h"

#include <Windows.h>
#include <ShlObj.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

using MFCreateVirtualCameraFn = HRESULT(WINAPI*)(
    MFVirtualCameraType,
    MFVirtualCameraLifetime,
    MFVirtualCameraAccess,
    PCWSTR,
    PCWSTR,
    IMFAttributes*,
    ULONG,
    IMFVirtualCamera**);

std::wstring HrToString(HRESULT hr) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase << hr;
    return stream.str();
}

std::wstring GetExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (length >= path.size() - 1) {
        path.resize(path.size() * 2, L'\0');
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    path.resize(length);

    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    return path.substr(0, slash);
}

std::wstring DefaultMediaSourcePath() {
    return GetExecutableDirectory() + L"\\" + virtual_camera::kMediaSourceFileName;
}

std::wstring BuildSharedFramePath() {
    PWSTR public_documents = nullptr;
    std::wstring base_path = L"C:\\Users\\Public\\Documents";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_PublicDocuments, 0, nullptr, &public_documents)) &&
        public_documents != nullptr) {
        base_path = public_documents;
    }
    if (public_documents != nullptr) {
        CoTaskMemFree(public_documents);
    }
    return base_path + L"\\" + virtual_camera::kSharedDirectoryName + L"\\" + virtual_camera::kSharedFrameFileName;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned);
    CloseHandle(token);
    return ok == TRUE && elevation.TokenIsElevated != 0;
}

std::wstring QueryRegisteredPath() {
    HKEY key = nullptr;
    const std::wstring registry_path =
        std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + virtual_camera::kMediaSourceClsidString + L"\\InprocServer32";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, registry_path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }

    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, nullptr, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ ||
        bytes == 0) {
        RegCloseKey(key);
        return {};
    }

    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(
            key,
            nullptr,
            nullptr,
            &type,
            reinterpret_cast<LPBYTE>(value.data()),
            &bytes) != ERROR_SUCCESS) {
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

MFCreateVirtualCameraFn ResolveMFCreateVirtualCamera() {
    static MFCreateVirtualCameraFn create_virtual_camera = []() -> MFCreateVirtualCameraFn {
        const wchar_t* module_names[] = {L"mfplat.dll", L"mf.dll"};
        for (const auto* module_name : module_names) {
            HMODULE module = GetModuleHandleW(module_name);
            if (module == nullptr) {
                module = LoadLibraryW(module_name);
            }
            if (module == nullptr) {
                continue;
            }

            auto* function = reinterpret_cast<MFCreateVirtualCameraFn>(GetProcAddress(module, "MFCreateVirtualCamera"));
            if (function != nullptr) {
                return function;
            }
        }
        return nullptr;
    }();

    return create_virtual_camera;
}

HRESULT InstallMediaSource(const std::wstring& dll_path) {
    const DWORD attributes = GetFileAttributesW(dll_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    HKEY clsid_key = nullptr;
    std::wstring clsid_path = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + virtual_camera::kMediaSourceClsidString;
    LONG result = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE,
        clsid_path.c_str(),
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_WRITE,
        nullptr,
        &clsid_key,
        nullptr);
    if (result != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(result);
    }

    const std::wstring display_name = virtual_camera::kFriendlyName;
    result = RegSetValueExW(
        clsid_key,
        nullptr,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(display_name.c_str()),
        static_cast<DWORD>((display_name.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(clsid_key);
    if (result != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(result);
    }

    HKEY inproc_key = nullptr;
    clsid_path += L"\\InprocServer32";
    result = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE,
        clsid_path.c_str(),
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_WRITE,
        nullptr,
        &inproc_key,
        nullptr);
    if (result != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(result);
    }

    result = RegSetValueExW(
        inproc_key,
        nullptr,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(dll_path.c_str()),
        static_cast<DWORD>((dll_path.size() + 1) * sizeof(wchar_t)));
    if (result == ERROR_SUCCESS) {
        const std::wstring threading_model = L"Both";
        result = RegSetValueExW(
            inproc_key,
            L"ThreadingModel",
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(threading_model.c_str()),
            static_cast<DWORD>((threading_model.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(inproc_key);
    return HRESULT_FROM_WIN32(result);
}

HRESULT UninstallMediaSource() {
    const std::wstring clsid_path = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + virtual_camera::kMediaSourceClsidString;
    const LONG result = RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsid_path.c_str());
    if (result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND) {
        return S_OK;
    }
    return HRESULT_FROM_WIN32(result);
}

struct SharedFrameWriter {
    HANDLE file_handle = INVALID_HANDLE_VALUE;
    HANDLE mapping_handle = nullptr;
    BYTE* view = nullptr;

    ~SharedFrameWriter() {
        Close();
    }

    void Close() {
        if (view != nullptr) {
            UnmapViewOfFile(view);
            view = nullptr;
        }
        if (mapping_handle != nullptr) {
            CloseHandle(mapping_handle);
            mapping_handle = nullptr;
        }
        if (file_handle != nullptr && file_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(file_handle);
            file_handle = INVALID_HANDLE_VALUE;
        }
    }

    HRESULT Open() {
        Close();

        const std::wstring path = BuildSharedFramePath();
        const size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
        }

        file_handle = CreateFileW(
            path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file_handle == INVALID_HANDLE_VALUE) {
            file_handle = nullptr;
            return HRESULT_FROM_WIN32(GetLastError());
        }

        LARGE_INTEGER size{};
        size.QuadPart = static_cast<LONGLONG>(virtual_camera::kSharedMemoryBytes);
        if (!SetFilePointerEx(file_handle, size, nullptr, FILE_BEGIN) || !SetEndOfFile(file_handle)) {
            const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
            Close();
            return hr;
        }

        mapping_handle = CreateFileMappingW(
            file_handle,
            nullptr,
            PAGE_READWRITE,
            0,
            static_cast<DWORD>(virtual_camera::kSharedMemoryBytes),
            nullptr);
        if (mapping_handle == nullptr) {
            const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
            Close();
            return hr;
        }

        view = static_cast<BYTE*>(
            MapViewOfFile(mapping_handle, FILE_MAP_ALL_ACCESS, 0, 0, virtual_camera::kSharedMemoryBytes));
        if (view == nullptr) {
            const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
            Close();
            return hr;
        }

        return S_OK;
    }

    void WriteFrame(uint64_t frame_counter) {
        if (view == nullptr) {
            return;
        }

        auto* header = reinterpret_cast<virtual_camera::SharedFrameHeader*>(view);
        BYTE* payload = view + sizeof(virtual_camera::SharedFrameHeader);
        const uint64_t begin_sequence = (frame_counter << 1) | 1ull;
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&header->sequence), static_cast<LONG64>(begin_sequence));
        MemoryBarrier();

        for (UINT32 y = 0; y < virtual_camera::kOutputHeight; ++y) {
            BYTE* row = payload + static_cast<size_t>(y) * virtual_camera::kOutputStride;
            for (UINT32 x = 0; x < virtual_camera::kOutputWidth; ++x) {
                row[x * 4u + 0] = static_cast<BYTE>((x + frame_counter * 4ull) % 256ull);
                row[x * 4u + 1] = static_cast<BYTE>((y * 255u) / (virtual_camera::kOutputHeight - 1u));
                row[x * 4u + 2] = static_cast<BYTE>(((x / 32u) % 2u) ? 220 : 40);
                row[x * 4u + 3] = 0xFF;
            }
        }

        header->magic = virtual_camera::kFrameMagic;
        header->version = virtual_camera::kFrameVersion;
        header->width = virtual_camera::kOutputWidth;
        header->height = virtual_camera::kOutputHeight;
        header->stride = virtual_camera::kOutputStride;
        header->pixel_format = virtual_camera::kPixelFormatBgra32;
        header->frame_counter = frame_counter;
        header->pts_us = frame_counter * virtual_camera::kFrameIntervalUs;
        header->last_update_tick_ms = GetTickCount64();
        header->has_frame = 1;
        MemoryBarrier();
        InterlockedExchange64(
            reinterpret_cast<volatile LONG64*>(&header->sequence),
            static_cast<LONG64>(begin_sequence + 1));
        FlushViewOfFile(view, virtual_camera::kSharedMemoryBytes);
    }
};

HRESULT StartVirtualCamera(IMFVirtualCamera** camera) {
    if (camera == nullptr) {
        return E_POINTER;
    }
    *camera = nullptr;

    auto* create_virtual_camera = ResolveMFCreateVirtualCamera();
    if (create_virtual_camera == nullptr) {
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }

    IMFVirtualCamera* raw_camera = nullptr;
    HRESULT hr = create_virtual_camera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_Session,
        MFVirtualCameraAccess_CurrentUser,
        virtual_camera::kFriendlyName,
        virtual_camera::kMediaSourceClsidString,
        nullptr,
        0,
        &raw_camera);
    if (FAILED(hr)) {
        return hr;
    }

    hr = raw_camera->Start(nullptr);
    if (FAILED(hr)) {
        raw_camera->Shutdown();
        raw_camera->Release();
        return hr;
    }

    *camera = raw_camera;
    return S_OK;
}

void StopVirtualCamera(IMFVirtualCamera*& camera) {
    if (camera == nullptr) {
        return;
    }
    camera->Stop();
    camera->Shutdown();
    camera->Release();
    camera = nullptr;
}

HRESULT RunSession(bool write_test_pattern, DWORD duration_seconds) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_initialized = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return hr;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        if (co_initialized) {
            CoUninitialize();
        }
        return hr;
    }

    IMFVirtualCamera* camera = nullptr;
    hr = StartVirtualCamera(&camera);
    if (FAILED(hr)) {
        MFShutdown();
        if (co_initialized) {
            CoUninitialize();
        }
        return hr;
    }

    std::atomic<bool> running{true};
    SharedFrameWriter writer;
    std::thread writer_thread;
    if (write_test_pattern) {
        hr = writer.Open();
        if (FAILED(hr)) {
            StopVirtualCamera(camera);
            MFShutdown();
            if (co_initialized) {
                CoUninitialize();
            }
            return hr;
        }

        writer_thread = std::thread([&running, &writer]() {
            uint64_t frame_counter = 1;
            while (running.load()) {
                writer.WriteFrame(frame_counter++);
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
            }
        });
    }

    if (write_test_pattern) {
        std::wcout << L"已启动虚拟摄像头自检画面，请在采集软件中选择“直播投屏助手虚拟摄像头”。\n";
    } else {
        std::wcout << L"已启动虚拟摄像头会话，请在采集软件中选择“直播投屏助手虚拟摄像头”。\n";
    }

    if (duration_seconds == 0) {
        std::wcout << L"按回车结束。\n";
        std::wstring line;
        std::getline(std::wcin, line);
    } else {
        std::wcout << L"将在 " << duration_seconds << L" 秒后自动结束。\n";
        Sleep(duration_seconds * 1000u);
    }

    running.store(false);
    if (writer_thread.joinable()) {
        writer_thread.join();
    }
    writer.Close();
    StopVirtualCamera(camera);
    MFShutdown();
    if (co_initialized) {
        CoUninitialize();
    }
    return S_OK;
}

void PrintUsage() {
    std::wcout
        << L"\u76F4\u64AD\u6295\u5C4F\u52A9\u624B\u865A\u62DF\u6444\u50CF\u5934\u5DE5\u5177\n"
        << L"\u7528\u6CD5:\n"
        << L"  live-cast-virtual-camera-tool.exe install\n"
        << L"  live-cast-virtual-camera-tool.exe uninstall\n"
        << L"  live-cast-virtual-camera-tool.exe status\n"
        << L"  live-cast-virtual-camera-tool.exe start [seconds]\n"
        << L"  live-cast-virtual-camera-tool.exe selftest [seconds]\n";
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    const std::wstring command = argv[1];
    if (command == L"status") {
        const std::wstring registered_path = QueryRegisteredPath();
        if (registered_path.empty()) {
            std::wcout << L"\u72B6\u6001: \u672A\u5B89\u88C5\n";
            return 0;
        }

        std::wcout << L"\u72B6\u6001: \u5DF2\u5B89\u88C5\n";
        std::wcout << L"DLL: " << registered_path << L"\n";
        return 0;
    }

    if (command == L"start" || command == L"selftest") {
        DWORD duration_seconds = 0;
        if (argc >= 3) {
            duration_seconds = static_cast<DWORD>(_wtoi(argv[2]));
        }

        const HRESULT hr = RunSession(command == L"selftest", duration_seconds);
        if (FAILED(hr)) {
            std::wcerr << L"\u542F\u52A8\u5931\u8D25\uFF0C\u9519\u8BEF\u7801 " << HrToString(hr) << L"\n";
            return 1;
        }
        return 0;
    }

    if (!IsElevated()) {
        std::wcerr << L"\u8BF7\u4EE5\u7BA1\u7406\u5458\u8EAB\u4EFD\u8FD0\u884C\u6B64\u5DE5\u5177\u3002\n";
        return 1;
    }

    if (command == L"install") {
        const std::wstring dll_path = DefaultMediaSourcePath();
        const HRESULT hr = InstallMediaSource(dll_path);
        if (FAILED(hr)) {
            std::wcerr
                << L"\u5B89\u88C5\u5931\u8D25\uFF0C\u9519\u8BEF\u7801 "
                << HrToString(hr)
                << L"\nDLL: "
                << dll_path
                << L"\n";
            return 1;
        }
        std::wcout << L"\u5B89\u88C5\u5B8C\u6210\u3002\nDLL: " << dll_path << L"\n";
        return 0;
    }

    if (command == L"uninstall") {
        const HRESULT hr = UninstallMediaSource();
        if (FAILED(hr)) {
            std::wcerr << L"\u5378\u8F7D\u5931\u8D25\uFF0C\u9519\u8BEF\u7801 " << HrToString(hr) << L"\n";
            return 1;
        }
        std::wcout << L"\u5378\u8F7D\u5B8C\u6210\u3002\n";
        return 0;
    }

    PrintUsage();
    return 1;
}
