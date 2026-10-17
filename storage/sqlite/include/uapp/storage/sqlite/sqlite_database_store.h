// SPDX-License-Identifier: Apache-2.0
//
// storage/sqlite/sqlite_database_store.h — the SQLite adapter for
// DatabaseStore (spec section 15).
//
// Like sqlite_block_store.h, this header exposes ONLY a factory returning the
// core-owned interface: no <sqlite3.h>, no sqlite3 type in any signature.
// That is what keeps PHASE0_SUMMARY.md H-3 true in practice rather than by
// convention.
//
// A NOTE ON THE FILE IT OPENS: a database is anchored to a `blocks::Page`
// (docs/adr/0004-database-anchored-to-page.md), so `db_collection.page_id`
// carries a real foreign key into `page`. This adapter therefore creates the
// page/block schema too, from the ONE shared constant in
// storage/sqlite/src/document_sql.h -- not a second copy of it. Opening a
// database store and a block store on the same file is the intended
// arrangement and the order does not matter; opening one on ":memory:" gives
// a private database with an empty `page` table, which is why the tests that
// need a page create one through the block store on a REAL FILE.

#ifndef UAPP_STORAGE_SQLITE_SQLITE_DATABASE_STORE_H
#define UAPP_STORAGE_SQLITE_SQLITE_DATABASE_STORE_H

#include <memory>
#include <string>

#include "uapp/database/database_store.h"
#include "uapp/status.h"

namespace uapp::storage::sqlite {

struct SqliteDatabaseStoreOptions final {
    /// Filesystem path. ":memory:" gives a private in-memory database.
    std::string path;

    /// Open without write permission; every mutating call then fails with
    /// Status::StoreReadOnly instead of silently no-op'ing.
    bool read_only = false;

    /// How long a writer waits for a competing writer before giving up.
    int busy_timeout_ms = 5000;
};

/// Open (creating if necessary) the local database-system tables and return a
/// store. On success `out` owns the connection and the schema is current; on
/// failure `out` is untouched and the status says which stage failed.
[[nodiscard]] Status open_sqlite_database_store(
    const SqliteDatabaseStoreOptions& options,
    std::unique_ptr<database::DatabaseStore>& out);

/// Schema version this build writes and expects, recorded in `schema_meta`
/// under 'database_schema_version'. Bumping it without a migration is how
/// silent data loss happens, so it lives next to the code.
///
/// It is SEPARATE from kBlockSchemaVersion on purpose: the two namespaces
/// evolve independently, and a shared counter would force a migration of one
/// whenever the other changed.
inline constexpr int kDatabaseSchemaVersion = 1;

}  // namespace uapp::storage::sqlite

#endif  // UAPP_STORAGE_SQLITE_SQLITE_DATABASE_STORE_H
