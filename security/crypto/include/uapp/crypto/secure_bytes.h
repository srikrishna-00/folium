// SPDX-License-Identifier: Apache-2.0
//
// security/crypto/secure_bytes.h — a buffer for key material and recovered
// plaintext, backed by libsodium's guarded allocator.
//
// WHY THIS TYPE EXISTS, i.e. why key material does not live in a
// std::vector<std::uint8_t>:
//
//   A std::vector's buffer is ordinary heap memory. On destruction the
//   allocator reclaims it WITHOUT zeroing it, so the key bytes stay in the
//   process's address space until something else happens to overwrite them.
//   Worse, nothing stops them being written to swap or captured in a core
//   dump, and a compiler is entitled to delete a `memset(p, 0, n)` just
//   before a `free(p)` as a dead store. `security-model.md` §10 forbids keys
//   in logs and crash reports; a key sitting in an unzeroed freed heap block
//   is in the crash report whether we logged it or not.
//
//   sodium_malloc() answers all of that at once, and the properties are
//   libsodium's documented ones rather than our inferences:
//     * a guard page before and after the allocation, so an overflow or
//       underflow is an immediate SIGSEGV instead of a silent key leak into
//       an adjacent object;
//     * a canary before the data, verified by sodium_free(), so a heap
//       overwrite aborts the process rather than producing a wrong key;
//     * the region is sodium_mlock()ed, so it is excluded from swap and
//       (where the platform supports it) from core dumps;
//     * sodium_free() ZEROES the region before releasing it, using
//       sodium_memzero(), which libsodium implements so the compiler cannot
//       elide it;
//     * the page immediately after the data is PROT_NONE, so a read past the
//       end traps.
//
// WHAT THIS TYPE DOES NOT DO — the honest limits, because a security type
// that oversells itself is worse than none:
//
//   1. **It is not a Secure Enclave / StrongBox.** `security-model.md` §5A
//      says device keys are generated in hardware and never leave it. A
//      sodium_malloc region is ordinary userspace memory with better hygiene.
//      Anything with debugger access to the process can read it. The
//      hardware-backed path is the `security/key-management` adapter in
//      `architecture.md`'s layer diagram; it does not exist
//      (third_party/libsodium/README.md item 6).
//   2. **It does not survive `fork()` safely**, and it does not pin anything
//      across a platform process suspension. On iOS an app moved to the
//      background may be frozen with this memory resident; that is a key
//      *lifetime* policy question (when do we drop keys?) which belongs
//      above this layer and is NOT IMPLEMENTED.
//   3. **It is not a general-purpose allocator**, and libsodium says so
//      outright. Each allocation costs at least three extra pages. Do not
//      put a 500 MB file through it. The size cap in crypto.h
//      (kMaxPlaintextBytes) is partly there to keep that true.
//   4. **sodium_mlock() can fail** on a host with a low RLIMIT_MEMLOCK. When
//      it does, sodium_malloc still succeeds and the region is simply not
//      locked, so the swap guarantee silently degrades. libsodium exposes no
//      way to ask whether the lock happened, so neither does this type.
//      Recorded rather than hidden.
//
// Move-only, by design. A copy constructor would mean a second copy of a key
// in memory with a second lifetime to get wrong, and essentially every
// accidental key duplication starts as an implicit copy. Deleting it makes
// the duplication a compile error.

#ifndef UAPP_SECURITY_CRYPTO_SECURE_BYTES_H
#define UAPP_SECURITY_CRYPTO_SECURE_BYTES_H

#include <cstddef>
#include <cstdint>

#include "uapp/status.h"

namespace uapp::crypto {

class SecureBytes final {
  public:
    SecureBytes() noexcept = default;
    ~SecureBytes();

    SecureBytes(SecureBytes&& other) noexcept;
    SecureBytes& operator=(SecureBytes&& other) noexcept;

    SecureBytes(const SecureBytes&)            = delete;
    SecureBytes& operator=(const SecureBytes&) = delete;

    /// Allocate `n` guarded, zeroed bytes into `out`, replacing whatever
    /// `out` held (which is securely released first).
    ///
    /// `n == 0` is legal and leaves `out` empty with a null data() — it does
    /// NOT allocate three pages to hold nothing.
    ///
    /// Returns Status::CryptoSecureAllocFailed if sodium_malloc() refuses.
    /// That is a real condition rather than a theoretical one: guarded
    /// allocations consume address space and mapping quota far faster than
    /// malloc does, and on a 2 GB device floor (ADR-0002 decision 3) it can
    /// fail while ordinary allocation still succeeds. Callers must treat it
    /// as an error, not as an abort-worthy impossibility.
    [[nodiscard]] static Status allocate(std::size_t n, SecureBytes& out);

    /// Zero and release. Idempotent. After this, size() == 0, data() == null.
    void reset() noexcept;

    [[nodiscard]] std::uint8_t*       data() noexcept { return data_; }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t         size() const noexcept { return size_; }
    [[nodiscard]] bool                empty() const noexcept { return size_ == 0; }

  private:
    std::uint8_t* data_ = nullptr;
    std::size_t   size_ = 0;
};

/// Zero `n` bytes at `p` such that the compiler cannot optimise it away.
///
/// This is a thin wrapper over sodium_memzero, and it exists so that callers
/// outside this module never have to #include <sodium.h> (see crypto.h's
/// note on why the libsodium header is private to this module). Use it for
/// key material living in memory this module does not own — a stack array, a
/// caller's buffer, a std::vector that a keystore adapter filled.
///
/// `std::memset` is NOT an acceptable substitute. A memset whose result is
/// never read is a dead store and a conforming compiler may delete it; this
/// is a well-known class of real CVE, not a theoretical concern.
void secure_zero(void* p, std::size_t n) noexcept;

/// Constant-time equality over `n` bytes. Returns true iff equal.
///
/// Wraps sodium_memcmp. Use this and never `std::memcmp` when either side is
/// secret: memcmp returns as soon as it finds a difference, so its timing
/// leaks how many leading bytes matched, which is enough to recover a MAC or
/// a key byte-by-byte. The tests use it to compare keys for the same reason.
[[nodiscard]] bool secure_equal(const void* a, const void* b, std::size_t n) noexcept;

}  // namespace uapp::crypto

#endif  // UAPP_SECURITY_CRYPTO_SECURE_BYTES_H
