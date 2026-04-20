#include "VoiceMusicLibrary.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_set>

namespace {

constexpr wchar_t kChineseDigits[] = L"\u96F6\u4E00\u4E8C\u4E09\u56DB\u4E94\u516D\u4E03\u516B\u4E5D";

std::wstring ToLowerAscii(const std::wstring& value) {
    std::wstring lowered = value;
    std::transform(
        lowered.begin(),
        lowered.end(),
        lowered.begin(),
        [](wchar_t ch) {
            if (ch >= L'A' && ch <= L'Z') {
                return static_cast<wchar_t>(ch - L'A' + L'a');
            }
            return ch;
        });
    return lowered;
}

bool IsIgnoredPunctuation(wchar_t ch) {
    switch (ch) {
    case L',':
    case L'.':
    case L'!':
    case L'?':
    case L':':
    case L';':
    case L'"':
    case L'\'':
    case L'\uFF0C':
    case L'\u3002':
    case L'\uFF01':
    case L'\uFF1F':
    case L'\uFF1A':
    case L'\uFF1B':
    case L'\u3001':
    case L'\u300C':
    case L'\u300D':
    case L'\u201C':
    case L'\u201D':
    case L'\uFF08':
    case L'\uFF09':
    case L'(':
    case L')':
    case L'[':
    case L']':
    case L'{':
    case L'}':
        return true;
    default:
        return false;
    }
}

std::wstring WideFromUtf8(const std::string& value) {
    if (value.empty()) {
        return L"";
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return L"";
    }

    std::wstring converted(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), converted.data(), length);
    return converted;
}

std::wstring TrimWhitespace(const std::wstring& value) {
    size_t begin = 0;
    while (begin < value.size() && std::iswspace(value[begin])) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::iswspace(value[end - 1])) {
        --end;
    }

    return value.substr(begin, end - begin);
}

std::wstring NormalizeAliasText(const std::wstring& value) {
    std::wstring normalized;
    normalized.reserve(value.size());
    for (wchar_t ch : value) {
        if (std::iswspace(ch) || IsIgnoredPunctuation(ch)) {
            continue;
        }
        normalized.push_back(ch);
    }
    return ToLowerAscii(normalized);
}

bool ContainsAnyPhrase(const std::wstring& haystack, std::initializer_list<const wchar_t*> needles) {
    if (haystack.empty()) {
        return false;
    }

    for (const wchar_t* needle : needles) {
        if (needle == nullptr || *needle == L'\0') {
            continue;
        }
        if (haystack.find(needle) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

bool LooksLikePositiveMusicRequest(const std::wstring& normalized_phrase) {
    if (normalized_phrase.empty()) {
        return false;
    }

    if (ContainsAnyPhrase(
            normalized_phrase,
            {
                L"\u505C\u6B62",
                L"\u5173\u95ED",
                L"\u505C\u6389",
                L"\u522B\u653E",
                L"\u4E0D\u8981",
                L"\u53D6\u6D88",
                L"\u5148\u522B",
            })) {
        return false;
    }

    return ContainsAnyPhrase(
        normalized_phrase,
        {
            L"\u64AD\u653E",
            L"\u70B9\u6B4C",
            L"\u70B9\u64AD",
            L"\u6765\u70B9",
            L"\u6765\u9996",
            L"\u6765\u4E00\u9996",
            L"\u653E\u4E00\u9996",
            L"\u653E\u9996",
            L"\u64AD\u4E00\u9996",
            L"\u968F\u673A\u64AD\u653E",
            L"\u968F\u673A\u6765\u9996",
        });
}

void AddAliasesFromUtf8File(
    const std::filesystem::path& file_path,
    std::unordered_set<std::wstring>* alias_set) {
    if (alias_set == nullptr) {
        return;
    }

    std::ifstream input(file_path, std::ios::binary);
    if (!input.is_open()) {
        return;
    }

    std::string raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (raw.size() >= 3 &&
        static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF) {
        raw.erase(0, 3);
    }

    std::istringstream stream(raw);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const std::wstring trimmed = TrimWhitespace(WideFromUtf8(line));
        if (trimmed.empty() || trimmed[0] == L'#' || trimmed[0] == L';') {
            continue;
        }

        alias_set->insert(NormalizeAliasText(trimmed));
    }
}

}  // namespace

VoiceMusicLibrary::VoiceMusicLibrary(std::wstring root_directory)
    : root_directory_(std::move(root_directory)),
      random_engine_(static_cast<uint32_t>(GetTickCount64())) {}

void VoiceMusicLibrary::SetRootDirectory(const std::wstring& root_directory) {
    std::lock_guard<std::mutex> lock(mutex_);
    root_directory_ = root_directory;
    cached_folders_.clear();
    last_selected_file_by_folder_.clear();
    index_ready_ = false;
}

const std::wstring& VoiceMusicLibrary::root_directory() const {
    return root_directory_;
}

bool VoiceMusicLibrary::EnsureRootDirectory() const {
    if (root_directory_.empty()) {
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(root_directory_), error);
    return !error;
}

bool VoiceMusicLibrary::RefreshIndex() {
    std::wstring root_directory;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        root_directory = root_directory_;
        cached_folders_.clear();
        index_ready_ = false;
    }

    if (root_directory.empty() || !EnsureRootDirectory()) {
        return false;
    }

    std::vector<LibraryFolder> folders = ScanFolders(root_directory);
    std::unordered_set<std::wstring> valid_folder_paths;
    valid_folder_paths.reserve(folders.size());
    for (const auto& folder : folders) {
        valid_folder_paths.insert(folder.path);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    cached_folders_ = std::move(folders);
    for (auto it = last_selected_file_by_folder_.begin(); it != last_selected_file_by_folder_.end();) {
        if (valid_folder_paths.find(it->first) == valid_folder_paths.end()) {
            it = last_selected_file_by_folder_.erase(it);
        } else {
            ++it;
        }
    }
    index_ready_ = true;
    return true;
}

std::vector<std::wstring> VoiceMusicLibrary::CollectAliases() {
    if (!EnsureIndexReady(nullptr)) {
        return {};
    }

    std::unordered_set<std::wstring> dedup;
    std::vector<std::wstring> aliases;

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& folder : cached_folders_) {
        for (const auto& alias : folder.aliases) {
            if (alias.empty()) {
                continue;
            }
            if (dedup.insert(alias).second) {
                aliases.push_back(alias);
            }
        }
    }

    std::sort(aliases.begin(), aliases.end());
    return aliases;
}

