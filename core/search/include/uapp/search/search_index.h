// SPDX-License-Identifier: Apache-2.0
//
// core/search/search_index.h — the unified search PORT.
//
// Same seam, same rules as core/notes/note_store.h and
// core/blocks/block_store.h: PHASE0_SUMMARY.md H-3 ("nothing outside
// storage/sqlite includes sqlite3.h") and spec section 32 ("do not tightly
// couple business logic to one database engine"). core/search links this
// abstract interface only; storage/sqlite implements it over FTS5.
//
// WHAT THIS IS, AND WHAT IT IS NOT
// ---------------------------------------------------------------------------
// `notes::NoteStore::search` already exists and still exists: it is PER-STORE
// search over one domain, returning `notes::SearchHit`. This port is the
// UNIFIED layer above it — one query string, results from every indexed
// source kind, each one self-describing about which kind it is and which ids
// open it.
//
// This interface is READ-ONLY on purpose. There is no `index()` / `reindex()`
// method, because an index that a caller has to remember to update is an
// index that silently rots (database-design.md principle 7: "Index and
// content commit together"). Index maintenance is therefore the *content
// writer's* job, inside the same transaction as the content write — see
// storage/sqlite/src/search_index_sql.h. The only way to put something into
// the index is to store a note, page or block.
//
// Contract every implementation MUST honour:
//  * `query` NEVER passes raw user text into the index's query language. See
//    storage/sqlite/include/uapp/storage/sqlite/fts_query.h.
//  * An empty result set is Status::Ok with an empty vector, NOT NotFound.
//  * A query with no usable terms is Status::SearchQueryEmpty — a distinct
//    condition from "no results", because the UI response differs.
//  * `out` is cleared before anything else, including on failure paths, so a
//    caller reusing a vector can never see stale results from a failed call.
//  * No method throws (ADR-0001: -fno-exceptions).

#ifndef UAPP_CORE_SEARCH_SEARCH_INDEX_H
#define UAPP_CORE_SEARCH_SEARCH_INDEX_H

#include <cstddef>
#include <string_view>
#include <vector>

#include "uapp/search/search_result.h"
#include "uapp/status.h"

namespace uapp::search {

class SearchIndex {
public:
    virtual ~SearchIndex();
    SearchIndex(const SearchIndex&) = delete;
    SearchIndex& operator=(const SearchIndex&) = delete;

    /// Run one query across every indexed source kind.
    ///
    /// `query` is RAW USER INPUT. The implementation is responsible for
    /// sanitising it; callers must never have to, and must never be asked to
    /// pre-escape anything.
    ///
    /// `limit` is clamped to kMaxResults; 0 is Status::InvalidArgument rather
    /// than a silently empty answer. Results are ordered best-first.
    ///
    /// Returns Status::SearchQueryEmpty when the query contains no indexable
    /// term (empty, whitespace only, punctuation only). Callers must treat
    /// that as "ask for a better query", not as "nothing matched".
    [[nodiscard]] virtual Status query(std::string_view query_text,
                                       std::size_t limit,
                                       std::vector<SearchResult>& out) const noexcept = 0;

    /// Total number of indexed entries, across every source kind. For
    /// diagnostics and for tests that assert the index did not go stale.
    [[nodiscard]] virtual Status entry_count(std::size_t& out) const noexcept = 0;

protected:
    SearchIndex() noexcept;
};

}  // namespace uapp::search

#endif  // UAPP_CORE_SEARCH_SEARCH_INDEX_H
