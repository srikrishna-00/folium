// SPDX-License-Identifier: Apache-2.0
//
// The only translation unit in the project that includes <sqlite3.h> outside
// of this directory's own internal headers. See PHASE0_SUMMARY.md H-3.

#include "uapp/storage/sqlite/sqlite_note_store.h"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "uapp/notes/note.h"
#include "uapp/storage/sqlite/fts_query.h"
#include "sqlite_raii.h"

namespace uapp::storage::sqlite {
namespace {

using notes::Note;
using notes::NoteId;
using notes::SearchHit;

// ---------------------------------------------------------------------------
// Schema
//
// `note` mirrors the shape that database-design.md section 3.3 gives
// `document` (16-byte BLOB primary key, TEXT title) minus everything this
// slice does not have yet: workspaces, block trees, sync columns, soft
// deletes. `note_fts` mirrors section 3.9's `fts_content`, including the
// tokenizer and prefix settings, so that search behaviour does not change
// underneath us when the real table arrives.
//
// note_fts is a content-stored FTS5 table keyed by the same rowid as `note`.
// That costs a second copy of the text; the external-content pattern the
// design doc eventually wants is a Phase 9 optimisation, and doing it now
// would mean hand-maintaining delete-shadow rows for no benefit at this size.
// ---------------------------------------------------------------------------
constexpr char kSchemaSql[] =
    "CREATE TABLE IF NOT EXISTS schema_meta ("
    "  key   TEXT PRIMARY KEY,"
    "  value INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS note ("
    "  id             BLOB    NOT NULL UNIQUE CHECK(length(id) = 16),"
    "  title          TEXT    NOT NULL,"
    "  body           TEXT    NOT NULL,"
    "  created_at_ms  INTEGER NOT NULL CHECK(created_at_ms  >= 0),"
    "  modified_at_ms INTEGER NOT NULL CHECK(modified_at_ms >= 0)"
    ");"
    "CREATE INDEX IF NOT EXISTS ix_note_modified ON note(modified_at_ms DESC);"
    "CREATE VIRTUAL TABLE IF NOT EXISTS note_fts USING fts5("
    "  title,"
    "  body,"
    "  tokenize = 'unicode61 remove_diacritics 2',"
    "  prefix   = '2 3'"
    ");";

/// Map a SQLite result code to our status vocabulary for a *write*.
Status write_status_for(int rc) noexcept {
    switch (rc) {
        case SQLITE_OK:
        case SQLITE_DONE:
        case SQLITE_ROW:
            return Status::Ok;
        case SQLITE_READONLY:
            return Status::StoreReadOnly;
        case SQLITE_CONSTRAINT:
            return Status::AlreadyExists;
        default:
            return Status::StoreWriteFailed;
    }
}

// ---------------------------------------------------------------------------

class SqliteNoteStore final : public notes::NoteStore {
public:
    SqliteNoteStore(Connection conn, bool read_only) noexcept
        : conn_(std::move(conn)), read_only_(read_only) {}

    [[nodiscard]] Status insert(const Note& note) noexcept override;
    [[nodiscard]] Status update(const Note& note) noexcept override;
    [[nodiscard]] Status get(const NoteId& id, Note& out) const noexcept override;
    [[nodiscard]] Status remove(const NoteId& id) noexcept override;
    [[nodiscard]] Status search(std::string_view query,
                                std::size_t limit,
                                std::vector<SearchHit>& out) const noexcept override;
    [[nodiscard]] Status count(std::size_t& out) const noexcept override;

private:
    /// Look up the integer rowid backing a note id. Status::NotFound if absent.
    [[nodiscard]] Status find_rowid(std::int64_t& out, const NoteId& id) const noexcept;

