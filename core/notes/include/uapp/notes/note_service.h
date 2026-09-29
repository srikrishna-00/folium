// SPDX-License-Identifier: Apache-2.0
//
// core/notes/note_service.h — the public API of the note module.
//
// This is the one header an application layer (CLI today, the C ABI and the
// mobile bindings later) needs. It implements exactly the vertical slice named
// in spec section 40:
//
//     CREATE -> STORE -> SEARCH -> OPEN -> EDIT -> SAVE
//
//   create()  = CREATE + STORE LOCALLY
//   search()  = SEARCH NOTE
//   open()    = OPEN NOTE
//   save()    = SAVE NOTE   (EDIT happens on the caller's copy of the Note,
//                            which is why open() hands back a value type)
//
// Thread affinity: a NoteService is "use from one thread at a time". It holds
// no mutable state of its own, so concurrent use is safe only insofar as the
// injected NoteStore is (architecture.md section 6).

#ifndef UAPP_CORE_NOTES_NOTE_SERVICE_H
#define UAPP_CORE_NOTES_NOTE_SERVICE_H

#include <cstddef>
#include <string_view>
#include <vector>

#include "uapp/notes/note.h"
#include "uapp/notes/note_store.h"
#include "uapp/notes/ports.h"
#include "uapp/status.h"

namespace uapp::notes {

/// Default number of search hits returned when the caller does not care.
inline constexpr std::size_t kDefaultSearchLimit = 20;

class NoteService final {
public:
    /// All three collaborators are borrowed, not owned: they must outlive the
    /// service. Taking references rather than pointers makes "null was passed"
    /// unrepresentable instead of something to check at runtime.
    NoteService(NoteStore& store, Clock& clock, IdGenerator& ids) noexcept;

    NoteService(const NoteService&) = delete;
    NoteService& operator=(const NoteService&) = delete;

    /// CREATE + STORE. Validates, allocates an id, stamps created/modified,
    /// persists and indexes. `out` receives the stored note on success and is
    /// left untouched on failure.
    [[nodiscard]] Status create(std::string_view title,
                                std::string_view body,
                                Note& out) noexcept;

    /// OPEN. Status::NotFound if the id is unknown.
    [[nodiscard]] Status open(const NoteId& id, Note& out) const noexcept;

    /// SAVE. Persists an edited note: validates it, refreshes modified_at from
    /// the clock, and rewrites content + index atomically. created_at is taken
    /// from the stored row, not from the caller, so a client cannot rewrite
    /// history by handing back a doctored value.
    ///
    /// `note` is updated in place with the exact values that were persisted
    /// (notably the new modified_at), so the caller's copy never goes stale.
    [[nodiscard]] Status save(Note& note) noexcept;

    /// SEARCH. `query` is untrusted user input. Returns Status::Ok with an
    /// empty vector when nothing matches; Status::SearchQueryEmpty when the
    /// query has no searchable terms at all.
    [[nodiscard]] Status search(std::string_view query,
                                std::vector<SearchHit>& out,
                                std::size_t limit = kDefaultSearchLimit) const noexcept;

    /// Delete a note. Status::NotFound if absent.
    [[nodiscard]] Status remove(const NoteId& id) noexcept;

    /// Number of notes in the store.
    [[nodiscard]] Status count(std::size_t& out) const noexcept;

private:
    NoteStore&    store_;
    Clock&        clock_;
    IdGenerator&  ids_;
};

}  // namespace uapp::notes

#endif  // UAPP_CORE_NOTES_NOTE_SERVICE_H
