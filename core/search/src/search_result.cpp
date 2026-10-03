// SPDX-License-Identifier: Apache-2.0
#include "uapp/search/search_result.h"

#include "uapp/search/search_index.h"

namespace uapp::search {

// Out-of-line virtual destructor: one vtable per program instead of one per
// translation unit that includes the header. Same reason as
// core/common/src/ports.cpp.
SearchIndex::SearchIndex() noexcept = default;
SearchIndex::~SearchIndex() = default;

const char* source_kind_name(SourceKind kind) noexcept {
    switch (kind) {
        case SourceKind::Note:  return "note";
        case SourceKind::Page:  return "page";
        case SourceKind::Block: return "block";
    }
    // Reachable only via an integer cast into the enum, which is exactly what
    // reading the persisted `source_kind` column does. Never null.
    return "unknown";
}

bool source_kind_is_valid(SourceKind kind) noexcept {
    const auto raw = static_cast<std::int32_t>(kind);
    return raw >= 0 && raw < kSourceKindCount;
}

}  // namespace uapp::search
