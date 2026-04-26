#include "DanmakuController.h"

#include <Windows.h>
#include <d3d11.h>
#include <objbase.h>
#include <oleacc.h>
#include <UIAutomationClient.h>
#include <mmsystem.h>
#include <sapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <windowsx.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>

namespace {

#pragma comment(lib, "Oleacc.lib")

constexpr wchar_t kRegionOverlayClassName[] = L"LiveCastDanmakuRegionOverlay";
constexpr int kMinSelectionSize = 18;
constexpr ULONGLONG kDedupeWindowMs = 2500;
constexpr ULONGLONG kGiftCooldownMs = 4500;
constexpr int kUiProbeDelayMs = 3000;
constexpr int kUiLivePollIntervalMs = 180;
constexpr size_t kUiInitialCatchupLines = 4;
constexpr size_t kMaxRecentEvents = 12;
constexpr size_t kMaxProbeLines = 10;
constexpr size_t kMaxUiLiveVisibleLines = 40;
constexpr size_t kMaxDedupeEntries = 32;
constexpr size_t kMaxSpeechQueue = 6;
constexpr int kGiftStableFrames = 2;
constexpr int kGiftHashTolerance = 10;
constexpr int kUiReadDedupeBucketPx = 12;
constexpr UINT kOverlayAlpha = 118;
constexpr DWORD kGraphicsCaptureFrameWaitMs = 220;
constexpr int kGraphicsCaptureMaxAttempts = 4;

struct OverlaySelectionContext {
    RECT bounds{};
    POINT start{};
    POINT current{};
    RECT selection{};
    bool dragging = false;
    bool finished = false;
    bool cancelled = true;
    bool has_selection = false;
};

struct ScopedComApartment {
    HRESULT init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ~ScopedComApartment() {
        if (SUCCEEDED(init_result)) {
            CoUninitialize();
        }
    }

    bool ready() const {
        return SUCCEEDED(init_result) || init_result == RPC_E_CHANGED_MODE;
    }
};

struct GraphicsCaptureDeviceContext {
    std::mutex mutex;
    winrt::com_ptr<ID3D11Device> d3d_device;
    winrt::com_ptr<ID3D11DeviceContext> d3d_context;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt_device{nullptr};
};

struct RawCaptureFrame {
    int width = 0;
    int height = 0;
    RECT screen_rect{};
    std::vector<uint8_t> pixels;
};

std::wstring TrimWhitespace(const std::wstring& text) {
    const size_t start = text.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) {
        return {};
    }
    const size_t end = text.find_last_not_of(L" \t\r\n");
    return text.substr(start, end - start + 1);
}

std::wstring ToLowerCopy(const std::wstring& text) {
    std::wstring lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return lowered;
}

bool ContainsCaseInsensitive(const std::wstring& text, const std::wstring& token) {
    if (text.empty() || token.empty()) {
        return false;
    }
    return ToLowerCopy(text).find(ToLowerCopy(token)) != std::wstring::npos;
}

bool ContainsChinese(const std::wstring& text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t ch) {
        return ch >= 0x3400 && ch <= 0x9FFF;
    });
}

int CountChineseChars(const std::wstring& text) {
    int count = 0;
    for (wchar_t ch : text) {
        if (ch >= 0x3400 && ch <= 0x9FFF) {
            ++count;
        }
    }
    return count;
}

int CountAsciiLettersAndDigits(const std::wstring& text) {
    int count = 0;
    for (wchar_t ch : text) {
        if ((ch >= L'0' && ch <= L'9') ||
            (ch >= L'a' && ch <= L'z') ||
            (ch >= L'A' && ch <= L'Z')) {
            ++count;
        }
    }
    return count;
}

std::wstring ReadWindowText(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return {};
    }

    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return {};
    }

    std::wstring text(static_cast<size_t>(length), L'\0');
    const int copied = GetWindowTextW(hwnd, text.data(), length + 1);
    if (copied <= 0) {
        return {};
    }
    text.resize(static_cast<size_t>(copied));
    return TrimWhitespace(text);
}

std::wstring ReadWindowClassName(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return {};
    }

    wchar_t class_name[256] = {};
    const int copied = GetClassNameW(hwnd, class_name, static_cast<int>(std::size(class_name)));
    if (copied <= 0) {
        return {};
    }
    return TrimWhitespace(class_name);
}

std::wstring GetExecutableDirectoryPath() {
    wchar_t module_path[MAX_PATH] = {};
    const DWORD copied = GetModuleFileNameW(nullptr, module_path, static_cast<DWORD>(std::size(module_path)));
    if (copied == 0 || copied >= std::size(module_path)) {
        return {};
    }

    std::error_code error;
    const std::filesystem::path parent = std::filesystem::path(module_path).parent_path();
    if (parent.empty() || error) {
        return {};
    }
    return parent.wstring();
}

std::wstring GetDanmakuReminderSoundPath() {
    const std::wstring executable_directory = GetExecutableDirectoryPath();
    if (executable_directory.empty()) {
        return {};
    }

    const std::filesystem::path sound_path =
        std::filesystem::path(executable_directory) / L"danmaku-reminder.wav";
    std::error_code error;
    if (!std::filesystem::exists(sound_path, error) || error) {
        return {};
    }
    return sound_path.wstring();
}

std::wstring GetGiftReminderSoundPath() {
    const std::wstring executable_directory = GetExecutableDirectoryPath();
    if (executable_directory.empty()) {
        return {};
    }

    const std::filesystem::path sound_path =
        std::filesystem::path(executable_directory) / L"gift-reminder.wav";
    std::error_code error;
    if (!std::filesystem::exists(sound_path, error) || error) {
        return {};
    }
    return sound_path.wstring();
}

GraphicsCaptureDeviceContext& GetGraphicsCaptureDeviceContext() {
    static GraphicsCaptureDeviceContext context;
    return context;
}

bool EnsureGraphicsCaptureDevice(std::wstring* error) {
    auto& context = GetGraphicsCaptureDeviceContext();
    std::lock_guard<std::mutex> lock(context.mutex);
    if (context.d3d_device != nullptr &&
        context.d3d_context != nullptr &&
        context.winrt_device != nullptr) {
        return true;
    }

    UINT device_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL created_level = D3D_FEATURE_LEVEL_10_0;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        device_flags,
        feature_levels,
        static_cast<UINT>(std::size(feature_levels)),
        D3D11_SDK_VERSION,
        context.d3d_device.put(),
        &created_level,
        context.d3d_context.put());
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            device_flags,
            feature_levels,
            static_cast<UINT>(std::size(feature_levels)),
            D3D11_SDK_VERSION,
            context.d3d_device.put(),
            &created_level,
            context.d3d_context.put());
    }
    if (FAILED(hr) || context.d3d_device == nullptr || context.d3d_context == nullptr) {
        if (error != nullptr) {
            *error = L"\u65e0\u6cd5\u521d\u59cb\u5316 Windows \u7a97\u53e3\u753b\u9762\u6293\u53d6\u8bbe\u5907";
        }
        context.d3d_device = nullptr;
        context.d3d_context = nullptr;
        context.winrt_device = nullptr;
        return false;
    }

    winrt::com_ptr<IDXGIDevice> dxgi_device = context.d3d_device.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> inspectable;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put());
    if (FAILED(hr) || inspectable == nullptr) {
        if (error != nullptr) {
            *error = L"\u65e0\u6cd5\u521d\u59cb\u5316 Windows \u7a97\u53e3\u753b\u9762\u6293\u53d6 WinRT \u8bbe\u5907";
        }
        context.d3d_device = nullptr;
        context.d3d_context = nullptr;
        context.winrt_device = nullptr;
        return false;
    }

    context.winrt_device =
        inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
    return context.winrt_device != nullptr;
}

bool CreateGraphicsCaptureItemForWindow(
    HWND target_window,
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem* item,
    std::wstring* error) {
    if (item == nullptr) {
        if (error != nullptr) {
            *error = L"\u5185\u90e8\u9519\u8bef";
        }
        return false;
    }

    if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
        if (error != nullptr) {
            *error = L"\u5f53\u524d Windows \u7248\u672c\u4e0d\u652f\u6301\u6309\u7a97\u53e3\u53e5\u67c4\u6293\u53d6";
        }
        return false;
    }

    try {
        const auto interop = winrt::get_activation_factory<
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
        HRESULT hr = interop->CreateForWindow(
            target_window,
            winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
            winrt::put_abi(*item));
        if (FAILED(hr) || *item == nullptr) {
            if (error != nullptr) {
                *error = L"\u65e0\u6cd5\u4e3a\u5f39\u5e55\u60ac\u6d6e\u7a97\u521b\u5efa\u53e5\u67c4\u6293\u53d6\u9879";
            }
            return false;
        }
        return true;
    } catch (const winrt::hresult_error& exception) {
        if (error != nullptr) {
            std::wostringstream stream;
            stream << L"\u521b\u5efa\u7a97\u53e3\u6293\u53d6\u9879\u5931\u8d25\uff0c\u9519\u8bef 0x" << std::hex << exception.code().value;
            *error = stream.str();
        }
        return false;
    }
}

bool CopyTexturePixelsToFrameCapture(
    ID3D11Texture2D* texture,
    const RECT& screen_rect,
    RawCaptureFrame* frame,
    std::wstring* error) {
    if (texture == nullptr || frame == nullptr) {
        if (error != nullptr) {
            *error = L"\u6293\u53d6\u7ed3\u679c\u4e3a\u7a7a";
        }
        return false;
    }

    auto& context = GetGraphicsCaptureDeviceContext();
    std::lock_guard<std::mutex> lock(context.mutex);
    if (context.d3d_device == nullptr || context.d3d_context == nullptr) {
        if (error != nullptr) {
            *error = L"\u7a97\u53e3\u6293\u53d6\u8bbe\u5907\u672a\u521d\u59cb\u5316";
        }
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) {
        if (error != nullptr) {
            *error = L"\u6293\u53d6\u753b\u9762\u5c3a\u5bf8\u65e0\u6548";
        }
        return false;
    }

    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.BindFlags = 0;
    staging_desc.MiscFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.ArraySize = 1;
    staging_desc.MipLevels = 1;

    winrt::com_ptr<ID3D11Texture2D> staging_texture;
    HRESULT hr = context.d3d_device->CreateTexture2D(&staging_desc, nullptr, staging_texture.put());
    if (FAILED(hr) || staging_texture == nullptr) {
        if (error != nullptr) {
            *error = L"\u65e0\u6cd5\u521b\u5efa\u7a97\u53e3\u6293\u53d6\u8bfb\u53d6\u7f13\u51b2";
        }
        return false;
    }

    context.d3d_context->CopyResource(staging_texture.get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context.d3d_context->Map(staging_texture.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || mapped.pData == nullptr) {
        if (error != nullptr) {
            *error = L"\u65e0\u6cd5\u8bfb\u53d6\u7a97\u53e3\u6293\u53d6\u50cf\u7d20";
        }
        return false;
    }

    frame->width = static_cast<int>(desc.Width);
    frame->height = static_cast<int>(desc.Height);
    frame->screen_rect = screen_rect;
    frame->pixels.resize(static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height) * 4u);

    const size_t row_bytes = static_cast<size_t>(frame->width) * 4u;
    for (int y = 0; y < frame->height; ++y) {
        const auto* src = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(mapped.RowPitch) * static_cast<size_t>(y);
        auto* dst = frame->pixels.data() + static_cast<size_t>(y) * row_bytes;
        std::memcpy(dst, src, row_bytes);
    }

    context.d3d_context->Unmap(staging_texture.get(), 0);
    return true;
}

bool CaptureWindowWithGraphicsCapture(
    HWND target_window,
    const RECT& capture_rect,
    RawCaptureFrame* frame,
    std::wstring* error) {
    if (!EnsureGraphicsCaptureDevice(error)) {
        return false;
    }

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    if (!CreateGraphicsCaptureItemForWindow(target_window, &item, error)) {
        return false;
    }

    const auto item_size = item.Size();
    if (item_size.Width <= 0 || item_size.Height <= 0) {
        if (error != nullptr) {
            *error = L"\u5f39\u5e55\u60ac\u6d6e\u7a97\u5c3a\u5bf8\u65e0\u6548";
        }
        return false;
    }

    try {
        auto& context = GetGraphicsCaptureDeviceContext();
        const auto frame_pool =
            winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                context.winrt_device,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                1,
                item_size);
        const auto session = frame_pool.CreateCaptureSession(item);
        try {
            session.IsCursorCaptureEnabled(false);
        } catch (...) {
        }
        try {
            session.IsBorderRequired(false);
        } catch (...) {
        }

        winrt::handle frame_ready_event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!frame_ready_event) {
            if (error != nullptr) {
                *error = L"\u65e0\u6cd5\u521b\u5efa\u7a97\u53e3\u6293\u53d6\u540c\u6b65\u4e8b\u4ef6";
            }
            return false;
        }

        const auto frame_ready_handle = frame_ready_event.get();
        const auto token = frame_pool.FrameArrived([frame_ready_handle](auto&, auto&) {
            SetEvent(frame_ready_handle);
        });

        session.StartCapture();

        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame capture_frame{nullptr};
        for (int attempt = 0; attempt < kGraphicsCaptureMaxAttempts && capture_frame == nullptr; ++attempt) {
            WaitForSingleObject(frame_ready_event.get(), kGraphicsCaptureFrameWaitMs);
            capture_frame = frame_pool.TryGetNextFrame();
        }
        frame_pool.FrameArrived(token);

        if (capture_frame == nullptr) {
            if (error != nullptr) {
                *error = L"\u7a97\u53e3\u753b\u9762\u6293\u53d6\u8d85\u65f6\uff0c\u672a\u53d6\u5230\u5f39\u5e55\u5e27";
            }
            return false;
        }

        const auto surface = capture_frame.Surface();
        const auto dxgi_access =
            surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        HRESULT hr = dxgi_access->GetInterface(
            __uuidof(ID3D11Texture2D),
            texture.put_void());
        if (FAILED(hr) || texture == nullptr) {
            if (error != nullptr) {
                *error = L"\u65e0\u6cd5\u5c06\u7a97\u53e3\u6293\u53d6\u5e27\u8f6c\u4e3a D3D11 \u7eb9\u7406";
            }
            return false;
        }

        return CopyTexturePixelsToFrameCapture(texture.get(), capture_rect, frame, error);
    } catch (const winrt::hresult_error& exception) {
        if (error != nullptr) {
            std::wostringstream stream;
            stream << L"\u7a97\u53e3\u753b\u9762\u6293\u53d6\u5931\u8d25\uff0c\u9519\u8bef 0x" << std::hex << exception.code().value;
            *error = stream.str();
        }
        return false;
    }
}

std::wstring ReadSpeechVoiceName(ISpObjectToken* token) {
    if (token == nullptr) {
        return {};
    }

    LPWSTR value = nullptr;
    HRESULT hr = token->GetStringValue(L"Name", &value);
    if (FAILED(hr) || value == nullptr) {
        hr = token->GetStringValue(nullptr, &value);
    }
    if (SUCCEEDED(hr) && value != nullptr) {
        const std::wstring name = TrimWhitespace(value);
        CoTaskMemFree(value);
        return name;
    }

    LPWSTR token_id = nullptr;
    if (SUCCEEDED(token->GetId(&token_id)) && token_id != nullptr) {
        std::wstring name = TrimWhitespace(token_id);
        CoTaskMemFree(token_id);
        const size_t separator = name.find_last_of(L'\\');
        if (separator != std::wstring::npos) {
            name = TrimWhitespace(name.substr(separator + 1));
        }
        return name;
    }
    return {};
}

bool CreateSpeechVoiceEnumerator(IEnumSpObjectTokens** enum_tokens) {
    if (enum_tokens == nullptr) {
        return false;
    }
    *enum_tokens = nullptr;

    ISpObjectTokenCategory* category = nullptr;
    const HRESULT create_result = CoCreateInstance(
        CLSID_SpObjectTokenCategory,
        nullptr,
        CLSCTX_ALL,
        IID_ISpObjectTokenCategory,
        reinterpret_cast<void**>(&category));
    if (FAILED(create_result) || category == nullptr) {
        return false;
    }

    const HRESULT set_id_result = category->SetId(SPCAT_VOICES, FALSE);
    const HRESULT enum_result = SUCCEEDED(set_id_result)
        ? category->EnumTokens(nullptr, nullptr, enum_tokens)
        : set_id_result;
    category->Release();
    return SUCCEEDED(enum_result) && *enum_tokens != nullptr;
}

std::vector<std::wstring> EnumerateSpeechVoiceNames() {
    std::vector<std::wstring> voice_names;
    const HRESULT apartment_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment_result) && apartment_result != RPC_E_CHANGED_MODE) {
        return voice_names;
    }

    IEnumSpObjectTokens* enum_tokens = nullptr;
    if (CreateSpeechVoiceEnumerator(&enum_tokens) && enum_tokens != nullptr) {
        while (true) {
            ISpObjectToken* token = nullptr;
            ULONG fetched = 0;
            const HRESULT next_result = enum_tokens->Next(1, &token, &fetched);
            if (FAILED(next_result) || fetched == 0 || token == nullptr) {
                break;
            }

            const std::wstring voice_name = ReadSpeechVoiceName(token);
            if (!voice_name.empty()) {
                voice_names.push_back(voice_name);
            }
            token->Release();
        }
        enum_tokens->Release();
    }

    if (SUCCEEDED(apartment_result)) {
        CoUninitialize();
    }
    return voice_names;
}

std::wstring BuildSpeechVoiceLabel(const std::vector<std::wstring>& voice_names, size_t voice_index) {
    if (voice_names.empty()) {
        return L"\u7cfb\u7edf\u9ed8\u8ba4";
    }
    if (voice_index >= voice_names.size()) {
        return voice_names.front();
    }
    return voice_names[voice_index];
}

