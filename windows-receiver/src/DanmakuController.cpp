#include "DanmakuController.h"

#include <Windows.h>
#include <objbase.h>
#include <UIAutomationClient.h>
#include <mmsystem.h>
#include <sapi.h>
#include <windowsx.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>

namespace {

constexpr wchar_t kRegionOverlayClassName[] = L"LiveCastDanmakuRegionOverlay";
constexpr int kMinSelectionSize = 18;
constexpr ULONGLONG kDedupeWindowMs = 2500;
constexpr ULONGLONG kGiftCooldownMs = 4500;
constexpr int kUiProbeDelayMs = 3000;
constexpr int kUiLivePollIntervalMs = 180;
constexpr size_t kMaxRecentEvents = 12;
constexpr size_t kMaxProbeLines = 10;
constexpr size_t kMaxUiLiveVisibleLines = 40;
constexpr size_t kMaxDedupeEntries = 32;
constexpr size_t kMaxSpeechQueue = 6;
constexpr int kGiftStableFrames = 2;
constexpr int kGiftHashTolerance = 10;
constexpr UINT kOverlayAlpha = 118;

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

bool IsUiDanmakuMessageText(const std::wstring& text) {
    const std::wstring cleaned = CleanUiDanmakuText(text);
    if (cleaned.empty()) {
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
        return !left.empty() && !right.empty();
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

size_t FindUiLineOverlap(const std::deque<std::wstring>& previous_lines, const std::vector<std::wstring>& current_lines) {
    const size_t max_overlap = std::min(previous_lines.size(), current_lines.size());
    for (size_t overlap = max_overlap; overlap > 0; --overlap) {
        bool matched = true;
        for (size_t index = 0; index < overlap; ++index) {
            const auto& left = previous_lines[previous_lines.size() - overlap + index];
            const auto& right = current_lines[index];
            if (NormalizeUiCompareText(left) != NormalizeUiCompareText(right)) {
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
    const std::deque<std::wstring>& previous_lines,
    const std::vector<std::wstring>& current_lines) {
    std::vector<size_t> fresh_indices;
    if (previous_lines.empty() || current_lines.empty()) {
        return fresh_indices;
    }

    std::vector<std::wstring> previous_normalized;
    previous_normalized.reserve(previous_lines.size());
    for (const auto& line : previous_lines) {
        previous_normalized.push_back(NormalizeUiCompareText(line));
    }

    std::vector<std::wstring> current_normalized;
    current_normalized.reserve(current_lines.size());
    for (const auto& line : current_lines) {
        current_normalized.push_back(NormalizeUiCompareText(line));
    }

    std::vector<std::vector<int>> lcs(
        previous_normalized.size() + 1,
        std::vector<int>(current_normalized.size() + 1, 0));
    for (size_t prev_index = 1; prev_index <= previous_normalized.size(); ++prev_index) {
        for (size_t current_index = 1; current_index <= current_normalized.size(); ++current_index) {
            if (!previous_normalized[prev_index - 1].empty() &&
                previous_normalized[prev_index - 1] == current_normalized[current_index - 1]) {
                lcs[prev_index][current_index] = lcs[prev_index - 1][current_index - 1] + 1;
            } else {
                lcs[prev_index][current_index] =
                    std::max(lcs[prev_index - 1][current_index], lcs[prev_index][current_index - 1]);
            }
        }
    }

    std::vector<bool> matched_current(current_normalized.size(), false);
    size_t prev_index = previous_normalized.size();
    size_t current_index = current_normalized.size();
    while (prev_index > 0 && current_index > 0) {
        if (!previous_normalized[prev_index - 1].empty() &&
            previous_normalized[prev_index - 1] == current_normalized[current_index - 1] &&
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

    for (size_t index = 0; index < current_normalized.size(); ++index) {
        if (!matched_current[index] && !current_normalized[index].empty()) {
            fresh_indices.push_back(index);
        }
    }
    return fresh_indices;
}

struct UiDanmakuReadResult {
    std::wstring target_title;
    std::vector<std::wstring> lines;
    std::wstring error;
};

std::wstring ReadElementName(IUIAutomationElement* element);
std::wstring ReadElementClassName(IUIAutomationElement* element);
std::wstring ControlTypeLabel(CONTROLTYPEID control_type);
bool IsIgnoredUiProbeText(const std::wstring& text);
int ScoreUiProbeText(const std::wstring& text, CONTROLTYPEID control_type);

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
    std::vector<std::wstring> text_lines;
    list_item_lines.reserve(64);
    text_lines.reserve(64);
    for (int index = 0; index < length && index < 1200; ++index) {
        winrt::com_ptr<IUIAutomationElement> element;
        if (FAILED(elements->GetElement(index, element.put())) || !element) {
            continue;
        }

        CONTROLTYPEID control_type_id = 0;
        element->get_CurrentControlType(&control_type_id);
        if (control_type_id != UIA_ListItemControlTypeId && control_type_id != UIA_TextControlTypeId) {
            continue;
        }

        const std::wstring text = CleanUiDanmakuText(ReadElementName(element.get()));
        if (!IsUiDanmakuMessageText(text)) {
            continue;
        }

        if (control_type_id == UIA_ListItemControlTypeId) {
            list_item_lines.push_back(text);
        } else {
            text_lines.push_back(text);
        }
    }

    result.lines = !list_item_lines.empty() ? std::move(list_item_lines) : std::move(text_lines);
    if (result.lines.size() > kMaxUiLiveVisibleLines) {
        result.lines.erase(
            result.lines.begin(),
            result.lines.end() - static_cast<std::ptrdiff_t>(kMaxUiLiveVisibleLines));
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

bool DanmakuController::SelectRegion() {
    const RECT screen_bounds = GetDesktopCaptureBounds();
    NormalizedRegion next_region{};
    if (!ShowRegionOverlay(screen_bounds, &next_region)) {
        SetStatus(L"\u5df2\u53d6\u6d88\u5f39\u5e55\u533a\u57df\u9009\u62e9");
        return false;
    }

    std::wstring label;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        region_ = next_region;
        ResetGiftCandidateLocked();
        visual_baseline_ready_ = false;
        last_gift_tick_ = 0;
        SaveRegionLocked();
        label = BuildRegionLabelLocked();
        status_text_ = L"\u5f39\u5e55\u533a\u57df\u5df2\u66f4\u65b0";
    }

    if (log_fn_) {
        log_fn_(std::wstring(L"\u5f39\u5e55\u8bc6\u522b\uff1a\u5df2\u66f4\u65b0\u533a\u57df\uff0c") + label);
    }
    return true;
}

bool DanmakuController::TestRecognizeFrame() {
    return CaptureAndRecognizeFrame(true);
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
        ui_probe_status_ = L"\u8bf7\u5728 3 \u79d2\u5185\u5207\u5230\u89c6\u9891\u53f7\u76f4\u64ad\u52a9\u624b\u8bc4\u8bba\u533a";
        recent_probe_lines_.clear();
    }

    if (ui_probe_thread_.joinable()) {
        ui_probe_thread_.join();
    }
    ui_probe_thread_ = std::thread(&DanmakuController::UiProbeLoop, this);
    if (log_fn_) {
        log_fn_(L"\u5f39\u5e55\u8bc6\u522b\uff1a\u5df2\u542f\u52a8 UIA \u63a2\u6d4b\uff0c\u8bf7\u5c06\u89c6\u9891\u53f7\u76f4\u64ad\u52a9\u624b\u5207\u5230\u524d\u53f0\u3002");
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
    if (ui_probe_running_) {
        ui_probe_status_ = L"\u6b63\u5728\u8fdb\u884c UIA \u63a2\u6d4b";
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
        if (!ui_probe_running_) {
            ui_probe_status_ = L"\u7b49\u5f85\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97";
        }
        status_text_ = L"\u76d1\u542c\u4e2d\uff0c\u7b49\u5f85\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\u51fa\u73b0";
        return false;
    }

    UiDanmakuReadResult result = ReadUiDanmakuWindow(preferred_windows.front().hwnd);
    if (!result.error.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ui_live_target_title_ = result.target_title;
        if (!result.target_title.empty()) {
            ui_probe_target_title_ = result.target_title;
        }
        ui_probe_status_ = std::wstring(L"\u8bfb\u53d6\u5931\u8d25\uff1a") + result.error;
        status_text_ = std::wstring(L"\u76d1\u542c\u4e2d\uff0c") + result.error;
        return false;
    }

    std::deque<std::wstring> previous_lines;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous_lines = ui_live_visible_lines_;
    }

    const size_t overlap = FindUiLineOverlap(previous_lines, result.lines);
    std::vector<size_t> fresh_line_indices;
    if (!previous_lines.empty()) {
        if (overlap > 0) {
            fresh_line_indices.reserve(result.lines.size() - overlap);
            for (size_t index = overlap; index < result.lines.size(); ++index) {
                fresh_line_indices.push_back(index);
            }
        } else {
            fresh_line_indices = CollectUiFreshLineIndicesByLcs(previous_lines, result.lines);
        }
    }
    const ULONGLONG now_tick = GetTickCount64();
    std::vector<std::pair<EventKind, std::wstring>> accepted_events;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ui_live_target_title_ = result.target_title;
        if (!result.target_title.empty()) {
            ui_probe_target_title_ = result.target_title;
        }
        ui_probe_status_ = result.lines.empty()
            ? L"\u5df2\u8fde\u63a5\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\uff0c\u6682\u65e0\u53ef\u89c1\u5f39\u5e55"
            : (std::wstring(L"\u5df2\u8fde\u63a5\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\uff0c\u5f53\u524d\u53ef\u89c1 ") +
                std::to_wstring(result.lines.size()) + L" \u6761");

        if (result.lines.empty()) {
            status_text_ = result.target_title.empty()
                ? L"\u5df2\u8fde\u63a5\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\uff0c\u6682\u65e0\u53ef\u89c1\u5f39\u5e55"
                : (std::wstring(L"\u5df2\u8fde\u63a5 ") + result.target_title + L"\uff0c\u6682\u65e0\u53ef\u89c1\u5f39\u5e55");
        } else if (previous_lines.empty()) {
            status_text_ = result.target_title.empty()
                ? L"\u5df2\u8fde\u63a5\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\uff0c\u5df2\u540c\u6b65\u5f53\u524d\u53ef\u89c1\u5f39\u5e55"
                : (std::wstring(L"\u5df2\u8fde\u63a5 ") + result.target_title + L"\uff0c\u5df2\u540c\u6b65\u5f53\u524d\u53ef\u89c1\u5f39\u5e55");
        } else {
            for (size_t line_index : fresh_line_indices) {
                const std::wstring cleaned = CleanUiDanmakuText(result.lines[line_index]);
                if (cleaned.empty()) {
                    continue;
                }

                EventKind kind = EventKind::kText;
                std::wstring accepted_text = cleaned;
                if (!IsUiTextDanmakuText(cleaned) && IsUiGiftMessageText(cleaned)) {
                    kind = EventKind::kImageGift;
                    accepted_text = L"\u6709\u793c\u7269";
                } else {
                    const std::wstring normalized = NormalizeText(cleaned);
                    if (normalized.empty()) {
                        continue;
                    }
                }

                AppendEventLocked(kind, accepted_text, std::wstring{}, now_tick);
                accepted_events.emplace_back(kind, accepted_text);
            }

            if (accepted_events.empty()) {
                status_text_ = result.target_title.empty()
                    ? L"\u76d1\u542c\u4e2d\uff0c\u5df2\u8fde\u63a5\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97"
                    : (std::wstring(L"\u76d1\u542c\u4e2d\uff0c\u5df2\u8fde\u63a5 ") + result.target_title);
            }
        }

        ui_live_visible_lines_.clear();
        for (const auto& line : result.lines) {
            ui_live_visible_lines_.push_back(line);
        }
        while (ui_live_visible_lines_.size() > kMaxUiLiveVisibleLines) {
            ui_live_visible_lines_.pop_front();
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
        ApplyUiProbeResult(ProbeWindowByUiAutomation(preferred_windows.front().hwnd));
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

    ApplyUiProbeResult(ProbeWindowByUiAutomation(target_window));
}

void DanmakuController::ApplyUiProbeResult(UiProbeResult result) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ui_probe_running_ = false;
        ui_probe_target_title_ = std::move(result.target_title);
        ui_probe_status_ = result.status_text.empty()
            ? L"UIA \u63a2\u6d4b\u5df2\u5b8c\u6210"
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
        const std::wstring log_line = std::wstring(L"\u5f39\u5e55\u8bc6\u522b\uff1aUIA \u63a2\u6d4b -> ") + GetSnapshot().ui_probe_status;
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
            duplicate = IsDuplicateLocked(normalized, now_tick);
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
        AppendEventLocked(event_kind, accepted_text, capture_path, now_tick);
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

    const int capture_width = capture_rect.right > capture_rect.left ? static_cast<int>(capture_rect.right - capture_rect.left) : 1;
    const int capture_height = capture_rect.bottom > capture_rect.top ? static_cast<int>(capture_rect.bottom - capture_rect.top) : 1;

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
            *error = L"\u65e0\u6cd5\u521b\u5efa\u5f39\u5e55\u622a\u56fe\u7f13\u51b2";
        }
        return false;
    }

    const HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
    if (!BitBlt(memory_dc, 0, 0, capture_width, capture_height, screen_dc, capture_rect.left, capture_rect.top, SRCCOPY | CAPTUREBLT)) {
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
    frame->screen_rect = capture_rect;
    frame->pixels.resize(static_cast<size_t>(capture_width) * static_cast<size_t>(capture_height) * 4u);

    if (GetDIBits(memory_dc, bitmap, 0, static_cast<UINT>(capture_height), frame->pixels.data(), &bitmap_info, DIB_RGB_COLORS) == 0) {
        frame->pixels.clear();
        SelectObject(memory_dc, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        if (error != nullptr) {
            *error = L"\u5f39\u5e55\u622a\u56fe\u50cf\u7d20\u8bfb\u53d6\u5931\u8d25";
        }
        return false;
    }

    SelectObject(memory_dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    return true;
}

std::wstring DanmakuController::RecognizeText(const FrameCapture& frame, std::wstring* error) const {
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) {
        if (error != nullptr) {
            *error = L"\u622a\u56fe\u5185\u5bb9\u4e3a\u7a7a";
        }
        return {};
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
            return {};
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
        std::wostringstream stream;
        bool first = true;
        const auto lines = result.Lines();
        for (uint32_t index = 0; index < lines.Size(); ++index) {
            const auto line = lines.GetAt(index);
            const std::wstring line_text = TrimWhitespace(line.Text().c_str());
            if (line_text.empty()) {
                continue;
            }
            if (!first) {
                stream << L" / ";
            }
            first = false;
            stream << line_text;
        }
        return stream.str();
    } catch (const winrt::hresult_error& exception) {
        if (error != nullptr) {
            std::wostringstream stream;
            stream << L"OCR \u89e3\u6790\u5931\u8d25\uff0c\u9519\u8bef 0x" << std::hex << exception.code().value;
            *error = stream.str();
        }
        return {};
    }
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

bool DanmakuController::IsDuplicateLocked(const std::wstring& normalized_text, ULONGLONG now_tick) {
    while (!dedupe_entries_.empty() && now_tick - dedupe_entries_.front().tick > kDedupeWindowMs) {
        dedupe_entries_.pop_front();
    }

    for (const auto& entry : dedupe_entries_) {
        if (entry.normalized_text == normalized_text) {
            return true;
        }
    }
    return false;
}

void DanmakuController::AppendEventLocked(
    EventKind kind,
    const std::wstring& text,
    const std::wstring& capture_path,
    ULONGLONG now_tick) {
    const std::wstring normalized = NormalizeText(text);
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
    bool speech_enabled = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reminder_enabled = reminder_enabled_;
        speech_enabled = speech_enabled_;
    }

    if (reminder_enabled) {
        const std::wstring reminder_sound_path = GetDanmakuReminderSoundPath();
        if (!reminder_sound_path.empty()) {
            PlaySoundW(reminder_sound_path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        } else {
            PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
        }
    }
    if (speech_enabled) {
        QueueSpeech(BuildSpeechText(kind, text));
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

DanmakuController::UiProbeResult DanmakuController::ProbeWindowByUiAutomation(HWND target_window) {
    UiProbeResult result{};
    result.target_title = ReadWindowText(target_window);
    if (result.target_title.empty()) {
        result.target_title = ReadWindowClassName(target_window);
    }

    if (target_window == nullptr || !IsWindow(target_window)) {
        result.status_text = L"\u6ca1\u6293\u5230\u6709\u6548\u7684\u524d\u53f0\u7a97\u53e3\uff0c\u8bf7\u91cd\u8bd5";
        return result;
    }

    const HRESULT apartment_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment_result) && apartment_result != RPC_E_CHANGED_MODE) {
        result.status_text = L"UIA COM \u521d\u59cb\u5316\u5931\u8d25";
        return result;
    }

    winrt::com_ptr<IUIAutomation> automation;
    const HRESULT automation_result = CoCreateInstance(
        CLSID_CUIAutomation,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(automation.put()));
    if (FAILED(automation_result) || !automation) {
        result.status_text = L"\u65e0\u6cd5\u521b\u5efa Windows UI Automation \u63a2\u6d4b\u5668";
        if (SUCCEEDED(apartment_result)) {
            CoUninitialize();
        }
        return result;
    }

    winrt::com_ptr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(target_window, root.put())) || !root) {
        result.status_text = L"\u65e0\u6cd5\u8bfb\u53d6\u76ee\u6807\u7a97\u53e3\u7684 UIA \u6839\u8282\u70b9";
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
        result.status_text = L"UIA \u63a7\u4ef6\u6811\u626b\u63cf\u5931\u8d25";
        if (SUCCEEDED(apartment_result)) {
            CoUninitialize();
        }
        return result;
    }

    struct ProbeHit {
        int score = 0;
        std::wstring text;
        std::wstring control_type;
        std::wstring class_name;
    };

    int length = 0;
    elements->get_Length(&length);
    std::vector<ProbeHit> hits;
    std::vector<std::wstring> fallback_lines;
    std::vector<std::wstring> seen_texts;
    seen_texts.reserve(64);

    const auto already_seen = [&seen_texts](const std::wstring& text) {
        return std::find(seen_texts.begin(), seen_texts.end(), text) != seen_texts.end();
    };

    for (int index = 0; index < length && index < 900; ++index) {
        winrt::com_ptr<IUIAutomationElement> element;
        if (FAILED(elements->GetElement(index, element.put())) || !element) {
            continue;
        }

        const std::wstring text = ReadElementName(element.get());
        if (text.empty() || already_seen(text)) {
            continue;
        }

        CONTROLTYPEID control_type_id = 0;
        element->get_CurrentControlType(&control_type_id);
        const int score = ScoreUiProbeText(text, control_type_id);
        const std::wstring control_type = ControlTypeLabel(control_type_id);
        const std::wstring class_name = ReadElementClassName(element.get());
        seen_texts.push_back(text);

        if (score >= 4) {
            hits.push_back({score, text, control_type, class_name});
        } else if (!IsIgnoredUiProbeText(text) && text.size() >= 2 && fallback_lines.size() < kMaxProbeLines) {
            std::wstring line = control_type + L"\uff1a" + text;
            if (!class_name.empty()) {
                line += L"  (" + class_name + L")";
            }
            fallback_lines.push_back(std::move(line));
        }
    }

    std::sort(hits.begin(), hits.end(), [](const ProbeHit& left, const ProbeHit& right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return left.text.size() < right.text.size();
    });

    if (!result.target_title.empty()) {
        result.lines.push_back(L"\u76ee\u6807\u7a97\u53e3\uff1a" + result.target_title);
    }

    if (!hits.empty()) {
        const size_t limit = std::min(hits.size(), kMaxProbeLines - result.lines.size());
        for (size_t index = 0; index < limit; ++index) {
            std::wstring line =
                L"\u5019\u9009 " + std::to_wstring(index + 1) +
                L"\uff08" + hits[index].control_type + L"\uff09\uff1a" + hits[index].text;
            if (!hits[index].class_name.empty()) {
                line += L"  (" + hits[index].class_name + L")";
            }
            result.lines.push_back(std::move(line));
        }
        result.status_text =
            L"UIA \u53ef\u8bfb\u53d6\uff0c\u5df2\u627e\u5230 " + std::to_wstring(hits.size()) +
            L" \u6761\u5019\u9009\u6587\u672c\uff0c\u9002\u5408\u7ee7\u7eed\u505a\u76f4\u63a5\u6293\u53d6";
    } else if (!fallback_lines.empty()) {
        for (size_t index = 0; index < fallback_lines.size() && result.lines.size() < kMaxProbeLines; ++index) {
            result.lines.push_back(fallback_lines[index]);
        }
        result.status_text =
            L"UIA \u80fd\u8bfb\u5230\u4e00\u4e9b\u63a7\u4ef6\u6587\u5b57\uff0c\u4f46\u8fd8\u6ca1\u9501\u5b9a\u5230\u660e\u786e\u7684\u8bc4\u8bba\u5217\u8868";
    } else {
        result.status_text =
            L"UIA \u51e0\u4e4e\u8bfb\u4e0d\u5230\u6709\u6548\u6587\u5b57\uff0c\u8fd9\u4e2a\u7a97\u53e3\u53ef\u80fd\u662f\u81ea\u7ed8\u6216 GPU \u6e32\u67d3\u63a7\u4ef6";
    }

    if (SUCCEEDED(apartment_result)) {
        CoUninitialize();
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
