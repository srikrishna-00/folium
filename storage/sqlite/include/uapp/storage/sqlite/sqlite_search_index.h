// SPDX-License-Identifier: Apache-2.0
//
// storage/sqlite/sqlite_search_index.h — the SQLite/FTS5 adapter for the
// unified search port.
//
// Like the two store factories next to it, this header exposes ONLY a factory
// returning the core-owned interface: no <sqlite3.h>, no sqlite3 type in any
// signature (PHASE0_SUMMARY.md H-3).
//
// ===========================================================================
// HOW "UNIFIED" IS ACTUALLY ACHIEVED — READ THIS BEFORE WIRING IT UP
// ===========================================================================
// There is ONE index table, written by every content writer. For a query to
// see notes AND pages/blocks, the note store, the block store and this index
// must all be opened on the SAME database `path`.
//
// That is not a limitation bolted on to make the implementation easier; it is
// architecture.md section 3 Revision 2 ("There is exactly one persistence
// port. Document, file, search, AI-metadata and sync modules all go through
// it"). The two stores already coexist in one file without colliding: their
// tables are disjoint and they version themselves under different
// `schema_meta` keys.
//
// If they are opened on DIFFERENT files — which the pre-existing per-store
// unit tests still do, each on its own private ":memory:" database — then
// each file holds an index covering only its own content, and a query against
// one of them returns only that file's hits. That degrades honestly (you get
// correct results for what is actually in the file you asked about) rather
// than silently returning half an answer from a file that claims to hold
// everything.
//
// ":memory:" DOES NOT WORK FOR UNIFIED SEARCH. Each sqlite3_open of
// ":memory:" creates a private database, so a note store and a block store
// both opened on ":memory:" are two different databases that cannot see each
// other's index rows. Unified tests therefore use a real file in a scratch
// directory. Said here explicitly because the failure mode — zero results,
// no error — is otherwise a confusing afternoon.

#ifndef UAPP_STORAGE_SQLITE_SQLITE_SEARCH_INDEX_H
#define UAPP_STORAGE_SQLITE_SQLITE_SEARCH_INDEX_H

#include <memory>
#include <string>

#include "uapp/search/search_index.h"
#include "uapp/status.h"

namespace uapp::storage::sqlite {

struct SqliteSearchIndexOptions final {
    /// Filesystem path to the database that the content stores write to.
    std::string path;

    /// Open the query connection read-only.
    ///
    /// The DEFAULT IS TRUE, and that default is the point: this port cannot
    /// write (core/search/search_index.h explains why index maintenance
    /// belongs to the content writer), so the connection behind it has no
    /// business holding write permission. A caller that passes `false` gets a
    /// read-write connection — useful only for creating the index tables in a
    /// database no store has opened yet, and the factory says so below.
    bool read_only = true;

    /// How long a reader waits for a competing writer before giving up.
    int busy_timeout_ms = 5000;
};

/// Open a query handle onto the unified index.
///
/// With `read_only = true` (the default) the index tables must already exist,
/// i.e. a store must have been opened read-write on this path at least once;
/// otherwise this returns Status::StoreOpenFailed rather than reporting
/// "no results" for every query forever.
///
/// On success `out` owns the connection; on failure `out` is untouched.
[[nodiscard]] Status open_sqlite_search_index(const SqliteSearchIndexOptions& options,
                                              std::unique_ptr<search::SearchIndex>& out);

/// Schema version of the unified index that this build writes and expects.
/// Exposed for diagnostics and for the tests; the DDL itself is internal.
[[nodiscard]] int sqlite_search_index_schema_version() noexcept;

}  // namespace uapp::storage::sqlite

#endif  // UAPP_STORAGE_SQLITE_SQLITE_SEARCH_INDEX_H