bool IsChineseSpeechVoiceName(const std::wstring& voice_name) {
    if (voice_name.empty()) {
        return false;
    }

    static const wchar_t* const kChineseVoiceTokens[] = {
        L"Chinese",
        L"\u4e2d\u6587",
        L"Mandarin",
        L"Putonghua",
        L"Huihui",
        L"Xiaoxiao",
        L"Xiaoyi",
        L"Yunxi",
        L"Yaoyao",
    };
    for (const auto* token : kChineseVoiceTokens) {
        if (ContainsCaseInsensitive(voice_name, token)) {
            return true;
        }
    }
    return false;
}

size_t FindPreferredChineseSpeechVoiceIndex(const std::vector<std::wstring>& voice_names) {
    for (size_t index = 0; index < voice_names.size(); ++index) {
        if (IsChineseSpeechVoiceName(voice_names[index])) {
            return index;
        }
    }
    return std::numeric_limits<size_t>::max();
}

size_t ResolveSpeechVoiceIndexForText(
    const std::vector<std::wstring>& voice_names,
    size_t requested_voice_index,
    const std::wstring& text) {
    if (voice_names.empty()) {
        return requested_voice_index;
    }

    const size_t clamped_index = std::min(requested_voice_index, voice_names.size() - 1);
    if (!ContainsChinese(text) || IsChineseSpeechVoiceName(voice_names[clamped_index])) {
        return clamped_index;
    }

    const size_t preferred_chinese_index = FindPreferredChineseSpeechVoiceIndex(voice_names);
    if (preferred_chinese_index < voice_names.size()) {
        return preferred_chinese_index;
    }
    return clamped_index;
}

bool ApplySpeechVoiceByIndex(ISpVoice* voice, size_t voice_index, std::wstring* applied_name) {
    if (voice == nullptr) {
        return false;
    }

    IEnumSpObjectTokens* enum_tokens = nullptr;
    if (!CreateSpeechVoiceEnumerator(&enum_tokens) || enum_tokens == nullptr) {
        return false;
    }

    ULONG count = 0;
    enum_tokens->GetCount(&count);
    if (count == 0) {
        enum_tokens->Release();
        return false;
    }

    const ULONG target_index = static_cast<ULONG>(std::min<size_t>(voice_index, count - 1));
    bool applied = false;
    for (ULONG current_index = 0; current_index <= target_index; ++current_index) {
        ISpObjectToken* token = nullptr;
        ULONG fetched = 0;
        if (FAILED(enum_tokens->Next(1, &token, &fetched)) || fetched == 0 || token == nullptr) {
            break;
        }

        if (current_index == target_index) {
            applied = SUCCEEDED(voice->SetVoice(token));
            if (applied_name != nullptr) {
                *applied_name = ReadSpeechVoiceName(token);
            }
        }
        token->Release();
    }

    enum_tokens->Release();
    return applied;
}

struct UiProbeWindowCandidate {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring class_name;
    int score = 0;
};

int ScoreUiProbeWindowCandidate(const std::wstring& title, const std::wstring& class_name) {
    int score = 0;
    if (class_name == L"FinderLiveCommentFloatWnd") {
        score += 120;
    } else if (class_name == L"FinderLiveMainWnd") {
        score += 80;
    }

    if (ContainsCaseInsensitive(title, L"\u4e92\u52a8\u6d88\u606f")) {
        score += 100;
    }
    if (ContainsCaseInsensitive(title, L"\u89c6\u9891\u53f7\u76f4\u64ad\u4f34\u4fa3")) {
        score += 70;
    }
    if (ContainsCaseInsensitive(title, L"\u89c6\u9891\u53f7")) {
        score += 20;
    }
    if (ContainsCaseInsensitive(title, L"\u76f4\u64ad\u4f34\u4fa3")) {
        score += 20;
    }

    return score;
}

struct UiProbeWindowEnumContext {
    HWND owner_window = nullptr;
    HWND video_window = nullptr;
    std::vector<UiProbeWindowCandidate>* candidates = nullptr;
};

BOOL CALLBACK CollectUiProbeWindowsProc(HWND hwnd, LPARAM lparam) {
    auto* context = reinterpret_cast<UiProbeWindowEnumContext*>(lparam);
    if (context == nullptr || context->candidates == nullptr) {
        return TRUE;
    }
    if (hwnd == nullptr || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return TRUE;
    }
    if (hwnd == context->owner_window || hwnd == context->video_window) {
        return TRUE;
    }

    const std::wstring title = ReadWindowText(hwnd);
    const std::wstring class_name = ReadWindowClassName(hwnd);
    const int score = ScoreUiProbeWindowCandidate(title, class_name);
    if (score <= 0) {
        return TRUE;
    }

    context->candidates->push_back(UiProbeWindowCandidate{
        hwnd,
        title,
        class_name,
        score,
    });
    return TRUE;
}

std::vector<UiProbeWindowCandidate> CollectPreferredUiProbeWindows(HWND owner_window, HWND video_window) {
    std::vector<UiProbeWindowCandidate> candidates;
    UiProbeWindowEnumContext context{};
    context.owner_window = owner_window;
    context.video_window = video_window;
    context.candidates = &candidates;
    EnumWindows(CollectUiProbeWindowsProc, reinterpret_cast<LPARAM>(&context));

    std::sort(candidates.begin(), candidates.end(), [](const UiProbeWindowCandidate& left, const UiProbeWindowCandidate& right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        if (left.title.size() != right.title.size()) {
            return left.title.size() < right.title.size();
        }
        return left.class_name < right.class_name;
    });
    return candidates;
}

RECT GetDesktopCaptureBounds() {
    const int left = static_cast<int>(GetSystemMetrics(SM_XVIRTUALSCREEN));
    const int top = static_cast<int>(GetSystemMetrics(SM_YVIRTUALSCREEN));
    const int width = std::max(1, static_cast<int>(GetSystemMetrics(SM_CXVIRTUALSCREEN)));
    const int height = std::max(1, static_cast<int>(GetSystemMetrics(SM_CYVIRTUALSCREEN)));
    return RECT{left, top, left + width, top + height};
}

std::wstring NormalizeUiCompareText(const std::wstring& text) {
    std::wstring normalized;
    normalized.reserve(text.size());
    for (wchar_t ch : text) {
        if (std::iswspace(ch)) {
            continue;
        }
        normalized.push_back(ch);
    }
    return normalized;
}

std::wstring CleanUiDanmakuText(const std::wstring& text) {
    std::wstring cleaned = TrimWhitespace(text);
    while (cleaned.size() >= 4 &&
           cleaned.size() % 2 == 0 &&
           cleaned.substr(0, cleaned.size() / 2) == cleaned.substr(cleaned.size() / 2)) {
        cleaned = TrimWhitespace(cleaned.substr(0, cleaned.size() / 2));
    }

    size_t cut_pos = std::wstring::npos;
    for (const auto& token : {std::wstring(L"\u6253\u8d4f\u7b49\u7ea7"), std::wstring(L"\u7c89\u4e1d\u56e2\u7b49\u7ea7")}) {
        const size_t pos = cleaned.find(token);
        if (pos != std::wstring::npos) {
            cut_pos = cut_pos == std::wstring::npos ? pos : std::min(cut_pos, pos);
        }
    }
    if (cut_pos != std::wstring::npos) {
        cleaned = TrimWhitespace(cleaned.substr(0, cut_pos));
    }

    const size_t colon_pos = cleaned.find(L'\uff1a');
    const size_t ascii_colon_pos = cleaned.find(L':');
    const size_t separator = colon_pos != std::wstring::npos ? colon_pos : ascii_colon_pos;
    if (separator != std::wstring::npos) {
        const std::wstring left = TrimWhitespace(cleaned.substr(0, separator));
        const std::wstring right = TrimWhitespace(cleaned.substr(separator + 1));
        if (!right.empty() &&
            CountChineseChars(left) == 0 &&
            CountAsciiLettersAndDigits(left) == 0) {
            cleaned = right;
        }
    }
    return cleaned;
}

bool IsUiGiftMessageText(const std::wstring& text) {
    const std::wstring normalized = NormalizeUiCompareText(text);
    return normalized.find(L"\u9001\u51fa") != std::wstring::npos ||
        normalized.find(L"\u9001\u4e86") != std::wstring::npos ||
        normalized.find(L"\u793c\u7269") != std::wstring::npos ||
        normalized.find(L"\u6253\u8d4f") != std::wstring::npos ||
        normalized.find(L"\u5609\u5e74\u534e") != std::wstring::npos ||
        normalized.find(L"\u5c0f\u5fc3\u5fc3") != std::wstring::npos;
}

bool IsUiSystemNoticeText(const std::wstring& text) {
    const std::wstring normalized = NormalizeUiCompareText(text);
    if (normalized.empty()) {
        return true;
    }

    static const wchar_t* const kSystemNoticeTokens[] = {
        L"\u901a\u77e5",
        L"\u4e3b\u9898",
        L"\u901a\u8fc7\u76f4\u64ad\u63a8",
        L"\u901a\u8fc7\u76f4\u64ad\u63a8\u8350\u8fdb\u5165",
        L"\u6b22\u8fce\u6765\u5230\u76f4",
        L"\u8fdb\u5165\u76f4\u64ad\u95f4",
        L"\u6b22\u8fce\u6765\u5230\u76f4\u64ad\u95f4",
        L"\u6253\u8d4f\u793c\u7269",
        L"\u79c1\u4e0b\u4ea4\u6613",
        L"\u7406\u6027\u6d88\u8d39",
        L"\u8fdd\u89c4\u884c\u4e3a",
        L"\u8bf7\u53ca\u65f6\u6295\u8bc9",
        L"\u76f4\u64ad\u95f4\u5185\u7981\u6b62",
        L"\u76f4\u64ad\u53ca\u8fde\u9ea6\u65f6\u7981\u6b62",
        L"\u5e73\u53f0\u6d41\u91cf\u6276\u6301\u5df2\u5f00\u542f",
        L"\u4e3b\u64ad\u6210\u957f\u5361",
        L"\u5feb\u70b9\u51fb\u53bb\u4f7f\u7528\u5427",
        L"\u6682\u65e0\u89c2\u4f17\u9001\u793c",
    };
    for (const auto* token : kSystemNoticeTokens) {
        if (normalized.find(token) != std::wstring::npos) {
            return true;
        }
    }

    const size_t colon_pos = normalized.find(L':');
    const size_t fullwidth_colon_pos = normalized.find(L'\uff1a');
    const size_t separator = colon_pos != std::wstring::npos ? colon_pos : fullwidth_colon_pos;
    if (separator == std::wstring::npos &&
        normalized.starts_with(L"\u4e3b\u64ad") &&
        (normalized.find(L"\u5929") != std::wstring::npos ||
         normalized.find(L"\u5468") != std::wstring::npos ||
         normalized.find(L"\u6708") != std::wstring::npos ||
         normalized.find(L"\u5e74") != std::wstring::npos)) {
        return true;
    }

    return false;
}

bool IsUiDanmakuMessageText(const std::wstring& text) {
    const std::wstring cleaned = CleanUiDanmakuText(text);
    if (cleaned.empty()) {
        return false;
    }
    if (IsUiSystemNoticeText(cleaned)) {
        return false;
    }

    static const wchar_t* const kIgnoredTokens[] = {
        L"\u4e92\u52a8\u6d88\u606f",
        L"\u6dfb\u52a0\u8bc4\u8bba",
        L"\u53d6\u6d88\u6d6e\u7a97",
        L"\u9501\u5b9a",
        L"\u6682\u65e0\u89c2\u4f17\u9001\u793c",
        L"\u53d1\u9001",
    };
    for (const auto* token : kIgnoredTokens) {
        if (ContainsCaseInsensitive(cleaned, token)) {
            return false;
        }
    }

    const size_t colon_pos = cleaned.find(L'\uff1a');
    const size_t ascii_colon_pos = cleaned.find(L':');
    const size_t separator = colon_pos != std::wstring::npos ? colon_pos : ascii_colon_pos;
    if (separator != std::wstring::npos) {
        const std::wstring left = TrimWhitespace(cleaned.substr(0, separator));
        const std::wstring right = TrimWhitespace(cleaned.substr(separator + 1));
        return !left.empty() && !right.empty() && right.size() <= 48;
    }

    return IsUiGiftMessageText(cleaned);
}

bool IsUiTextDanmakuText(const std::wstring& text) {
    const std::wstring cleaned = CleanUiDanmakuText(text);
    const size_t colon_pos = cleaned.find(L'\uff1a');
    const size_t ascii_colon_pos = cleaned.find(L':');
    const size_t separator = colon_pos != std::wstring::npos ? colon_pos : ascii_colon_pos;
    if (separator == std::wstring::npos) {
        return false;
    }

    const std::wstring left = TrimWhitespace(cleaned.substr(0, separator));
    const std::wstring right = TrimWhitespace(cleaned.substr(separator + 1));
    return !left.empty() && !right.empty();
}

size_t FindUiLineOverlap(const std::vector<std::wstring>& previous_lines, const std::vector<std::wstring>& current_lines) {
    const size_t max_overlap = std::min(previous_lines.size(), current_lines.size());
    for (size_t overlap = max_overlap; overlap > 0; --overlap) {
        bool matched = true;
        for (size_t index = 0; index < overlap; ++index) {
            const auto& left = previous_lines[previous_lines.size() - overlap + index];
            const auto& right = current_lines[index];
            if (left != right) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return overlap;
        }
    }
    return 0;
}

std::vector<size_t> CollectUiFreshLineIndicesByLcs(
    const std::vector<std::wstring>& previous_lines,
    const std::vector<std::wstring>& current_lines) {
    std::vector<size_t> fresh_indices;
    if (previous_lines.empty() || current_lines.empty()) {
        return fresh_indices;
    }

    std::vector<std::vector<int>> lcs(
        previous_lines.size() + 1,
        std::vector<int>(current_lines.size() + 1, 0));
    for (size_t prev_index = 1; prev_index <= previous_lines.size(); ++prev_index) {
        for (size_t current_index = 1; current_index <= current_lines.size(); ++current_index) {
            if (!previous_lines[prev_index - 1].empty() &&
                previous_lines[prev_index - 1] == current_lines[current_index - 1]) {
                lcs[prev_index][current_index] = lcs[prev_index - 1][current_index - 1] + 1;
            } else {
                lcs[prev_index][current_index] =
                    std::max(lcs[prev_index - 1][current_index], lcs[prev_index][current_index - 1]);
            }
        }
    }

    std::vector<bool> matched_current(current_lines.size(), false);
    size_t prev_index = previous_lines.size();
    size_t current_index = current_lines.size();
    while (prev_index > 0 && current_index > 0) {
        if (!previous_lines[prev_index - 1].empty() &&
            previous_lines[prev_index - 1] == current_lines[current_index - 1] &&
            lcs[prev_index][current_index] == lcs[prev_index - 1][current_index - 1] + 1) {
            matched_current[current_index - 1] = true;
            --prev_index;
            --current_index;
            continue;
        }
        if (lcs[prev_index - 1][current_index] >= lcs[prev_index][current_index - 1]) {
            --prev_index;
        } else {
            --current_index;
        }
    }

    for (size_t index = 0; index < current_lines.size(); ++index) {
        if (!matched_current[index] && !current_lines[index].empty()) {
            fresh_indices.push_back(index);
        }
    }
    return fresh_indices;
}

struct UiDanmakuReadResult {
    std::wstring target_title;
    std::vector<std::wstring> lines;
    std::vector<std::wstring> compare_keys;
    std::wstring error;
};

struct UiElementSnapshot {
    CONTROLTYPEID control_type = 0;
    std::wstring class_name;
    RECT bounds{};
    std::wstring runtime_id;
    std::vector<std::wstring> texts;
};

struct UiCollectedLine {
    std::wstring text;
    std::wstring compare_key;
    RECT bounds{};
    bool is_gift_marker = false;
};

std::wstring ReadAccessibleName(IAccessible* accessible, VARIANT child);
std::wstring ReadAccessibleValue(IAccessible* accessible, VARIANT child);
std::wstring ReadAccessibleDescription(IAccessible* accessible, VARIANT child);
bool ReadAccessibleBounds(IAccessible* accessible, VARIANT child, RECT* bounds);
std::vector<std::wstring> ReadAccessibleTextCandidates(IAccessible* accessible, VARIANT child);
void CollectMsaaDanmakuLinesFromAccessible(
    IAccessible* accessible,
    int depth,
    size_t max_nodes,
    size_t* visited_nodes,
    std::vector<UiCollectedLine>* lines);
std::vector<UiCollectedLine> ReadMsaaDanmakuLines(HWND target_window);

std::wstring ReadElementName(IUIAutomationElement* element);
std::wstring ReadElementClassName(IUIAutomationElement* element);
std::wstring ReadElementRuntimeId(IUIAutomationElement* element);
std::wstring ControlTypeLabel(CONTROLTYPEID control_type);
bool IsIgnoredUiProbeText(const std::wstring& text);
int ScoreUiProbeText(const std::wstring& text, CONTROLTYPEID control_type);
void AppendUniqueUiText(std::vector<std::wstring>* texts, const std::wstring& text);
std::vector<std::wstring> ReadElementTextCandidates(IUIAutomationElement* element);
bool CollectUiElementSnapshots(
    IUIAutomation* automation,
    IUIAutomationElement* root,
    size_t max_elements,
    std::vector<UiElementSnapshot>* snapshots);
bool IsUiGiftCounterText(const std::wstring& text);
std::wstring NormalizeUiGiftCounterText(const std::wstring& text);
std::wstring BuildUiGiftMarkerCompareKey(const RECT& bounds, const std::wstring& marker_text);
bool IsUiGiftMarkerLineInGiftBand(const UiCollectedLine& line, int first_text_top, const RECT& window_bounds);

int BuildUiReadDedupeBucket(const RECT& bounds) {
    const bool valid = bounds.bottom > bounds.top && bounds.right > bounds.left;
    if (!valid) {
        return std::numeric_limits<int>::min();
    }
    const int center_y = bounds.top + ((bounds.bottom - bounds.top) / 2);
    return center_y / kUiReadDedupeBucketPx;
}

