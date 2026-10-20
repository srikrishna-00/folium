// SPDX-License-Identifier: Apache-2.0
#include "uapp/ai/ai_types.h"

namespace uapp::ai {

const char* capability_name(Capability c) noexcept {
    switch (c) {
        case Capability::Generate:  return "GENERATE";
        case Capability::Chat:      return "CHAT";
        case Capability::Embed:     return "EMBED";
        case Capability::Summarize: return "SUMMARIZE";
        case Capability::Classify:  return "CLASSIFY";
        case Capability::Extract:   return "EXTRACT";
        case Capability::Translate: return "TRANSLATE";
    }
    // Reached only if a caller casts a bogus integer in. The enum being closed
    // is not a reason to trust the value, per core/common/status.cpp.
    return "UNKNOWN_CAPABILITY";
}

const char* locality_name(Locality l) noexcept {
    switch (l) {
        case Locality::OnDevice:         return "ON_DEVICE";
        case Locality::FirstPartyServer: return "FIRST_PARTY_SERVER";
        case Locality::ThirdParty:       return "THIRD_PARTY";
    }
    return "UNKNOWN_LOCALITY";
}

const char* output_provenance_name(OutputProvenance p) noexcept {
    switch (p) {
        // Spelled out rather than shortened. This string is what a log line
        // or a debug overlay shows, and "HEURISTIC" alone would read as a kind
        // of model.
        case OutputProvenance::HeuristicNoModel: return "HEURISTIC_NO_MODEL";
        case OutputProvenance::LanguageModel:    return "LANGUAGE_MODEL";
    }
    return "UNKNOWN_PROVENANCE";
}

const char* role_name(Role r) noexcept {
    switch (r) {
        case Role::System:    return "SYSTEM";
        case Role::User:      return "USER";
        case Role::Document:  return "DOCUMENT";
        case Role::Assistant: return "ASSISTANT";
        case Role::Tool:      return "TOOL";
    }
    return "UNKNOWN_ROLE";
}

}  // namespace uapp::ai
