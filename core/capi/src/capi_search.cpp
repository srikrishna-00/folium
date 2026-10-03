// SPDX-License-Identifier: Apache-2.0
//
// core/capi/src/capi_search.cpp — the C99 face of uapp::search::SearchIndex.
//
// The house rules from capi_notes.cpp apply unchanged: validate every pointer,
// wrap every body in UAPP_CAPI_GUARD_*, write out-parameters only on success,
// return a status rather than a sentinel.
//
// This is the smallest of the three surfaces, because the port it wraps is
// read-only by design: one query function and one diagnostic count. There is
// nothing here to translate beyond flattening a std::vector<SearchResult> into
// rows whose strings are stable for the handle's lifetime — the same shape
// capi_notes.cpp's uapp_search_results_t already uses, and for the same reason
// (no allocation per row, so JNI builds jstrings straight from the borrowed
// pointers).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capi_internal.h"
#include "uapp/capi/uapp_capi.h"
#include "uapp/capi/uapp_search.h"
#include "uapp/object_id.h"
#include "uapp/search/search_index.h"
#include "uapp/search/search_result.h"
#include "uapp/status.h"
#include "uapp/storage/sqlite/sqlite_search_index.h"

namespace {

using uapp::Status;
using uapp::capi::kSearchIndexMagic;
using uapp::capi::kUnifiedResultMagic;
using uapp::capi::to_c_status;
using uapp::search::SearchResult;
using uapp::search::SourceKind;

/// The C enum and the C++ enum must agree, forever. These values are PERSISTED
/// in the index's `source_kind` column, so a renumbering has to be a compile
/// error here rather than a client that reports a block hit as a note hit.
#define UAPP_CAPI_ASSERT_SOURCE_KIND(cpp_name, c_name)                                \
    static_assert(static_cast<std::int32_t>(SourceKind::cpp_name) ==                   \
                      static_cast<std::int32_t>(c_name),                               \
                  "uapp_source_kind_t drifted from uapp::search::SourceKind for " #cpp_name)

static_assert(sizeof(uapp_source_kind_t) == 4u, "uapp_source_kind_t must be exactly 32 bits");

UAPP_CAPI_ASSERT_SOURCE_KIND(Note, UAPP_SOURCE_KIND_NOTE);
UAPP_CAPI_ASSERT_SOURCE_KIND(Page, UAPP_SOURCE_KIND_PAGE);
UAPP_CAPI_ASSERT_SOURCE_KIND(Block, UAPP_SOURCE_KIND_BLOCK);

#undef UAPP_CAPI_ASSERT_SOURCE_KIND

// The nil-id string the public header promises for a hit with no container.
// Asserted against the shared id length so the two cannot drift.
static_assert(sizeof(UAPP_ID_NIL_HEX) - 1u == uapp::kObjectIdHexChars,
              "UAPP_ID_NIL_HEX must be exactly one id's worth of hex characters");
static_assert(UAPP_ID_HEX_LENGTH == uapp::kObjectIdHexChars,
              "UAPP_ID_HEX_LENGTH must match uapp::kObjectIdHexChars");

template <typename Handle>
[[nodiscard]] bool handle_valid(const Handle* handle, std::uint32_t expected) noexcept {
    return handle != nullptr && handle->magic == expected;
}

/// One unified hit, flattened so every string the ABI hands out is already
/// materialised and stable for the lifetime of the results handle.
struct CapiUnifiedHit final {
    std::string  source_id_hex;
    /// Rendered as UAPP_ID_NIL_HEX rather than left empty when the hit is its
    /// own container, so the accessor can promise a non-NULL, fixed-length
    /// value for every row and a binding needs no special case.
    std::string  container_id_hex;
    std::string  title;
    std::string  snippet;
    std::int32_t kind = 0;
    double       rank = 0.0;
};

[[nodiscard]] bool source_kind_in_range(uapp_source_kind_t kind) noexcept {
    const auto value = static_cast<std::int32_t>(kind);
    return value >= 0 && value < uapp::search::kSourceKindCount;
}

}  // namespace

// --- handle definitions -----------------------------------------------------

struct uapp_search_index final {
    std::uint32_t magic = kSearchIndexMagic;

