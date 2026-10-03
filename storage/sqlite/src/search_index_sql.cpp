// SPDX-License-Identifier: Apache-2.0
//
// The unified search index's schema and maintenance primitives. See
// search_index_sql.h for the three design decisions behind the shape.

#include "search_index_sql.h"

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "uapp/object_id.h"
#include "uapp/search/search_result.h"
#include "uapp/status.h"
#include "sqlite_raii.h"

namespace uapp::storage::sqlite::search_index {
namespace {

// ---------------------------------------------------------------------------
// Schema
//
// `schema_meta` is repeated here (it is also in both stores' own DDL) so that
// this DDL is self-sufficient: apply_schema must work regardless of which
// adapter opened the file first.
//
// `search_fts` is a content-stored FTS5 table keyed by the same rowid as
// `search_entry`, mirroring the `note` / `note_fts` pairing already in this
// directory. The external-content pattern database-design.md section 3.9
// eventually wants would avoid the second copy of the text; it also requires
// hand-maintaining 'delete' shadow rows with the ORIGINAL text, which means
// every delete path has to read the old content back first and gets silently
// wrong if it does not. Deferred deliberately, not overlooked.
//
// Tokenizer and prefix settings are copied EXACTLY from `note_fts` so that
// moving a query from the per-store search to the unified one cannot change
// what matches. `remove_diacritics 2` is the Unicode-correct variant;
// `prefix = '2 3'` is what makes the trailing `*` in the MATCH expression
// built by fts_query.cpp cheap enough for search-as-you-type.
//
// NOT IMPLEMENTED: a CJK-capable tokenizer. `unicode61` does not segment
// Chinese/Japanese/Korean, so a CJK query degrades to whole-run matching.
// database-design.md section 3.9 names the fix (a custom ICU tokenizer
// registered at open) and tracks it as a Phase 9 task of its own.
// ---------------------------------------------------------------------------
constexpr char kSchemaSql[] =
    "CREATE TABLE IF NOT EXISTS schema_meta ("
    "  key   TEXT PRIMARY KEY,"
    "  value INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS search_entry ("
    "  source_kind  INTEGER NOT NULL CHECK(source_kind >= 0),"
    "  source_id    BLOB    NOT NULL CHECK(length(source_id) = 16),"
    "  container_id BLOB             CHECK(container_id IS NULL OR"
    "                                      length(container_id) = 16),"
    "  UNIQUE(source_kind, source_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS ix_search_entry_container "
    "  ON search_entry(container_id);"
    "CREATE VIRTUAL TABLE IF NOT EXISTS search_fts USING fts5("
    "  title,"
    "  body,"
    "  tokenize = 'unicode61 remove_diacritics 2',"
    "  prefix   = '2 3'"
    ");";

/// Map a SQLite result code to our vocabulary for a *write*. Deliberately
/// NOT shared with the two store adapters' same-named helpers: those map
/// SQLITE_CONSTRAINT to AlreadyExists, which is right for a content insert
/// and wrong here. A constraint violation on the index means the index and
/// the content disagree about what exists, which is our bug, not the
/// caller's — so it is Status::Internal and it rolls the content write back.
Status index_write_status_for(int rc) noexcept {
    switch (rc) {
        case SQLITE_OK:
        case SQLITE_DONE:
        case SQLITE_ROW:
            return Status::Ok;
        case SQLITE_READONLY:
            return Status::StoreReadOnly;
        case SQLITE_CONSTRAINT:
            return Status::Internal;
        default:
            return Status::StoreWriteFailed;
    }
}

Status bind_object_id(Statement& stmt, int index, const ObjectIdBytes& bytes) noexcept {
    return stmt.bind_blob(index, bytes.data(), kObjectIdBytes);
}

Status bind_kind(Statement& stmt, int index, search::SourceKind kind) noexcept {
    if (!search::source_kind_is_valid(kind)) { return Status::Internal; }
    return stmt.bind_int64(index, static_cast<std::int64_t>(kind));
}

/// Run a semicolon-separated batch of DDL statements. Same shape, and the
/// same reason for discarding the error text, as the two store adapters:
/// sqlite3_exec's message can contain statement fragments and spec section 34
/// forbids user content in logs.
Status exec_batch(sqlite3* db, const char* sql, Status failure) noexcept {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err != nullptr) {
        sqlite3_free(err);  // allocated by sqlite3_malloc; ours to free
    }
    return rc == SQLITE_OK ? Status::Ok : failure;
}

/// Look up the rowid of an existing entry. Status::NotFound if absent.
Status find_entry_rowid(sqlite3* db,
                        search::SourceKind kind,
                        const ObjectIdBytes& source_id,
                        std::int64_t& out) noexcept {
    Statement stmt;
    if (const Status s = stmt.prepare(
            db, "SELECT rowid FROM search_entry WHERE source_kind = ?1 AND source_id = ?2;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_kind(stmt, 1, kind);            !is_ok(s)) { return s; }
    if (const Status s = bind_object_id(stmt, 2, source_id);  !is_ok(s)) { return s; }

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW)  { out = stmt.column_int64(0); return Status::Ok; }
    if (rc == SQLITE_DONE) { return Status::NotFound; }
    return Status::StoreReadFailed;
}

/// Write the two text columns of an existing FTS row.
Status write_fts_text(sqlite3* db,
                      std::int64_t rowid,
                      std::string_view title,
                      std::string_view body,
                      bool insert) noexcept {
    Statement stmt;
    const char* sql = insert
                          ? "INSERT INTO search_fts(rowid, title, body) VALUES(?1, ?2, ?3);"
                          : "UPDATE search_fts SET title = ?2, body = ?3 WHERE rowid = ?1;";
    if (const Status s = stmt.prepare(db, sql);        !is_ok(s)) { return s; }
    if (const Status s = stmt.bind_int64(1, rowid);    !is_ok(s)) { return s; }
    if (const Status s = stmt.bind_text(2, title);     !is_ok(s)) { return s; }
    if (const Status s = stmt.bind_text(3, body);      !is_ok(s)) { return s; }

    const int rc = sqlite3_step(stmt.get());
    return rc == SQLITE_DONE ? Status::Ok : index_write_status_for(rc);
}

}  // namespace

Status apply_schema(sqlite3* db) noexcept {
    if (db == nullptr) { return Status::Internal; }

    if (const Status s = exec_batch(db, kSchemaSql, Status::StoreSchemaFailed); !is_ok(s)) {
        return s;
    }

    Statement stmt;
    if (const Status s = stmt.prepare(
            db,
            "INSERT INTO schema_meta(key, value) VALUES('search_schema_version', ?1) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
        !is_ok(s)) {
        return Status::StoreSchemaFailed;
    }
    if (const Status s = stmt.bind_int64(1, kSchemaVersion); !is_ok(s)) {
        return Status::StoreSchemaFailed;
    }
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return Status::StoreSchemaFailed;
    }
    return Status::Ok;
}

Status check_schema_version(sqlite3* db) noexcept {
    if (db == nullptr) { return Status::Internal; }

    Statement stmt;
    if (const Status s = stmt.prepare(
            db, "SELECT value FROM schema_meta WHERE key = 'search_schema_version';");
        !is_ok(s)) {
        // Table absent => a fresh or pre-search database. Not an error: the
        // index is derived data and will be created on the first write open.
        return Status::Ok;
    }
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) { return Status::Ok; }
    if (rc != SQLITE_ROW)  { return Status::StoreSchemaFailed; }

