// SPDX-License-Identifier: Apache-2.0
//
// The read side of the unified search index. The write side lives in the two
// store adapters, inside their content transactions — see search_index_sql.h.

#include "uapp/storage/sqlite/sqlite_search_index.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "uapp/notes/note.h"
#include "uapp/object_id.h"
#include "uapp/search/search_index.h"
#include "uapp/search/search_result.h"
#include "uapp/storage/sqlite/fts_query.h"
#include "search_index_sql.h"
#include "sqlite_raii.h"

namespace uapp::storage::sqlite {
namespace {

using search::SearchResult;
using search::SourceKind;

// The query-length cap is declared in two places: core/search (so the port's
// contract is readable without the adapter) and core/notes (where the MATCH
// builder reads it from, because that builder predates core/search and is
// still used by NoteStore::search). They must agree, or a query of between
// the two lengths would be accepted by one path and rejected by the other.
// Assert it rather than hope.
static_assert(search::kMaxQueryBytes == notes::kMaxQueryBytes,
              "search::kMaxQueryBytes and notes::kMaxQueryBytes have drifted; "
              "fts_query.cpp validates against the notes one");

class SqliteSearchIndex final : public search::SearchIndex {
public:
    explicit SqliteSearchIndex(Connection conn) noexcept : conn_(std::move(conn)) {}

    [[nodiscard]] Status query(std::string_view query_text,
                               std::size_t limit,
                               std::vector<SearchResult>& out) const noexcept override;
    [[nodiscard]] Status entry_count(std::size_t& out) const noexcept override;

private:
    Connection conn_;
};

Status SqliteSearchIndex::query(std::string_view query_text,
                                std::size_t limit,
                                std::vector<SearchResult>& out) const noexcept {
    // Cleared FIRST, before any early return, so a caller reusing a vector
    // across calls can never read stale hits out of a failed one.
    out.clear();

    if (limit == 0) { return Status::InvalidArgument; }
    const std::size_t effective_limit = std::min(limit, search::kMaxResults);

    // The ONE place untrusted text enters. `build_fts5_match_expression` is
    // the allow-list builder already used by NoteStore::search: it keeps only
    // word bytes, emits each surviving term as a double-quoted FTS5 string,
    // and ANDs them. Reused rather than reimplemented precisely so the
    // unified layer cannot reopen the injection surface that fts_query.cpp
    // already closed — see tests/search/unified_search_test.cpp.
    std::string match_expr;
    if (const Status s = build_fts5_match_expression(query_text, match_expr); !is_ok(s)) {
        return s;
    }

    Statement stmt;
    // snippet()'s column argument is -1: "pick the leftmost column that
    // actually matched". That is what makes one statement serve all three
    // source kinds -- a Page entry has text only in `title`, a Block entry
    // only in `body`, and a Note in either -- instead of needing a per-kind
    // query or returning an empty snippet for two kinds out of three.
    //
    // `e.container_id IS NOT NULL` decides whether the title column is the
    // matched object's own title or its container's; the join below supplies
    // the container's title for a Block, which is what makes a block hit
    // presentable in a result list without a second query per row.
    if (const Status s = stmt.prepare(
            conn_.get(),
            "SELECT e.source_kind, e.source_id, e.container_id, "
            "       CASE WHEN e.container_id IS NULL THEN search_fts.title "
            "            ELSE COALESCE((SELECT f.title FROM search_fts f "
            "                            WHERE f.rowid = (SELECT ce.rowid "
            "                                               FROM search_entry ce "
            "                                              WHERE ce.source_id = "
            "                                                      e.container_id "
            "                                                AND ce.container_id IS NULL)"
            "                          ), '') END, "
            "       snippet(search_fts, -1, '[', ']', ' ... ', 12), "
            "       bm25(search_fts) "
            "FROM search_fts "
            "JOIN search_entry e ON e.rowid = search_fts.rowid "
            "WHERE search_fts MATCH ?1 "
            "ORDER BY bm25(search_fts) "
            "LIMIT ?2;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = stmt.bind_text(1, match_expr); !is_ok(s)) { return s; }
    if (effective_limit > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        return Status::InvalidArgument;
    }
    if (const Status s = stmt.bind_int64(2, static_cast<std::int64_t>(effective_limit));
        !is_ok(s)) {
        return s;
    }

    std::vector<SearchResult> hits;
    for (;;) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) { break; }
        if (rc != SQLITE_ROW) {
            // Includes the case where a malformed MATCH expression somehow
            // slipped past the sanitiser. Reported, not swallowed: returning
            // "no results" for a broken query is how a search bug survives
            // for a year.
            return Status::SearchFailed;
        }

        SearchResult hit;

        const std::int64_t raw_kind = stmt.column_int64(0);
        if (raw_kind < 0 || raw_kind >= search::kSourceKindCount) {
            // The row came out of a file on disk, which is attacker-reachable
            // (a synced or restored .db). Spec section 28: never assume an
            // input is valid, including one that used to be ours.
            return Status::StoreReadFailed;
        }
        hit.kind = static_cast<SourceKind>(static_cast<std::int32_t>(raw_kind));

        if (const Status s = stmt.column_fixed_blob(1, hit.source_id.data(),
                                                    hit.source_id.size());
            !is_ok(s)) {
            return s;
        }
        if (!stmt.column_is_null(2)) {
            if (const Status s = stmt.column_fixed_blob(2, hit.container_id.data(),
                                                        hit.container_id.size());
                !is_ok(s)) {
                return s;
            }
        }

        hit.title   = stmt.column_text(3);
        hit.snippet = stmt.column_text(4);
        hit.rank    = stmt.column_double(5);
        hits.push_back(std::move(hit));

        if (hits.size() >= effective_limit) { break; }
    }

    out = std::move(hits);
    return Status::Ok;
}

Status SqliteSearchIndex::entry_count(std::size_t& out) const noexcept {
    return search_index::entry_count(conn_.get(), out);
}

}  // namespace

int sqlite_search_index_schema_version() noexcept { return search_index::kSchemaVersion; }

Status open_sqlite_search_index(const SqliteSearchIndexOptions& options,
                                std::unique_ptr<search::SearchIndex>& out) {
    if (options.path.empty()) {
        return Status::InvalidArgument;
    }
    if (options.path.find('\0') != std::string::npos) {
        // sqlite3_open_v2 takes a NUL-terminated string; an embedded NUL
        // would silently open a different, shorter path than requested.
        return Status::InvalidArgument;
    }
    if (options.busy_timeout_ms < 0) {
        return Status::InvalidArgument;
    }

    const int flags = options.read_only
                          ? SQLITE_OPEN_READONLY
                          : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);