std::wstring BuildUiReadDedupeKey(const std::wstring& normalized_text, const RECT& bounds) {
    if (normalized_text.empty()) {
        return {};
    }
    const int bucket = BuildUiReadDedupeBucket(bounds);
    if (bucket == std::numeric_limits<int>::min()) {
        return normalized_text;
    }
    return normalized_text + L"@" + std::to_wstring(bucket);
}

bool HasValidBounds(const RECT& bounds) {
    return bounds.right > bounds.left && bounds.bottom > bounds.top;
}

std::wstring BuildUiCompareBaseKey(const UiElementSnapshot&, const std::wstring& normalized_text) {
    if (!normalized_text.empty()) {
        return L"text:" + normalized_text;
    }
    return {};
}

std::vector<std::wstring> BuildUiOccurrenceKeys(const std::vector<std::wstring>& base_keys) {
    std::vector<std::wstring> occurrence_keys;
    occurrence_keys.reserve(base_keys.size());

    std::vector<std::pair<std::wstring, size_t>> seen_counts;
    seen_counts.reserve(base_keys.size());
    for (const auto& raw_key : base_keys) {
        const std::wstring key = TrimWhitespace(raw_key);
        if (key.empty()) {
            occurrence_keys.push_back({});
            continue;
        }

        size_t occurrence = 1;
        auto it = std::find_if(seen_counts.begin(), seen_counts.end(), [&key](const auto& entry) {
            return entry.first == key;
        });
        if (it == seen_counts.end()) {
            seen_counts.emplace_back(key, 1);
        } else {
            it->second += 1;
            occurrence = it->second;
        }

        occurrence_keys.push_back(
            occurrence <= 1 ? key : (key + L"#" + std::to_wstring(occurrence)));
    }
    return occurrence_keys;
}

std::vector<std::wstring> BuildUiOccurrenceKeys(const std::deque<std::wstring>& base_keys) {
    std::vector<std::wstring> keys(base_keys.begin(), base_keys.end());
    return BuildUiOccurrenceKeys(keys);
}

UiDanmakuReadResult ReadUiDanmakuWindow(HWND target_window) {
    UiDanmakuReadResult result{};
    result.target_title = ReadWindowText(target_window);
    if (result.target_title.empty()) {
        result.target_title = ReadWindowClassName(target_window);
    }
    if (target_window == nullptr || !IsWindow(target_window)) {
        result.error = L"UIA \u76d1\u542c\u6ca1\u6293\u5230\u6709\u6548\u7a97\u53e3";
        return result;
    }

    const std::wstring target_class_name = ReadWindowClassName(target_window);
    const bool prefer_msaa =
        target_class_name == L"FinderLiveCommentFloatWnd" ||
        (ContainsCaseInsensitive(result.target_title, L"\u4e92\u52a8\u6d88\u606f") &&
            ContainsCaseInsensitive(target_class_name, L"FinderLive"));
    auto try_read_msaa = [&result, target_window]() -> bool {
        const auto msaa_lines = ReadMsaaDanmakuLines(target_window);
        if (msaa_lines.empty()) {
            return false;
        }

        result.lines.clear();
        result.compare_keys.clear();
        result.lines.reserve(msaa_lines.size());
        result.compare_keys.reserve(msaa_lines.size());
        for (const auto& line : msaa_lines) {
            result.lines.push_back(line.text);
            result.compare_keys.push_back(line.compare_key);
        }
        if (result.lines.size() > kMaxUiLiveVisibleLines) {
            const auto remove_count = result.lines.size() - kMaxUiLiveVisibleLines;
            result.lines.erase(
                result.lines.begin(),
                result.lines.begin() + static_cast<std::ptrdiff_t>(remove_count));
            result.compare_keys.erase(
                result.compare_keys.begin(),
                result.compare_keys.begin() + static_cast<std::ptrdiff_t>(remove_count));
        }
        return true;
    };
    if (prefer_msaa && try_read_msaa()) {
        return result;
    }

    const HRESULT apartment_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment_result) && apartment_result != RPC_E_CHANGED_MODE) {
        result.error = L"UIA COM \u521d\u59cb\u5316\u5931\u8d25";
        return result;
    }

    winrt::com_ptr<IUIAutomation> automation;
    const HRESULT automation_result = CoCreateInstance(
        CLSID_CUIAutomation,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(automation.put()));
    if (FAILED(automation_result) || !automation) {
        result.error = L"\u65e0\u6cd5\u521b\u5efa UIA \u76d1\u542c\u5668";
        if (SUCCEEDED(apartment_result)) {
            CoUninitialize();
        }
        return result;
    }

    winrt::com_ptr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(target_window, root.put())) || !root) {
        result.error = L"\u65e0\u6cd5\u8bfb\u53d6\u4e92\u52a8\u6d88\u606f\u7a97\u53e3\u8282\u70b9";
        if (SUCCEEDED(apartment_result)) {
            CoUninitialize();
        }
        return result;
    }

    winrt::com_ptr<IUIAutomationCondition> true_condition;
    winrt::com_ptr<IUIAutomationElementArray> elements;
    if (FAILED(automation->CreateTrueCondition(true_condition.put())) ||
        !true_condition ||
        FAILED(root->FindAll(TreeScope_Subtree, true_condition.get(), elements.put())) ||
        !elements) {
        result.error = L"UIA \u76d1\u542c\u63a7\u4ef6\u6811\u8bfb\u53d6\u5931\u8d25";
        if (SUCCEEDED(apartment_result)) {
            CoUninitialize();
        }
        return result;
    }
    int length = 0;
    elements->get_Length(&length);
    std::vector<std::wstring> list_item_lines;
    std::vector<std::wstring> list_item_keys;
    std::vector<std::wstring> text_lines;
    std::vector<std::wstring> text_keys;
    list_item_lines.reserve(64);
    list_item_keys.reserve(64);
    text_lines.reserve(64);
    text_keys.reserve(64);
    for (int index = 0; index < length && index < 1200; ++index) {
        winrt::com_ptr<IUIAutomationElement> element;
        if (FAILED(elements->GetElement(index, element.put())) || !element) {
            continue;
        }

        CONTROLTYPEID control_type_id = 0;
        element->get_CurrentControlType(&control_type_id);
        if (control_type_id != UIA_ListItemControlTypeId &&
            control_type_id != UIA_TextControlTypeId) {
            continue;
        }

        const std::wstring text = CleanUiDanmakuText(ReadElementName(element.get()));
        if (!IsUiDanmakuMessageText(text)) {
            continue;
        }

        const std::wstring compare_key = L"text:" + NormalizeUiCompareText(text);
        if (control_type_id == UIA_ListItemControlTypeId) {
            list_item_lines.push_back(text);
            list_item_keys.push_back(compare_key);
        } else {
            text_lines.push_back(text);
            text_keys.push_back(compare_key);
        }
    }

    std::vector<std::wstring> best_lines;
    std::vector<std::wstring> best_keys;
    if (!list_item_lines.empty()) {
        best_lines = std::move(list_item_lines);
        best_keys = std::move(list_item_keys);
    } else {
        best_lines = std::move(text_lines);
        best_keys = std::move(text_keys);
    }

    if (best_lines.empty() && !prefer_msaa) {
        const auto msaa_lines = ReadMsaaDanmakuLines(target_window);
        if (!msaa_lines.empty()) {
            best_lines.reserve(msaa_lines.size());
            best_keys.reserve(msaa_lines.size());
            for (const auto& line : msaa_lines) {
                best_lines.push_back(line.text);
                best_keys.push_back(line.compare_key);
            }
        }
    }

    result.lines = std::move(best_lines);
    result.compare_keys = std::move(best_keys);

    if (result.lines.size() > kMaxUiLiveVisibleLines) {
        const auto remove_count = result.lines.size() - kMaxUiLiveVisibleLines;
        result.lines.erase(
            result.lines.begin(),
            result.lines.begin() + static_cast<std::ptrdiff_t>(remove_count));
        result.compare_keys.erase(
            result.compare_keys.begin(),
            result.compare_keys.begin() + static_cast<std::ptrdiff_t>(remove_count));
    }

    if (SUCCEEDED(apartment_result)) {
        CoUninitialize();
    }
    return result;
}

std::wstring TakeBstrAndFree(BSTR value) {
    if (value == nullptr) {
        return {};
    }
    std::wstring text(value, SysStringLen(value));
    SysFreeString(value);
    return TrimWhitespace(text);
}

std::wstring ReadElementName(IUIAutomationElement* element) {
    if (element == nullptr) {
        return {};
    }
    BSTR value = nullptr;
    if (FAILED(element->get_CurrentName(&value))) {
        return {};
    }
    return TakeBstrAndFree(value);
}

std::wstring ReadElementClassName(IUIAutomationElement* element) {
    if (element == nullptr) {
        return {};
    }
    BSTR value = nullptr;
    if (FAILED(element->get_CurrentClassName(&value))) {
        return {};
    }
    return TakeBstrAndFree(value);
}

std::wstring ReadElementRuntimeId(IUIAutomationElement* element) {
    if (element == nullptr) {
        return {};
    }

    SAFEARRAY* runtime_id = nullptr;
    if (FAILED(element->GetRuntimeId(&runtime_id)) || runtime_id == nullptr) {
        return {};
    }

    LONG lower_bound = 0;
    LONG upper_bound = -1;
    if (FAILED(SafeArrayGetLBound(runtime_id, 1, &lower_bound)) ||
        FAILED(SafeArrayGetUBound(runtime_id, 1, &upper_bound)) ||
        upper_bound < lower_bound) {
        SafeArrayDestroy(runtime_id);
        return {};
    }

    std::wostringstream stream;
    for (LONG index = lower_bound; index <= upper_bound; ++index) {
        int value = 0;
        if (FAILED(SafeArrayGetElement(runtime_id, &index, &value))) {
            continue;
        }
        if (stream.tellp() > 0) {
            stream << L'.';
        }
        stream << value;
    }

    SafeArrayDestroy(runtime_id);
    return stream.str();
}

std::wstring ReadElementHelpText(IUIAutomationElement* element) {
    if (element == nullptr) {
        return {};
    }
    BSTR value = nullptr;
    if (FAILED(element->get_CurrentHelpText(&value))) {
        return {};
    }
    return TakeBstrAndFree(value);
}

std::wstring ReadElementItemStatus(IUIAutomationElement* element) {
    if (element == nullptr) {
        return {};
    }
    BSTR value = nullptr;
    if (FAILED(element->get_CurrentItemStatus(&value))) {
        return {};
    }
    return TakeBstrAndFree(value);
}

std::wstring TakeAccessibleBstrAndFree(BSTR value) {
    if (value == nullptr) {
        return {};
    }
    std::wstring text(value, SysStringLen(value));
    SysFreeString(value);
    return TrimWhitespace(text);
}

std::wstring ReadAccessibleName(IAccessible* accessible, VARIANT child) {
    if (accessible == nullptr) {
        return {};
    }
    BSTR value = nullptr;
    if (FAILED(accessible->get_accName(child, &value))) {
        return {};
    }
    return TakeAccessibleBstrAndFree(value);
}

std::wstring ReadAccessibleValue(IAccessible* accessible, VARIANT child) {
    if (accessible == nullptr) {
        return {};
    }
    BSTR value = nullptr;
    if (FAILED(accessible->get_accValue(child, &value))) {
        return {};
    }
    return TakeAccessibleBstrAndFree(value);
}

std::wstring ReadAccessibleDescription(IAccessible* accessible, VARIANT child) {
    if (accessible == nullptr) {
        return {};
    }
    BSTR value = nullptr;
    if (FAILED(accessible->get_accDescription(child, &value))) {
        return {};
    }
    return TakeAccessibleBstrAndFree(value);
}

bool ReadAccessibleBounds(IAccessible* accessible, VARIANT child, RECT* bounds) {
    if (accessible == nullptr || bounds == nullptr) {
        return false;
    }

    long left = 0;
    long top = 0;
    long width = 0;
    long height = 0;
    if (FAILED(accessible->accLocation(&left, &top, &width, &height, child))) {
        return false;
    }
    if (width <= 0 || height <= 0) {
        return false;
    }

    bounds->left = left;
    bounds->top = top;
    bounds->right = left + width;
    bounds->bottom = top + height;
    return true;
}

std::vector<std::wstring> ReadAccessibleTextCandidates(IAccessible* accessible, VARIANT child) {
    std::vector<std::wstring> texts;
    AppendUniqueUiText(&texts, ReadAccessibleName(accessible, child));
    AppendUniqueUiText(&texts, ReadAccessibleValue(accessible, child));
    AppendUniqueUiText(&texts, ReadAccessibleDescription(accessible, child));
    return texts;
}

std::wstring NormalizeUiGiftCounterText(const std::wstring& text) {
    std::wstring normalized;
    normalized.reserve(text.size());
    for (wchar_t ch : TrimWhitespace(text)) {
        if (std::iswspace(ch)) {
            continue;
        }
        if (ch >= L'０' && ch <= L'９') {
            normalized.push_back(static_cast<wchar_t>(L'0' + (ch - L'０')));
            continue;
        }
        if (ch == L'×' || ch == L'X') {
            normalized.push_back(L'x');
            continue;
        }
        normalized.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }
    return normalized;
}

bool IsUiGiftCounterText(const std::wstring& text) {
    const std::wstring normalized = NormalizeUiGiftCounterText(text);
    if (normalized.size() < 2 || normalized[0] != L'x') {
        return false;
    }

    bool has_digit = false;
    for (size_t index = 1; index < normalized.size(); ++index) {
        if (!std::iswdigit(normalized[index])) {
            return false;
        }
        has_digit = true;
    }
    return has_digit;
}

std::wstring BuildUiGiftMarkerCompareKey(const RECT& bounds, const std::wstring& marker_text) {
    const std::wstring normalized_marker = NormalizeUiGiftCounterText(marker_text);
    if (normalized_marker.empty()) {
        return {};
    }

    const std::wstring vertical_key = BuildUiReadDedupeKey(normalized_marker, bounds);
    if (!HasValidBounds(bounds)) {
        return L"gift:" + vertical_key;
    }

    const int center_x = bounds.left + ((bounds.right - bounds.left) / 2);
    const int horizontal_bucket = center_x / 40;
    return L"gift:" + vertical_key + L":" + std::to_wstring(horizontal_bucket);
}

bool IsUiGiftMarkerLineInGiftBand(const UiCollectedLine& line, int first_text_top, const RECT& window_bounds) {
    if (!line.is_gift_marker || !HasValidBounds(line.bounds)) {
        return false;
    }

    const int width = line.bounds.right - line.bounds.left;
    const int height = line.bounds.bottom - line.bounds.top;
    if (width < 10 || width > 160 || height < 12 || height > 96) {
        return false;
    }

    if (first_text_top != std::numeric_limits<int>::max()) {
        return line.bounds.top + 8 < first_text_top;
    }

    if (!HasValidBounds(window_bounds)) {
        return true;
    }

    const int window_height = window_bounds.bottom - window_bounds.top;
    if (window_height <= 0) {
        return true;
    }

    const int relative_top = line.bounds.top - window_bounds.top;
    return relative_top >= 0 && relative_top < (window_height * 3 / 4);
}

int ScoreDanmakuCandidateText(const std::wstring& text) {
    if (text.empty()) {
        return std::numeric_limits<int>::min();
    }

    int score = 0;
    if (IsUiTextDanmakuText(text)) {
        score += 40;
    }
    if (IsUiGiftMessageText(text)) {
        score += 28;
    }
    if (ContainsChinese(text)) {
        score += 6;
    }
    if (text.size() >= 4 && text.size() <= 96) {
        score += 4;
    }
    return score;
}

bool TryAppendBestAccessibleDanmakuLine(
    const std::vector<std::wstring>& texts,
    const RECT& bounds,
    std::vector<UiCollectedLine>* lines) {
    if (lines == nullptr || texts.empty()) {
        return false;
    }

    int best_score = std::numeric_limits<int>::min();
    std::wstring best_text;
    for (const auto& raw_text : texts) {
        const std::wstring cleaned = CleanUiDanmakuText(raw_text);
        // MSAA 会暴露很多窗口说明文本，这里只接收强特征的真实弹幕/礼物文本。
        const bool strong_text = IsUiTextDanmakuText(cleaned);
        const bool gift_text = IsUiGiftMessageText(cleaned);
        if (!strong_text && !gift_text) {
            continue;
        }
        const int score = ScoreDanmakuCandidateText(cleaned);
        if (score > best_score) {
            best_score = score;
            best_text = cleaned;
        }
    }

    if (best_text.empty()) {
        return false;
    }

    UiCollectedLine line{};
    line.text = std::move(best_text);
    line.compare_key = L"text:" + NormalizeUiCompareText(line.text);
    line.bounds = bounds;
    lines->push_back(std::move(line));
    return true;
}

bool TryAppendAccessibleGiftMarkerLine(
    const std::vector<std::wstring>& texts,
    const RECT& bounds,
    std::vector<UiCollectedLine>* lines) {
    if (lines == nullptr || texts.empty() || !HasValidBounds(bounds)) {
        return false;
    }

    std::wstring marker_text;
    for (const auto& raw_text : texts) {
        const std::wstring cleaned = TrimWhitespace(raw_text);
        if (IsUiGiftCounterText(cleaned)) {
            marker_text = cleaned;
            break;
        }
    }
    if (marker_text.empty()) {
        return false;
    }

    UiCollectedLine line{};
    line.text = L"有礼物";
    line.compare_key = BuildUiGiftMarkerCompareKey(bounds, marker_text);
    line.bounds = bounds;
    line.is_gift_marker = true;
    lines->push_back(std::move(line));
    return true;
}

