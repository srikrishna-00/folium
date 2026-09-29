// SPDX-License-Identifier: Apache-2.0
//
// core/notes/note_store.h — the persistence PORT for notes.
//
// This is the seam demanded by PHASE0_SUMMARY.md H-3 ("One persistence port.
// Nothing outside storage/sqlite includes sqlite3.h") and by spec section 32
// ("do not tightly couple business logic to one database engine"). core/notes
// links against this abstract interface only; the SQLite implementation lives
// in storage/sqlite and is injected at construction.
//
// Contract notes that implementations MUST honour:
//  * Every method is all-or-nothing. A failed `upsert` leaves the store
//    exactly as it was, including the search index -- content and index commit
//    in one transaction (database-design.md section 3.9).
//  * No method throws (ADR-0001: -fno-exceptions).
//  * Out-parameters are only written on Status::Ok, except where documented.

#ifndef UAPP_CORE_NOTES_NOTE_STORE_H
#define UAPP_CORE_NOTES_NOTE_STORE_H

#include <cstddef>
#include <string_view>
#include <vector>

#include "uapp/notes/note.h"
#include "uapp/status.h"

namespace uapp::notes {

class NoteStore {
public:
    virtual ~NoteStore();
    NoteStore(const NoteStore&) = delete;
    NoteStore& operator=(const NoteStore&) = delete;

    /// Insert a brand-new note. Returns Status::AlreadyExists if the id is
    /// taken. Also writes the full-text index entry, in the same transaction.
    [[nodiscard]] virtual Status insert(const Note& note) noexcept = 0;

    /// Overwrite an existing note. Returns Status::NotFound if it is absent --
    /// this method never creates. Reindexes in the same transaction.
    [[nodiscard]] virtual Status update(const Note& note) noexcept = 0;

    /// Load a note by id. Status::NotFound if absent.
    [[nodiscard]] virtual Status get(const NoteId& id, Note& out) const noexcept = 0;

    /// Delete a note and its index entry. Status::NotFound if absent.
    [[nodiscard]] virtual Status remove(const NoteId& id) noexcept = 0;

    /// Full-text search. `query` is raw user input; the implementation is
    /// responsible for sanitising it -- callers must never have to.
    /// `limit` is clamped to kMaxSearchLimit. `out` is cleared first.
    /// An empty result set is Status::Ok with an empty vector, NOT NotFound.
    [[nodiscard]] virtual Status search(std::string_view query,
                                        std::size_t limit,
                                        std::vector<SearchHit>& out) const noexcept = 0;

    /// Total number of stored notes. Mainly for diagnostics and tests.
    [[nodiscard]] virtual Status count(std::size_t& out) const noexcept = 0;

protected:
    NoteStore() noexcept;
};

}  // namespace uapp::notes

#endif  // UAPP_CORE_NOTES_NOTE_STORE_H