    return stmt.column_int64(0) > kSchemaVersion ? Status::StoreSchemaFailed : Status::Ok;
}

Status put(sqlite3* db,
           search::SourceKind kind,
           const ObjectIdBytes& source_id,
           const ObjectIdBytes* container,
           std::string_view title,
           std::string_view body) noexcept {
    if (db == nullptr) { return Status::Internal; }
    // The nil id is the "unset"/"top level" sentinel and is never a real
    // object, so indexing one would create an entry nothing can ever open.
    if (detail::object_id_is_nil(source_id)) { return Status::Internal; }
    if (container != nullptr && detail::object_id_is_nil(*container)) {
        return Status::Internal;
    }

    std::int64_t rowid = 0;
    const Status found = find_entry_rowid(db, kind, source_id, rowid);

    if (is_ok(found)) {
        // The container can legitimately change (not today — a cross-page
        // move is NOT IMPLEMENTED in the block store — but the index must not
        // be the thing that makes it impossible later), so rewrite it too.
        Statement stmt;
        if (const Status s = stmt.prepare(
                db, "UPDATE search_entry SET container_id = ?2 WHERE rowid = ?1;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = stmt.bind_int64(1, rowid); !is_ok(s)) { return s; }
        const Status bound = container != nullptr ? bind_object_id(stmt, 2, *container)
                                                  : stmt.bind_null(2);
        if (!is_ok(bound)) { return bound; }

        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) { return index_write_status_for(rc); }

        return write_fts_text(db, rowid, title, body, /*insert=*/false);
    }
    if (found != Status::NotFound) { return found; }

    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                db,
                "INSERT INTO search_entry(source_kind, source_id, container_id) "
                "VALUES(?1, ?2, ?3);");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_kind(stmt, 1, kind);           !is_ok(s)) { return s; }
        if (const Status s = bind_object_id(stmt, 2, source_id); !is_ok(s)) { return s; }
        const Status bound = container != nullptr ? bind_object_id(stmt, 3, *container)
                                                  : stmt.bind_null(3);
        if (!is_ok(bound)) { return bound; }

        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) { return index_write_status_for(rc); }
        rowid = sqlite3_last_insert_rowid(db);
    }

    return write_fts_text(db, rowid, title, body, /*insert=*/true);
}

