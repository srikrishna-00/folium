// SPDX-License-Identifier: Apache-2.0
#include "uapp/notes/note.h"

#include <algorithm>

namespace uapp::notes {
namespace {

/// Hex nibble decode without <cctype>: locale-independent and total. Returns
/// -1 for anything that is not a hex digit, which is how `parse` rejects
/// input rather than silently coercing it.
constexpr int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

constexpr char kHexDigits[] = "0123456789abcdef";

}  // namespace

Status NoteId::parse(std::string_view hex, NoteId& out) noexcept {
    if (hex.size() != kNoteIdHexChars) {
        return Status::NoteIdMalformed;
    }

    // Decode into a scratch buffer first: `out` must not be half-written if a
    // later character turns out to be garbage.
    std::array<std::uint8_t, kNoteIdBytes> scratch{};
    for (std::size_t i = 0; i < kNoteIdBytes; ++i) {
        const int hi = hex_value(hex[i * 2]);
        const int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return Status::NoteIdMalformed;
        }
        scratch[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }

    out = NoteId(scratch);
    return Status::Ok;
}

std::string NoteId::to_hex() const {
    std::string hex;
    hex.resize(kNoteIdHexChars);
    for (std::size_t i = 0; i < kNoteIdBytes; ++i) {
        const std::uint8_t b = bytes_[i];
        hex[i * 2]     = kHexDigits[(b >> 4) & 0x0Fu];
        hex[i * 2 + 1] = kHexDigits[b & 0x0Fu];
    }
    return hex;
}

bool NoteId::is_nil() const noexcept {
    return std::all_of(bytes_.begin(), bytes_.end(),
                       [](std::uint8_t b) { return b == 0u; });
}

Status validate_title(std::string_view title) noexcept {
    // An empty title is allowed -- users create untitled notes constantly and
    // database-design.md gives `document.title` a DEFAULT ''. Only the upper
    // bound is a hard error.
    if (title.size() > kMaxTitleBytes) {
        return Status::NoteTitleTooLong;
    }
    // Embedded NUL would truncate the value the moment it touched any C API
    // (sqlite3_bind_text, printf, the future C ABI). Reject it here, once,
    // rather than discovering a silently shortened note later.
    if (title.find('\0') != std::string_view::npos) {
        return Status::InvalidArgument;
    }
    return Status::Ok;
}

Status validate_body(std::string_view body) noexcept {
    if (body.size() > kMaxBodyBytes) {
        return Status::NoteBodyTooLarge;
    }
    if (body.find('\0') != std::string_view::npos) {
        return Status::InvalidArgument;
    }
    return Status::Ok;
}

Status validate_note(const Note& note) noexcept {
    if (note.id.is_nil()) {
        return Status::NoteIdMalformed;
    }
    if (const Status s = validate_title(note.title); !is_ok(s)) {
        return s;
    }
    if (const Status s = validate_body(note.body); !is_ok(s)) {
        return s;
    }
    if (note.created_at_ms < 0 || note.modified_at_ms < 0) {
        return Status::InvalidArgument;
    }
    return Status::Ok;
}

}  // namespace uapp::notes
