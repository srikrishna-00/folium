// SPDX-License-Identifier: Apache-2.0
#include "uapp/crypto/secure_bytes.h"

#include <sodium.h>

namespace uapp::crypto {

SecureBytes::~SecureBytes() { reset(); }

SecureBytes::SecureBytes(SecureBytes&& other) noexcept
    : data_(other.data_), size_(other.size_) {
    // The moved-from object must NOT keep a pointer it no longer owns: its
    // destructor runs too, and a double sodium_free() aborts the process via
    // the canary check. Clearing both fields is what makes the move safe,
    // not an optimisation.
    other.data_ = nullptr;
    other.size_ = 0;
}

SecureBytes& SecureBytes::operator=(SecureBytes&& other) noexcept {
    if (this == &other) { return *this; }
    // Release what we hold FIRST — and release it securely, via reset(), not
    // by leaking it. Self-assignment is checked above because reset() would
    // otherwise zero the very bytes we are about to adopt.
    reset();
    data_       = other.data_;
    size_       = other.size_;
    other.data_ = nullptr;
    other.size_ = 0;
    return *this;
}

Status SecureBytes::allocate(std::size_t n, SecureBytes& out) {
    out.reset();
    if (n == 0) { return Status::Ok; }

    void* p = sodium_malloc(n);
    if (p == nullptr) { return Status::CryptoSecureAllocFailed; }

    // sodium_malloc does not zero; it fills with 0xdb precisely so that
    // "forgot to initialise" produces an obviously wrong value rather than a
    // plausible all-zero key. We zero anyway, because this type's contract
    // to its callers is "allocate() gives you zeroed storage" and because an
    // all-zero key is at least a value our own validity checks can reject,
    // whereas 0xdb repeated is not.
    sodium_memzero(p, n);

    out.data_ = static_cast<std::uint8_t*>(p);
    out.size_ = n;
    return Status::Ok;
}

void SecureBytes::reset() noexcept {
    if (data_ != nullptr) {
        // sodium_free() verifies the canary (aborting on a heap overwrite)
        // and zeroes the region before unmapping it. The explicit
        // sodium_memzero here is therefore redundant with libsodium's own
        // behaviour — it is kept because this is the one place in the
        // codebase where "the key is gone" has to be true, and depending on
        // a documented side effect of a free function for that is a worse
        // bet than doing it twice. Cost: one memset of at most 8 MiB, on a
        // path that runs once per key.
        sodium_memzero(data_, size_);
        sodium_free(data_);
        data_ = nullptr;
    }
    size_ = 0;
}

void secure_zero(void* p, std::size_t n) noexcept {
    if (p == nullptr || n == 0) { return; }
    sodium_memzero(p, n);
}

bool secure_equal(const void* a, const void* b, std::size_t n) noexcept {
    if (a == nullptr || b == nullptr) { return false; }
    if (n == 0) { return true; }
    return sodium_memcmp(a, b, n) == 0;
}

}  // namespace uapp::crypto