void CollectMsaaDanmakuLinesFromAccessible(
    IAccessible* accessible,
    int depth,
    size_t max_nodes,
    size_t* visited_nodes,
    std::vector<UiCollectedLine>* lines) {
    if (accessible == nullptr ||
        lines == nullptr ||
        visited_nodes == nullptr ||
        depth > 14 ||
        *visited_nodes >= max_nodes ||
        lines->size() >= kMaxUiLiveVisibleLines) {
        return;
    }

    VARIANT self{};
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    RECT self_bounds{};
    ReadAccessibleBounds(accessible, self, &self_bounds);
    const auto self_texts = ReadAccessibleTextCandidates(accessible, self);
    if (!TryAppendBestAccessibleDanmakuLine(self_texts, self_bounds, lines)) {
        TryAppendAccessibleGiftMarkerLine(self_texts, self_bounds, lines);
    }
    if (lines->size() >= kMaxUiLiveVisibleLines) {
        return;
    }

    long child_count = 0;
    if (FAILED(accessible->get_accChildCount(&child_count)) || child_count <= 0) {
        return;
    }

    std::vector<VARIANT> children(static_cast<size_t>(child_count));
    for (auto& child : children) {
        VariantInit(&child);
    }

    long obtained = 0;
    const HRESULT children_result = AccessibleChildren(
        accessible,
        0,
        child_count,
        children.data(),
        &obtained);
    if (FAILED(children_result) || obtained <= 0) {
        for (auto& child : children) {
            VariantClear(&child);
        }
        return;
    }

    for (long index = 0; index < obtained && *visited_nodes < max_nodes; ++index) {
        VARIANT& child = children[static_cast<size_t>(index)];
        *visited_nodes += 1;

        if (child.vt == VT_DISPATCH && child.pdispVal != nullptr) {
            winrt::com_ptr<IAccessible> child_accessible;
            child.pdispVal->QueryInterface(IID_PPV_ARGS(child_accessible.put()));
            if (child_accessible) {
                CollectMsaaDanmakuLinesFromAccessible(
                    child_accessible.get(),
                    depth + 1,
                    max_nodes,
                    visited_nodes,
                    lines);
            }
        } else if (child.vt == VT_I4) {
            RECT child_bounds{};
            ReadAccessibleBounds(accessible, child, &child_bounds);
            const auto child_texts = ReadAccessibleTextCandidates(accessible, child);
            if (!TryAppendBestAccessibleDanmakuLine(child_texts, child_bounds, lines)) {
                TryAppendAccessibleGiftMarkerLine(child_texts, child_bounds, lines);
            }
        }

        VariantClear(&child);
        if (lines->size() >= kMaxUiLiveVisibleLines) {
            break;
        }
    }
}

std::vector<UiCollectedLine> ReadMsaaDanmakuLines(HWND target_window) {
    std::vector<UiCollectedLine> lines;
    if (target_window == nullptr || !IsWindow(target_window)) {
        return lines;
    }

    winrt::com_ptr<IAccessible> accessible;
    HRESULT result = AccessibleObjectFromWindow(
        target_window,
        OBJID_WINDOW,
        IID_IAccessible,
        reinterpret_cast<void**>(accessible.put()));
    if (FAILED(result) || !accessible) {
        result = AccessibleObjectFromWindow(
            target_window,
            OBJID_CLIENT,
            IID_IAccessible,
            reinterpret_cast<void**>(accessible.put()));
        if (FAILED(result) || !accessible) {
            return lines;
        }
    }

    size_t visited_nodes = 0;
    CollectMsaaDanmakuLinesFromAccessible(
        accessible.get(),
        0,
        2400,
        &visited_nodes,
        &lines);

    RECT window_bounds{};
    if (!GetWindowRect(target_window, &window_bounds)) {
        window_bounds = RECT{};
    }

    int first_text_top = std::numeric_limits<int>::max();
    for (const auto& line : lines) {
        if (line.is_gift_marker || !HasValidBounds(line.bounds)) {
            continue;
        }
        if (IsUiTextDanmakuText(line.text)) {
            first_text_top = std::min(first_text_top, static_cast<int>(line.bounds.top));
        }
    }

    std::vector<UiCollectedLine> filtered_lines;
    filtered_lines.reserve(lines.size());
    for (const auto& line : lines) {
        if (line.is_gift_marker &&
            !IsUiGiftMarkerLineInGiftBand(line, first_text_top, window_bounds)) {
            continue;
        }

        const bool duplicated = std::any_of(
            filtered_lines.begin(),
            filtered_lines.end(),
            [&line](const UiCollectedLine& existing) {
                return !line.compare_key.empty() && existing.compare_key == line.compare_key;
            });
        if (duplicated) {
            continue;
        }
        filtered_lines.push_back(line);
    }

    std::stable_sort(
        filtered_lines.begin(),
        filtered_lines.end(),
        [](const UiCollectedLine& left, const UiCollectedLine& right) {
            const bool left_valid = HasValidBounds(left.bounds);
            const bool right_valid = HasValidBounds(right.bounds);
            if (left_valid != right_valid) {
                return left_valid;
            }
            if (left_valid && right_valid) {
                if (left.bounds.top != right.bounds.top) {
                    return left.bounds.top < right.bounds.top;
                }
                if (left.bounds.left != right.bounds.left) {
                    return left.bounds.left < right.bounds.left;
                }
            }
            return left.compare_key < right.compare_key;
        });

    lines = std::move(filtered_lines);

    return lines;
}

void AppendUniqueUiText(std::vector<std::wstring>* texts, const std::wstring& text) {
    if (texts == nullptr) {
        return;
    }
    const std::wstring trimmed = TrimWhitespace(text);
    if (trimmed.empty()) {
        return;
    }
    if (std::find(texts->begin(), texts->end(), trimmed) != texts->end()) {
        return;
    }
    texts->push_back(trimmed);
}

std::vector<std::wstring> ReadElementTextCandidates(IUIAutomationElement* element) {
    std::vector<std::wstring> texts;
    if (element == nullptr) {
        return texts;
    }

    AppendUniqueUiText(&texts, ReadElementName(element));
    AppendUniqueUiText(&texts, ReadElementHelpText(element));
    AppendUniqueUiText(&texts, ReadElementItemStatus(element));

    winrt::com_ptr<IUIAutomationValuePattern> value_pattern;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(value_pattern.put()))) &&
        value_pattern) {
        BSTR value = nullptr;
        if (SUCCEEDED(value_pattern->get_CurrentValue(&value))) {
            AppendUniqueUiText(&texts, TakeBstrAndFree(value));
        }
    }

    winrt::com_ptr<IUIAutomationLegacyIAccessiblePattern> legacy_pattern;
    if (SUCCEEDED(element->GetCurrentPatternAs(
            UIA_LegacyIAccessiblePatternId,
            IID_PPV_ARGS(legacy_pattern.put()))) &&
        legacy_pattern) {
        BSTR legacy_name = nullptr;
        BSTR legacy_value = nullptr;
        std::wstring legacy_name_text;
        std::wstring legacy_value_text;
        if (SUCCEEDED(legacy_pattern->get_CurrentName(&legacy_name))) {
            legacy_name_text = TakeBstrAndFree(legacy_name);
            AppendUniqueUiText(&texts, legacy_name_text);
        }
        if (SUCCEEDED(legacy_pattern->get_CurrentValue(&legacy_value))) {
            legacy_value_text = TakeBstrAndFree(legacy_value);
            AppendUniqueUiText(&texts, legacy_value_text);
        }
        if (!legacy_name_text.empty() && !legacy_value_text.empty()) {
            const bool has_separator = legacy_value_text.find(L'\uff1a') != std::wstring::npos ||
                legacy_value_text.find(L':') != std::wstring::npos;
            AppendUniqueUiText(
                &texts,
                has_separator
                    ? (legacy_name_text + legacy_value_text)
                    : (legacy_name_text + L"\uff1a" + legacy_value_text));
        }
    }

    return texts;
}

void AppendUiSnapshotFromElement(
    IUIAutomationElement* element,
    std::vector<UiElementSnapshot>* snapshots,
    size_t max_elements) {
    if (element == nullptr || snapshots == nullptr || snapshots->size() >= max_elements) {
        return;
    }

    UiElementSnapshot snapshot{};
    element->get_CurrentControlType(&snapshot.control_type);
    snapshot.class_name = ReadElementClassName(element);
    element->get_CurrentBoundingRectangle(&snapshot.bounds);
    snapshot.runtime_id = ReadElementRuntimeId(element);
    snapshot.texts = ReadElementTextCandidates(element);
    if (!snapshot.texts.empty()) {
        snapshots->push_back(std::move(snapshot));
    }
}

void CollectUiElementSnapshotsRawRecursive(
    IUIAutomationTreeWalker* walker,
    IUIAutomationElement* parent,
    int depth,
    size_t max_elements,
    size_t* visited_nodes,
    std::vector<UiElementSnapshot>* snapshots) {
    if (walker == nullptr ||
        parent == nullptr ||
        snapshots == nullptr ||
        visited_nodes == nullptr ||
        depth > 24 ||
        *visited_nodes >= 2400 ||
        snapshots->size() >= max_elements) {
        return;
    }

    winrt::com_ptr<IUIAutomationElement> child;
    if (FAILED(walker->GetFirstChildElement(parent, child.put())) || !child) {
        return;
    }

    while (child && *visited_nodes < 2400 && snapshots->size() < max_elements) {
        *visited_nodes += 1;
        AppendUiSnapshotFromElement(child.get(), snapshots, max_elements);
        CollectUiElementSnapshotsRawRecursive(
            walker,
            child.get(),
            depth + 1,
            max_elements,
            visited_nodes,
            snapshots);

        winrt::com_ptr<IUIAutomationElement> sibling;
        if (FAILED(walker->GetNextSiblingElement(child.get(), sibling.put()))) {
            break;
        }
        child = std::move(sibling);
    }
}

bool CollectUiElementSnapshots(
    IUIAutomation* automation,
    IUIAutomationElement* root,
    size_t max_elements,
    std::vector<UiElementSnapshot>* snapshots) {
    if (snapshots == nullptr) {
        return false;
    }
    snapshots->clear();
    if (automation == nullptr || root == nullptr || max_elements == 0) {
        return false;
    }

    bool tree_read_ok = false;
    winrt::com_ptr<IUIAutomationCondition> true_condition;
    winrt::com_ptr<IUIAutomationElementArray> elements;
    if (SUCCEEDED(automation->CreateTrueCondition(true_condition.put())) &&
        true_condition &&
        SUCCEEDED(root->FindAll(TreeScope_Descendants, true_condition.get(), elements.put())) &&
        elements) {
        tree_read_ok = true;
        int length = 0;
        elements->get_Length(&length);
        for (int index = 0; index < length && snapshots->size() < max_elements; ++index) {
            winrt::com_ptr<IUIAutomationElement> element;
            if (FAILED(elements->GetElement(index, element.put())) || !element) {
                continue;
            }
            AppendUiSnapshotFromElement(element.get(), snapshots, max_elements);
        }
    }

    if (snapshots->size() >= 3) {
        std::stable_sort(snapshots->begin(), snapshots->end(), [](const UiElementSnapshot& left, const UiElementSnapshot& right) {
            const bool left_valid = left.bounds.bottom > left.bounds.top && left.bounds.right > left.bounds.left;
            const bool right_valid = right.bounds.bottom > right.bounds.top && right.bounds.right > right.bounds.left;
            if (left_valid != right_valid) {
                return left_valid && !right_valid;
            }
            if (!left_valid && !right_valid) {
                return false;
            }
            if (left.bounds.top != right.bounds.top) {
                return left.bounds.top < right.bounds.top;
            }
            if (left.bounds.left != right.bounds.left) {
                return left.bounds.left < right.bounds.left;
            }
            return left.control_type < right.control_type;
        });
        return true;
    }

    winrt::com_ptr<IUIAutomationTreeWalker> raw_walker;
    if (SUCCEEDED(automation->get_RawViewWalker(raw_walker.put())) && raw_walker) {
        tree_read_ok = true;
        size_t visited_nodes = 0;
        CollectUiElementSnapshotsRawRecursive(
            raw_walker.get(),
            root,
            0,
            max_elements,
            &visited_nodes,
            snapshots);
    }
    std::stable_sort(snapshots->begin(), snapshots->end(), [](const UiElementSnapshot& left, const UiElementSnapshot& right) {
        const bool left_valid = left.bounds.bottom > left.bounds.top && left.bounds.right > left.bounds.left;
        const bool right_valid = right.bounds.bottom > right.bounds.top && right.bounds.right > right.bounds.left;
        if (left_valid != right_valid) {
            return left_valid && !right_valid;
        }
        if (!left_valid && !right_valid) {
            return false;
        }
        if (left.bounds.top != right.bounds.top) {
            return left.bounds.top < right.bounds.top;
        }
        if (left.bounds.left != right.bounds.left) {
            return left.bounds.left < right.bounds.left;
        }
        return left.control_type < right.control_type;
    });
    return tree_read_ok;
}

std::wstring ControlTypeLabel(CONTROLTYPEID control_type) {
    switch (control_type) {
    case UIA_TextControlTypeId:
        return L"\u6587\u672c";
    case UIA_ListItemControlTypeId:
        return L"\u5217\u8868\u9879";
    case UIA_ListControlTypeId:
        return L"\u5217\u8868";
    case UIA_DocumentControlTypeId:
        return L"\u6587\u6863";
    case UIA_CustomControlTypeId:
        return L"\u81ea\u5b9a\u4e49";
    case UIA_GroupControlTypeId:
        return L"\u5206\u7ec4";
    case UIA_EditControlTypeId:
        return L"\u8f93\u5165\u6846";
    case UIA_ButtonControlTypeId:
        return L"\u6309\u94ae";
    case UIA_TabItemControlTypeId:
        return L"\u6807\u7b7e";
    default:
        return L"\u63a7\u4ef6";
    }
}

bool IsIgnoredUiProbeText(const std::wstring& text) {
    if (text.empty()) {
        return true;
    }

    static const wchar_t* const kIgnoredTokens[] = {
        L"\u6700\u5c0f\u5316",
        L"\u5173\u95ed",
        L"\u7b49\u5f85\u8fde\u63a5",
        L"\u753b\u9762",
        L"\u8bed\u97f3",
        L"\u8bf4\u660e",
        L"\u5f39\u5e55",
        L"\u73b0\u5728\u5f00\u542f",
        L"\u6253\u5f00",
        L"\u67e5\u770b",
        L"\u5237\u65b0",
        L"\u505c\u6b62",
        L"\u672a\u5f00\u542f",
        L"\u7a7a\u95f2",
        L"AirPlay",
    };

    for (const auto* token : kIgnoredTokens) {
        if (ContainsCaseInsensitive(text, token)) {
            return true;
        }
    }
    return false;
}

int ScoreUiProbeText(const std::wstring& text, CONTROLTYPEID control_type) {
    if (text.empty() || text.size() > 96) {
        return -100;
    }

    int score = 0;
    if (ContainsChinese(text)) {
        score += 4;
    }
    if (text.size() >= 4 && text.size() <= 40) {
        score += 3;
    } else if (text.size() >= 2) {
        score += 1;
    }

    switch (control_type) {
    case UIA_TextControlTypeId:
    case UIA_ListItemControlTypeId:
    case UIA_ListControlTypeId:
    case UIA_DocumentControlTypeId:
    case UIA_CustomControlTypeId:
        score += 3;
        break;
    case UIA_GroupControlTypeId:
    case UIA_EditControlTypeId:
        score += 1;
        break;
    case UIA_ButtonControlTypeId:
    case UIA_TabItemControlTypeId:
    case UIA_MenuItemControlTypeId:
        score -= 3;
        break;
    default:
        break;
    }

    if (IsIgnoredUiProbeText(text)) {
        score -= 5;
    }
    return score;
}

RECT BuildRectFromPoints(POINT start, POINT current) {
    RECT rect{};
    rect.left = std::min(start.x, current.x);
    rect.top = std::min(start.y, current.y);
    rect.right = std::max(start.x, current.x);
    rect.bottom = std::max(start.y, current.y);
    return rect;
}

std::string ReadUtf8File(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) {
        return {};
    }

    std::string data;
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    if (length > 0) {
        data.resize(static_cast<size_t>(length));
        fseek(file, 0, SEEK_SET);
        fread(data.data(), 1, data.size(), file);
    }
    fclose(file);

    if (data.size() >= 3 &&
        static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data.erase(0, 3);
    }
    return data;
}

bool WriteUtf8File(const std::wstring& path, const std::string& data) {
    if (path.empty()) {
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
        return false;
    }

    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    fwrite(bom, 1, sizeof(bom), file);
    if (!data.empty()) {
        fwrite(data.data(), 1, data.size(), file);
    }
    fclose(file);
    return true;
}

