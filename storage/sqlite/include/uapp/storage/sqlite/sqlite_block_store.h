// SPDX-License-Identifier: Apache-2.0
//
// storage/sqlite/sqlite_block_store.h — the SQLite adapter for BlockStore.
//
// Like sqlite_note_store.h, this header exposes ONLY a factory returning the
// core-owned interface: no <sqlite3.h>, no sqlite3 type in any signature.
// That is what keeps PHASE0_SUMMARY.md H-3 true in practice rather than by
// convention.

#ifndef UAPP_STORAGE_SQLITE_SQLITE_BLOCK_STORE_H
#define UAPP_STORAGE_SQLITE_SQLITE_BLOCK_STORE_H

#include <memory>
#include <string>

#include "uapp/blocks/block_store.h"
#include "uapp/status.h"

namespace uapp::storage::sqlite {

struct SqliteBlockStoreOptions final {
    /// Filesystem path. ":memory:" gives a private in-memory database, which
    /// is what most unit tests use.
    std::string path;

    /// Open without write permission; every mutating call then fails with
    /// Status::StoreReadOnly instead of silently no-op'ing.
    bool read_only = false;

    /// How long a writer waits for a competing writer before giving up.
    int busy_timeout_ms = 5000;
};

/// Open (creating if necessary) the local document database and return a
/// store. On success `out` owns the connection and the schema is current; on
/// failure `out` is untouched and the status says which stage failed.
[[nodiscard]] Status open_sqlite_block_store(const SqliteBlockStoreOptions& options,
                                             std::unique_ptr<blocks::BlockStore>& out);

/// Schema version this build writes and expects. Bumping it without a
/// migration is how silent data loss happens, so it lives next to the code.
inline constexpr int kBlockSchemaVersion = 1;

}  // namespace uapp::storage::sqlite

#endif  // UAPP_STORAGE_SQLITE_SQLITE_BLOCK_STORE_H