    Connection conn_;
    bool       read_only_ = false;
};

Status SqliteNoteStore::find_rowid(std::int64_t& out, const NoteId& id) const noexcept {
    Statement stmt;
    if (const Status s = stmt.prepare(conn_.get(), "SELECT rowid FROM note WHERE id = ?1;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = stmt.bind_blob(1, id.bytes().data(), notes::kNoteIdBytes);
        !is_ok(s)) {
        return s;
    }

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        out = stmt.column_int64(0);
        return Status::Ok;
    }
    if (rc == SQLITE_DONE) {
        return Status::NotFound;
    }
    return Status::StoreReadFailed;
}

Status SqliteNoteStore::insert(const Note& note) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    // Re-validate at the adapter boundary. The service validates too, but this
    // class is reachable through the NoteStore interface by anyone, and the
    // schema CHECK constraints are our last line rather than our only one.
    if (const Status s = notes::validate_note(note); !is_ok(s)) { return s; }

    Transaction txn;
    if (const Status s = txn.begin(conn_.get()); !is_ok(s)) { return s; }

    std::int64_t rowid = 0;
    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "INSERT INTO note(id, title, body, created_at_ms, modified_at_ms) "
                "VALUES(?1, ?2, ?3, ?4, ?5);");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = stmt.bind_blob(1, note.id.bytes().data(), notes::kNoteIdBytes);
            !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_text(2, note.title);  !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_text(3, note.body);   !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(4, note.created_at_ms);  !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(5, note.modified_at_ms); !is_ok(s)) { return s; }

        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) {
            return write_status_for(rc);
        }
        rowid = sqlite3_last_insert_rowid(conn_.get());
    }

    // Index in the SAME transaction as the content. If this fails, the
    // Transaction destructor rolls the content insert back too, so the store
    // can never hold a note that search cannot find.
    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "INSERT INTO note_fts(rowid, title, body) VALUES(?1, ?2, ?3);");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = stmt.bind_int64(1, rowid);      !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_text(2, note.title);  !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_text(3, note.body);   !is_ok(s)) { return s; }

        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) {
            return write_status_for(rc);
        }
    }

    return txn.commit();
}

Status SqliteNoteStore::update(const Note& note) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    if (const Status s = notes::validate_note(note); !is_ok(s)) { return s; }

    Transaction txn;
    if (const Status s = txn.begin(conn_.get()); !is_ok(s)) { return s; }

    std::int64_t rowid = 0;
    if (const Status s = find_rowid(rowid, note.id); !is_ok(s)) {
        return s;  // NotFound propagates: update never creates.
    }

    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "UPDATE note SET title = ?2, body = ?3, modified_at_ms = ?4 "
                "WHERE rowid = ?1;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = stmt.bind_int64(1, rowid);      !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_text(2, note.title);  !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_text(3, note.body);   !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(4, note.modified_at_ms); !is_ok(s)) { return s; }

        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) { return write_status_for(rc); }
    }

    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "UPDATE note_fts SET title = ?2, body = ?3 WHERE rowid = ?1;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = stmt.bind_int64(1, rowid);      !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_text(2, note.title);  !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_text(3, note.body);   !is_ok(s)) { return s; }

        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) { return write_status_for(rc); }
    }

    return txn.commit();
}

Status SqliteNoteStore::get(const NoteId& id, Note& out) const noexcept {
    if (id.is_nil()) { return Status::NoteIdMalformed; }

    Statement stmt;
    if (const Status s = stmt.prepare(
            conn_.get(),
            "SELECT id, title, body, created_at_ms, modified_at_ms "
            "FROM note WHERE id = ?1;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = stmt.bind_blob(1, id.bytes().data(), notes::kNoteIdBytes);
        !is_ok(s)) {
        return s;
    }

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) { return Status::NotFound; }
    if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

    // Build into a local first so a mid-read failure cannot leave the caller
    // with a half-populated Note that looks valid.
    Note loaded;
    std::array<std::uint8_t, notes::kNoteIdBytes> raw{};
    if (const Status s = stmt.column_fixed_blob(0, raw.data(), raw.size()); !is_ok(s)) {
        return s;
    }
    loaded.id             = NoteId(raw);
    loaded.title          = stmt.column_text(1);
    loaded.body           = stmt.column_text(2);
    loaded.created_at_ms  = stmt.column_int64(3);
    loaded.modified_at_ms = stmt.column_int64(4);

    // The row came out of our own database, but the file on disk is
    // attacker-reachable (a synced or restored .db). Spec section 28: never
    // assume an input is valid, including one that used to be ours.
    if (const Status s = notes::validate_note(loaded); !is_ok(s)) {
        return Status::StoreReadFailed;
    }

    out = std::move(loaded);
    return Status::Ok;
}