bool RegisterOverlayWindowClass() {
    static bool registered = false;
    static bool attempted = false;
    if (attempted) {
        return registered;
    }
    attempted = true;

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) -> LRESULT {
        auto* context = reinterpret_cast<OverlaySelectionContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
            context = reinterpret_cast<OverlaySelectionContext*>(create_struct->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
            return TRUE;
        }

        switch (message) {
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        case WM_LBUTTONDOWN:
            if (context != nullptr) {
                context->dragging = true;
                context->has_selection = true;
                context->cancelled = false;
                context->start.x = GET_X_LPARAM(l_param);
                context->start.y = GET_Y_LPARAM(l_param);
                context->current = context->start;
                context->selection = BuildRectFromPoints(context->start, context->current);
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (context != nullptr && context->dragging) {
                context->current.x = GET_X_LPARAM(l_param);
                context->current.y = GET_Y_LPARAM(l_param);
                context->selection = BuildRectFromPoints(context->start, context->current);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (context != nullptr && context->dragging) {
                context->dragging = false;
                ReleaseCapture();
                context->current.x = GET_X_LPARAM(l_param);
                context->current.y = GET_Y_LPARAM(l_param);
                context->selection = BuildRectFromPoints(context->start, context->current);
                if ((context->selection.right - context->selection.left) >= kMinSelectionSize &&
                    (context->selection.bottom - context->selection.top) >= kMinSelectionSize) {
                    context->finished = true;
                    context->cancelled = false;
                    DestroyWindow(hwnd);
                } else {
                    context->has_selection = false;
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
            }
            return 0;
        case WM_KEYDOWN:
            if (w_param == VK_ESCAPE) {
                if (context != nullptr) {
                    context->finished = true;
                    context->cancelled = true;
                }
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        case WM_RBUTTONUP:
            if (context != nullptr) {
                context->finished = true;
                context->cancelled = true;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
        }

        if (message == WM_PAINT) {
            PAINTSTRUCT paint_struct{};
            const HDC dc = BeginPaint(hwnd, &paint_struct);

            RECT client_rect{};
            GetClientRect(hwnd, &client_rect);
            HBRUSH background_brush = CreateSolidBrush(RGB(12, 16, 26));
            FillRect(dc, &client_rect, background_brush);
            DeleteObject(background_brush);

            if (context != nullptr && context->has_selection) {
                HPEN outline_pen = CreatePen(PS_SOLID, 2, RGB(120, 200, 255));
                HGDIOBJ old_pen = SelectObject(dc, outline_pen);
                HGDIOBJ old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
                Rectangle(dc, context->selection.left, context->selection.top, context->selection.right, context->selection.bottom);
                SelectObject(dc, old_brush);
                SelectObject(dc, old_pen);
                DeleteObject(outline_pen);
            }

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            RECT text_rect = client_rect;
            text_rect.left += 20;
            text_rect.top += 16;
            text_rect.right -= 20;
            const std::wstring hint =
                L"\u62d6\u52a8\u9f20\u6807\u6846\u9009\u5f39\u5e55\u533a\u57df\u3002"
                L"\r\nEsc \u6216\u53f3\u952e\u53d6\u6d88\uff0c\u653e\u5f00\u9f20\u6807\u5b8c\u6210\u9009\u533a\u3002";
            DrawTextW(dc, hint.c_str(), -1, &text_rect, DT_LEFT | DT_TOP | DT_WORDBREAK);

            if (context != nullptr && context->has_selection) {
                std::wostringstream size_stream;
                size_stream << L"\u5f53\u524d\u533a\u57df\uff1a"
                            << (context->selection.right - context->selection.left)
                            << L" x "
                            << (context->selection.bottom - context->selection.top);
                RECT size_rect = client_rect;
                size_rect.left += 20;
                size_rect.bottom -= 20;
                DrawTextW(dc, size_stream.str().c_str(), -1, &size_rect, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
            }

            EndPaint(hwnd, &paint_struct);
            return 0;
        }

        if (message == WM_DESTROY) {
            if (context != nullptr) {
                context->finished = true;
            }
            return 0;
        }

        return DefWindowProcW(hwnd, message, w_param, l_param);
    };
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = kRegionOverlayClassName;

    registered = RegisterClassExW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

bool ShowRegionOverlay(const RECT& bounds, DanmakuController::NormalizedRegion* result) {
    if (result == nullptr) {
        return false;
    }
    if (!RegisterOverlayWindowClass()) {
        return false;
    }

    OverlaySelectionContext context{};
    context.bounds = bounds;
    const int overlay_width = bounds.right > bounds.left ? static_cast<int>(bounds.right - bounds.left) : 1;
    const int overlay_height = bounds.bottom > bounds.top ? static_cast<int>(bounds.bottom - bounds.top) : 1;

    HWND overlay = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        kRegionOverlayClassName,
        L"",
        WS_POPUP | WS_VISIBLE,
        bounds.left,
        bounds.top,
        overlay_width,
        overlay_height,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        &context);
    if (overlay == nullptr) {
        return false;
    }

    SetLayeredWindowAttributes(overlay, 0, static_cast<BYTE>(kOverlayAlpha), LWA_ALPHA);
    ShowWindow(overlay, SW_SHOW);
    UpdateWindow(overlay);
    SetForegroundWindow(overlay);
    SetFocus(overlay);

    MSG message{};
    while (!context.finished && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (context.cancelled || !context.has_selection) {
        return false;
    }

    const double width = static_cast<double>(overlay_width);
    const double height = static_cast<double>(overlay_height);
    result->x = std::clamp(static_cast<double>(context.selection.left) / width, 0.0, 1.0);
    result->y = std::clamp(static_cast<double>(context.selection.top) / height, 0.0, 1.0);
    result->width = std::clamp(static_cast<double>(context.selection.right - context.selection.left) / width, 0.0, 1.0);
    result->height = std::clamp(static_cast<double>(context.selection.bottom - context.selection.top) / height, 0.0, 1.0);
    result->valid = result->width > 0.0 && result->height > 0.0;
    return result->valid;
}

}  // namespace

DanmakuController::DanmakuController(
    std::wstring region_file_path,
    std::wstring capture_root_path,
    LogFn log_fn)
    : log_fn_(std::move(log_fn)),
      region_file_path_(std::move(region_file_path)),
      capture_root_path_(std::move(capture_root_path)) {
    std::error_code error;
    std::filesystem::create_directories(capture_root_path_, error);
    speech_thread_ = std::thread(&DanmakuController::SpeechLoop, this);

    std::lock_guard<std::mutex> lock(mutex_);
    LoadRegionLocked();
    speech_voice_names_ = EnumerateSpeechVoiceNames();
    if (!speech_voice_names_.empty()) {
        const size_t preferred_chinese_index = FindPreferredChineseSpeechVoiceIndex(speech_voice_names_);
        if (speech_voice_index_loaded_) {
            if (speech_voice_index_ >= speech_voice_names_.size()) {
                speech_voice_index_ =
                    preferred_chinese_index < speech_voice_names_.size() ? preferred_chinese_index : 0;
            }
        } else if (preferred_chinese_index < speech_voice_names_.size()) {
            speech_voice_index_ = preferred_chinese_index;
        } else if (speech_voice_index_ >= speech_voice_names_.size()) {
            speech_voice_index_ = 0;
        }
    }
}

DanmakuController::~DanmakuController() {
    Stop();
    if (ui_probe_thread_.joinable()) {
        ui_probe_thread_.join();
    }
    {
        std::lock_guard<std::mutex> speech_lock(speech_mutex_);
        speech_stop_requested_ = true;
        speech_queue_.clear();
    }
    speech_cv_.notify_all();
    if (speech_thread_.joinable()) {
        speech_thread_.join();
    }
}

void DanmakuController::SetWindows(HWND owner_window, HWND video_window) {
    std::lock_guard<std::mutex> lock(mutex_);
    owner_window_ = owner_window;
    video_window_ = video_window;
}

void DanmakuController::SetEventFn(EventFn event_fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_fn_ = std::move(event_fn);
}

bool DanmakuController::SelectRegion() {
    SetStatus(L"\u5df2\u79fb\u9664\u5f39\u5e55\u56fe\u50cf\u8bc6\u522b\uff0c\u4ec5\u4fdd\u7559\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\u7684 UI \u6587\u672c\u6811\u8bc6\u522b");
    if (log_fn_) {
        log_fn_(L"\u5f39\u5e55\u8bc6\u522b\uff1a\u5df2\u7981\u7528\u56fe\u50cf\u9009\u533a\u5165\u53e3\uff0c\u5f53\u524d\u53ea\u4f7f\u7528 UI \u6587\u672c\u6811\u65b9\u6848\u3002");
    }
    return false;
}

bool DanmakuController::TestRecognizeFrame() {
    SetStatus(L"\u5df2\u79fb\u9664\u5f39\u5e55\u56fe\u50cf\u8bc6\u522b\u6d4b\u8bd5\uff0c\u5f53\u524d\u53ea\u4f7f\u7528 UI \u6587\u672c\u6811\u65b9\u6848");
    if (log_fn_) {
        log_fn_(L"\u5f39\u5e55\u8bc6\u522b\uff1a\u5df2\u7981\u7528\u56fe\u50cf\u6d4b\u8bd5\u5165\u53e3\uff0c\u5f53\u524d\u53ea\u4f7f\u7528 UI \u6587\u672c\u6811\u65b9\u6848\u3002");
    }
    return false;
}

bool DanmakuController::Start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return true;
        }
        stop_requested_ = false;
        ResetGiftCandidateLocked();
        visual_baseline_ready_ = false;
        last_gift_tick_ = 0;
        ui_live_target_title_.clear();
        ui_live_visible_lines_.clear();
        ui_live_visible_keys_.clear();
        running_ = true;
        status_text_ = L"\u76d1\u542c\u4e2d\uff0c\u7b49\u5f85\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97";
        if (ui_probe_target_title_.empty()) {
            ui_probe_status_ = L"\u7b49\u5f85\u63a2\u6d4b\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97";
        }
    }

    worker_thread_ = std::thread(&DanmakuController::WorkerLoop, this);
    if (log_fn_) {
        log_fn_(L"\u5f39\u5e55\u8bc6\u522b\uff1a\u5df2\u5f00\u59cb\u76d1\u542c\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\u3002");
    }
    return true;
}

void DanmakuController::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !worker_thread_.joinable()) {
            return;
        }
        stop_requested_ = true;
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    if (ui_probe_thread_.joinable()) {
        ui_probe_thread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    ui_probe_running_ = false;
    ResetGiftCandidateLocked();
    last_gift_tick_ = 0;
    ui_live_target_title_.clear();
    ui_live_visible_lines_.clear();
    ui_live_visible_keys_.clear();
    status_text_ = L"\u5df2\u505c\u6b62\u76d1\u542c";
}

bool DanmakuController::StartUiAutomationProbe() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ui_probe_running_) {
            return false;
        }
        ui_probe_running_ = true;
        ui_probe_target_title_.clear();
        ui_probe_status_ = L"\u8bf7\u5728 3 \u79d2\u5185\u5207\u5230\u89c6\u9891\u53f7\u76f4\u64ad\u52a9\u624b\u201c\u4e92\u52a8\u6d88\u606f\u201d\u60ac\u6d6e\u7a97";
        recent_probe_lines_.clear();
    }

    if (ui_probe_thread_.joinable()) {
        ui_probe_thread_.join();
    }
    ui_probe_thread_ = std::thread(&DanmakuController::UiProbeLoop, this);
    if (log_fn_) {
        log_fn_(L"\u5f39\u5e55\u8bc6\u522b\uff1a\u5df2\u542f\u52a8\u60ac\u6d6e\u7a97\u63a2\u6d4b\uff0c\u8bf7\u5c06\u89c6\u9891\u53f7\u76f4\u64ad\u52a9\u624b\u5207\u5230\u524d\u53f0\u3002");
    }
    return true;
}

bool DanmakuController::ToggleReminder() {
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reminder_enabled_ = !reminder_enabled_;
        SaveRegionLocked();
        status_text_ = reminder_enabled_
            ? L"\u5df2\u5f00\u542f\u65b0\u5f39\u5e55\u63d0\u9192"
            : L"\u5df2\u5173\u95ed\u65b0\u5f39\u5e55\u63d0\u9192";
        enabled = reminder_enabled_;
    }
    if (log_fn_) {
        log_fn_(enabled
            ? L"\u5f39\u5e55\u8bc6\u522b\uff1a\u5df2\u5f00\u542f\u65b0\u5f39\u5e55\u63d0\u9192\u97f3\u3002"
            : L"\u5f39\u5e55\u8bc6\u522b\uff1a\u5df2\u5173\u95ed\u65b0\u5f39\u5e55\u63d0\u9192\u97f3\u3002");
    }
    return enabled;
}

bool DanmakuController::ToggleGiftReminder() {
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        gift_reminder_enabled_ = !gift_reminder_enabled_;
        SaveRegionLocked();
        status_text_ = gift_reminder_enabled_
            ? L"\u5df2\u5f00\u542f\u65b0\u793c\u7269\u63d0\u9192"
            : L"\u5df2\u5173\u95ed\u65b0\u793c\u7269\u63d0\u9192";
        enabled = gift_reminder_enabled_;
    }
    if (log_fn_) {
        log_fn_(enabled
            ? L"\u5f39\u5e55\u8bc6\u522b\uff1a\u5df2\u5f00\u542f\u65b0\u793c\u7269\u63d0\u9192\u97f3\u3002"
            : L"\u5f39\u5e55\u8bc6\u522b\uff1a\u5df2\u5173\u95ed\u65b0\u793c\u7269\u63d0\u9192\u97f3\u3002");
    }
    return enabled;
}

bool DanmakuController::ToggleSpeech() {
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        speech_enabled_ = !speech_enabled_;
        SaveRegionLocked();
        status_text_ = speech_enabled_
            ? L"\u5df2\u5f00\u542f\u5f39\u5e55\u64ad\u62a5"
            : L"\u5df2\u5173\u95ed\u5f39\u5e55\u64ad\u62a5";
        enabled = speech_enabled_;
    }
    if (!enabled) {
        std::lock_guard<std::mutex> speech_lock(speech_mutex_);
        speech_queue_.clear();
    }
    speech_cv_.notify_all();
    if (log_fn_) {
        log_fn_(enabled
            ? L"\u5f39\u5e55\u8bc6\u522b\uff1a\u5df2\u5f00\u542f\u65b0\u5f39\u5e55\u8bed\u97f3\u64ad\u62a5\u3002"
            : L"\u5f39\u5e55\u8bc6\u522b\uff1a\u5df2\u5173\u95ed\u65b0\u5f39\u5e55\u8bed\u97f3\u64ad\u62a5\u3002");
    }
    return enabled;
}

bool DanmakuController::CycleSpeechVoice() {
    const auto voice_names = EnumerateSpeechVoiceNames();
    std::wstring current_voice;
    bool changed = false;
    bool only_one_voice = false;
    bool no_voice_found = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        speech_voice_names_ = voice_names;
        if (speech_voice_names_.empty()) {
            no_voice_found = true;
            current_voice = L"\u7cfb\u7edf\u9ed8\u8ba4";
            status_text_ = L"\u672a\u68c0\u6d4b\u5230\u53ef\u5207\u6362\u7684 Windows \u8bed\u97f3\uff0c\u5f53\u524d\u4ecd\u4f7f\u7528\u7cfb\u7edf\u9ed8\u8ba4";
        } else {
            if (speech_voice_index_ >= speech_voice_names_.size()) {
                const size_t preferred_chinese_index = FindPreferredChineseSpeechVoiceIndex(speech_voice_names_);
                speech_voice_index_ =
                    preferred_chinese_index < speech_voice_names_.size() ? preferred_chinese_index : 0;
            }
            if (speech_voice_names_.size() > 1) {
                speech_voice_index_ = (speech_voice_index_ + 1) % speech_voice_names_.size();
                changed = true;
            } else {
                only_one_voice = true;
            }
            current_voice = BuildSpeechVoiceLabel(speech_voice_names_, speech_voice_index_);
            status_text_ = changed
                ? (std::wstring(L"\u5df2\u5207\u6362\u5f39\u5e55\u64ad\u62a5\u97f3\u8272\uff1a") + current_voice)
                : (std::wstring(L"\u5f53\u524d\u53ef\u7528\u5f39\u5e55\u64ad\u62a5\u97f3\u8272\uff1a") + current_voice);
        }
        SaveRegionLocked();
    }

    if (log_fn_) {
        if (changed) {
            log_fn_(std::wstring(L"\u5f39\u5e55\u8bc6\u522b\uff1a\u5df2\u5207\u6362\u5f39\u5e55\u64ad\u62a5\u97f3\u8272 -> ") + current_voice);
        } else if (only_one_voice) {
            log_fn_(std::wstring(L"\u5f39\u5e55\u8bc6\u522b\uff1a\u53ea\u68c0\u6d4b\u5230 1 \u4e2a Windows \u8bed\u97f3\uff0c\u5f53\u524d\u4e3a -> ") + current_voice);
        } else if (no_voice_found) {
            log_fn_(L"\u5f39\u5e55\u8bc6\u522b\uff1a\u672a\u68c0\u6d4b\u5230\u53ef\u5207\u6362\u7684 Windows \u64ad\u62a5\u97f3\u8272\uff0c\u4fdd\u6301\u7cfb\u7edf\u9ed8\u8ba4\u3002");
        }
    }
    return changed;
}

void DanmakuController::ClearRecentEvents() {
    std::lock_guard<std::mutex> lock(mutex_);
    recent_events_.clear();
    recent_probe_lines_.clear();
    dedupe_entries_.clear();
    ResetGiftCandidateLocked();
    visual_baseline_ready_ = false;
    last_gift_tick_ = 0;
    last_text_.clear();
    ui_live_target_title_.clear();
    ui_live_visible_lines_.clear();
    ui_live_visible_keys_.clear();
    if (ui_probe_running_) {
        ui_probe_status_ = L"\u6b63\u5728\u8fdb\u884c\u60ac\u6d6e\u7a97\u63a2\u6d4b";
    } else if (ui_probe_target_title_.empty()) {
        ui_probe_status_ = L"\u7b49\u5f85\u63a2\u6d4b\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97";
    }
    status_text_ = running_
        ? L"\u5df2\u6e05\u7a7a\u8bc6\u522b\u7ed3\u679c\uff0c\u60ac\u6d6e\u7a97\u76d1\u542c\u7ee7\u7eed"
        : L"\u5df2\u6e05\u7a7a\u8bc6\u522b\u7ed3\u679c";
}

DanmakuController::Snapshot DanmakuController::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot snapshot{};
    snapshot.region_ready = region_.valid;
    snapshot.running = running_;
    snapshot.ui_probe_running = ui_probe_running_;
    snapshot.reminder_enabled = reminder_enabled_;
    snapshot.gift_reminder_enabled = gift_reminder_enabled_;
    snapshot.speech_enabled = speech_enabled_;
    snapshot.speech_voice_count = static_cast<int>(speech_voice_names_.size());
    snapshot.status_text = status_text_;
    snapshot.region_label = BuildRegionLabelLocked();
    snapshot.ui_probe_status = ui_probe_status_;
    snapshot.ui_probe_target_title = ui_probe_target_title_;
    snapshot.speech_voice_name = BuildSpeechVoiceLabel(speech_voice_names_, speech_voice_index_);
    snapshot.last_text = last_text_;
    snapshot.last_capture_path = last_capture_path_;
    snapshot.recent_events.assign(recent_events_.begin(), recent_events_.end());
    snapshot.recent_probe_lines.assign(recent_probe_lines_.begin(), recent_probe_lines_.end());
    return snapshot;
}

