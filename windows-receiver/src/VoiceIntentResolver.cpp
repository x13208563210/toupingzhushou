#include "VoiceIntentResolver.h"

#include <algorithm>
#include <array>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <unordered_set>
#include <vector>

namespace {

struct IntentAliasEntry {
    VoiceMediaCommandIntent intent;
    const wchar_t* phrase;
};

constexpr std::array<IntentAliasEntry, 7> kIntentAliases = {{
    {VoiceMediaCommandIntent::kPlay, L"\u64AD\u653E\u97F3\u4E50"},
    {VoiceMediaCommandIntent::kPlay, L"\u97F3\u4E50\u64AD\u653E"},
    {VoiceMediaCommandIntent::kPause, L"\u6682\u505C\u97F3\u4E50"},
    {VoiceMediaCommandIntent::kPause, L"\u97F3\u4E50\u6682\u505C"},
    {VoiceMediaCommandIntent::kPrevious, L"\u4E0A\u4E00\u66F2"},
    {VoiceMediaCommandIntent::kPrevious, L"\u4E0A\u4E00\u9996"},
    {VoiceMediaCommandIntent::kPrevious, L"\u4E0A\u4E00\u9996\u6B4C"},
}};

constexpr std::array<IntentAliasEntry, 3> kNextIntentAliases = {{
    {VoiceMediaCommandIntent::kNext, L"\u4E0B\u4E00\u66F2"},
    {VoiceMediaCommandIntent::kNext, L"\u4E0B\u4E00\u9996"},
    {VoiceMediaCommandIntent::kNext, L"\u4E0B\u4E00\u9996\u6B4C"},
}};

constexpr std::array<const wchar_t*, 9> kPrefixFillers = {{
    L"\u8BF7",
    L"\u9EBB\u70E6",
    L"\u5E2E\u6211",
    L"\u7ED9\u6211",
    L"\u7ED9",
    L"\u5E2E",
    L"\u73B0\u5728",
    L"\u5148",
    L"\u9A6C\u4E0A",
}};

constexpr std::array<const wchar_t*, 8> kSuffixFillers = {{
    L"\u5427",
    L"\u5440",
    L"\u554A",
    L"\u5462",
    L"\u8C22\u8C22",
    L"\u4E00\u4E0B",
    L"\u4E00\u4E0B\u5427",
    L"\u4E00\u4E0B\u554A",
}};

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

std::wstring NormalizeText(const std::wstring& value) {
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

bool StartsWith(const std::wstring& text, const wchar_t* prefix) {
    if (prefix == nullptr) {
        return false;
    }

    const size_t prefix_length = std::wcslen(prefix);
    return text.size() >= prefix_length && text.compare(0, prefix_length, prefix) == 0;
}

bool EndsWith(const std::wstring& text, const wchar_t* suffix) {
    if (suffix == nullptr) {
        return false;
    }

    const size_t suffix_length = std::wcslen(suffix);
    return text.size() >= suffix_length &&
           text.compare(text.size() - suffix_length, suffix_length, suffix) == 0;
}

std::wstring StripOuterFillers(std::wstring value) {
    bool changed = true;
    while (changed && !value.empty()) {
        changed = false;
        for (const wchar_t* prefix : kPrefixFillers) {
            if (StartsWith(value, prefix)) {
                value.erase(0, std::wcslen(prefix));
                changed = true;
                break;
            }
        }
        if (changed || value.empty()) {
            continue;
        }
        for (const wchar_t* suffix : kSuffixFillers) {
            if (EndsWith(value, suffix)) {
                value.erase(value.size() - std::wcslen(suffix));
                changed = true;
                break;
            }
        }
    }
    return value;
}

size_t EditDistanceWithinLimit(const std::wstring& lhs, const std::wstring& rhs, size_t limit) {
    if (lhs == rhs) {
        return 0;
    }

    const size_t lhs_length = lhs.size();
    const size_t rhs_length = rhs.size();
    if (lhs_length == 0 || rhs_length == 0) {
        const size_t distance = lhs_length + rhs_length;
        return distance <= limit ? distance : limit + 1;
    }

    if (lhs_length > rhs_length + limit || rhs_length > lhs_length + limit) {
        return limit + 1;
    }

    std::vector<size_t> previous(rhs_length + 1);
    std::vector<size_t> current(rhs_length + 1);
    for (size_t j = 0; j <= rhs_length; ++j) {
        previous[j] = j;
    }

    for (size_t i = 1; i <= lhs_length; ++i) {
        current[0] = i;
        size_t row_min = current[0];
        for (size_t j = 1; j <= rhs_length; ++j) {
            const size_t replace_cost = lhs[i - 1] == rhs[j - 1] ? 0 : 1;
            current[j] = std::min({
                previous[j] + 1,
                current[j - 1] + 1,
                previous[j - 1] + replace_cost,
            });
            row_min = std::min(row_min, current[j]);
        }
        if (row_min > limit) {
            return limit + 1;
        }
        previous.swap(current);
    }

    return previous[rhs_length];
}

template <size_t N>
void AppendAliases(
    const std::array<IntentAliasEntry, N>& aliases,
    std::vector<std::wstring>* phrases) {
    if (phrases == nullptr) {
        return;
    }

    for (const auto& alias : aliases) {
        if (alias.phrase != nullptr && *alias.phrase != L'\0') {
            phrases->push_back(alias.phrase);
        }
    }
}

template <size_t N>
void ResolveAliases(
    const std::array<IntentAliasEntry, N>& aliases,
    const std::wstring& normalized_phrase,
    const std::wstring& normalized_core_phrase,
    VoiceMediaIntentMatch* best_match) {
    if (best_match == nullptr) {
        return;
    }

    auto try_alias = [&](const IntentAliasEntry& alias_entry) {
        const std::wstring alias = NormalizeText(alias_entry.phrase == nullptr ? L"" : alias_entry.phrase);
        if (alias.empty()) {
            return;
        }

        auto update_match = [&](const std::wstring& matched_input, float score) {
            if (score <= best_match->score) {
                return;
            }
            best_match->intent = alias_entry.intent;
            best_match->matched_phrase = alias;
            best_match->score = score;
            best_match->accepted = score >= 0.86f;
            if (matched_input == normalized_core_phrase && normalized_core_phrase != normalized_phrase) {
                best_match->normalized_core_phrase = normalized_core_phrase;
            }
        };

        if (!normalized_phrase.empty() && normalized_phrase == alias) {
            update_match(normalized_phrase, 1.0f);
            return;
        }

        if (!normalized_core_phrase.empty() && normalized_core_phrase == alias) {
            update_match(normalized_core_phrase, 0.96f);
            return;
        }

        const std::wstring candidate =
            normalized_core_phrase.empty() ? normalized_phrase : normalized_core_phrase;
        if (candidate.size() < 3 || alias.size() < 3) {
            return;
        }

        const size_t distance = EditDistanceWithinLimit(candidate, alias, 1);
        if (distance == 1) {
            update_match(candidate, 0.87f);
        }
    };

    for (const auto& alias : aliases) {
        try_alias(alias);
    }
}

}  // namespace

namespace VoiceIntentResolver {

std::vector<std::wstring> CollectMediaCommandPhrases() {
    std::unordered_set<std::wstring> dedup;
    std::vector<std::wstring> phrases;

    auto append_unique = [&](const std::wstring& phrase) {
        if (phrase.empty()) {
            return;
        }
        if (dedup.insert(phrase).second) {
            phrases.push_back(phrase);
        }
    };

    std::vector<std::wstring> collected;
    collected.reserve(kIntentAliases.size() + kNextIntentAliases.size());
    AppendAliases(kIntentAliases, &collected);
    AppendAliases(kNextIntentAliases, &collected);
    for (const auto& phrase : collected) {
        append_unique(phrase);
    }
    return phrases;
}

std::wstring NormalizePhrase(const std::wstring& phrase) {
    return NormalizeText(phrase);
}

VoiceMediaIntentMatch ResolveMediaCommand(const std::wstring& phrase) {
    VoiceMediaIntentMatch match{};
    match.original_phrase = phrase;
    match.normalized_phrase = NormalizeText(phrase);
    match.normalized_core_phrase = StripOuterFillers(match.normalized_phrase);
    if (match.normalized_core_phrase.empty()) {
        match.normalized_core_phrase = match.normalized_phrase;
    }

    ResolveAliases(kIntentAliases, match.normalized_phrase, match.normalized_core_phrase, &match);
    ResolveAliases(kNextIntentAliases, match.normalized_phrase, match.normalized_core_phrase, &match);
    return match;
}

const wchar_t* IntentDisplayText(VoiceMediaCommandIntent intent) {
    switch (intent) {
    case VoiceMediaCommandIntent::kPlay:
        return L"\u64AD\u653E";
    case VoiceMediaCommandIntent::kPause:
        return L"\u6682\u505C";
    case VoiceMediaCommandIntent::kPrevious:
        return L"\u4E0A\u4E00\u9996";
    case VoiceMediaCommandIntent::kNext:
        return L"\u4E0B\u4E00\u9996";
    case VoiceMediaCommandIntent::kNone:
    default:
        return L"\u672A\u5339\u914D";
    }
}

}  // namespace VoiceIntentResolver