Status SqliteNoteStore::remove(const NoteId& id) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    if (id.is_nil()) { return Status::NoteIdMalformed; }

    Transaction txn;
    if (const Status s = txn.begin(conn_.get()); !is_ok(s)) { return s; }

    std::int64_t rowid = 0;
    if (const Status s = find_rowid(rowid, id); !is_ok(s)) { return s; }

    {
        Statement stmt;
        if (const Status s = stmt.prepare(conn_.get(), "DELETE FROM note WHERE rowid = ?1;");
            !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(1, rowid); !is_ok(s)) { return s; }
        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) { return write_status_for(rc); }
    }
    {
        Statement stmt;
        if (const Status s = stmt.prepare(conn_.get(),
                                          "DELETE FROM note_fts WHERE rowid = ?1;");
            !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(1, rowid); !is_ok(s)) { return s; }
        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) { return write_status_for(rc); }
    }

    return txn.commit();
}

Status SqliteNoteStore::search(std::string_view query,
                               std::size_t limit,
                               std::vector<SearchHit>& out) const noexcept {
    out.clear();

    if (limit == 0) { return Status::InvalidArgument; }
    const std::size_t effective_limit = std::min(limit, notes::kMaxSearchLimit);

    // Untrusted input is converted to a safe MATCH expression here, before it
    // reaches SQLite. See fts_query.h for why binding it raw is not enough.
    std::string match_expr;
    if (const Status s = build_fts5_match_expression(query, match_expr); !is_ok(s)) {
        return s;
    }

    Statement stmt;
    if (const Status s = stmt.prepare(
            conn_.get(),
            "SELECT n.id, n.title, "
            "       snippet(note_fts, 1, '[', ']', ' ... ', 12), "
            "       bm25(note_fts) "
            "FROM note_fts "
            "JOIN note n ON n.rowid = note_fts.rowid "
            "WHERE note_fts MATCH ?1 "
            "ORDER BY bm25(note_fts) "
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

    std::vector<SearchHit> hits;
    for (;;) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) { break; }
        if (rc != SQLITE_ROW) {
            // Includes the case where a malformed MATCH expression slipped
            // past the sanitiser. Report it rather than returning "no results".
            return Status::SearchFailed;
        }

        SearchHit hit;
        std::array<std::uint8_t, notes::kNoteIdBytes> raw{};
        if (const Status s = stmt.column_fixed_blob(0, raw.data(), raw.size()); !is_ok(s)) {
            return s;
        }
        hit.id      = NoteId(raw);
        hit.title   = stmt.column_text(1);
        hit.snippet = stmt.column_text(2);
        hit.rank    = stmt.column_double(3);
        hits.push_back(std::move(hit));

        if (hits.size() >= effective_limit) { break; }
    }

    out = std::move(hits);
    return Status::Ok;
}

Status SqliteNoteStore::count(std::size_t& out) const noexcept {
    Statement stmt;
    if (const Status s = stmt.prepare(conn_.get(), "SELECT COUNT(*) FROM note;");
        !is_ok(s)) {
        return s;
    }
    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_ROW) { return Status::StoreReadFailed; }

    const std::int64_t n = stmt.column_int64(0);
    if (n < 0) { return Status::StoreReadFailed; }
    out = static_cast<std::size_t>(n);
    return Status::Ok;
}

