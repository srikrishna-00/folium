// SPDX-License-Identifier: Apache-2.0
//
// core/common/object_id.h — the shared 128-bit object identifier.
//
// database-design.md section 2 makes one identifier decision for the whole
// product: "UUIDv7 rendered as a 16-byte BLOB", client-generatable so that
// offline creation never waits for a server key. `core/notes` already encodes
// that shape in its own `NoteId`; `core/blocks` needs the same shape for
// pages AND for blocks, and a third and fourth hand-rolled copy of hex
// parsing is how one of them ends up with a subtly different validator.
//
// So the bytes, the parser and the renderer live here once. The *type* is
// still per-domain: `ObjectId` is a phantom-tag template, so a `BlockId` will
// not bind to a parameter expecting a `PageId` even though both are 16 bytes.
// That is a compile-time property with no runtime cost -- the class is a
// standard-layout aggregate of exactly 16 bytes, which is also what keeps it
// trivially describable to the future C ABI (ADR-0001) as `uint8_t[16]`.
//
// NOTE on `notes::NoteId`: it is deliberately NOT retyped in terms of this
// template in this change. See docs/adr/0003-page-supersedes-note.md --
// core/notes is frozen, not refactored, and rewriting its public id type
// would churn core/capi, which this change is not allowed to touch.

#ifndef UAPP_CORE_COMMON_OBJECT_ID_H
#define UAPP_CORE_COMMON_OBJECT_ID_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "uapp/status.h"

namespace uapp {

inline constexpr std::size_t kObjectIdBytes    = 16;
inline constexpr std::size_t kObjectIdHexChars = kObjectIdBytes * 2;

using ObjectIdBytes = std::array<std::uint8_t, kObjectIdBytes>;

namespace detail {

/// Decode 32 hex characters (either case) into `out`. Returns
/// Status::IdMalformed for any other length or any non-hex byte, and never
/// partially writes `out` on failure.
[[nodiscard]] Status parse_object_id_hex(std::string_view hex, ObjectIdBytes& out) noexcept;

/// Render as 32 lowercase hex characters.
[[nodiscard]] std::string object_id_to_hex(const ObjectIdBytes& bytes);

[[nodiscard]] bool object_id_is_nil(const ObjectIdBytes& bytes) noexcept;

}  // namespace detail

/// An opaque 128-bit identifier, tagged with the domain type it names.
///
/// `Tag` is never instantiated -- it exists only to make `ObjectId<PageTag>`
/// and `ObjectId<BlockTag>` distinct types to the compiler.
template <class Tag>
class ObjectId final {
public:
    constexpr ObjectId() noexcept = default;

    explicit constexpr ObjectId(const ObjectIdBytes& raw) noexcept : bytes_(raw) {}

    /// The only way to build one from untrusted text. There is no
    /// "trust me" constructor from a string.
    [[nodiscard]] static Status parse(std::string_view hex, ObjectId& out) noexcept {
        ObjectIdBytes scratch{};
        if (const Status s = detail::parse_object_id_hex(hex, scratch); !is_ok(s)) {
            return s;
        }
        out = ObjectId(scratch);
        return Status::Ok;
    }

    [[nodiscard]] std::string to_hex() const { return detail::object_id_to_hex(bytes_); }

    [[nodiscard]] const ObjectIdBytes& bytes() const noexcept { return bytes_; }

    /// The all-zero id. Used as "unset" and, for a block's parent, as
    /// "top level"; it is never persisted as a real object's id.
    [[nodiscard]] bool is_nil() const noexcept { return detail::object_id_is_nil(bytes_); }

    friend bool operator==(const ObjectId& a, const ObjectId& b) noexcept {
        return a.bytes_ == b.bytes_;
    }
    friend bool operator!=(const ObjectId& a, const ObjectId& b) noexcept { return !(a == b); }

    /// Ordering exists so ids can key a std::map during tree assembly. It is
    /// lexicographic over the raw bytes and carries no domain meaning.
    friend bool operator<(const ObjectId& a, const ObjectId& b) noexcept {
        return a.bytes_ < b.bytes_;
    }

private:
    ObjectIdBytes bytes_{};
};

}  // namespace uapp

#endif  // UAPP_CORE_COMMON_OBJECT_ID_H
