// SPDX-License-Identifier: Apache-2.0
//
// core/capi/src/capi_notes.cpp — the C99 face of uapp::notes::NoteService.
//
// House rules for every exported function in this file, without exception:
//
//   1. Validate every pointer before dereferencing it. The caller is JNI or
//      Swift interop; "the binding would never pass null" is not an argument
//      that survives contact with a generated wrapper.
//   2. Wrap the body in UAPP_CAPI_GUARD_*, so nothing can unwind out of a C
//      frame. See capi_internal.h for why this is not theatre under
//      -fno-exceptions.
//   3. Write out-parameters only on success. A caller that ignores the status
//      (they exist) must not be handed a half-filled struct; and a caller that
//      checks it must not have its previous value clobbered by a failure.
//   4. Return a status. Never a sentinel, never a nullable pointer as the
//      error signal.
//
// This layer adds no behaviour. Where it validates, it validates the same
// things the core does, earlier — never differently.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "capi_internal.h"
#include "uapp/capi/uapp_capi.h"
#include "uapp/capi/uapp_notes.h"
#include "uapp/notes/note.h"
#include "uapp/notes/note_service.h"
#include "uapp/notes/note_store.h"
#include "uapp/notes/ports.h"
#include "uapp/status.h"
#include "uapp/storage/sqlite/sqlite_note_store.h"

namespace {

using uapp::Status;
using uapp::capi::dup_string;
using uapp::capi::kNoteMagic;
using uapp::capi::kResultsMagic;
using uapp::capi::kServiceMagic;
using uapp::capi::to_c_status;

/// One search row, flattened so every string the ABI hands out is already
/// materialised and stable for the lifetime of the results handle.
struct CapiHit final {
    std::string id_hex;
    std::string title;
    std::string snippet;
    double      rank = 0.0;
};

/// `magic` is the first member of every handle struct, deliberately: the check
/// below then rejects a wrong-type handle (the mistake a generated binding is
/// most likely to make) as well as a null or zeroed one.
template <typename Handle>
[[nodiscard]] bool handle_valid(const Handle* handle, std::uint32_t expected) noexcept {
    return handle != nullptr && handle->magic == expected;
}

}  // namespace

// --- handle definitions -----------------------------------------------------
//
// Opaque to every consumer; the tags match the typedefs in uapp_notes.h.

struct uapp_note_service final {
    std::uint32_t magic = kServiceMagic;

    // Declaration order is lifetime order. NoteService holds references to the
    // other three, so it must be destroyed first — which, members being
    // destroyed in reverse order of declaration, it is.
    std::unique_ptr<uapp::notes::NoteStore>    store;
    std::unique_ptr<uapp::notes::Clock>        clock;
    std::unique_ptr<uapp::notes::IdGenerator>  ids;
    std::unique_ptr<uapp::notes::NoteService>  service;
};

struct uapp_note final {
    std::uint32_t magic = kNoteMagic;

    uapp::notes::Note note;
    /// Cached rendering of note.id, so uapp_note_get_id_hex() can hand out a
    /// borrowed pointer instead of allocating on every call. Refreshed
    /// wherever `note.id` can change, which is only at construction.
    std::string id_hex;
};

struct uapp_search_results final {
    std::uint32_t magic = kResultsMagic;

    std::vector<CapiHit> hits;
};

