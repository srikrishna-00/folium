// SPDX-License-Identifier: Apache-2.0
#include "uapp/object_id.h"

#include <algorithm>

namespace uapp::detail {
namespace {

/// Locale-independent, total hex decode. Returns -1 for anything that is not
/// a hex digit, which is how the parser rejects input instead of coercing it.
constexpr int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

constexpr char kHexDigits[] = "0123456789abcdef";

}  // namespace

Status parse_object_id_hex(std::string_view hex, ObjectIdBytes& out) noexcept {
    if (hex.size() != kObjectIdHexChars) {
        return Status::IdMalformed;
    }

    // Decode into a scratch buffer: `out` must not be half-written when a
    // later character turns out to be garbage.
    ObjectIdBytes scratch{};
    for (std::size_t i = 0; i < kObjectIdBytes; ++i) {
        const int hi = hex_value(hex[i * 2]);
        const int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return Status::IdMalformed;
        }
        scratch[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }

    out = scratch;
    return Status::Ok;
}

std::string object_id_to_hex(const ObjectIdBytes& bytes) {
    std::string hex;
    hex.resize(kObjectIdHexChars);
    for (std::size_t i = 0; i < kObjectIdBytes; ++i) {
        const std::uint8_t b = bytes[i];
        hex[i * 2]     = kHexDigits[(b >> 4) & 0x0Fu];
        hex[i * 2 + 1] = kHexDigits[b & 0x0Fu];
    }
    return hex;
}

bool object_id_is_nil(const ObjectIdBytes& bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(),
                       [](std::uint8_t b) { return b == 0u; });
}

}  // namespace uapp::detail
