// SPDX-License-Identifier: Apache-2.0
#include "uapp/notes/note_service.h"

#include <algorithm>

namespace uapp::notes {

NoteStore::NoteStore() noexcept = default;
NoteStore::~NoteStore() = default;

NoteService::NoteService(NoteStore& store, Clock& clock, IdGenerator& ids) noexcept
    : store_(store), clock_(clock), ids_(ids) {}

Status NoteService::create(std::string_view title,
                           std::string_view body,
                           Note& out) noexcept {
    // Validate BEFORE allocating an id or touching the clock: a rejected
    // create must consume nothing and leave no trace.
    if (const Status s = validate_title(title); !is_ok(s)) { return s; }
    if (const Status s = validate_body(body);   !is_ok(s)) { return s; }

    NoteId id;
    if (const Status s = ids_.generate(id); !is_ok(s)) { return s; }

    const std::int64_t now = clock_.now_unix_ms();

    Note note;
    note.id             = id;
    note.title.assign(title.data(), title.size());
    note.body.assign(body.data(), body.size());
    note.created_at_ms  = now;
    note.modified_at_ms = now;

    // Belt and braces: the store's contract assumes a valid note, so assert
    // that assumption here rather than inside every adapter.
    if (const Status s = validate_note(note); !is_ok(s)) { return s; }

    if (const Status s = store_.insert(note); !is_ok(s)) { return s; }

    out = note;  // only written once the write has actually committed
    return Status::Ok;
}

Status NoteService::open(const NoteId& id, Note& out) const noexcept {
    if (id.is_nil()) {
        return Status::NoteIdMalformed;
    }
    return store_.get(id, out);
}

Status NoteService::save(Note& note) noexcept {
    if (note.id.is_nil()) {
        return Status::NoteIdMalformed;
    }
    if (const Status s = validate_title(note.title); !is_ok(s)) { return s; }
    if (const Status s = validate_body(note.body);   !is_ok(s)) { return s; }

    // Read the stored row first. Two reasons, both deliberate:
    //  1. It turns "save a note that was deleted" into a clean NotFound
    //     instead of a silent no-op.
    //  2. created_at is authoritative in the store. A caller handing back a
    //     mutated Note must not be able to rewrite when the note was created.
    Note stored;
    if (const Status s = store_.get(note.id, stored); !is_ok(s)) { return s; }

    Note to_write     = note;
    to_write.created_at_ms  = stored.created_at_ms;
    to_write.modified_at_ms = clock_.now_unix_ms();

    if (const Status s = validate_note(to_write); !is_ok(s)) { return s; }
    if (const Status s = store_.update(to_write); !is_ok(s)) { return s; }

    note = to_write;  // caller's copy now matches what is on disk
    return Status::Ok;
}

Status NoteService::search(std::string_view query,
                           std::vector<SearchHit>& out,
                           std::size_t limit) const noexcept {
    out.clear();

    if (query.size() > kMaxQueryBytes) {
        // Reject rather than truncate: a silently truncated query returns
        // plausible-looking wrong results, which is worse than an error.
        return Status::InvalidArgument;
    }
    if (limit == 0) {
        return Status::InvalidArgument;
    }

    const std::size_t effective_limit = std::min(limit, kMaxSearchLimit);
    return store_.search(query, effective_limit, out);
}

Status NoteService::remove(const NoteId& id) noexcept {
    if (id.is_nil()) {
        return Status::NoteIdMalformed;
    }
    return store_.remove(id);
}

Status NoteService::count(std::size_t& out) const noexcept {
    return store_.count(out);
}

}  // namespace uapp::notes