/// Run a semicolon-separated batch of DDL/PRAGMA statements.
Status exec_batch(sqlite3* db, const char* sql, Status failure) noexcept {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err != nullptr) {
        // sqlite3_exec allocates this with sqlite3_malloc; it is ours to free.
        // We deliberately do NOT propagate the text: it can contain fragments
        // of the statement, and spec section 34 forbids user content in logs.
        sqlite3_free(err);
    }
    return rc == SQLITE_OK ? Status::Ok : failure;
}

Status apply_schema(sqlite3* db) noexcept {
    if (const Status s = exec_batch(db, kSchemaSql, Status::StoreSchemaFailed); !is_ok(s)) {
        return s;
    }

    Statement stmt;
    if (const Status s = stmt.prepare(
            db,
            "INSERT INTO schema_meta(key, value) VALUES('note_schema_version', ?1) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
        !is_ok(s)) {
        return Status::StoreSchemaFailed;
    }
    if (const Status s = stmt.bind_int64(1, kNoteSchemaVersion); !is_ok(s)) {
        return Status::StoreSchemaFailed;
    }
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return Status::StoreSchemaFailed;
    }
    return Status::Ok;
}

/// Reject a database written by a newer build. Opening it read-write would
/// silently drop whatever columns we do not know about.
Status check_schema_version(sqlite3* db) noexcept {
    Statement stmt;
    if (const Status s = stmt.prepare(
            db, "SELECT value FROM schema_meta WHERE key = 'note_schema_version';");
        !is_ok(s)) {
        // Table absent => a fresh or pre-schema database. Not an error here.
        return Status::Ok;
    }
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) { return Status::Ok; }
    if (rc != SQLITE_ROW)  { return Status::StoreSchemaFailed; }

    const std::int64_t found = stmt.column_int64(0);
    if (found > kNoteSchemaVersion) {
        return Status::StoreSchemaFailed;
    }
    return Status::Ok;
}

}  // namespace

Status open_sqlite_note_store(const SqliteNoteStoreOptions& options,
                              std::unique_ptr<notes::NoteStore>& out) {
    if (options.path.empty()) {
        return Status::InvalidArgument;
    }
    if (options.path.find('\0') != std::string::npos) {
        // sqlite3_open_v2 takes a NUL-terminated C string; an embedded NUL
        // would silently open a *different*, shorter path than requested.
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
    // sqlite3_open_v2 can hand back a handle even on failure; wrapping it
    // unconditionally is what guarantees it gets closed.
    Connection conn(raw_db);
    if (rc != SQLITE_OK || !conn) {
        return Status::StoreOpenFailed;
    }

    if (sqlite3_busy_timeout(conn.get(), options.busy_timeout_ms) != SQLITE_OK) {
        return Status::StoreOpenFailed;
    }

    if (options.read_only) {
        if (const Status s = check_schema_version(conn.get()); !is_ok(s)) { return s; }
    } else {
        // WAL: many readers, exactly one writer (architecture.md section 6).
        // foreign_keys is off by default in SQLite and must be re-enabled per
        // connection -- easy to forget, so it is done here, once.
        if (const Status s = exec_batch(conn.get(),
                                        "PRAGMA journal_mode = WAL;"
                                        "PRAGMA foreign_keys = ON;"
                                        "PRAGMA synchronous  = NORMAL;",
                                        Status::StoreOpenFailed);
            !is_ok(s)) {
            return s;
        }
        if (const Status s = check_schema_version(conn.get()); !is_ok(s)) { return s; }
        if (const Status s = apply_schema(conn.get());         !is_ok(s)) { return s; }
    }

    auto* store = new (std::nothrow) SqliteNoteStore(std::move(conn), options.read_only);
    if (store == nullptr) {
        return Status::StoreOpenFailed;  // allocation failure is checked, not assumed away
    }
    out.reset(store);
    return Status::Ok;
}

}  // namespace uapp::storage::sqlite
