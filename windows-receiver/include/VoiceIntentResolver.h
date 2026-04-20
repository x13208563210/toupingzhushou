#pragma once

#include <string>
#include <vector>

enum class VoiceMediaCommandIntent {
    kNone,
    kPlay,
    kPause,
    kPrevious,
    kNext,
};

struct VoiceMediaIntentMatch {
    VoiceMediaCommandIntent intent = VoiceMediaCommandIntent::kNone;
    std::wstring original_phrase;
    std::wstring normalized_phrase;
    std::wstring normalized_core_phrase;
    std::wstring matched_phrase;
    float score = 0.0f;
    bool accepted = false;
};

namespace VoiceIntentResolver {

std::vector<std::wstring> CollectMediaCommandPhrases();
std::wstring NormalizePhrase(const std::wstring& phrase);
VoiceMediaIntentMatch ResolveMediaCommand(const std::wstring& phrase);
const wchar_t* IntentDisplayText(VoiceMediaCommandIntent intent);

}  // namespace VoiceIntentResolver
