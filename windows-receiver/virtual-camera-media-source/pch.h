#pragma once

#include <unknwn.h>
#include <windows.h>
#include <propvarutil.h>
#include <devpropdef.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <ShlObj.h>
#include <strsafe.h>

#include <ole2.h>
#include <initguid.h>
#include <Ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <mfvirtualcamera.h>
#include <d3d9types.h>

#define RESULT_DIAGNOSTICS_LEVEL 4

#include <wil/cppwinrt.h>
#include <wil/result.h>
#include <wil/com.h>

#include <cstdio>
#include <string>

#include "EventHandler.h"
#include "SimpleFrameGenerator.h"
#include "SimpleMediaSource.h"
#include "SimpleMediaStream.h"
#include "VirtualCameraMediaSource.h"
#include "VirtualCameraMediaSourceActivate.h"
#include "VirtualCameraShared.h"

#pragma comment(lib, "mfuuid")
#pragma comment(lib, "mf")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "Mfsensorgroup")

inline void DebugPrint(LPCWSTR szFormat, ...)
{
    WCHAR szBuffer[MAX_PATH] = {};

    va_list pArgs;
    va_start(pArgs, szFormat);
    StringCbVPrintf(szBuffer, sizeof(szBuffer), szFormat, pArgs);
    va_end(pArgs);
    OutputDebugStringW(szBuffer);
}

#define DEBUG_MSG(msg, ...)         \
{                                   \
    DebugPrint(L"[%s@%d] ", TEXT(__FUNCTION__), __LINE__); \
    DebugPrint(msg, __VA_ARGS__);   \
    DebugPrint(L"\n");              \
}

inline std::wstring BuildVirtualCameraTracePath()
{
    PWSTR publicDocuments = nullptr;
    std::wstring basePath = L"C:\\Users\\Public\\Documents";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_PublicDocuments, 0, nullptr, &publicDocuments)) &&
        publicDocuments != nullptr)
    {
        basePath = publicDocuments;
    }
    if (publicDocuments != nullptr)
    {
        CoTaskMemFree(publicDocuments);
    }
    return basePath + L"\\" + virtual_camera::kSharedDirectoryName + L"\\virtual-camera-source.log";
}

inline void AppendVirtualCameraTrace(const std::string& message)
{
    const std::wstring path = BuildVirtualCameraTracePath();
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
    {
        SHCreateDirectoryExW(nullptr, path.substr(0, slash).c_str(), nullptr);
    }

    HANDLE file = CreateFileW(
        path.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);

    char line[2048];
    const int count = sprintf_s(
        line,
        "[%02u:%02u:%02u.%03u] %s\r\n",
        static_cast<unsigned>(st.wHour),
        static_cast<unsigned>(st.wMinute),
        static_cast<unsigned>(st.wSecond),
        static_cast<unsigned>(st.wMilliseconds),
        message.c_str());
    if (count > 0)
    {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(count), &written, nullptr);
    }
    CloseHandle(file);
}

namespace wilEx
{
    template<typename T>
    wil::unique_cotaskmem_array_ptr<T> make_unique_cotaskmem_array(size_t numOfElements)
    {
        wil::unique_cotaskmem_array_ptr<T> arr;
        size_t cb = sizeof(wil::details::element_traits<T>::type) * numOfElements;
        void* ptr = ::CoTaskMemAlloc(cb);
        if (ptr != nullptr)
        {
            ZeroMemory(ptr, cb);
            arr.reset(reinterpret_cast<typename wil::details::element_traits<T>::type*>(ptr), numOfElements);
        }
        return arr;
    }
}

namespace winrt
{
    template<> bool is_guid_of<IMFMediaSourceEx>(guid const& id) noexcept;
    template<> bool is_guid_of<IMFMediaStream2>(guid const& id) noexcept;
    template<> bool is_guid_of<IMFActivate>(guid const& id) noexcept;
}

#define CHECKHR_GOTO(_hr, _lbl) { hr = _hr; if (FAILED(hr)) { DEBUG_MSG(L"hr=0x%08x", _hr); goto _lbl; } }
#define CHECKNULL_GOTO(_ptr, _hr, _lbl) { if (_ptr == nullptr) { hr = _hr; if (FAILED(hr)) { DEBUG_MSG(L"hr=0x%08x", _hr); goto _lbl; } } }