Status erase(sqlite3* db,
             search::SourceKind kind,
             const ObjectIdBytes& source_id) noexcept {
    if (db == nullptr) { return Status::Internal; }
    if (detail::object_id_is_nil(source_id)) { return Status::Internal; }

    // FTS ROWS FIRST, AND THE ORDER IS LOAD-BEARING.
    //
    // The subquery that locates the FTS row reads `search_entry`, so deleting
    // the entry first leaves the FTS row with nothing pointing at it. The
    // immediate symptom is benign -- the query in sqlite_search_index.cpp
    // INNER JOINs search_entry, so an orphan is invisible to readers.
    //
    // The eventual symptom is not. SQLite hands a new row the smallest unused
    // rowid above the current maximum, and after deletions that is a rowid the
    // orphan may already occupy; the next `put` would then INSERT into
    // search_fts at a rowid that already holds the DELETED object's text. One
    // object's text, attached to a different object's entry. The regression
    // test for this is "deleting then re-adding content does not resurrect
    // the deleted text".
    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                db,
                "DELETE FROM search_fts WHERE rowid IN ("
                "  SELECT rowid FROM search_entry "
                "  WHERE source_kind = ?1 AND source_id = ?2);");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_kind(stmt, 1, kind);           !is_ok(s)) { return s; }
        if (const Status s = bind_object_id(stmt, 2, source_id); !is_ok(s)) { return s; }
        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) { return index_write_status_for(rc); }
    }
    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                db,
                "DELETE FROM search_entry WHERE source_kind = ?1 AND source_id = ?2;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_kind(stmt, 1, kind);           !is_ok(s)) { return s; }
        if (const Status s = bind_object_id(stmt, 2, source_id); !is_ok(s)) { return s; }
        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) { return index_write_status_for(rc); }
    }
    return Status::Ok;
}

