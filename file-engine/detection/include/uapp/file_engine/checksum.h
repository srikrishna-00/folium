// SPDX-License-Identifier: Apache-2.0
//
// file-engine/detection/checksum.h — the `File::checksum` field of spec §8.
//
// ===========================================================================
// THE ALGORITHM CHOICE, AND WHY
// ===========================================================================
// Spec section 8 says "checksum" and nothing more, so the choice is ours and
// has to be justified rather than defaulted.
//
// WHAT THIS FIELD IS FOR (v1.0):
//   * integrity — "did these bytes change since we last looked?" for the
//     cache-invalidation and re-index paths;
//   * a cheap inequality test before an expensive byte comparison;
//   * a stable value to log when diagnosing "the viewer showed the old file".
//
// WHAT IT IS EXPLICITLY NOT:
//   * a signature or a tamper-evidence mechanism. An attacker who can change
//     the bytes can change the stored digest; that is what signing is for,
//     and per file-format-support.md §5.1 signing is P2/TBD.
//   * content addressing. See the NOT IMPLEMENTED note below.
//
// THE DECISION: **FNV-1a 64 with a finalising avalanche mix**, tagged
// `ChecksumAlgorithm::Fnv1a64`.
//
// Reasoning, against the two real alternatives:
//
//  1. *SHA-256* would be the choice if this field had to serve
//     content-addressing later (§7.1 already gives libsodium an ADOPT
//     verdict, so a vetted implementation is available in principle). It is
//     rejected **for now** on two grounds. First, libsodium is not vendored
//     yet, and file-format-support.md §7.10 requires seven things —
//     SPDX id, vendored licence text, pinned hash verified in CI, a
//     THIRD_PARTY_NOTICES row, static-link confirmation, a named CVE owner —
//     before any dependency lands. Taking on that process to compute an
//     integrity digest is the wrong trade, and the alternative, hand-rolling
//     SHA-256 here, is worse: the file-detection module is not where this
//     project should acquire its own unaudited crypto primitive. Second,
//     SHA-256 is roughly an order of magnitude slower per byte than a
//     non-cryptographic hash, and this runs over every byte of every file
//     the user opens, on Tier D hardware (adr/0002 decision 3).
//
//  2. *xxHash64 / wyhash* would be measurably better distributed and faster
//     than FNV-1a, but they are third-party code and hit the same §7.10
//     process for a hash we can write in fifteen lines.
//
//  FNV-1a's known weakness is poor avalanche in the low bits, which matters
//  when the digest is used directly as a hash-table index. The finalising
//  mix (the splitmix64/MurmurHash3 xor-shift-multiply finaliser) removes
//  that, and costs three instructions once per file rather than per byte.
//
// ===========================================================================
// NOT IMPLEMENTED: content-addressed storage and cross-device dedup.
// ===========================================================================
// `core/blocks/src/block.cpp` already anticipates "media and file blocks
// reference blob-store objects by content hash". A 64-bit digest is NOT
// adequate for that: at ~5 billion objects the birthday bound puts a
// collision at near-certainty, and a collision in a content-addressed store
// means one user's file silently resolving to another's. When the blob store
// is built it must use SHA-256 via libsodium.
//
// This is why `Checksum` carries an **algorithm tag and a variable-length
// byte array sized for SHA-256** rather than a bare `uint64_t`. Adding the
// second algorithm is then a new enumerator plus a new producer — no change
// to the field's type, no schema migration, and stored digests stay
// interpretable because each one says what produced it. Getting that shape
// right now is cheap; retrofitting it later is a data migration.

#ifndef UAPP_FILE_ENGINE_CHECKSUM_H
#define UAPP_FILE_ENGINE_CHECKSUM_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "uapp/file_engine/byte_span.h"

namespace uapp::files {

/// APPEND-ONLY: these values are persisted alongside the digest bytes.
enum class ChecksumAlgorithm : std::int32_t {
    None     = 0,  ///< no digest computed (e.g. detection-only on a path)
    Fnv1a64  = 1,  ///< FNV-1a 64 + avalanche finaliser; integrity only
    // Sha256 = 2 — NOT IMPLEMENTED. See the header comment.
};

[[nodiscard]] const char* checksum_algorithm_name(ChecksumAlgorithm algorithm) noexcept;

/// Widest digest the field must hold. Sized for SHA-256 so the eventual
/// second algorithm needs no type change.
inline constexpr std::size_t kMaxChecksumBytes = 32;

struct Checksum final {
    ChecksumAlgorithm                          algorithm = ChecksumAlgorithm::None;
    std::uint8_t                               length    = 0;  ///< valid bytes in `bytes`
    std::array<std::uint8_t, kMaxChecksumBytes> bytes{};

    /// Lowercase hex of the valid bytes. Empty for ChecksumAlgorithm::None.
    [[nodiscard]] std::string to_hex() const;

    [[nodiscard]] bool empty() const noexcept { return algorithm == ChecksumAlgorithm::None; }

    friend bool operator==(const Checksum& a, const Checksum& b) noexcept;
    friend bool operator!=(const Checksum& a, const Checksum& b) noexcept { return !(a == b); }
};

/// Streaming FNV-1a 64. Streaming because a path-based digest must not
/// require the whole file in memory — the detector reads a bounded buffer at
/// a time and feeds it here.
class Fnv1a64 final {
public:
    Fnv1a64() noexcept = default;

    void update(ByteSpan bytes) noexcept;

    /// The finalised digest, big-endian in `Checksum::bytes` so the hex
    /// rendering reads most-significant-first on every platform. Calling
    /// this does not reset the state; further `update` calls remain valid.
    [[nodiscard]] Checksum finish() const noexcept;

    /// The raw finalised 64-bit value, for tests and for hash-table use.
    [[nodiscard]] std::uint64_t value() const noexcept;

private:
    /// FNV offset basis (14695981039346656037).
    std::uint64_t state_ = 0xcbf29ce484222325ull;
};

/// One-shot convenience over a buffer already in memory.
[[nodiscard]] Checksum checksum_of(ByteSpan bytes) noexcept;

}  // namespace uapp::files

#endif  // UAPP_FILE_ENGINE_CHECKSUM_H