namespace {

/// Build a note handle from a core Note. Returns nullptr on allocation
/// failure; the caller maps that to UAPP_STATUS_OUT_OF_MEMORY.
[[nodiscard]] uapp_note* make_note_handle(const uapp::notes::Note& note) noexcept {
    uapp_note* handle = new (std::nothrow) uapp_note();
    if (handle == nullptr) {
        return nullptr;
    }
    handle->note   = note;
    handle->id_hex = note.id.to_hex();
    return handle;
}

/// Shared prologue for every row accessor: validate the handle and the index,
/// then hand back the row.
[[nodiscard]] uapp_status_t row_at(const uapp_search_results* results,
                                   std::size_t index,
                                   const CapiHit*& out) noexcept {
    if (!handle_valid(results, kResultsMagic)) {
        return UAPP_STATUS_INVALID_ARGUMENT;
    }
    if (index >= results->hits.size()) {
        // An out-of-range index is a caller bug, not a missing note. Saying
        // NOT_FOUND here would make a loop bound error look like data loss.
        return UAPP_STATUS_INVALID_ARGUMENT;
    }
    out = &results->hits[index];
    return UAPP_STATUS_OK;
}

/// Shared implementation of the rule-B (borrowed) string accessors.
[[nodiscard]] uapp_status_t borrow_note_string(const uapp_note* note,
                                               const std::string& (*pick)(const uapp_note&),
                                               const char** out) noexcept {
    if (!handle_valid(note, kNoteMagic) || out == nullptr) {
        return UAPP_STATUS_INVALID_ARGUMENT;
    }
    *out = pick(*note).c_str();
    return UAPP_STATUS_OK;
}

/// Shared implementation of the rule-O (owned) string accessors.
[[nodiscard]] uapp_status_t copy_note_string(const uapp_note* note,
                                             const std::string& (*pick)(const uapp_note&),
                                             char** out) noexcept {
    if (!handle_valid(note, kNoteMagic) || out == nullptr) {
        return UAPP_STATUS_INVALID_ARGUMENT;
    }
    const std::string& value = pick(*note);
    char* copy = dup_string(value.c_str(), value.size());
    if (copy == nullptr) {
        return UAPP_STATUS_OUT_OF_MEMORY;
    }
    *out = copy;
    return UAPP_STATUS_OK;
}

const std::string& pick_id_hex(const uapp_note& n) noexcept { return n.id_hex; }
const std::string& pick_title(const uapp_note& n) noexcept { return n.note.title; }
const std::string& pick_body(const uapp_note& n) noexcept { return n.note.body; }

}  // namespace

// ---------------------------------------------------------------------------
// Exported surface
// ---------------------------------------------------------------------------

