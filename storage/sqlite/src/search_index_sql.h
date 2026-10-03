// SPDX-License-Identifier: Apache-2.0
//
// storage/sqlite/search_index_sql.h — the ONE unified full-text index, and
// the primitives both store adapters use to keep it in sync.
//
// Internal to this target. Does not leave storage/sqlite (PHASE0_SUMMARY H-3).
//
// ===========================================================================
// DESIGN DECISION 1: ONE SHARED FTS5 TABLE, NOT TWO TABLES MERGED IN THE APP
// ===========================================================================
// The alternative was: leave `note_fts` alone, add a `block_fts`, query both
// in parallel and merge the two result lists in C++. Rejected, for a reason
// that is not about convenience:
//
//   BM25 SCORES FROM TWO DIFFERENT FTS5 TABLES ARE NOT COMPARABLE.
//
// bm25() is computed from that index's own corpus statistics — the number of
// rows, the average row length, the per-term document frequency. A note
// corpus and a block corpus have wildly different average row lengths (a note
// body is up to 4 MiB; a paragraph block is a sentence), so the same term
// scores differently in each. Merging two such lists means inventing a fusion
// heuristic (score normalisation, or reciprocal-rank fusion) and then owning
// its calibration forever. With one table, the ranking across kinds is a
// single `ORDER BY bm25(...)` over one set of corpus statistics, which is
// *correct by construction* rather than tuned. Spec section 16 asks for one
// search across Notes and Pages; one ranking is part of what that means.
//
// Secondary, but real: one table means one tokenizer configuration. When the
// ICU/CJK tokenizer lands (database-design.md section 3.9 tracks it as a
// Phase 9 task), it gets configured once. Two tables is two places to forget.
//
// database-design.md section 3.9 already specifies this shape — a single
// `fts_content` with `object_kind` / `object_id` addressing columns. This is
// that table.
//
// ===========================================================================
// DESIGN DECISION 2: BLOCK-LEVEL GRANULARITY, NOT PAGE-LEVEL
// ===========================================================================
// A page could be indexed as ONE row holding all of its blocks' text
// concatenated. That is simpler. It is also wrong here, for three reasons:
//
//  1. WRITE AMPLIFICATION. A page may hold up to kMaxBlocksPerPage (50 000)
//     blocks. Editing one paragraph would mean re-reading the whole page,
//     re-concatenating it and rewriting one FTS row covering every block —
//     O(page) work on every keystroke-ish edit, inside the write transaction.
//     Block-level rows make the same edit O(1).
//  2. THE RESULT WOULD NOT BE ACTIONABLE. snippet() on a concatenated blob
//     tells you the match is "somewhere on page Y" with no way to map the
//     offset back to a block id. A user searching a 400-block page then has
//     to find the match by eye. Block-level rows let the hit carry both the
//     block id (where to scroll) and the page id (what to open) — which is
//     what `search::SearchResult` exposes.
//  3. DELETES. Deleting one block out of a page-level row means rebuilding
//     that row from the surviving blocks; deleting a block row is a delete.
//
// The cost, stated plainly: each row is matched INDEPENDENTLY, so a two-term
// query whose terms sit in two DIFFERENT blocks of the same page does not
// match that page. (It does match a note, whose title+body are one row.) A
// page-level roll-up row *in addition to* the block rows would fix it, at the
// write-amplification cost above; that is NOT IMPLEMENTED here and is
// recorded in docs/changelog.md as a known limitation rather than hidden.
//
// Page TITLES are indexed, as their own SourceKind::Page row — a page title
// is not inside any block, so without that row searching for a page by name
// would fail.
//
// ===========================================================================
// DESIGN DECISION 3: TWO TABLES (ADDRESSING + FTS), JOINED BY ROWID
// ===========================================================================
// database-design.md section 3.9 sketches the ids as UNINDEXED columns inside
// the FTS5 table. We split them out into an ordinary `search_entry` table
// joined to `search_fts` by rowid, exactly as `note` / `note_fts` already do.
// Why, concretely:
//
//   * An UNINDEXED FTS5 column cannot carry a CHECK constraint and cannot be
//     indexed. Index MAINTENANCE is all by id — "replace the row for block
//     X", "drop every row whose container is page Y" — and against UNINDEXED
//     columns each of those is a full table scan of the index. With
//     `search_entry` they are B-tree lookups (UNIQUE(source_kind, source_id)
//     and ix_search_entry_container).
//   * The 16-byte length CHECK on the id columns is the same "never assume an
//     input is valid, including one that used to be ours" guard (spec section
//     28) that `note` and `block` already carry.
//
// ===========================================================================
// WHY THESE ARE FREE FUNCTIONS TAKING A `sqlite3*`
// ===========================================================================
// Because the caller MUST be inside its own content-write transaction when it
// calls them (database-design.md principle 7: "Index and content commit
// together. A separate indexer thread that 'catches up' is how search
// silently rots"). These functions deliberately do NOT open a transaction of
// their own: they inherit the caller's, so an index write that fails rolls
// the content write back with it. Handing them a connection rather than a
// store object is what makes that relationship obvious at every call site.