    std::unique_ptr<uapp::search::SearchIndex> index;
};

struct uapp_unified_results final {
    std::uint32_t magic = kUnifiedResultMagic;

    std::vector<CapiUnifiedHit> hits;
};

namespace {

/// Shared prologue for every row accessor: validate the handle and the index.
[[nodiscard]] uapp_status_t row_at(const uapp_unified_results* results,
                                   std::size_t index,
                                   const CapiUnifiedHit*& out) noexcept {
    if (!handle_valid(results, kUnifiedResultMagic)) {
        return UAPP_STATUS_INVALID_ARGUMENT;
    }
    if (index >= results->hits.size()) {
        // An out-of-range index is a caller bug, not a missing object.
        return UAPP_STATUS_INVALID_ARGUMENT;
    }
    out = &results->hits[index];
    return UAPP_STATUS_OK;
}

}  // namespace

// ---------------------------------------------------------------------------
// Exported surface
// ---------------------------------------------------------------------------

extern "C" {

const char* uapp_source_kind_name(uapp_source_kind_t kind) {
    UAPP_CAPI_GUARD_BEGIN
        if (!source_kind_in_range(kind)) {
            return "unknown";
        }
        return uapp::search::source_kind_name(
            static_cast<SourceKind>(static_cast<std::int32_t>(kind)));
    UAPP_CAPI_GUARD_END_FALLBACK("unknown")
}

uapp_status_t uapp_search_get_limits(uapp_search_limits_t* out_limits) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_limits == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (out_limits->struct_size < sizeof(uapp_search_limits_t)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        out_limits->id_hex_length     = uapp::kObjectIdHexChars;
        out_limits->max_query_bytes   = uapp::search::kMaxQueryBytes;
        out_limits->max_results       = uapp::search::kMaxResults;
        out_limits->source_kind_count = uapp::search::kSourceKindCount;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_search_index_config_init(uapp_search_index_config_t* out_config) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_config == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        out_config->struct_size   = static_cast<std::uint32_t>(sizeof(uapp_search_index_config_t));
        out_config->database_path = nullptr;
        // TRUE, matching SqliteSearchIndexOptions. This surface cannot write,
        // so the connection behind it holds no write permission either.
        out_config->read_only       = UAPP_TRUE;
        out_config->busy_timeout_ms = 5000;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_search_index_open(const uapp_search_index_config_t* config,
                                     uapp_search_index_t** out_index) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_index == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (config == nullptr || config->struct_size < sizeof(uapp_search_index_config_t) ||
            config->database_path == nullptr || config->busy_timeout_ms < 0) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        std::unique_ptr<uapp_search_index> handle(new (std::nothrow) uapp_search_index());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }

        uapp::storage::sqlite::SqliteSearchIndexOptions options;
        options.path            = config->database_path;
        options.read_only       = (config->read_only != UAPP_FALSE);
        options.busy_timeout_ms = static_cast<int>(config->busy_timeout_ms);

        const Status opened =
            uapp::storage::sqlite::open_sqlite_search_index(options, handle->index);
        if (!uapp::is_ok(opened)) {
            return to_c_status(opened);
        }

        *out_index = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

void uapp_search_index_close(uapp_search_index_t** index) {
    UAPP_CAPI_GUARD_BEGIN
        if (index == nullptr || *index == nullptr) {
            return;
        }
        if (!handle_valid(*index, kSearchIndexMagic)) {
            return;
        }
        (*index)->magic = 0u;
        delete *index;
        *index = nullptr;
    UAPP_CAPI_GUARD_END_VOID
}

uapp_status_t uapp_search_index_query(uapp_search_index_t* index,
                                      const char* query,
                                      size_t limit,
                                      uapp_unified_results_t** out_results) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_results == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(index, kSearchIndexMagic) || query == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        // `limit == 0` is passed straight through and comes back as
        // InvalidArgument from the port. It is NOT silently turned into a
        // default: uapp_search.h documents the difference from
        // uapp_note_service_search() explicitly, and inventing a default the
        // core does not have is exactly the kind of behaviour this layer is
        // not allowed to add.
        std::vector<SearchResult> hits;
        const Status status = index->index->query(std::string_view(query), limit, hits);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }

