// SPDX-License-Identifier: Apache-2.0
//
// storage/sqlite/sqlite_note_store.h — the SQLite adapter for NoteStore.
//
// This header exposes ONLY a factory returning the core-owned interface. It
// does not include <sqlite3.h> and no sqlite3 type appears in its signature,
// which is what keeps PHASE0_SUMMARY.md H-3 true in practice: application code
// can construct the adapter without ever seeing the database engine.

#ifndef UAPP_STORAGE_SQLITE_SQLITE_NOTE_STORE_H
#define UAPP_STORAGE_SQLITE_SQLITE_NOTE_STORE_H

#include <memory>
#include <string>

#include "uapp/notes/note_store.h"
#include "uapp/status.h"

namespace uapp::storage::sqlite {

struct SqliteNoteStoreOptions final {
    /// Filesystem path to the database. The special value ":memory:" gives a
    /// private in-memory database, which is what the unit tests use.
    std::string path;

    /// Open without write permission. Every mutating call then fails with
    /// Status::StoreReadOnly instead of corrupting or silently no-op'ing.
    /// Used by the tests to exercise the write-failure path deterministically.
    bool read_only = false;

    /// How long a writer waits for a competing writer before giving up.
    int busy_timeout_ms = 5000;
};

/// Open (creating if necessary) the local note database and return a store.
///
/// On success `out` owns the connection and the schema is at the current
/// version. On failure `out` is left untouched and the returned status says
/// which stage failed (open vs. schema).
[[nodiscard]] Status open_sqlite_note_store(const SqliteNoteStoreOptions& options,
                                            std::unique_ptr<notes::NoteStore>& out);

/// Schema version this build writes and expects. Bumping it without a
/// migration is how silent data loss happens, so it lives next to the code.
inline constexpr int kNoteSchemaVersion = 1;

}  // namespace uapp::storage::sqlite

#endif  // UAPP_STORAGE_SQLITE_SQLITE_NOTE_STORE_H