#ifndef UAPP_STORAGE_SQLITE_SEARCH_INDEX_SQL_H
#define UAPP_STORAGE_SQLITE_SEARCH_INDEX_SQL_H

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "uapp/object_id.h"
#include "uapp/search/search_result.h"
#include "uapp/status.h"

namespace uapp::storage::sqlite::search_index {

/// Schema version this build writes and expects, recorded in `schema_meta`
/// under 'search_schema_version'. Bumping it without a migration is how
/// silent data loss happens, so it lives next to the code.
inline constexpr int kSchemaVersion = 1;

/// Create the unified index tables if absent, and record the version.
///
/// Called by EVERY store adapter that writes content, on open. That is not an
/// accident: when a note store and a block store are opened on the SAME
/// database file (which is what makes search unified — see
/// sqlite_search_index.h), whichever opens first creates the tables and the
/// other one's `IF NOT EXISTS` is a no-op. The DDL is a single shared string
/// precisely so the two can never disagree about the shape.
[[nodiscard]] Status apply_schema(sqlite3* db) noexcept;

/// Refuse a database whose index was written by a newer build.
[[nodiscard]] Status check_schema_version(sqlite3* db) noexcept;

/// Insert or replace the index entry for one object.
///
/// `container` may be nullptr, meaning "this object is its own container"
/// (Note, Page). For a Block it is the page id.
///
/// `title` and `body` are the two indexed columns. The split is not
/// decoration: a title match should outrank a body match, which bm25's
/// per-column weighting gives for free once they are separate columns.
///
/// MUST be called inside the caller's content-write transaction.
[[nodiscard]] Status put(sqlite3* db,
                         search::SourceKind kind,
                         const ObjectIdBytes& source_id,
                         const ObjectIdBytes* container,
                         std::string_view title,
                         std::string_view body) noexcept;

/// Drop the index entry for one object. Succeeds when there was none —
/// "after this call nothing matches X" is the contract, and a store that
/// deletes a never-indexed object (a divider block, say) is not an error.
///
/// MUST be called inside the caller's content-write transaction.
[[nodiscard]] Status erase(sqlite3* db,
                           search::SourceKind kind,
                           const ObjectIdBytes& source_id) noexcept;

/// Drop every entry whose `container_id` is `container`, plus the entry for
/// the container itself. This is the page-delete path: one statement instead
/// of one call per block, because a page can hold 50 000 of them.
///
/// MUST be called inside the caller's content-write transaction.
[[nodiscard]] Status erase_container(sqlite3* db,
                                     search::SourceKind container_kind,
                                     const ObjectIdBytes& container) noexcept;

/// Drop the Block entries for every id produced by walking `block.parent_id`
/// downwards from `root` — i.e. the block and its whole subtree.
///
/// This one statement pair knows about the `block` table, so it is the one
/// primitive here that is not source-kind agnostic. It lives here anyway
/// rather than in sqlite_block_store.cpp because it is the only place outside
/// this file that would need the index table names, and duplicating those is
/// how the two drift. `depth_cap` bounds the recursion so a corrupt or cyclic
/// parent chain is an error rather than a hang (same guard as the block
/// store's own subtree delete).
///
/// MUST be called inside the caller's content-write transaction, BEFORE the
/// blocks themselves are deleted — the walk reads `block`.
[[nodiscard]] Status erase_block_subtree(sqlite3* db,
                                         const ObjectIdBytes& root,
                                         std::int64_t depth_cap) noexcept;

/// Total entries, across every source kind. Diagnostics and tests.
[[nodiscard]] Status entry_count(sqlite3* db, std::size_t& out) noexcept;

}  // namespace uapp::storage::sqlite::search_index

#endif  // UAPP_STORAGE_SQLITE_SEARCH_INDEX_SQL_H