bool VoiceMusicLibrary::EnsureIndexReady(std::wstring* detail) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index_ready_) {
            return true;
        }
    }

    if (RefreshIndex()) {
        return true;
    }

    if (detail != nullptr) {
        *detail = L"\u65E0\u6CD5\u521D\u59CB\u5316\u8BED\u97F3\u70B9\u6B4C\u76EE\u5F55";
    }
    return false;
}

VoiceMusicResolveStatus VoiceMusicLibrary::ResolveRandomTrack(
    const std::wstring& phrase,
    VoiceMusicSelection* selection,
    std::wstring* detail) {
    if (selection != nullptr) {
        *selection = VoiceMusicSelection{};
    }
    if (detail != nullptr) {
        detail->clear();
    }

    const std::wstring normalized_phrase = NormalizePhrase(phrase);
    if (normalized_phrase.empty()) {
        return VoiceMusicResolveStatus::kNoMatch;
    }

    if (!EnsureIndexReady(detail)) {
        return VoiceMusicResolveStatus::kError;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const LibraryFolder* best_folder = nullptr;
    std::wstring best_alias;
    size_t best_alias_length = 0;
    const bool allow_substring_match = LooksLikePositiveMusicRequest(normalized_phrase);

    for (const auto& folder : cached_folders_) {
        for (const auto& alias : folder.aliases) {
            if (alias.empty()) {
                continue;
            }
            const bool exact_match = normalized_phrase == alias;
            const bool substring_match =
                allow_substring_match &&
                normalized_phrase.size() > alias.size() &&
                normalized_phrase.find(alias) != std::wstring::npos;
            if (exact_match || substring_match) {
                if (best_folder == nullptr || alias.size() > best_alias_length) {
                    best_folder = &folder;
                    best_alias = alias;
                    best_alias_length = alias.size();
                }
            }
        }
    }

    if (best_folder == nullptr) {
        return VoiceMusicResolveStatus::kNoMatch;
    }

    if (selection != nullptr) {
        selection->matched_alias = best_alias;
        selection->folder_name = best_folder->name;
        selection->folder_path = best_folder->path;
        selection->file_name.clear();
        selection->file_path.clear();
    }

    if (best_folder->files.empty()) {
        if (detail != nullptr) {
            *detail = L"\u76EE\u5F55\u5DF2\u547D\u4E2D\uFF0C\u4F46\u91CC\u9762\u8FD8\u6CA1\u6709\u53EF\u64AD\u653E\u7684\u97F3\u9891\u6587\u4EF6";
        }
        return VoiceMusicResolveStatus::kNoFiles;
    }

    size_t chosen_index = 0;
    if (best_folder->files.size() > 1) {
        std::uniform_int_distribution<size_t> distribution(0, best_folder->files.size() - 1);
        const auto last_it = last_selected_file_by_folder_.find(best_folder->path);
        for (size_t attempt = 0; attempt < 8; ++attempt) {
            chosen_index = distribution(random_engine_);
            if (last_it == last_selected_file_by_folder_.end() ||
                best_folder->files[chosen_index] != last_it->second) {
                break;
            }
        }
    }

    const std::wstring& selected_path = best_folder->files[chosen_index];
    last_selected_file_by_folder_[best_folder->path] = selected_path;

    if (selection != nullptr) {
        selection->file_path = selected_path;
        const size_t slash = selected_path.find_last_of(L"\\/");
        selection->file_name = slash == std::wstring::npos ? selected_path : selected_path.substr(slash + 1);
    }

    return VoiceMusicResolveStatus::kSelected;
}

std::wstring VoiceMusicLibrary::NormalizePhrase(const std::wstring& value) {
    std::wstring normalized;
    normalized.reserve(value.size());
    for (wchar_t ch : value) {
        if (std::iswspace(ch) || IsIgnoredPunctuation(ch)) {
            continue;
        }
        normalized.push_back(ch);
    }
    return ToLowerAscii(normalized);
}

bool VoiceMusicLibrary::IsSupportedAudioFile(const std::wstring& extension) {
    static const std::unordered_set<std::wstring> kExtensions = {
        L".mp3",
        L".wav",
        L".m4a",
        L".aac",
        L".flac",
        L".wma",
    };
    return kExtensions.contains(ToLowerAscii(extension));
}

bool VoiceMusicLibrary::IsDigitsOnly(const std::wstring& value) {
    if (value.empty()) {
        return false;
    }

    return std::all_of(
        value.begin(),
        value.end(),
        [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
}

std::wstring VoiceMusicLibrary::BuildChineseDigitAlias(const std::wstring& value) {
    if (!IsDigitsOnly(value)) {
        return {};
    }

    std::wstring alias;
    alias.reserve(value.size());
    for (wchar_t ch : value) {
        alias.push_back(kChineseDigits[ch - L'0']);
    }
    return alias;
}

std::vector<VoiceMusicLibrary::LibraryFolder> VoiceMusicLibrary::ScanFolders(const std::wstring& root_directory) {
    std::vector<LibraryFolder> folders;
    if (root_directory.empty()) {
        return folders;
    }

    std::error_code error;
    const std::filesystem::path root_path(root_directory);
    if (!std::filesystem::exists(root_path, error) || error) {
        return folders;
    }

    for (const auto& entry : std::filesystem::directory_iterator(root_path, error)) {
        if (error) {
            break;
        }
        if (!entry.is_directory()) {
            continue;
        }

        LibraryFolder folder;
        folder.path = entry.path().wstring();
        folder.name = entry.path().filename().wstring();

        std::unordered_set<std::wstring> alias_set;
        const std::wstring normalized_name = NormalizePhrase(folder.name);
        if (!normalized_name.empty()) {
            alias_set.insert(normalized_name);
        }

        const std::wstring digit_alias = BuildChineseDigitAlias(folder.name);
        if (!digit_alias.empty()) {
            alias_set.insert(NormalizePhrase(digit_alias));
        }

        AddAliasesFromUtf8File(entry.path() / "aliases.txt", &alias_set);
        AddAliasesFromUtf8File(entry.path() / "corrections.txt", &alias_set);
        AddAliasesFromUtf8File(entry.path() / L"\u522B\u540D.txt", &alias_set);
        AddAliasesFromUtf8File(entry.path() / L"\u7EA0\u9519.txt", &alias_set);

        folder.aliases.assign(alias_set.begin(), alias_set.end());
        std::sort(folder.aliases.begin(), folder.aliases.end());

        for (const auto& file_entry : std::filesystem::recursive_directory_iterator(entry.path(), error)) {
            if (error) {
                break;
            }
            if (!file_entry.is_regular_file()) {
                continue;
            }
            if (!IsSupportedAudioFile(file_entry.path().extension().wstring())) {
                continue;
            }
            folder.files.push_back(file_entry.path().wstring());
        }

        std::sort(folder.files.begin(), folder.files.end());
        folders.push_back(std::move(folder));
    }

    return folders;
}