void DanmakuController::WorkerLoop() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_requested_) {
                break;
            }
        }

        PollUiDanmakuWindow();

        const int sleep_steps = std::max(1, kUiLivePollIntervalMs / 90);
        for (int step = 0; step < sleep_steps; ++step) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_requested_) {
                    return;
                }
            }
            Sleep(90);
        }
    }
}

bool DanmakuController::PollUiDanmakuWindow() {
    HWND owner_window = nullptr;
    HWND video_window = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        owner_window = owner_window_;
        video_window = video_window_;
    }

    const auto preferred_windows = CollectPreferredUiProbeWindows(owner_window, video_window);
    if (preferred_windows.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ui_live_target_title_.clear();
        ui_live_visible_lines_.clear();
        ui_live_visible_keys_.clear();
        if (!ui_probe_running_) {
            ui_probe_status_ = L"\u7b49\u5f85\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97";
        }
        status_text_ = L"\u76d1\u542c\u4e2d\uff0c\u7b49\u5f85\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\u51fa\u73b0";
        return false;
    }

    const auto& target_window = preferred_windows.front();
    std::wstring target_title = !target_window.title.empty()
        ? target_window.title
        : ReadWindowText(target_window.hwnd);
    if (target_title.empty()) {
        target_title = !target_window.class_name.empty()
            ? target_window.class_name
            : ReadWindowClassName(target_window.hwnd);
    }

    const auto read_result = ReadUiDanmakuWindow(target_window.hwnd);
    if (!read_result.error.empty() && read_result.lines.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ui_live_target_title_ = target_title;
        if (!target_title.empty()) {
            ui_probe_target_title_ = target_title;
        }
        ui_probe_status_ = std::wstring(L"\u5df2\u8fde\u63a5\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\uff0c\u4f46 UI \u6587\u672c\u6811\u8bfb\u53d6\u5931\u8d25\uff1a") + read_result.error;
        status_text_ = target_title.empty()
            ? (std::wstring(L"\u76d1\u542c\u4e2d\uff0cUI \u6587\u672c\u6811\u8bfb\u53d6\u5931\u8d25\uff1a") + read_result.error)
            : (std::wstring(L"\u76d1\u542c\u4e2d\uff0c") + target_title + L" UI \u6587\u672c\u6811\u8bfb\u53d6\u5931\u8d25\uff1a" + read_result.error);
        return false;
    }

    const std::vector<std::wstring> visible_lines = read_result.lines;
    std::vector<std::wstring> visible_compare_keys = read_result.compare_keys;
    if (visible_compare_keys.size() != visible_lines.size()) {
        visible_compare_keys.clear();
        visible_compare_keys.reserve(visible_lines.size());
        for (const auto& line : visible_lines) {
            visible_compare_keys.push_back(L"text:" + NormalizeUiCompareText(line));
        }
    }
    const std::vector<std::wstring> visible_occurrence_keys = BuildUiOccurrenceKeys(visible_compare_keys);

    std::deque<std::wstring> previous_lines;
    std::deque<std::wstring> previous_compare_keys;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous_lines = ui_live_visible_lines_;
        previous_compare_keys = ui_live_visible_keys_;
    }
    if (!previous_compare_keys.empty() && previous_compare_keys.size() != previous_lines.size()) {
        previous_compare_keys.clear();
    }
    const std::vector<std::wstring> previous_occurrence_keys = BuildUiOccurrenceKeys(previous_compare_keys);

    const size_t overlap = FindUiLineOverlap(previous_occurrence_keys, visible_occurrence_keys);
    std::vector<size_t> fresh_line_indices;
    if (previous_lines.empty() || previous_compare_keys.empty()) {
        fresh_line_indices.clear();
    } else {
        if (overlap > 0) {
            fresh_line_indices.reserve(visible_lines.size() - overlap);
            for (size_t index = overlap; index < visible_lines.size(); ++index) {
                fresh_line_indices.push_back(index);
            }
        } else {
            fresh_line_indices = CollectUiFreshLineIndicesByLcs(previous_occurrence_keys, visible_occurrence_keys);
        }
    }
    const ULONGLONG now_tick = GetTickCount64();
    std::vector<std::pair<EventKind, std::wstring>> accepted_events;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ui_live_target_title_ = target_title;
        if (!target_title.empty()) {
            ui_probe_target_title_ = target_title;
        }
        ui_probe_status_ = visible_lines.empty()
            ? L"\u5df2\u8fde\u63a5\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\uff0cUI \u6587\u672c\u6811\u5f53\u524d\u672a\u8bc6\u522b\u5230\u5f39\u5e55"
            : (std::wstring(L"\u5df2\u8fde\u63a5\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\uff0cUI \u6587\u672c\u6811\u8bc6\u522b\u5230 ") +
                std::to_wstring(visible_lines.size()) + L" \u6761\u53ef\u89c1\u6587\u672c");

        if (visible_lines.empty()) {
            status_text_ = target_title.empty()
                ? L"\u5df2\u8fde\u63a5\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\uff0cUI \u6587\u672c\u6811\u5f53\u524d\u672a\u8bc6\u522b\u5230\u5f39\u5e55"
                : (std::wstring(L"\u5df2\u8fde\u63a5 ") + target_title + L"\uff0c\u5f53\u524d\u672a\u8bc6\u522b\u5230\u5f39\u5e55");
        } else if (previous_lines.empty()) {
            status_text_ = target_title.empty()
                ? L"\u5df2\u8fde\u63a5\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\uff0c\u6b63\u5728\u540c\u6b65\u5f53\u524d\u53ef\u89c1\u5185\u5bb9"
                : (std::wstring(L"\u5df2\u8fde\u63a5 ") + target_title + L"\uff0c\u6b63\u5728\u540c\u6b65\u5f53\u524d\u53ef\u89c1\u5185\u5bb9");
        }

        for (size_t line_index : fresh_line_indices) {
            const std::wstring cleaned = CleanUiDanmakuText(visible_lines[line_index]);
            if (cleaned.empty()) {
                continue;
            }
            const std::wstring dedupe_key = line_index < visible_occurrence_keys.size()
                ? (L"ui:" + visible_occurrence_keys[line_index])
                : (L"ui:text:" + NormalizeText(cleaned));

            EventKind kind = EventKind::kText;
            std::wstring accepted_text = cleaned;
            if (!IsUiTextDanmakuText(cleaned) && IsUiGiftMessageText(cleaned)) {
                kind = EventKind::kImageGift;
                accepted_text = L"\u6709\u793c\u7269";
            } else {
                if (NormalizeText(cleaned).empty()) {
                    continue;
                }
                if (IsDuplicateLocked(dedupe_key, now_tick)) {
                    continue;
                }
            }

            AppendEventLocked(kind, accepted_text, std::wstring{}, dedupe_key, now_tick);
            accepted_events.emplace_back(kind, accepted_text);
        }

        if (accepted_events.empty() && !visible_lines.empty()) {
            status_text_ = target_title.empty()
                ? L"\u76d1\u542c\u4e2d\uff0c\u5df2\u8fde\u63a5\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97"
                : (std::wstring(L"\u76d1\u542c\u4e2d\uff0c\u5df2\u8fde\u63a5 ") + target_title);
        }

        ui_live_visible_lines_.clear();
        for (const auto& line : visible_lines) {
            ui_live_visible_lines_.push_back(line);
        }
        ui_live_visible_keys_.clear();
        for (const auto& key : visible_compare_keys) {
            ui_live_visible_keys_.push_back(key);
        }
        while (ui_live_visible_lines_.size() > kMaxUiLiveVisibleLines) {
            ui_live_visible_lines_.pop_front();
        }
        while (ui_live_visible_keys_.size() > kMaxUiLiveVisibleLines) {
            ui_live_visible_keys_.pop_front();
        }
    }

    for (const auto& event : accepted_events) {
        NotifyAcceptedEvent(event.first, event.second, false);
    }
    return true;
}

void DanmakuController::SpeechLoop() {
    const HRESULT apartment_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ISpVoice* voice = nullptr;
    bool voice_unavailable_logged = false;
    bool voice_selection_unavailable_logged = false;
    size_t applied_voice_index = std::numeric_limits<size_t>::max();
    std::wstring last_logged_voice_name;

    if (SUCCEEDED(apartment_result)) {
        HRESULT create_result = CoCreateInstance(
            CLSID_SpVoice,
            nullptr,
            CLSCTX_ALL,
            IID_ISpVoice,
            reinterpret_cast<void**>(&voice));
        if (SUCCEEDED(create_result) && voice != nullptr) {
            voice->SetVolume(100);
            voice->SetRate(0);
        }
    }

    while (true) {
        std::wstring next_text;
        {
            std::unique_lock<std::mutex> lock(speech_mutex_);
            speech_cv_.wait(lock, [this]() { return speech_stop_requested_ || !speech_queue_.empty(); });
            if (speech_stop_requested_ && speech_queue_.empty()) {
                break;
            }
            next_text = std::move(speech_queue_.front());
            speech_queue_.pop_front();
        }

        if (next_text.empty()) {
            continue;
        }

        if (voice == nullptr) {
            if (!voice_unavailable_logged && log_fn_) {
                log_fn_(L"\u5f39\u5e55\u8bc6\u522b\uff1a\u7cfb\u7edf\u8bed\u97f3\u64ad\u62a5\u4e0d\u53ef\u7528\uff0c\u8bf7\u68c0\u67e5 Windows TTS \u7ec4\u4ef6\u3002");
                voice_unavailable_logged = true;
            }
            continue;
        }

        size_t requested_voice_index = 0;
        std::vector<std::wstring> available_voice_names;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requested_voice_index = speech_voice_index_;
            available_voice_names = speech_voice_names_;
        }
        if (available_voice_names.empty()) {
            available_voice_names = EnumerateSpeechVoiceNames();
            if (!available_voice_names.empty()) {
                std::lock_guard<std::mutex> lock(mutex_);
                speech_voice_names_ = available_voice_names;
            }
        }
        const size_t effective_voice_index =
            ResolveSpeechVoiceIndexForText(available_voice_names, requested_voice_index, next_text);
        if (effective_voice_index != applied_voice_index) {
            std::wstring applied_voice_name;
            if (ApplySpeechVoiceByIndex(voice, effective_voice_index, &applied_voice_name)) {
                applied_voice_index = effective_voice_index;
                if (!applied_voice_name.empty()) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (effective_voice_index < speech_voice_names_.size()) {
                        speech_voice_names_[effective_voice_index] = applied_voice_name;
                    }
                }
                if (!applied_voice_name.empty() &&
                    applied_voice_name != last_logged_voice_name &&
                    log_fn_ != nullptr) {
                    log_fn_(std::wstring(L"\u5f39\u5e55\u8bc6\u522b\uff1a\u64ad\u62a5\u97f3\u8272\u5df2\u751f\u6548 -> ") +
                            applied_voice_name);
                    last_logged_voice_name = applied_voice_name;
                }
            } else {
                applied_voice_index = effective_voice_index;
                if (!voice_selection_unavailable_logged && log_fn_) {
                    log_fn_(L"\u5f39\u5e55\u8bc6\u522b\uff1a\u672a\u80fd\u5957\u7528\u6307\u5b9a\u64ad\u62a5\u97f3\u8272\uff0c\u5df2\u56de\u9000\u7cfb\u7edf\u9ed8\u8ba4\u97f3\u8272\u3002");
                    voice_selection_unavailable_logged = true;
                }
            }
        }

        voice->Speak(next_text.c_str(), SPF_IS_NOT_XML, nullptr);
    }

    if (voice != nullptr) {
        voice->Speak(L"", SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, nullptr);
        voice->Release();
    }
    if (SUCCEEDED(apartment_result)) {
        CoUninitialize();
    }
}

void DanmakuController::UiProbeLoop() {
    Sleep(kUiProbeDelayMs);

    HWND owner_window = nullptr;
    HWND video_window = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        owner_window = owner_window_;
        video_window = video_window_;
    }

    const auto preferred_windows = CollectPreferredUiProbeWindows(owner_window, video_window);
    if (!preferred_windows.empty()) {
        ApplyUiProbeResult(ProbeWindowByVisibleContent(preferred_windows.front().hwnd));
        return;
    }

    HWND target_window = GetForegroundWindow();
    bool is_own_window = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        is_own_window = IsOwnWindowLocked(target_window);
    }
    if (is_own_window) {
        UiProbeResult result{};
        result.status_text = L"\u8fd9\u6b21\u8fd8\u662f\u505c\u5728\u672c\u7a0b\u5e8f\uff0c\u8bf7\u91cd\u8bd5\uff1a\u70b9\u63a2\u6d4b\u540e 3 \u79d2\u5185\u5207\u5230\u89c6\u9891\u53f7\u76f4\u64ad\u52a9\u624b";
        ApplyUiProbeResult(std::move(result));
        return;
    }

    ApplyUiProbeResult(ProbeWindowByVisibleContent(target_window));
}

void DanmakuController::ApplyUiProbeResult(UiProbeResult result) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ui_probe_running_ = false;
        ui_probe_target_title_ = std::move(result.target_title);
        ui_probe_status_ = result.status_text.empty()
            ? L"\u60ac\u6d6e\u7a97\u63a2\u6d4b\u5df2\u5b8c\u6210"
            : std::move(result.status_text);
        recent_probe_lines_.clear();
        for (const auto& line : result.lines) {
            recent_probe_lines_.push_back(line);
            if (recent_probe_lines_.size() >= kMaxProbeLines) {
                break;
            }
        }
    }

    if (log_fn_) {
        const std::wstring log_line = std::wstring(L"\u5f39\u5e55\u8bc6\u522b\uff1a\u60ac\u6d6e\u7a97\u63a2\u6d4b -> ") + GetSnapshot().ui_probe_status;
        log_fn_(log_line);
    }
}

bool DanmakuController::IsOwnWindowLocked(HWND target) const {
    if (target == nullptr) {
        return true;
    }
    if (target == owner_window_ || target == video_window_) {
        return true;
    }
    return (owner_window_ != nullptr && IsChild(owner_window_, target)) ||
        (video_window_ != nullptr && IsChild(video_window_, target));
}

bool DanmakuController::CaptureAndRecognizeFrame(bool manual_test) {
    FrameCapture frame{};
    std::wstring error;
    if (!CaptureCurrentRegion(&frame, &error)) {
        SetStatus(error.empty() ? L"\u5f53\u524d\u65e0\u6cd5\u622a\u53d6\u5f39\u5e55\u533a\u57df" : error);
        if (manual_test && log_fn_) {
            log_fn_(std::wstring(L"\u5f39\u5e55\u8bc6\u522b\uff1a\u6d4b\u8bd5\u622a\u5e27\u5931\u8d25\uff0c") + error);
        }
        return false;
    }

    const std::wstring text = TrimWhitespace(RecognizeText(frame, &error));
    const std::wstring normalized = NormalizeText(text);
    const VisualCandidate visual_candidate =
        (!manual_test && normalized.empty()) ? AnalyzeVisualCandidate(frame) : VisualCandidate{};

    bool duplicate = false;
    bool image_gift_hit = false;
    std::wstring capture_path;
    EventKind event_kind = EventKind::kText;
    std::wstring accepted_text = text;
    const ULONGLONG now_tick = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!normalized.empty()) {
            duplicate = IsDuplicateLocked(L"ocr:" + normalized, now_tick);
            visual_baseline_ready_ = true;
            ResetGiftCandidateLocked();
        } else if (!manual_test) {
            image_gift_hit = UpdateGiftCandidateLocked(visual_candidate, now_tick);
            if (image_gift_hit) {
                event_kind = EventKind::kImageGift;
                accepted_text = L"\u6709\u793c\u7269";
            }
        }
    }

    if (manual_test || image_gift_hit || (!normalized.empty() && !duplicate)) {
        capture_path = BuildCaptureFilePath(manual_test);
        if (!SaveBitmap(capture_path, frame)) {
            capture_path.clear();
        }
    }

    if (normalized.empty() && !image_gift_hit) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!capture_path.empty()) {
            last_capture_path_ = capture_path;
        }
        status_text_ = manual_test
            ? L"\u6d4b\u8bd5\u5b8c\u6210\uff0c\u8fd9\u4e00\u5e27\u6ca1\u8bc6\u522b\u5230\u6587\u5b57"
            : (visual_candidate.valid
                ? L"\u76d1\u542c\u4e2d\uff0c\u6b63\u5728\u89c2\u5bdf\u53ef\u80fd\u7684\u56fe\u7247\u793c\u7269"
                : L"\u76d1\u542c\u4e2d\uff0c\u6682\u672a\u8bc6\u522b\u5230\u6587\u5b57");
        return true;
    }

    if (!image_gift_hit && duplicate) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_text_ = text;
        if (!capture_path.empty()) {
            last_capture_path_ = capture_path;
        }
        status_text_ = manual_test
            ? std::wstring(L"\u6d4b\u8bd5\u8bc6\u522b\uff1a") + TruncateText(text, 42) + L"\uff08\u4e0e\u6700\u8fd1\u7ed3\u679c\u91cd\u590d\uff09"
            : L"\u76d1\u542c\u4e2d\uff0c\u91cd\u590d\u5185\u5bb9\u5df2\u53bb\u91cd";
        return true;
    }

    if (!image_gift_hit) {
        event_kind = EventKind::kText;
        accepted_text = text;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        AppendEventLocked(
            event_kind,
            accepted_text,
            capture_path,
            normalized.empty() ? std::wstring{} : (L"ocr:" + normalized),
            now_tick);
        if (manual_test) {
            status_text_ = std::wstring(L"\u6d4b\u8bd5\u8bc6\u522b\u6210\u529f\uff1a") + TruncateText(accepted_text, 42);
        }
    }

    NotifyAcceptedEvent(event_kind, accepted_text, manual_test);
    if (manual_test && log_fn_) {
        log_fn_(std::wstring(L"\u5f39\u5e55\u8bc6\u522b\uff1a\u6d4b\u8bd5\u547d\u4e2d -> ") + accepted_text);
    }
    return true;
}

