// SPDX-License-Identifier: Apache-2.0
#include "uapp/file_engine/checksum.h"

namespace uapp::files {
namespace {

/// FNV prime for the 64-bit variant (1099511628211).
constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

/// MurmurHash3 / splitmix64 finaliser. FNV-1a's low bits correlate strongly
/// with the last byte fed in; this is what fixes that. Run once, at finish(),
/// never per byte.
constexpr std::uint64_t avalanche(std::uint64_t x) noexcept {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

char hex_digit(std::uint8_t nibble) noexcept {
    return static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
}

}  // namespace

const char* checksum_algorithm_name(ChecksumAlgorithm algorithm) noexcept {
    switch (algorithm) {
        case ChecksumAlgorithm::None:    return "none";
        case ChecksumAlgorithm::Fnv1a64: return "fnv1a64";
    }
    return "unknown";
}

std::string Checksum::to_hex() const {
    if (algorithm == ChecksumAlgorithm::None) { return std::string(); }
    const std::size_t n = length <= kMaxChecksumBytes ? length : kMaxChecksumBytes;
    std::string       out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(hex_digit(static_cast<std::uint8_t>(bytes[i] >> 4)));
        out.push_back(hex_digit(static_cast<std::uint8_t>(bytes[i] & 0x0Fu)));
    }
    return out;
}

bool operator==(const Checksum& a, const Checksum& b) noexcept {
    if (a.algorithm != b.algorithm || a.length != b.length) { return false; }
    const std::size_t n = a.length <= kMaxChecksumBytes ? a.length : kMaxChecksumBytes;
    for (std::size_t i = 0; i < n; ++i) {
        if (a.bytes[i] != b.bytes[i]) { return false; }
    }
    return true;
}

void Fnv1a64::update(ByteSpan bytes) noexcept {
    const std::uint8_t* p = bytes.data();
    const std::size_t   n = bytes.size();
    for (std::size_t i = 0; i < n; ++i) {
        state_ ^= static_cast<std::uint64_t>(p[i]);
        state_ *= kFnvPrime;
    }
}

std::uint64_t Fnv1a64::value() const noexcept { return avalanche(state_); }

Checksum Fnv1a64::finish() const noexcept {
    const std::uint64_t v = value();
    Checksum            out;
    out.algorithm = ChecksumAlgorithm::Fnv1a64;
    out.length    = 8;
    // Big-endian, so to_hex() reads most-significant-first everywhere.
    for (std::size_t i = 0; i < 8; ++i) {
        const unsigned shift = static_cast<unsigned>(56 - (i * 8));
        out.bytes[i]         = static_cast<std::uint8_t>((v >> shift) & 0xFFull);
    }
    return out;
}

Checksum checksum_of(ByteSpan bytes) noexcept {
    Fnv1a64 hasher;
    hasher.update(bytes);
    return hasher.finish();
}

}  // namespace uapp::files