Status erase_container(sqlite3* db,
                       search::SourceKind container_kind,
                       const ObjectIdBytes& container) noexcept {
    if (db == nullptr) { return Status::Internal; }
    if (detail::object_id_is_nil(container)) { return Status::Internal; }

    // One predicate, used twice, matching both "everything inside the
    // container" and "the container's own entry". FTS rows go first here too,
    // for the rowid-reuse reason spelled out in erase().
    constexpr char kSelect[] =
        "SELECT rowid FROM search_entry "
        "WHERE container_id = ?1 OR (source_kind = ?2 AND source_id = ?1)";

    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                db, std::string("DELETE FROM search_fts WHERE rowid IN (") + kSelect + ");");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_object_id(stmt, 1, container);   !is_ok(s)) { return s; }
        if (const Status s = bind_kind(stmt, 2, container_kind);   !is_ok(s)) { return s; }
        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) { return index_write_status_for(rc); }
    }
    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                db,
                "DELETE FROM search_entry "
                "WHERE container_id = ?1 OR (source_kind = ?2 AND source_id = ?1);");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_object_id(stmt, 1, container);   !is_ok(s)) { return s; }
        if (const Status s = bind_kind(stmt, 2, container_kind);   !is_ok(s)) { return s; }
        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) { return index_write_status_for(rc); }
    }
    return Status::Ok;
}

Status erase_block_subtree(sqlite3* db,
                           const ObjectIdBytes& root,
                           std::int64_t depth_cap) noexcept {
    if (db == nullptr) { return Status::Internal; }
    if (detail::object_id_is_nil(root)) { return Status::Internal; }
    if (depth_cap <= 0) { return Status::Internal; }

    // `UNION` (not UNION ALL) plus the depth bound is what keeps the walk
    // terminating on a corrupt or cyclic parent chain, exactly as in the
    // block store's own subtree delete. Without both, a hand-edited database
    // turns this into an infinite loop inside SQLite.
    constexpr char kCte[] =
        "WITH RECURSIVE sub(id, depth) AS ("
        "  SELECT id, 1 FROM block WHERE id = ?1"
        "  UNION"
        "  SELECT b.id, sub.depth + 1"
        "    FROM block b JOIN sub ON b.parent_id = sub.id"
        "   WHERE sub.depth < ?2"
        ") ";

    const auto run = [&](const std::string& sql) noexcept -> Status {
        Statement stmt;
        if (const Status s = stmt.prepare(db, sql); !is_ok(s)) { return s; }
        if (const Status s = bind_object_id(stmt, 1, root);     !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(2, depth_cap);     !is_ok(s)) { return s; }
        if (const Status s = bind_kind(stmt, 3, search::SourceKind::Block); !is_ok(s)) {
            return s;
        }
        const int rc = sqlite3_step(stmt.get());
        return rc == SQLITE_DONE ? Status::Ok : index_write_status_for(rc);
    };

    // FTS rows first, for the same rowid-reuse reason as erase().
    if (const Status s = run(std::string(kCte) +
                             "DELETE FROM search_fts WHERE rowid IN ("
                             "  SELECT rowid FROM search_entry "
                             "  WHERE source_kind = ?3 "
                             "    AND source_id IN (SELECT id FROM sub));");
        !is_ok(s)) {
        return s;
    }
    return run(std::string(kCte) +
               "DELETE FROM search_entry "
               "WHERE source_kind = ?3 AND source_id IN (SELECT id FROM sub);");
}

Status entry_count(sqlite3* db, std::size_t& out) noexcept {
    if (db == nullptr) { return Status::Internal; }

    Statement stmt;
    if (const Status s = stmt.prepare(db, "SELECT COUNT(*) FROM search_entry;");
        !is_ok(s)) {
        return s;
    }
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) { return Status::StoreReadFailed; }

    const std::int64_t n = stmt.column_int64(0);
    if (n < 0) { return Status::StoreReadFailed; }
    out = static_cast<std::size_t>(n);
    return Status::Ok;
}

}  // namespace uapp::storage::sqlite::search_index