bool DanmakuController::CaptureScreenRect(const RECT& capture_rect, FrameCapture* frame, std::wstring* error) const {
    if (frame == nullptr) {
        if (error != nullptr) {
            *error = L"\u5185\u90e8\u9519\u8bef";
        }
        return false;
    }

    const RECT desktop_bounds = GetDesktopCaptureBounds();
    RECT clamped_rect = capture_rect;
    clamped_rect.left = std::clamp(clamped_rect.left, desktop_bounds.left, desktop_bounds.right);
    clamped_rect.top = std::clamp(clamped_rect.top, desktop_bounds.top, desktop_bounds.bottom);
    clamped_rect.right = std::clamp(clamped_rect.right, clamped_rect.left + 1, desktop_bounds.right);
    clamped_rect.bottom = std::clamp(clamped_rect.bottom, clamped_rect.top + 1, desktop_bounds.bottom);

    const int capture_width = clamped_rect.right > clamped_rect.left
        ? static_cast<int>(clamped_rect.right - clamped_rect.left)
        : 1;
    const int capture_height = clamped_rect.bottom > clamped_rect.top
        ? static_cast<int>(clamped_rect.bottom - clamped_rect.top)
        : 1;

    const HDC screen_dc = GetDC(nullptr);
    if (screen_dc == nullptr) {
        if (error != nullptr) {
            *error = L"\u65e0\u6cd5\u83b7\u53d6\u5c4f\u5e55 DC";
        }
        return false;
    }

    const HDC memory_dc = CreateCompatibleDC(screen_dc);
    const HBITMAP bitmap = CreateCompatibleBitmap(screen_dc, capture_width, capture_height);
    if (memory_dc == nullptr || bitmap == nullptr) {
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        if (memory_dc != nullptr) {
            DeleteDC(memory_dc);
        }
        ReleaseDC(nullptr, screen_dc);
        if (error != nullptr) {
            *error = L"\u65e0\u6cd5\u521b\u5efa\u622a\u56fe\u7f13\u51b2";
        }
        return false;
    }

    const HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
    if (!BitBlt(
            memory_dc,
            0,
            0,
            capture_width,
            capture_height,
            screen_dc,
            clamped_rect.left,
            clamped_rect.top,
            SRCCOPY | CAPTUREBLT)) {
        SelectObject(memory_dc, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        if (error != nullptr) {
            *error = L"\u5c4f\u5e55\u533a\u57df\u622a\u56fe\u5931\u8d25";
        }
        return false;
    }

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = capture_width;
    bitmap_info.bmiHeader.biHeight = -capture_height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    frame->width = capture_width;
    frame->height = capture_height;
    frame->screen_rect = clamped_rect;
    frame->pixels.resize(static_cast<size_t>(capture_width) * static_cast<size_t>(capture_height) * 4u);

    if (GetDIBits(
            memory_dc,
            bitmap,
            0,
            static_cast<UINT>(capture_height),
            frame->pixels.data(),
            &bitmap_info,
            DIB_RGB_COLORS) == 0) {
        frame->pixels.clear();
        SelectObject(memory_dc, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        if (error != nullptr) {
            *error = L"\u622a\u56fe\u50cf\u7d20\u8bfb\u53d6\u5931\u8d25";
        }
        return false;
    }

    SelectObject(memory_dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    return true;
}

bool DanmakuController::CaptureCurrentRegion(FrameCapture* frame, std::wstring* error) const {
    if (frame == nullptr) {
        if (error != nullptr) {
            *error = L"\u5185\u90e8\u9519\u8bef";
        }
        return false;
    }

    NormalizedRegion region{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        region = region_;
    }

    if (!region.valid) {
        if (error != nullptr) {
            *error = L"\u8bf7\u5148\u9009\u62e9\u5f39\u5e55\u533a\u57df";
        }
        return false;
    }

    const RECT desktop_bounds = GetDesktopCaptureBounds();
    const int desktop_width = std::max(1, static_cast<int>(desktop_bounds.right - desktop_bounds.left));
    const int desktop_height = std::max(1, static_cast<int>(desktop_bounds.bottom - desktop_bounds.top));
    RECT capture_rect{};
    capture_rect.left = desktop_bounds.left + static_cast<int>(std::lround(region.x * desktop_width));
    capture_rect.top = desktop_bounds.top + static_cast<int>(std::lround(region.y * desktop_height));
    capture_rect.right = capture_rect.left + static_cast<int>(std::lround(region.width * desktop_width));
    capture_rect.bottom = capture_rect.top + static_cast<int>(std::lround(region.height * desktop_height));
    capture_rect.left = std::clamp(capture_rect.left, desktop_bounds.left, desktop_bounds.right);
    capture_rect.top = std::clamp(capture_rect.top, desktop_bounds.top, desktop_bounds.bottom);
    capture_rect.right = std::clamp(capture_rect.right, capture_rect.left + 1, desktop_bounds.right);
    capture_rect.bottom = std::clamp(capture_rect.bottom, capture_rect.top + 1, desktop_bounds.bottom);
    return CaptureScreenRect(capture_rect, frame, error);
}

bool DanmakuController::CaptureWindowRegion(HWND target_window, FrameCapture* frame, std::wstring* error) const {
    if (frame == nullptr) {
        if (error != nullptr) {
            *error = L"\u5185\u90e8\u9519\u8bef";
        }
        return false;
    }
    if (target_window == nullptr || !IsWindow(target_window) || !IsWindowVisible(target_window) || IsIconic(target_window)) {
        if (error != nullptr) {
            *error = L"\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\u4e0d\u53ef\u7528";
        }
        return false;
    }

    RECT capture_rect{};
    if (!GetWindowRect(target_window, &capture_rect)) {
        if (error != nullptr) {
            *error = L"\u65e0\u6cd5\u8bfb\u53d6\u60ac\u6d6e\u7a97\u4f4d\u7f6e";
        }
        return false;
    }

    const std::wstring class_name = ReadWindowClassName(target_window);
    const int width = std::max(1, static_cast<int>(capture_rect.right - capture_rect.left));
    const int height = std::max(1, static_cast<int>(capture_rect.bottom - capture_rect.top));

    int inset_left = std::max(6, static_cast<int>(std::lround(width * 0.05)));
    int inset_right = std::max(6, static_cast<int>(std::lround(width * 0.05)));
    int inset_top = std::max(40, static_cast<int>(std::lround(height * 0.18)));
    int inset_bottom = std::max(48, static_cast<int>(std::lround(height * 0.10)));

    if (class_name == L"FinderLiveCommentFloatWnd") {
        inset_left = std::max(10, static_cast<int>(std::lround(width * 0.07)));
        inset_right = std::max(10, static_cast<int>(std::lround(width * 0.07)));
        inset_top = std::max(74, static_cast<int>(std::lround(height * 0.24)));
        inset_bottom = std::max(54, static_cast<int>(std::lround(height * 0.08)));
    }

    RECT cropped_rect = capture_rect;
    cropped_rect.left += inset_left;
    cropped_rect.right -= inset_right;
    cropped_rect.top += inset_top;
    cropped_rect.bottom -= inset_bottom;
    if (cropped_rect.right - cropped_rect.left < std::max(120, width / 3) ||
        cropped_rect.bottom - cropped_rect.top < std::max(120, height / 3)) {
        cropped_rect = capture_rect;
    }

    RawCaptureFrame full_frame{};
    if (!CaptureWindowWithGraphicsCapture(target_window, capture_rect, &full_frame, error)) {
        return false;
    }

    if (full_frame.width <= 0 || full_frame.height <= 0 || full_frame.pixels.empty()) {
        if (error != nullptr) {
            *error = L"\u7a97\u53e3\u6293\u53d6\u7ed3\u679c\u4e3a\u7a7a";
        }
        return false;
    }

    const int crop_left = std::clamp(static_cast<int>(cropped_rect.left - capture_rect.left), 0, full_frame.width - 1);
    const int crop_top = std::clamp(static_cast<int>(cropped_rect.top - capture_rect.top), 0, full_frame.height - 1);
    const int crop_right = std::clamp(static_cast<int>(cropped_rect.right - capture_rect.left), crop_left + 1, full_frame.width);
    const int crop_bottom = std::clamp(static_cast<int>(cropped_rect.bottom - capture_rect.top), crop_top + 1, full_frame.height);
    const int final_width = std::max(1, crop_right - crop_left);
    const int final_height = std::max(1, crop_bottom - crop_top);

    frame->width = final_width;
    frame->height = final_height;
    frame->screen_rect = cropped_rect;
    frame->pixels.resize(static_cast<size_t>(final_width) * static_cast<size_t>(final_height) * 4u);

    for (int y = 0; y < final_height; ++y) {
        const size_t src_offset =
            (static_cast<size_t>(crop_top + y) * static_cast<size_t>(full_frame.width) + static_cast<size_t>(crop_left)) * 4u;
        const size_t dst_offset =
            static_cast<size_t>(y) * static_cast<size_t>(final_width) * 4u;
        std::memcpy(
            frame->pixels.data() + dst_offset,
            full_frame.pixels.data() + src_offset,
            static_cast<size_t>(final_width) * 4u);
    }
    return true;
}

std::vector<std::wstring> DanmakuController::RecognizeTextLines(
    const FrameCapture& frame,
    std::wstring* error) const {
    std::vector<std::wstring> lines_out;
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) {
        if (error != nullptr) {
            *error = L"\u622a\u56fe\u5185\u5bb9\u4e3a\u7a7a";
        }
        return lines_out;
    }

    try {
        using namespace winrt::Windows::Graphics::Imaging;
        using namespace winrt::Windows::Media::Ocr;
        using namespace winrt::Windows::Storage::Streams;

        const OcrEngine engine = OcrEngine::TryCreateFromUserProfileLanguages();
        if (!engine) {
            if (error != nullptr) {
                *error = L"\u5f53\u524d\u7cfb\u7edf OCR \u4e0d\u53ef\u7528";
            }
            return lines_out;
        }

        DataWriter writer;
        writer.WriteBytes(winrt::array_view<uint8_t const>(frame.pixels));
        const IBuffer buffer = writer.DetachBuffer();
        const SoftwareBitmap bitmap = SoftwareBitmap::CreateCopyFromBuffer(
            buffer,
            BitmapPixelFormat::Bgra8,
            frame.width,
            frame.height,
            BitmapAlphaMode::Ignore);

        const auto result = engine.RecognizeAsync(bitmap).get();
        const auto lines = result.Lines();
        lines_out.reserve(lines.Size());
        for (uint32_t index = 0; index < lines.Size(); ++index) {
            const std::wstring line_text = TrimWhitespace(lines.GetAt(index).Text().c_str());
            if (!line_text.empty()) {
                lines_out.push_back(line_text);
            }
        }
        return lines_out;
    } catch (const winrt::hresult_error& exception) {
        if (error != nullptr) {
            std::wostringstream stream;
            stream << L"OCR \u89e3\u6790\u5931\u8d25\uff0c\u9519\u8bef 0x" << std::hex << exception.code().value;
            *error = stream.str();
        }
        return lines_out;
    }
}

std::wstring DanmakuController::RecognizeText(const FrameCapture& frame, std::wstring* error) const {
    const auto lines = RecognizeTextLines(frame, error);
    std::wostringstream stream;
    bool first = true;
    for (const auto& line : lines) {
        if (!first) {
            stream << L" / ";
        }
        first = false;
        stream << line;
    }
    return stream.str();
}

bool DanmakuController::SaveBitmap(const std::wstring& path, const FrameCapture& frame) const {
    if (path.empty() || frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) {
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
        return false;
    }

    const DWORD pixel_bytes = static_cast<DWORD>(frame.pixels.size());
    BITMAPFILEHEADER file_header{};
    file_header.bfType = 0x4D42;
    file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file_header.bfSize = file_header.bfOffBits + pixel_bytes;

    BITMAPINFOHEADER info_header{};
    info_header.biSize = sizeof(BITMAPINFOHEADER);
    info_header.biWidth = frame.width;
    info_header.biHeight = -frame.height;
    info_header.biPlanes = 1;
    info_header.biBitCount = 32;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = pixel_bytes;

    fwrite(&file_header, 1, sizeof(file_header), file);
    fwrite(&info_header, 1, sizeof(info_header), file);
    fwrite(frame.pixels.data(), 1, frame.pixels.size(), file);
    fclose(file);
    return true;
}

void DanmakuController::SetStatus(const std::wstring& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_text_ = status;
}

std::wstring DanmakuController::BuildRegionLabelLocked() const {
    if (!region_.valid) {
        return L"\u672a\u9009\u533a";
    }

    const RECT desktop_bounds = GetDesktopCaptureBounds();
    const int width = std::max(1, static_cast<int>(desktop_bounds.right - desktop_bounds.left));
    const int height = std::max(1, static_cast<int>(desktop_bounds.bottom - desktop_bounds.top));
    const int pixel_width = std::max(1, static_cast<int>(std::lround(region_.width * static_cast<double>(width))));
    const int pixel_height = std::max(1, static_cast<int>(std::lround(region_.height * static_cast<double>(height))));
    std::wostringstream stream;
    stream << L"\u5c4f\u5e55\u533a\u57df "
           << pixel_width << L"x" << pixel_height
           << L"\uff0c\u8ddd\u5de6 " << static_cast<int>(std::lround(region_.x * 100.0))
           << L"%\uff0c\u8ddd\u4e0a " << static_cast<int>(std::lround(region_.y * 100.0)) << L"%";
    return stream.str();
}

bool DanmakuController::LoadRegionLocked() {
    const std::string json = ReadUtf8File(region_file_path_);
    if (json.empty()) {
        return false;
    }

    speech_voice_index_loaded_ = false;

    std::smatch region_match;
    std::regex region_pattern(
        "\"x\"\\s*:\\s*([0-9.]+)[\\s\\S]*?\"y\"\\s*:\\s*([0-9.]+)[\\s\\S]*?\"width\"\\s*:\\s*([0-9.]+)[\\s\\S]*?\"height\"\\s*:\\s*([0-9.]+)",
        std::regex::icase);
    if (std::regex_search(json, region_match, region_pattern) && region_match.size() >= 5) {
        try {
            region_.x = std::stod(region_match[1].str());
            region_.y = std::stod(region_match[2].str());
            region_.width = std::stod(region_match[3].str());
            region_.height = std::stod(region_match[4].str());
            region_.valid = region_.width > 0.0 && region_.height > 0.0;
        } catch (...) {
            region_.valid = false;
        }
    }

    std::smatch bool_match;
    std::regex reminder_pattern("\"reminderEnabled\"\\s*:\\s*(true|false)", std::regex::icase);
    if (std::regex_search(json, bool_match, reminder_pattern) && bool_match.size() >= 2) {
        const std::string flag = bool_match[1].str();
        reminder_enabled_ = !flag.empty() && (flag[0] == 't' || flag[0] == 'T');
    }
    std::regex gift_reminder_pattern("\"giftReminderEnabled\"\\s*:\\s*(true|false)", std::regex::icase);
    if (std::regex_search(json, bool_match, gift_reminder_pattern) && bool_match.size() >= 2) {
        const std::string flag = bool_match[1].str();
        gift_reminder_enabled_ = !flag.empty() && (flag[0] == 't' || flag[0] == 'T');
    }
    std::regex speech_pattern("\"speechEnabled\"\\s*:\\s*(true|false)", std::regex::icase);
    if (std::regex_search(json, bool_match, speech_pattern) && bool_match.size() >= 2) {
        const std::string flag = bool_match[1].str();
        speech_enabled_ = !flag.empty() && (flag[0] == 't' || flag[0] == 'T');
    }
    std::smatch voice_index_match;
    std::regex voice_index_pattern("\"speechVoiceIndex\"\\s*:\\s*(\\d+)", std::regex::icase);
    if (std::regex_search(json, voice_index_match, voice_index_pattern) && voice_index_match.size() >= 2) {
        try {
            speech_voice_index_ = static_cast<size_t>(std::stoul(voice_index_match[1].str()));
            speech_voice_index_loaded_ = true;
        } catch (...) {
            speech_voice_index_ = 0;
            speech_voice_index_loaded_ = false;
        }
    }
    return region_.valid;
}

bool DanmakuController::SaveRegionLocked() const {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(6);
    stream << "{\n"
           << "  \"x\": " << region_.x << ",\n"
           << "  \"y\": " << region_.y << ",\n"
           << "  \"width\": " << region_.width << ",\n"
           << "  \"height\": " << region_.height << ",\n"
           << "  \"valid\": " << (region_.valid ? "true" : "false") << ",\n"
           << "  \"reminderEnabled\": " << (reminder_enabled_ ? "true" : "false") << ",\n"
           << "  \"giftReminderEnabled\": " << (gift_reminder_enabled_ ? "true" : "false") << ",\n"
           << "  \"speechEnabled\": " << (speech_enabled_ ? "true" : "false") << ",\n"
           << "  \"speechVoiceIndex\": " << speech_voice_index_ << "\n"
           << "}\n";
    return WriteUtf8File(region_file_path_, stream.str());
}

bool DanmakuController::HasUsableVideoWindow() const {
    HWND video_window = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        video_window = video_window_;
    }
    return video_window != nullptr && IsWindow(video_window) && IsWindowVisible(video_window) && !IsIconic(video_window);
}

std::wstring DanmakuController::BuildCaptureFilePath(bool manual_test) const {
    SYSTEMTIME local_time{};
    GetLocalTime(&local_time);

    std::wostringstream stream;
    stream << capture_root_path_
           << L"\\"
           << (manual_test ? L"test-" : L"hit-")
           << std::setfill(L'0')
           << std::setw(4) << local_time.wYear
           << std::setw(2) << local_time.wMonth
           << std::setw(2) << local_time.wDay
           << L"-"
           << std::setw(2) << local_time.wHour
           << std::setw(2) << local_time.wMinute
           << std::setw(2) << local_time.wSecond
           << L"-"
           << std::setw(3) << local_time.wMilliseconds
           << L".bmp";
    return stream.str();
}

bool DanmakuController::IsDuplicateLocked(const std::wstring& dedupe_key, ULONGLONG now_tick) {
    if (dedupe_key.empty()) {
        return false;
    }

    while (!dedupe_entries_.empty() && now_tick - dedupe_entries_.front().tick > kDedupeWindowMs) {
        dedupe_entries_.pop_front();
    }

    for (const auto& entry : dedupe_entries_) {
        if (entry.dedupe_key == dedupe_key) {
            return true;
        }
    }
    return false;
}

void DanmakuController::AppendEventLocked(
    EventKind kind,
    const std::wstring& text,
    const std::wstring& capture_path,
    const std::wstring& dedupe_key,
    ULONGLONG now_tick) {
    const std::wstring normalized = !dedupe_key.empty() ? dedupe_key : NormalizeText(text);
    if (normalized.empty()) {
        return;
    }

    dedupe_entries_.push_back({normalized, now_tick});
    while (dedupe_entries_.size() > kMaxDedupeEntries) {
        dedupe_entries_.pop_front();
    }

    const std::wstring prefix = kind == EventKind::kImageGift ? L"[\u793c\u7269]" : ClassifyPrefix(text);
    const std::wstring message = FormatClockNow() + L" " + prefix + L" " + text;
    recent_events_.push_front(message);
    while (recent_events_.size() > kMaxRecentEvents) {
        recent_events_.pop_back();
    }

    last_text_ = text;
    if (!capture_path.empty()) {
        last_capture_path_ = capture_path;
    }
    status_text_ = std::wstring(L"\u6700\u8fd1\u8bc6\u522b\uff1a") + TruncateText(text, 42);

    if (log_fn_) {
        log_fn_(std::wstring(L"\u5f39\u5e55\u8bc6\u522b\uff1a") + message);
    }
}

void DanmakuController::ResetGiftCandidateLocked() {
    visual_candidate_active_ = false;
    visual_candidate_hash_ = 0;
    visual_candidate_bounds_ = RECT{};
    visual_candidate_stable_frames_ = 0;
}

bool DanmakuController::UpdateGiftCandidateLocked(const VisualCandidate& candidate, ULONGLONG now_tick) {
    if (!candidate.valid) {
        visual_baseline_ready_ = true;
        ResetGiftCandidateLocked();
        return false;
    }

    if (!visual_baseline_ready_) {
        visual_candidate_active_ = true;
        visual_candidate_hash_ = candidate.hash;
        visual_candidate_bounds_ = candidate.bounds;
        visual_candidate_stable_frames_ = 1;
        return false;
    }

    const bool similar = visual_candidate_active_ &&
        HammingDistance64(visual_candidate_hash_, candidate.hash) <= kGiftHashTolerance &&
        AreRectsSimilar(visual_candidate_bounds_, candidate.bounds, 18);

    if (similar) {
        visual_candidate_stable_frames_ += 1;
    } else {
        visual_candidate_active_ = true;
        visual_candidate_stable_frames_ = 1;
    }

    visual_candidate_hash_ = candidate.hash;
    visual_candidate_bounds_ = candidate.bounds;

    if (visual_candidate_stable_frames_ < kGiftStableFrames) {
        return false;
    }
    if (now_tick - last_gift_tick_ < kGiftCooldownMs) {
        return false;
    }

    last_gift_tick_ = now_tick;
    visual_baseline_ready_ = false;
    ResetGiftCandidateLocked();
    return true;
}

void DanmakuController::NotifyAcceptedEvent(EventKind kind, const std::wstring& text, bool manual_test) {
    if (manual_test) {
        return;
    }

    bool reminder_enabled = false;
    bool gift_reminder_enabled = false;
    bool speech_enabled = false;
    EventFn event_fn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reminder_enabled = reminder_enabled_;
        gift_reminder_enabled = gift_reminder_enabled_;
        speech_enabled = speech_enabled_;
        event_fn = event_fn_;
    }

    const bool play_reminder = kind == EventKind::kImageGift ? gift_reminder_enabled : reminder_enabled;
    if (play_reminder) {
        std::wstring reminder_sound_path = kind == EventKind::kImageGift
            ? GetGiftReminderSoundPath()
            : GetDanmakuReminderSoundPath();
        if (reminder_sound_path.empty() && kind == EventKind::kImageGift) {
            reminder_sound_path = GetDanmakuReminderSoundPath();
        }
        if (!reminder_sound_path.empty()) {
            PlaySoundW(reminder_sound_path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        } else {
            PlaySoundW(
                kind == EventKind::kImageGift ? L"SystemExclamation" : L"SystemAsterisk",
                nullptr,
                SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
        }
    }
    if (speech_enabled) {
        QueueSpeech(BuildSpeechText(kind, text));
    }
    if (event_fn) {
        event_fn(kind == EventKind::kImageGift, text);
    }
}

void DanmakuController::QueueSpeech(const std::wstring& text) {
    const std::wstring trimmed = TrimWhitespace(text);
    if (trimmed.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(speech_mutex_);
        if (speech_queue_.size() >= kMaxSpeechQueue) {
            speech_queue_.pop_front();
        }
        speech_queue_.push_back(trimmed);
    }
    speech_cv_.notify_one();
}

std::wstring DanmakuController::NormalizeText(const std::wstring& text) {
    std::wstring normalized;
    normalized.reserve(text.size());
    for (wchar_t ch : text) {
        if (std::iswspace(ch)) {
            continue;
        }
        normalized.push_back(ch);
    }
    return normalized;
}

std::vector<std::wstring> DanmakuController::ExtractOcrDanmakuLines(const std::vector<std::wstring>& lines) {
    std::vector<std::wstring> filtered_lines;
    std::vector<std::wstring> seen_lines;
    filtered_lines.reserve(lines.size());
    seen_lines.reserve(lines.size());

    static const wchar_t* const kIgnoredTokens[] = {
        L"\u4e92\u52a8\u6d88\u606f",
        L"\u6dfb\u52a0\u8bc4\u8bba",
        L"\u53d1\u9001",
        L"\u6682\u65e0\u89c2\u4f17\u9001\u793c",
        L"\u5173\u95ed",
        L"\u6700\u5c0f\u5316",
        L"\u7b49\u5f85\u8fde\u63a5",
        L"\u7b49\u5f85\u68c0\u6d4b",
        L"\u68c0\u6d4b",
        L"\u76d1\u542c",
        L"\u753b\u9762",
        L"\u8bed\u97f3",
        L"\u5f39\u5e55",
        L"\u56de\u590d",
        L"\u8bf4\u660e",
        L"\u64ad\u62a5",
        L"\u97f3\u8272",
        L"\u60ac\u6d6e\u7a97",
        L"Microsoft",
        L"Huihui",
    };

    for (const auto& raw_line : lines) {
        std::wstring cleaned = CleanUiDanmakuText(raw_line);
        cleaned = TrimWhitespace(cleaned);
        if (cleaned.empty()) {
            continue;
        }
        const std::wstring normalized_cleaned = NormalizeUiCompareText(cleaned);
        if (normalized_cleaned.empty()) {
            continue;
        }

        bool ignored = false;
        for (const auto* token : kIgnoredTokens) {
            if (ContainsCaseInsensitive(cleaned, token)) {
                ignored = true;
                break;
            }
        }
        if (ignored) {
            continue;
        }
        if (IsUiSystemNoticeText(cleaned)) {
            continue;
        }
        const size_t colon_pos = cleaned.find(L'\uff1a');
        const size_t ascii_colon_pos = cleaned.find(L':');
        const size_t separator = colon_pos != std::wstring::npos ? colon_pos : ascii_colon_pos;
        const int chinese_count = CountChineseChars(cleaned);
        const int ascii_count = CountAsciiLettersAndDigits(cleaned);
        if (ascii_count >= 3 && ascii_count >= chinese_count * 2) {
            continue;
        }
        if (chinese_count <= 1 && ascii_count > 0) {
            continue;
        }
        if (separator == std::wstring::npos &&
            normalized_cleaned.size() >= 18 &&
            !IsUiGiftMessageText(cleaned)) {
            continue;
        }
        if (!ContainsChinese(cleaned) &&
            separator == std::wstring::npos &&
            !IsUiGiftMessageText(cleaned)) {
            continue;
        }
        if (cleaned.size() == 1 && !ContainsChinese(cleaned)) {
            continue;
        }
        if (separator != std::wstring::npos) {
            const std::wstring left = TrimWhitespace(cleaned.substr(0, separator));
            const std::wstring right = TrimWhitespace(cleaned.substr(separator + 1));
            if (left.empty() || right.empty()) {
                continue;
            }
            if (right.size() > 24) {
                continue;
            }
        } else if (chinese_count > 8 && !IsUiGiftMessageText(cleaned)) {
            continue;
        }

        if (std::find(seen_lines.begin(), seen_lines.end(), normalized_cleaned) != seen_lines.end()) {
            continue;
        }

        seen_lines.push_back(normalized_cleaned);
        filtered_lines.push_back(cleaned);
    }
    return filtered_lines;
}

std::wstring DanmakuController::TrimWhitespace(const std::wstring& text) {
    return ::TrimWhitespace(text);
}

std::wstring DanmakuController::TruncateText(const std::wstring& text, size_t max_chars) {
    if (text.size() <= max_chars) {
        return text;
    }
    return text.substr(0, max_chars) + L"...";
}

std::wstring DanmakuController::ClassifyPrefix(const std::wstring& text) {
    const std::wstring normalized = NormalizeText(text);
    if (normalized.find(L"\u8fdb\u5165\u76f4\u64ad\u95f4") != std::wstring::npos ||
        normalized.find(L"\u6765\u4e86") != std::wstring::npos) {
        return L"[\u8fdb\u573a]";
    }
    if (normalized.find(L"\u5173\u6ce8") != std::wstring::npos) {
        return L"[\u5173\u6ce8]";
    }
    if (normalized.find(L"\u9001") != std::wstring::npos ||
        normalized.find(L"\u793c\u7269") != std::wstring::npos ||
        normalized.find(L"\u5609\u5e74\u534e") != std::wstring::npos ||
        normalized.find(L"\u6296\u5e01") != std::wstring::npos ||
        normalized.find(L"\u706f\u724c") != std::wstring::npos) {
        return L"[\u793c\u7269]";
    }
    return L"[\u5f39\u5e55]";
}

DanmakuController::VisualCandidate DanmakuController::AnalyzeVisualCandidate(const FrameCapture& frame) {
    VisualCandidate candidate{};
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) {
        return candidate;
    }

    constexpr int kGridSize = 8;
    constexpr double kMinPixelRatio = 0.004;
    constexpr double kMaxPixelRatio = 0.20;
    constexpr double kMinBoundsRatio = 0.01;
    constexpr double kMaxBoundsRatio = 0.28;
    constexpr double kMinFillRatio = 0.08;
    constexpr double kMaxFillRatio = 0.85;

    std::array<int, kGridSize * kGridSize> block_hits{};
    const int frame_area = std::max(1, frame.width * frame.height);
    const int grid_cell_area = std::max(1, static_cast<int>(std::lround(static_cast<double>(frame_area) / 64.0)));
    const int grid_cell_threshold = std::max(4, static_cast<int>(std::lround(static_cast<double>(grid_cell_area) * 0.08)));

    int hit_count = 0;
    int min_x = frame.width;
    int min_y = frame.height;
    int max_x = -1;
    int max_y = -1;

    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const size_t pixel_index =
                (static_cast<size_t>(y) * static_cast<size_t>(frame.width) + static_cast<size_t>(x)) * 4u;
            const int blue = frame.pixels[pixel_index + 0];
            const int green = frame.pixels[pixel_index + 1];
            const int red = frame.pixels[pixel_index + 2];
            const int brightness = std::max({red, green, blue});
            const int min_channel = std::min({red, green, blue});
            const int saturation = brightness - min_channel;
            const bool colorful_highlight =
                (brightness >= 175 && saturation >= 48) ||
                (brightness >= 135 && saturation >= 90);
            if (!colorful_highlight) {
                continue;
            }

            ++hit_count;
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);

            const int block_x = std::min(kGridSize - 1, (x * kGridSize) / std::max(1, frame.width));
            const int block_y = std::min(kGridSize - 1, (y * kGridSize) / std::max(1, frame.height));
            block_hits[block_y * kGridSize + block_x] += 1;
        }
    }

    if (hit_count == 0 || max_x < min_x || max_y < min_y) {
        return candidate;
    }

    const int bounds_width = max_x - min_x + 1;
    const int bounds_height = max_y - min_y + 1;
    const int bounds_area = std::max(1, bounds_width * bounds_height);
    const double pixel_ratio = static_cast<double>(hit_count) / static_cast<double>(frame_area);
    const double bounds_ratio = static_cast<double>(bounds_area) / static_cast<double>(frame_area);
    const double fill_ratio = static_cast<double>(hit_count) / static_cast<double>(bounds_area);

    if (pixel_ratio < kMinPixelRatio || pixel_ratio > kMaxPixelRatio ||
        bounds_ratio < kMinBoundsRatio || bounds_ratio > kMaxBoundsRatio ||
        fill_ratio < kMinFillRatio || fill_ratio > kMaxFillRatio) {
        return candidate;
    }

    if (bounds_width < std::max(14, frame.width / 12) || bounds_height < std::max(14, frame.height / 6)) {
        return candidate;
    }

    uint64_t hash = 0;
    int occupied_blocks = 0;
    for (size_t index = 0; index < block_hits.size(); ++index) {
        if (block_hits[index] >= grid_cell_threshold) {
            hash |= (1ull << index);
            occupied_blocks += 1;
        }
    }
    if (occupied_blocks < 2 || occupied_blocks > 20) {
        return candidate;
    }

    candidate.valid = true;
    candidate.hash = hash;
    candidate.bounds = RECT{min_x, min_y, max_x + 1, max_y + 1};
    candidate.pixel_ratio = pixel_ratio;
    candidate.bounds_ratio = bounds_ratio;
    candidate.occupied_blocks = occupied_blocks;
    return candidate;
}

