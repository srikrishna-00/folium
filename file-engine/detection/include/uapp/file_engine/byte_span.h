// SPDX-License-Identifier: Apache-2.0
//
// file-engine/detection/byte_span.h — the bounds-checked window over bytes
// we do not trust.
//
// WHY THIS TYPE EXISTS AT ALL
// ---------------------------------------------------------------------------
// Every byte this module looks at is attacker-controlled (spec section 28/29:
// corrupted, malformed and truncated files are a named test class, and this
// detector will eventually run on uploaded files). The single most common way
// a sniffer gets an out-of-bounds read is `memcmp(p, "\x89PNG\r\n\x1a\n", 8)`
// on a two-byte file. Fixing that with an `if (size >= 8)` at every call site
// means the safety of the module is the sum of ~40 hand-written length checks,
// and one missing check is a heap overread.
//
// So the length check is not written at the call sites. It is written *once*,
// here, and there is no accessor on this type that can read out of range:
//
//   * `equal_at` returns false rather than comparing a short buffer.
//   * `read_u16le` / `read_u32le` return false rather than reading a partial
//     integer, and never leave `out` half-written.
//   * `subspan` clamps instead of producing a span past the end.
//   * offsets are `std::size_t` and every offset+length is computed with an
//     overflow guard, so a 0xFFFFFFFF length from a corrupt ZIP header cannot
//     wrap to a small number.
//
// C++17 has no std::span, and ADR-0001's subset does not permit pulling in a
// dependency for one (spec section 35 wants a licence review per dependency).
// This is deliberately narrower than std::span anyway: it is const, it is
// read-only, and it has no operator[].
//
// This type NEVER owns memory. The caller guarantees the pointed-to bytes
// outlive the span, exactly as with std::string_view.

#ifndef UAPP_FILE_ENGINE_BYTE_SPAN_H
#define UAPP_FILE_ENGINE_BYTE_SPAN_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace uapp::files {

class ByteSpan final {
public:
    constexpr ByteSpan() noexcept = default;

    /// A null pointer always yields an empty span, whatever `size` claims.
    /// (`ByteSpan(nullptr, 8)` is a caller bug; treating it as 8 readable
    /// bytes would be our bug.)
    ByteSpan(const std::uint8_t* data, std::size_t size) noexcept
        : data_(data == nullptr ? nullptr : data), size_(data == nullptr ? 0 : size) {}

    [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t         size() const noexcept { return size_; }
    [[nodiscard]] bool                empty() const noexcept { return size_ == 0; }

    /// True iff `[offset, offset + length)` lies wholly inside the span.
    /// Overflow-safe: the addition is never performed.
    [[nodiscard]] bool covers(std::size_t offset, std::size_t length) const noexcept {
        if (offset > size_) { return false; }
        return length <= size_ - offset;
    }

    /// Byte at `offset`, or `fallback` if that offset is out of range.
    [[nodiscard]] std::uint8_t at_or(std::size_t offset, std::uint8_t fallback) const noexcept {
        return offset < size_ ? data_[offset] : fallback;
    }

    /// True iff the `length` bytes at `offset` equal `pattern`. Returns false
    /// — never reads — when the range is short. A zero-length pattern matches
    /// only if the offset itself is in range, so it cannot be used to probe
    /// past the end.
    [[nodiscard]] bool equal_at(std::size_t offset, const void* pattern,
                                std::size_t length) const noexcept;

    [[nodiscard]] bool starts_with(const void* pattern, std::size_t length) const noexcept {
        return equal_at(0, pattern, length);
    }

    /// The largest in-range prefix of `[offset, offset + count)`. An offset
    /// past the end yields an empty span rather than undefined behaviour.
    [[nodiscard]] ByteSpan subspan(std::size_t offset, std::size_t count) const noexcept;

    /// Little-endian integer reads, as every ZIP structure field is stored.
    /// Return false and leave `out` untouched when the field is not fully
    /// present — a truncated central directory must not yield a plausible
    /// offset assembled from whatever followed the buffer.
    [[nodiscard]] bool read_u16le(std::size_t offset, std::uint16_t& out) const noexcept;
    [[nodiscard]] bool read_u32le(std::size_t offset, std::uint32_t& out) const noexcept;

    /// Find the last occurrence of `pattern` in the span, searching backwards
    /// from the end. Returns false if absent. Used for the ZIP end-of-central
    /// -directory record, which is located by a backwards scan by definition.
    [[nodiscard]] bool rfind(const void* pattern, std::size_t length,
                             std::size_t& out_offset) const noexcept;

    /// Interpret the bytes at `[offset, offset + length)` as text, for
    /// comparing ZIP member names. Empty when out of range. The result is a
    /// view into the same bytes and carries the same lifetime rules.
    [[nodiscard]] std::string_view text_at(std::size_t offset, std::size_t length) const noexcept;

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t         size_ = 0;
};

/// Build a span over an existing contiguous byte sequence. Provided so tests
/// and callers holding a std::string do not hand-roll the reinterpret_cast.
[[nodiscard]] ByteSpan byte_span_of(std::string_view text) noexcept;

}  // namespace uapp::files

#endif  // UAPP_FILE_ENGINE_BYTE_SPAN_H
