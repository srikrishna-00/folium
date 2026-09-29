// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for NoteId parsing/formatting and the note validation rules.
// These are the "never assume an input is valid" (spec section 28) tests for
// the domain layer: everything here is untrusted text arriving from a CLI
// argument, a deep link or a sync payload.

#include <array>
#include <string>

#include "uapp/notes/note.h"
#include "uapp/status.h"

#include "test_framework.h"

using uapp::Status;
using uapp::notes::Note;
using uapp::notes::NoteId;

UAPP_TEST("note_id.roundtrips through hex") {
    std::array<std::uint8_t, uapp::notes::kNoteIdBytes> raw{};
    for (std::size_t i = 0; i < raw.size(); ++i) {
        raw[i] = static_cast<std::uint8_t>(i * 17u);
    }
    const NoteId id(raw);
    const std::string hex = id.to_hex();
    UAPP_REQUIRE(hex.size() == uapp::notes::kNoteIdHexChars);

    NoteId parsed;
    UAPP_REQUIRE_OK(NoteId::parse(hex, parsed));
    UAPP_CHECK(parsed == id);
    UAPP_CHECK(parsed.to_hex() == hex);
}

UAPP_TEST("note_id.accepts uppercase and normalises to lowercase") {
    NoteId id;
    UAPP_REQUIRE_OK(NoteId::parse("0123456789ABCDEF0123456789ABCDEF", id));
    UAPP_CHECK(id.to_hex() == "0123456789abcdef0123456789abcdef");
}

UAPP_TEST("note_id.rejects wrong length") {
    NoteId id;
    UAPP_REQUIRE_STATUS(NoteId::parse("", id), Status::NoteIdMalformed);
    UAPP_REQUIRE_STATUS(NoteId::parse("abcd", id), Status::NoteIdMalformed);
    // 31 chars
    UAPP_REQUIRE_STATUS(NoteId::parse("0123456789abcdef0123456789abcde", id),
                        Status::NoteIdMalformed);
    // 33 chars
    UAPP_REQUIRE_STATUS(NoteId::parse("0123456789abcdef0123456789abcdef0", id),
                        Status::NoteIdMalformed);
}

UAPP_TEST("note_id.rejects non-hex characters and leaves the output untouched") {
    NoteId original;
    UAPP_REQUIRE_OK(NoteId::parse("11111111111111111111111111111111", original));

    NoteId id = original;
    // Correct length, but the last character is not a hex digit. The failure
    // happens on the final byte, which is exactly the case where a partial
    // write would be easy to miss.
    UAPP_REQUIRE_STATUS(NoteId::parse("0123456789abcdef0123456789abcdez", id),
                        Status::NoteIdMalformed);
    UAPP_CHECK_MSG(id == original, "parse must not partially overwrite its output");

    UAPP_REQUIRE_STATUS(NoteId::parse("0123456789abcdef0123456789abcd g", id),
                        Status::NoteIdMalformed);
    UAPP_REQUIRE_STATUS(NoteId::parse("-123456789abcdef0123456789abcdef", id),
                        Status::NoteIdMalformed);
}

UAPP_TEST("note_id.default is nil") {
    const NoteId id;
    UAPP_CHECK(id.is_nil());
    UAPP_CHECK(id.to_hex() == "00000000000000000000000000000000");

    NoteId nonzero;
    UAPP_REQUIRE_OK(NoteId::parse("00000000000000000000000000000001", nonzero));
    UAPP_CHECK(!nonzero.is_nil());
    UAPP_CHECK(nonzero != id);
}

UAPP_TEST("validate.title length boundary") {
    UAPP_CHECK(uapp::notes::validate_title("") == Status::Ok);
    UAPP_CHECK(uapp::notes::validate_title(std::string(uapp::notes::kMaxTitleBytes, 'x')) ==
               Status::Ok);
    UAPP_CHECK(uapp::notes::validate_title(
                   std::string(uapp::notes::kMaxTitleBytes + 1, 'x')) ==
               Status::NoteTitleTooLong);
}

UAPP_TEST("validate.rejects embedded NUL") {
    const std::string with_nul = std::string("before") + '\0' + "after";
    UAPP_CHECK(uapp::notes::validate_title(with_nul) == Status::InvalidArgument);
    UAPP_CHECK(uapp::notes::validate_body(with_nul) == Status::InvalidArgument);
}

UAPP_TEST("validate.body length boundary") {
    UAPP_CHECK(uapp::notes::validate_body(
                   std::string(uapp::notes::kMaxBodyBytes, 'y')) == Status::Ok);
    UAPP_CHECK(uapp::notes::validate_body(
                   std::string(uapp::notes::kMaxBodyBytes + 1, 'y')) ==
               Status::NoteBodyTooLarge);
}

UAPP_TEST("validate.note rejects nil id and negative timestamps") {
    Note n;
    UAPP_CHECK(uapp::notes::validate_note(n) == Status::NoteIdMalformed);

    UAPP_REQUIRE_OK(NoteId::parse("00000000000000000000000000000002", n.id));
    UAPP_CHECK(uapp::notes::validate_note(n) == Status::Ok);

    n.created_at_ms = -1;
    UAPP_CHECK(uapp::notes::validate_note(n) == Status::InvalidArgument);
    n.created_at_ms = 0;
    n.modified_at_ms = -5;
    UAPP_CHECK(uapp::notes::validate_note(n) == Status::InvalidArgument);
}

UAPP_TEST("status.every code has a distinct name and a message") {
    // Guards against the classic copy-paste bug where two enum values map to
    // the same string and telemetry silently merges two failure modes.
    const Status all[] = {
        Status::Ok,                 Status::InvalidArgument,  Status::NotFound,
        Status::AlreadyExists,      Status::Internal,         Status::NoteTitleTooLong,
        Status::NoteBodyTooLarge,   Status::NoteIdMalformed,  Status::StoreOpenFailed,
        Status::StoreSchemaFailed,  Status::StoreReadFailed,  Status::StoreWriteFailed,
        Status::StoreReadOnly,      Status::SearchQueryEmpty, Status::SearchFailed,
    };
    for (const Status a : all) {
        UAPP_CHECK(uapp::status_code(a) != nullptr);
        UAPP_CHECK(uapp::status_message(a) != nullptr);
        UAPP_CHECK(std::string(uapp::status_code(a)) != "UNKNOWN_STATUS");
        for (const Status b : all) {
            if (a == b) { continue; }
            UAPP_CHECK_MSG(std::string(uapp::status_code(a)) != uapp::status_code(b),
                           std::string("duplicate status code string: ") +
                               uapp::status_code(a));
        }
    }
}
