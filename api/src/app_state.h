#ifndef UAPP_API_APP_STATE_H
#define UAPP_API_APP_STATE_H

// The server's long-lived state: one of each capi service handle, opened
// once at startup against ONE real database file, held for the process's
// whole life and shared by every route handler.
//
// NOT per-request. uapp_search.h's own header is explicit that every
// handle querying the unified index must be opened on the SAME
// database_path as the note/document services that write it — opening a
// fresh handle per request would still satisfy that (same path every
// time), but would reopen a SQLite connection on every single HTTP
// request for no benefit. Opening once at startup is both the simpler
// design and the one that matches the ABI's own documented cost model
// (uapp_pdf.h's "no separate Library handle" note makes exactly this
// argument for a different handle).
//
// THREADING: every capi handle's own contract is "one thread at a time" —
// see core/capi/include/uapp/capi/uapp_capi.h. cpp-httplib dispatches
// requests across a thread pool by default, so every access to any field
// below MUST hold `mutex` first. This is the v1 choice the plan calls out
// explicitly as a scalability note, not a correctness gap: these handles
// are fast, and request volume for a single-user deployment is not the
// bottleneck a mutex would create.

#include <mutex>
#include <string>

#include "uapp/capi/uapp_blocks.h"
#include "uapp/capi/uapp_notes.h"
#include "uapp/capi/uapp_search.h"

namespace uapp::api {

struct AppState {
    std::mutex mutex;  // guards every handle below; take it before any capi call

    uapp_note_service_t*     notes    = nullptr;
    uapp_document_service_t* documents = nullptr;
    uapp_search_index_t*     search   = nullptr;

    AppState()                            = default;
    AppState(const AppState&)             = delete;
    AppState& operator=(const AppState&)  = delete;
    ~AppState();

    // Opens all three handles on `database_path` (never ":memory:" — see
    // uapp_search.h's operational rule). Returns a human-readable error and
    // false on failure; nothing is left partially open on failure (every
    // handle this call opened before the one that failed is closed again).
    [[nodiscard]] bool open(const std::string& database_path, std::string& out_error);
};

}  // namespace uapp::api

#endif  // UAPP_API_APP_STATE_H
