// SPDX-License-Identifier: Apache-2.0
#include "uapp/file_engine/byte_span.h"

#include <cstring>

namespace uapp::files {

bool ByteSpan::equal_at(std::size_t offset, const void* pattern,
                        std::size_t length) const noexcept {
    if (pattern == nullptr) { return false; }
    if (!covers(offset, length)) { return false; }
    if (length == 0) { return true; }
    return std::memcmp(data_ + offset, pattern, length) == 0;
}

ByteSpan ByteSpan::subspan(std::size_t offset, std::size_t count) const noexcept {
    if (offset >= size_) { return ByteSpan(); }
    const std::size_t available = size_ - offset;
    return ByteSpan(data_ + offset, count < available ? count : available);
}

bool ByteSpan::read_u16le(std::size_t offset, std::uint16_t& out) const noexcept {
    if (!covers(offset, 2)) { return false; }
    out = static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset]) |
                                     static_cast<std::uint16_t>(
                                         static_cast<std::uint16_t>(data_[offset + 1]) << 8));
    return true;
}

bool ByteSpan::read_u32le(std::size_t offset, std::uint32_t& out) const noexcept {
    if (!covers(offset, 4)) { return false; }
    out = static_cast<std::uint32_t>(data_[offset]) |
          (static_cast<std::uint32_t>(data_[offset + 1]) << 8) |
          (static_cast<std::uint32_t>(data_[offset + 2]) << 16) |
          (static_cast<std::uint32_t>(data_[offset + 3]) << 24);
    return true;
}

bool ByteSpan::rfind(const void* pattern, std::size_t length,
                     std::size_t& out_offset) const noexcept {
    if (pattern == nullptr || length == 0 || length > size_) { return false; }
    // `start` counts down from the last position where the pattern could
    // still fit. Using a post-decrement on an unsigned index would wrap at
    // zero, so the loop is written to test before it decrements.
    std::size_t start = size_ - length;
    for (;;) {
        if (std::memcmp(data_ + start, pattern, length) == 0) {
            out_offset = start;
            return true;
        }
        if (start == 0) { return false; }
        --start;
    }
}

std::string_view ByteSpan::text_at(std::size_t offset, std::size_t length) const noexcept {
    if (!covers(offset, length)) { return std::string_view(); }
    return std::string_view(reinterpret_cast<const char*>(data_ + offset), length);
}

ByteSpan byte_span_of(std::string_view text) noexcept {
    return ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

}  // namespace uapp::files