extern "C" {

uapp_status_t uapp_note_get_limits(uapp_note_limits_t* out_limits) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_limits == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        // The leading-struct_size rule (api-design.md §10 rule 2). A caller
        // compiled against a SMALLER struct than this build's cannot be served
        // — we would write past the end of their object. A caller compiled
        // against a larger one is fine: we fill what we know and leave their
        // tail alone, which is exactly the forward compatibility the rule buys.
        if (out_limits->struct_size < sizeof(uapp_note_limits_t)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        out_limits->id_hex_length        = uapp::notes::kNoteIdHexChars;
        out_limits->max_title_bytes      = uapp::notes::kMaxTitleBytes;
        out_limits->max_body_bytes       = uapp::notes::kMaxBodyBytes;
        out_limits->max_query_bytes      = uapp::notes::kMaxQueryBytes;
        out_limits->max_search_limit     = uapp::notes::kMaxSearchLimit;
        out_limits->default_search_limit = uapp::notes::kDefaultSearchLimit;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_note_service_config_init(uapp_note_service_config_t* out_config) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_config == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        out_config->struct_size     = static_cast<std::uint32_t>(sizeof(uapp_note_service_config_t));
        out_config->database_path   = nullptr;
        out_config->read_only       = UAPP_FALSE;
        out_config->busy_timeout_ms = 5000;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_note_service_open(const uapp_note_service_config_t* config,
                                     uapp_note_service_t** out_service) {
    UAPP_CAPI_GUARD_BEGIN
        // NOTE the absence of a `*out_service = nullptr` prologue. Out-parameters
        // are not written on a failure path — see the house rules at the top of
        // this file, and the matching promise in NoteService's own header. The
        // tempting "null it so the caller sees something safe" variant silently
        // discards whatever the caller's variable was holding, which is a
        // handle leak the caller cannot even observe. Leaving it untouched
        // keeps one rule for the whole codebase instead of two.
        if (config == nullptr || config->struct_size < sizeof(uapp_note_service_config_t) ||
            config->database_path == nullptr || config->busy_timeout_ms < 0) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        std::unique_ptr<uapp_note_service> handle(new (std::nothrow) uapp_note_service());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }

        uapp::storage::sqlite::SqliteNoteStoreOptions options;
        options.path            = config->database_path;
        options.read_only       = (config->read_only != UAPP_FALSE);
        options.busy_timeout_ms = static_cast<int>(config->busy_timeout_ms);

        const Status opened = uapp::storage::sqlite::open_sqlite_note_store(options, handle->store);
        if (!uapp::is_ok(opened)) {
            return to_c_status(opened);
        }

        handle->clock = uapp::notes::make_system_clock();
        handle->ids   = uapp::notes::make_random_id_generator();
        if (handle->clock == nullptr || handle->ids == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }

        handle->service.reset(new (std::nothrow) uapp::notes::NoteService(
            *handle->store, *handle->clock, *handle->ids));
        if (handle->service == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }

        *out_service = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

void uapp_note_service_close(uapp_note_service_t** service) {
    UAPP_CAPI_GUARD_BEGIN
        if (service == nullptr || *service == nullptr) {
            return;
        }
        if (!handle_valid(*service, kServiceMagic)) {
            // Not ours, or already scrubbed. Deleting it would be worse than
            // leaking it, and there is no channel to report on. Leave it.
            return;
        }
        (*service)->magic = 0u;
        delete *service;
        *service = nullptr;
    UAPP_CAPI_GUARD_END_VOID
}

uapp_status_t uapp_note_service_create(uapp_note_service_t* service,
                                       const char* title,
                                       const char* body,
                                       uapp_note_t** out_note) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_note == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(service, kServiceMagic) || title == nullptr || body == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        uapp::notes::Note created;
        const Status status = service->service->create(std::string_view(title),
                                                       std::string_view(body), created);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }

        uapp_note* handle = make_note_handle(created);
        if (handle == nullptr) {
            // The note IS persisted; only the handle failed. Reporting OOM
            // rather than success is right — the caller got no id back — and
            // the note remains reachable by search or by count.
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        *out_note = handle;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_note_service_open_note(uapp_note_service_t* service,
                                          const char* id_hex,
                                          uapp_note_t** out_note) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_note == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(service, kServiceMagic) || id_hex == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        uapp::notes::NoteId id;
        const Status parsed = uapp::notes::NoteId::parse(std::string_view(id_hex), id);
        if (!uapp::is_ok(parsed)) {
            return to_c_status(parsed);
        }

        uapp::notes::Note loaded;
        const Status status = service->service->open(id, loaded);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }

        uapp_note* handle = make_note_handle(loaded);
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        *out_note = handle;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_note_service_save(uapp_note_service_t* service, uapp_note_t* note) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(service, kServiceMagic) || !handle_valid(note, kNoteMagic)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        // NoteService::save updates the note in place with what was actually
        // persisted, so the handle is refreshed for free. On failure the core
        // leaves it untouched, so there is nothing to roll back here either.
        const Status status = service->service->save(note->note);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        note->id_hex = note->note.id.to_hex();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_note_service_delete(uapp_note_service_t* service, const char* id_hex) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(service, kServiceMagic) || id_hex == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        uapp::notes::NoteId id;
        const Status parsed = uapp::notes::NoteId::parse(std::string_view(id_hex), id);
        if (!uapp::is_ok(parsed)) {
            return to_c_status(parsed);
        }
        return to_c_status(service->service->remove(id));
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_note_service_count(uapp_note_service_t* service, size_t* out_count) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(service, kServiceMagic) || out_count == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        std::size_t count = 0;
        const Status status = service->service->count(count);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        *out_count = count;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_note_service_search(uapp_note_service_t* service,
                                       const char* query,
                                       size_t limit,
                                       uapp_search_results_t** out_results) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_results == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(service, kServiceMagic) || query == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        const std::size_t effective_limit =
            (limit == 0u) ? uapp::notes::kDefaultSearchLimit : limit;

        std::vector<uapp::notes::SearchHit> hits;
        const Status status =
            service->service->search(std::string_view(query), hits, effective_limit);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }

        std::unique_ptr<uapp_search_results> handle(new (std::nothrow) uapp_search_results());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        handle->hits.reserve(hits.size());
        for (const uapp::notes::SearchHit& hit : hits) {
            CapiHit row;
            row.id_hex  = hit.id.to_hex();
            row.title   = hit.title;
            row.snippet = hit.snippet;
            row.rank    = hit.rank;
            handle->hits.push_back(std::move(row));
        }

        *out_results = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- note handle: borrowed reads (rule B) ----------------------------------