int DanmakuController::HammingDistance64(uint64_t left, uint64_t right) {
    return std::popcount(left ^ right);
}

bool DanmakuController::AreRectsSimilar(const RECT& left, const RECT& right, int tolerance) {
    return std::abs(left.left - right.left) <= tolerance &&
        std::abs(left.top - right.top) <= tolerance &&
        std::abs(left.right - right.right) <= tolerance &&
        std::abs(left.bottom - right.bottom) <= tolerance;
}

std::wstring DanmakuController::BuildSpeechText(EventKind kind, const std::wstring& text) {
    if (kind == EventKind::kImageGift) {
        return L"\u6709\u793c\u7269";
    }

    std::wstring speech = TrimWhitespace(text);
    for (wchar_t& ch : speech) {
        if (ch == L'/' || ch == L'\\') {
            ch = L'\uff0c';
        }
    }
    const size_t colon_pos = speech.find(L'\uff1a');
    const size_t ascii_colon_pos = speech.find(L':');
    const size_t separator = colon_pos != std::wstring::npos ? colon_pos : ascii_colon_pos;
    if (separator != std::wstring::npos) {
        const std::wstring speaker = TrimWhitespace(speech.substr(0, separator));
        const std::wstring content = TrimWhitespace(speech.substr(separator + 1));
        if (!speaker.empty() && !content.empty()) {
            speech = speaker + L"\u8bf4" + content;
        }
    }
    speech = TruncateText(speech, 48);
    return speech;
}

DanmakuController::UiProbeResult DanmakuController::ProbeWindowByVisibleContent(HWND target_window) const {
    UiProbeResult result{};
    result.target_title = ReadWindowText(target_window);
    if (result.target_title.empty()) {
        result.target_title = ReadWindowClassName(target_window);
    }

    if (target_window == nullptr || !IsWindow(target_window)) {
        result.status_text = L"\u6ca1\u6293\u5230\u6709\u6548\u7684\u524d\u53f0\u7a97\u53e3\uff0c\u8bf7\u91cd\u8bd5";
        return result;
    }

    const auto read_result = ReadUiDanmakuWindow(target_window);
    if (!read_result.error.empty() && read_result.lines.empty()) {
        result.status_text = std::wstring(L"\u5df2\u627e\u5230\u60ac\u6d6e\u7a97\uff0c\u4f46 UI \u6587\u672c\u6811\u8bfb\u53d6\u5931\u8d25\uff1a") + read_result.error;
        if (!result.target_title.empty()) {
            result.lines.push_back(L"\u76ee\u6807\u7a97\u53e3\uff1a" + result.target_title);
        }
        return result;
    }

    const auto& filtered_lines = read_result.lines;
    if (!result.target_title.empty()) {
        result.lines.push_back(L"\u76ee\u6807\u7a97\u53e3\uff1a" + result.target_title);
    }
    for (const auto& line : filtered_lines) {
        if (result.lines.size() >= kMaxProbeLines) {
            break;
        }
        result.lines.push_back(L"\u53ef\u89c1\u6587\u672c\uff1a" + line);
    }

    if (!filtered_lines.empty()) {
        result.status_text =
            L"\u5df2\u8fde\u63a5\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\uff0cUI \u6587\u672c\u6811\u8bc6\u522b\u5230 " +
            std::to_wstring(filtered_lines.size()) + L" \u6761\u53ef\u89c1\u6587\u672c";
    } else {
        result.status_text =
            L"\u5df2\u8fde\u63a5\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\uff0cUI \u6587\u672c\u6811\u5f53\u524d\u672a\u8bc6\u522b\u5230\u53ef\u89c1\u5f39\u5e55";
    }
    return result;
}

std::wstring DanmakuController::FormatClockNow() {
    SYSTEMTIME local_time{};
    GetLocalTime(&local_time);
    std::wostringstream stream;
    stream << std::setfill(L'0')
           << std::setw(2) << local_time.wHour
           << L":"
           << std::setw(2) << local_time.wMinute
           << L":"
           << std::setw(2) << local_time.wSecond;
    return stream.str();
}