    sqlite3* raw_db = nullptr;
    const int rc = sqlite3_open_v2(options.path.c_str(), &raw_db, flags, nullptr);
    // open_v2 can hand back a handle even on failure; wrapping it
    // unconditionally is what guarantees it gets closed.
    Connection conn(raw_db);
    if (rc != SQLITE_OK || !conn) {
        return Status::StoreOpenFailed;
    }

    if (sqlite3_busy_timeout(conn.get(), options.busy_timeout_ms) != SQLITE_OK) {
        return Status::StoreOpenFailed;
    }

    if (const Status s = search_index::check_schema_version(conn.get()); !is_ok(s)) {
        return s;
    }

    if (options.read_only) {
        // Prove the index exists NOW rather than answering every query with
        // "no results" because the caller pointed us at a path no store has
        // ever written. A missing table is a wiring mistake, not an empty
        // index, and the two must not look alike.
        Statement probe;
        if (const Status s = probe.prepare(conn.get(), "SELECT COUNT(*) FROM search_entry;");
            !is_ok(s)) {
            return Status::StoreOpenFailed;
        }
        if (sqlite3_step(probe.get()) != SQLITE_ROW) {
            return Status::StoreOpenFailed;
        }
    } else {
        if (const Status s = search_index::apply_schema(conn.get()); !is_ok(s)) {
            return s;
        }
    }

    auto* index = new (std::nothrow) SqliteSearchIndex(std::move(conn));
    if (index == nullptr) {
        return Status::StoreOpenFailed;  // allocation failure is checked, not assumed away
    }
    out.reset(index);
    return Status::Ok;
}

}  // namespace uapp::storage::sqlite