uapp_status_t uapp_note_get_id_hex(const uapp_note_t* note, const char** out_id_hex) {
    UAPP_CAPI_GUARD_BEGIN
        return borrow_note_string(note, &pick_id_hex, out_id_hex);
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_note_get_title(const uapp_note_t* note, const char** out_title) {
    UAPP_CAPI_GUARD_BEGIN
        return borrow_note_string(note, &pick_title, out_title);
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_note_get_body(const uapp_note_t* note, const char** out_body) {
    UAPP_CAPI_GUARD_BEGIN
        return borrow_note_string(note, &pick_body, out_body);
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_note_get_title_length(const uapp_note_t* note, size_t* out_length) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(note, kNoteMagic) || out_length == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        *out_length = note->note.title.size();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_note_get_body_length(const uapp_note_t* note, size_t* out_length) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(note, kNoteMagic) || out_length == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        *out_length = note->note.body.size();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_note_get_timestamps(const uapp_note_t* note,
                                       int64_t* out_created_at_ms,
                                       int64_t* out_modified_at_ms) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(note, kNoteMagic)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        // Both out-parameters are optional; a caller wanting only one should
        // not have to invent storage for the other.
        if (out_created_at_ms != nullptr) {
            *out_created_at_ms = note->note.created_at_ms;
        }
        if (out_modified_at_ms != nullptr) {
            *out_modified_at_ms = note->note.modified_at_ms;
        }
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- note handle: owned copies (rule O) ------------------------------------

uapp_status_t uapp_note_copy_id_hex(const uapp_note_t* note, char** out_id_hex) {
    UAPP_CAPI_GUARD_BEGIN
        return copy_note_string(note, &pick_id_hex, out_id_hex);
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_note_copy_title(const uapp_note_t* note, char** out_title) {
    UAPP_CAPI_GUARD_BEGIN
        return copy_note_string(note, &pick_title, out_title);
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_note_copy_body(const uapp_note_t* note, char** out_body) {
    UAPP_CAPI_GUARD_BEGIN
        return copy_note_string(note, &pick_body, out_body);
    UAPP_CAPI_GUARD_END
}

// --- note handle: EDIT ------------------------------------------------------

uapp_status_t uapp_note_set_title(uapp_note_t* note, const char* title) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(note, kNoteMagic) || title == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const std::string_view candidate(title);
        const Status valid = uapp::notes::validate_title(candidate);
        if (!uapp::is_ok(valid)) {
            // Validate BEFORE assigning: a rejected edit must leave the
            // handle exactly as it was, not half-applied.
            return to_c_status(valid);
        }
        note->note.title.assign(candidate);
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_note_set_body(uapp_note_t* note, const char* body) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(note, kNoteMagic) || body == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const std::string_view candidate(body);
        const Status valid = uapp::notes::validate_body(candidate);
        if (!uapp::is_ok(valid)) {
            return to_c_status(valid);
        }
        note->note.body.assign(candidate);
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

void uapp_note_free(uapp_note_t** note) {
    UAPP_CAPI_GUARD_BEGIN
        if (note == nullptr || *note == nullptr) {
            return;
        }
        if (!handle_valid(*note, kNoteMagic)) {
            return;
        }
        (*note)->magic = 0u;
        delete *note;
        *note = nullptr;
    UAPP_CAPI_GUARD_END_VOID
}

// --- search results ---------------------------------------------------------

uapp_status_t uapp_search_results_count(const uapp_search_results_t* results, size_t* out_count) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(results, kResultsMagic) || out_count == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        *out_count = results->hits.size();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_search_results_get_id_hex(const uapp_search_results_t* results,
                                             size_t index,
                                             const char** out_id_hex) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_id_hex == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const CapiHit* row = nullptr;
        const uapp_status_t status = row_at(results, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        *out_id_hex = row->id_hex.c_str();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_search_results_get_title(const uapp_search_results_t* results,
                                            size_t index,
                                            const char** out_title) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_title == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const CapiHit* row = nullptr;
        const uapp_status_t status = row_at(results, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        *out_title = row->title.c_str();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_search_results_get_snippet(const uapp_search_results_t* results,
                                              size_t index,
                                              const char** out_snippet) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_snippet == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const CapiHit* row = nullptr;
        const uapp_status_t status = row_at(results, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        *out_snippet = row->snippet.c_str();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_search_results_get_rank(const uapp_search_results_t* results,
                                           size_t index,
                                           double* out_rank) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_rank == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const CapiHit* row = nullptr;
        const uapp_status_t status = row_at(results, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        *out_rank = row->rank;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

void uapp_search_results_free(uapp_search_results_t** results) {
    UAPP_CAPI_GUARD_BEGIN
        if (results == nullptr || *results == nullptr) {
            return;
        }
        if (!handle_valid(*results, kResultsMagic)) {
            return;
        }
        (*results)->magic = 0u;
        delete *results;
        *results = nullptr;
    UAPP_CAPI_GUARD_END_VOID
}

}  // extern "C"
