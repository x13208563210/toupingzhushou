#pragma once

#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

struct VoiceMusicSelection {
    std::wstring matched_alias;
    std::wstring folder_name;
    std::wstring folder_path;
    std::wstring file_name;
    std::wstring file_path;
};

enum class VoiceMusicResolveStatus {
    kNoMatch,
    kNoFiles,
    kSelected,
    kError,
};

class VoiceMusicLibrary {
public:
    explicit VoiceMusicLibrary(std::wstring root_directory);

    void SetRootDirectory(const std::wstring& root_directory);
    const std::wstring& root_directory() const;

    bool EnsureRootDirectory() const;
    bool RefreshIndex();
    std::vector<std::wstring> CollectAliases();
    VoiceMusicResolveStatus ResolveRandomTrack(
        const std::wstring& phrase,
        VoiceMusicSelection* selection,
        std::wstring* detail);

private:
    struct LibraryFolder {
        std::wstring name;
        std::wstring path;
        std::vector<std::wstring> aliases;
        std::vector<std::wstring> files;
    };

    static std::wstring NormalizePhrase(const std::wstring& value);
    static bool IsSupportedAudioFile(const std::wstring& extension);
    static bool IsDigitsOnly(const std::wstring& value);
    static std::wstring BuildChineseDigitAlias(const std::wstring& value);
    static std::vector<LibraryFolder> ScanFolders(const std::wstring& root_directory);
    bool EnsureIndexReady(std::wstring* detail);

    mutable std::mutex mutex_;
    std::wstring root_directory_;
    std::vector<LibraryFolder> cached_folders_;
    std::mt19937 random_engine_;
    std::unordered_map<std::wstring, std::wstring> last_selected_file_by_folder_;
    bool index_ready_ = false;
};