        std::unique_ptr<uapp_unified_results> handle(new (std::nothrow) uapp_unified_results());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        handle->hits.reserve(hits.size());
        for (const SearchResult& hit : hits) {
            CapiUnifiedHit row;
            row.kind          = static_cast<std::int32_t>(hit.kind);
            row.source_id_hex = uapp::detail::object_id_to_hex(hit.source_id);
            // uapp::detail::object_id_to_hex renders an all-zero id as 32 '0'
            // characters, which IS UAPP_ID_NIL_HEX — so no branch is needed to
            // honour the header's "never NULL, nil id for Note and Page"
            // promise. The static_assert above is what keeps that true.
            row.container_id_hex = uapp::detail::object_id_to_hex(hit.container_id);
            row.title            = hit.title;
            row.snippet          = hit.snippet;
            row.rank             = hit.rank;
            handle->hits.push_back(std::move(row));
        }

        *out_results = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_search_index_entry_count(uapp_search_index_t* index, size_t* out_count) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(index, kSearchIndexMagic) || out_count == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        std::size_t count = 0;
        const Status status = index->index->entry_count(count);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        *out_count = count;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- results ----------------------------------------------------------------

uapp_status_t uapp_unified_results_count(const uapp_unified_results_t* results,
                                         size_t* out_count) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(results, kUnifiedResultMagic) || out_count == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        *out_count = results->hits.size();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_unified_results_get_kind(const uapp_unified_results_t* results,
                                            size_t index,
                                            uapp_source_kind_t* out_kind) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_kind == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const CapiUnifiedHit* row = nullptr;
        const uapp_status_t status = row_at(results, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        *out_kind = static_cast<uapp_source_kind_t>(row->kind);
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_unified_results_get_source_id_hex(const uapp_unified_results_t* results,
                                                     size_t index,
                                                     const char** out_id_hex) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_id_hex == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const CapiUnifiedHit* row = nullptr;
        const uapp_status_t status = row_at(results, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        *out_id_hex = row->source_id_hex.c_str();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_unified_results_get_container_id_hex(const uapp_unified_results_t* results,
                                                        size_t index,
                                                        const char** out_id_hex) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_id_hex == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const CapiUnifiedHit* row = nullptr;
        const uapp_status_t status = row_at(results, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        *out_id_hex = row->container_id_hex.c_str();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_unified_results_get_title(const uapp_unified_results_t* results,
                                             size_t index,
                                             const char** out_title) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_title == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const CapiUnifiedHit* row = nullptr;
        const uapp_status_t status = row_at(results, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        *out_title = row->title.c_str();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_unified_results_get_snippet(const uapp_unified_results_t* results,
                                               size_t index,
                                               const char** out_snippet) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_snippet == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const CapiUnifiedHit* row = nullptr;
        const uapp_status_t status = row_at(results, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        *out_snippet = row->snippet.c_str();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_unified_results_get_rank(const uapp_unified_results_t* results,
                                            size_t index,
                                            double* out_rank) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_rank == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const CapiUnifiedHit* row = nullptr;
        const uapp_status_t status = row_at(results, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        *out_rank = row->rank;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

void uapp_unified_results_free(uapp_unified_results_t** results) {
    UAPP_CAPI_GUARD_BEGIN
        if (results == nullptr || *results == nullptr) {
            return;
        }
        if (!handle_valid(*results, kUnifiedResultMagic)) {
            return;
        }
        (*results)->magic = 0u;
        delete *results;
        *results = nullptr;
    UAPP_CAPI_GUARD_END_VOID
}

}  // extern "C"
