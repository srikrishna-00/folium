// SPDX-License-Identifier: Apache-2.0
//
// security/crypto/crypto.cpp — the only translation unit in the project that
// includes <sodium.h>.
//
// RULE 0 (crypto.h): there are no cryptographic primitives implemented here.
// Every line below is argument marshalling, bounds checking, nonce
// generation from the platform CSPRNG, and error-code mapping. If a future
// change adds arithmetic over key material to this file, that change is
// wrong and should be a libsodium call instead.

#include "uapp/crypto/crypto.h"

#include <sodium.h>

#include <atomic>
#include <cstring>

namespace uapp::crypto {
namespace {

// --- the sizes in crypto.h are checked against libsodium's, at compile time
//
// These are the reason crypto.h is allowed to declare its own constants
// rather than #including <sodium.h> (which would leak raw libsodium into
// every consumer). A libsodium roll that changed any of them stops the build
// here instead of producing a buffer that is the wrong length at runtime —
// and a wrong-length key or nonce buffer in a crypto module is a silent
// security failure, not a crash.
static_assert(kPublicKeyBytes == crypto_kx_PUBLICKEYBYTES, "X25519 public key size moved");
static_assert(kSecretKeyBytes == crypto_kx_SECRETKEYBYTES, "X25519 secret key size moved");
static_assert(kContentKeyBytes == crypto_aead_xchacha20poly1305_ietf_KEYBYTES,
              "AEAD key size moved");
static_assert(kNonceBytes == crypto_aead_xchacha20poly1305_ietf_NPUBBYTES,
              "AEAD nonce size moved — recheck the birthday-bound argument in crypto.h");
static_assert(kTagBytes == crypto_aead_xchacha20poly1305_ietf_ABYTES, "Poly1305 tag size moved");
static_assert(kSaltBytes == crypto_pwhash_SALTBYTES, "Argon2id salt size moved");
static_assert(kContentKeyBytes == crypto_kx_SESSIONKEYBYTES,
              "crypto_kx session key is no longer the same width as our AEAD key, so the "
              "KEK can no longer be used directly as an AEAD key");

// An all-zero key is what an uninitialised or partially-failed path produces,
// so it is worth being able to name. It is NOT a cryptographic check: a key
// can be catastrophically weak without being all-zero, and this must never be
// mistaken for key validation.
constexpr std::size_t kMaxAeadInputForOurCap = kMaxPlaintextBytes;

std::atomic<bool>& initialised_flag() noexcept {
    static std::atomic<bool> flag{false};
    return flag;
}

[[nodiscard]] Status require_init() noexcept {
    return initialised_flag().load(std::memory_order_acquire) ? Status::Ok
                                                              : Status::CryptoNotInitialised;
}

/// A fresh random nonce from the platform CSPRNG.
///
/// randombytes_buf cannot fail by contract — libsodium aborts the process if
/// the kernel CSPRNG is unavailable rather than returning weak bytes, which
/// is the correct behaviour and is why this returns void. The all-zero check
/// afterwards is a cheap sanity net against a catastrophically broken RNG
/// (a 1-in-2^192 false positive, i.e. never); it is NOT a randomness test
/// and must not be read as one.
[[nodiscard]] Status fresh_nonce(Nonce& out) noexcept {
    randombytes_buf(out.data(), out.size());
    if (sodium_is_zero(out.data(), out.size()) != 0) { return Status::CryptoRandomFailed; }
    return Status::Ok;
}

[[nodiscard]] Status check_view(ByteView v, std::size_t cap, Status too_large) noexcept {
    if (v.size != 0 && v.data == nullptr) { return Status::InvalidArgument; }
    if (v.size > cap) { return too_large; }
    return Status::Ok;
}

}  // namespace

// --- ContentKey ------------------------------------------------------------

Status ContentKey::allocate() { return SecureBytes::allocate(kContentKeyBytes, bytes_); }

Status ContentKey::import(const std::uint8_t* src, std::size_t n) {
    if (src == nullptr || n != kContentKeyBytes) { return Status::CryptoKeyInvalid; }
    if (!valid()) {
        const Status s = allocate();
        if (!is_ok(s)) { return s; }
    }
    std::memcpy(bytes_.data(), src, kContentKeyBytes);
    return Status::Ok;
}

// --- lifecycle -------------------------------------------------------------

Status init() {
    // sodium_init() returns 0 on first success, 1 if already initialised, and
    // -1 on failure. Treating 1 as success is what makes init() idempotent;
    // treating it as an error (an easy misreading) would make the second
    // caller in a process fail for no reason.
    const int rc = sodium_init();
    if (rc < 0) {
        initialised_flag().store(false, std::memory_order_release);
        return Status::CryptoNotInitialised;
    }
    initialised_flag().store(true, std::memory_order_release);
    return Status::Ok;
}

bool is_initialised() noexcept { return initialised_flag().load(std::memory_order_acquire); }

const char* library_version() noexcept {
    if (!is_initialised()) { return "uninitialised"; }
    return sodium_version_string();
}

// --- key generation --------------------------------------------------------

Status generate_device_keypair(DeviceKeyPair& out) {
    const Status init_status = require_init();
    if (!is_ok(init_status)) { return init_status; }

    out.public_key.fill(0);
    SecureBytes secret;
    const Status alloc = SecureBytes::allocate(kSecretKeyBytes, secret);
    if (!is_ok(alloc)) { return alloc; }

    // crypto_kx_keypair draws from the CSPRNG and clamps the scalar itself.
    // We never touch the scalar bits: X25519 clamping is exactly the kind of
    // detail Rule 0 exists to keep out of this file.
    if (crypto_kx_keypair(out.public_key.data(), secret.data()) != 0) {
        out.public_key.fill(0);
        return Status::CryptoRandomFailed;
    }

    out.secret_key = std::move(secret);
    return Status::Ok;
}

Status generate_content_key(ContentKey& out) {
    const Status init_status = require_init();
    if (!is_ok(init_status)) { return init_status; }

    std::uint8_t scratch[kContentKeyBytes];
    // ..._ietf_keygen() is randombytes_buf() with the right length baked in.
    // Using it rather than randombytes_buf(k, 32) means the length comes from
    // libsodium's own notion of its key size, so it cannot drift from ours.
    crypto_aead_xchacha20poly1305_ietf_keygen(scratch);

    const Status s = out.import(scratch, sizeof scratch);
    secure_zero(scratch, sizeof scratch);
    if (!is_ok(s)) { out.reset(); }
    return s;
}

Status generate_salt(Salt& out) {
    const Status init_status = require_init();
    if (!is_ok(init_status)) { return init_status; }
    randombytes_buf(out.data(), out.size());
    return Status::Ok;
}

// --- AEAD ------------------------------------------------------------------

Status seal(const ContentKey& key, ByteView plaintext, ByteView aad, SealedBlob& out) {
    out.nonce.fill(0);
    out.ciphertext.clear();

    const Status init_status = require_init();
    if (!is_ok(init_status)) { return init_status; }
    if (!key.valid()) { return Status::CryptoKeyInvalid; }

    Status v = check_view(plaintext, kMaxAeadInputForOurCap, Status::CryptoMessageTooLarge);
    if (!is_ok(v)) { return v; }
    v = check_view(aad, kMaxAadBytes, Status::CryptoMessageTooLarge);
    if (!is_ok(v)) { return v; }

    Nonce         nonce{};
    const Status  n = fresh_nonce(nonce);
    if (!is_ok(n)) { return n; }

    std::vector<std::uint8_t> ciphertext(plaintext.size + kTagBytes);

    unsigned long long written = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
        ciphertext.data(), &written, plaintext.data, static_cast<unsigned long long>(plaintext.size),
        aad.size == 0 ? nullptr : aad.data, static_cast<unsigned long long>(aad.size),
        nullptr,  // nsec: unused by this construction, libsodium requires null
        nonce.data(), key.bytes());

    if (rc != 0 || written != ciphertext.size()) {
        // Encryption failing is not a condition libsodium documents for valid
        // arguments, so there is no specific status for it; Internal is the
        // honest answer. The buffer is cleared rather than returned partly
        // filled, so a caller that ignores the Status cannot store garbage
        // as though it were ciphertext.
        secure_zero(ciphertext.data(), ciphertext.size());
        return Status::Internal;
    }

    out.nonce      = nonce;
    out.ciphertext = std::move(ciphertext);
    return Status::Ok;
}

Status open(const ContentKey& key, const SealedBlob& blob, ByteView aad, SecureBytes& out) {
    out.reset();

    const Status init_status = require_init();
    if (!is_ok(init_status)) { return init_status; }
    if (!key.valid()) { return Status::CryptoKeyInvalid; }

    // Untrusted input (spec §28). A blob shorter than a tag cannot be
    // authenticated at all, and the subtraction below would wrap if this
    // check were omitted — which is how a length check becomes a heap
    // overflow. Check before arithmetic, not after.
    if (blob.ciphertext.size() < kTagBytes) { return Status::CryptoSealedBlobBad; }

    const std::size_t plaintext_len = blob.ciphertext.size() - kTagBytes;
    if (plaintext_len > kMaxAeadInputForOurCap) { return Status::CryptoMessageTooLarge; }

    const Status v = check_view(aad, kMaxAadBytes, Status::CryptoMessageTooLarge);
    if (!is_ok(v)) { return v; }

    SecureBytes plaintext;
    const Status alloc = SecureBytes::allocate(plaintext_len, plaintext);
    if (!is_ok(alloc)) { return alloc; }

    unsigned long long written = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        plaintext.data(), &written,
        nullptr,  // nsec
        blob.ciphertext.data(), static_cast<unsigned long long>(blob.ciphertext.size()),
        aad.size == 0 ? nullptr : aad.data, static_cast<unsigned long long>(aad.size),
        blob.nonce.data(), key.bytes());

    if (rc != 0) {
        // THE IMPORTANT PART. libsodium verifies the Poly1305 tag before it
        // writes any plaintext, so on failure the buffer should already be
        // untouched — but "should" is not a property we are willing to rest
        // a confidentiality guarantee on across a dependency roll.
        // plaintext.reset() zeroes and releases, so no unauthenticated bytes
        // can reach the caller even if a future libsodium decrypted first and
        // authenticated second.
        //
        // ONE code for every cause (wrong key / tampered / truncated /
        // wrong nonce / wrong AAD). See status.h: anything finer is a
        // decryption oracle.
        plaintext.reset();
        return Status::CryptoAuthFailed;
    }

    if (written != plaintext_len) {
        plaintext.reset();
        return Status::Internal;
    }

    out = std::move(plaintext);
    return Status::Ok;
}

// --- key wrapping ----------------------------------------------------------
namespace {

/// Derive the directional KEK for a wrap or an unwrap.
///
/// `wrapping` selects the role: the wrapper is crypto_kx's "client" and uses
/// its tx key; the recipient is the "server" and uses its rx key. client.tx
/// == server.rx by construction, so both sides land on the same 32 bytes,
/// and the OTHER direction's key is different — which is what stops a
/// wrapped key being replayed back at its sender.
///
/// crypto_kx internally computes the X25519 shared point and hashes it
/// together with BOTH public keys in a fixed order. Hashing the public keys
/// in is what makes this an authenticated agreement rather than a raw DH:
/// without it, the shared secret alone says nothing about who holds the
/// other half. We rely on libsodium's construction rather than composing
/// scalarmult + a hash ourselves, per Rule 0.
[[nodiscard]] Status derive_kek(const DeviceKeyPair& ours, const PublicKey& theirs, bool wrapping,
                                ContentKey& kek) {
    if (!ours.valid()) { return Status::CryptoKeyInvalid; }

    std::uint8_t rx[crypto_kx_SESSIONKEYBYTES];
    std::uint8_t tx[crypto_kx_SESSIONKEYBYTES];

    const int rc = wrapping ? crypto_kx_client_session_keys(rx, tx, ours.public_key.data(),
                                                            ours.secret_key.data(), theirs.data())
                            : crypto_kx_server_session_keys(rx, tx, ours.public_key.data(),
                                                            ours.secret_key.data(), theirs.data());

    if (rc != 0) {
        // crypto_kx returns -1 when the peer's public key is not a usable
        // X25519 point — all-zero and the other small-order points drive the
        // agreement to a value an attacker already knows. Proceeding with a
        // predictable KEK would be the whole compromise, so this must be an
        // error and never a warning.
        secure_zero(rx, sizeof rx);
        secure_zero(tx, sizeof tx);
        return Status::CryptoKeyInvalid;
    }

    // The wrapper encrypts under its tx; the recipient decrypts under its rx.
    const Status s = kek.import(wrapping ? tx : rx, crypto_kx_SESSIONKEYBYTES);
    secure_zero(rx, sizeof rx);
    secure_zero(tx, sizeof tx);
    return s;
}

}  // namespace

Status wrap_content_key(const DeviceKeyPair& wrapper, const PublicKey& recipient_public,
                        const ContentKey& cek, WrappedKey& out) {
    out.nonce.fill(0);
    out.ciphertext.fill(0);

    const Status init_status = require_init();
    if (!is_ok(init_status)) { return init_status; }
    if (!cek.valid()) { return Status::CryptoKeyInvalid; }

    ContentKey   kek;
    const Status k = derive_kek(wrapper, recipient_public, /*wrapping=*/true, kek);
    if (!is_ok(k)) { return k; }

    // The CEK is just a 32-byte message to seal(), so the wrap inherits the
    // module's single nonce discipline (a fresh random 192-bit nonce) rather
    // than introducing a second one. The AAD binds both public keys, so a
    // wrapped key cannot be re-presented as having been exchanged between a
    // different pair of devices even if the same KEK were somehow reachable.
    std::uint8_t aad_buf[2 * kPublicKeyBytes];
    std::memcpy(aad_buf, wrapper.public_key.data(), kPublicKeyBytes);
    std::memcpy(aad_buf + kPublicKeyBytes, recipient_public.data(), kPublicKeyBytes);

    SealedBlob   sealed;
    const Status s = seal(kek, ByteView{cek.bytes(), kContentKeyBytes},
                          ByteView{aad_buf, sizeof aad_buf}, sealed);
    if (!is_ok(s)) { return s; }
    if (sealed.ciphertext.size() != out.ciphertext.size()) { return Status::Internal; }

    out.nonce = sealed.nonce;
    std::memcpy(out.ciphertext.data(), sealed.ciphertext.data(), out.ciphertext.size());
    return Status::Ok;
}

Status unwrap_content_key(const DeviceKeyPair& recipient, const PublicKey& wrapper_public,
                          const WrappedKey& wrapped, ContentKey& out) {
    out.reset();

    const Status init_status = require_init();
    if (!is_ok(init_status)) { return init_status; }

    ContentKey   kek;
    const Status k = derive_kek(recipient, wrapper_public, /*wrapping=*/false, kek);
    if (!is_ok(k)) { return k; }

    // Same AAD the wrapper bound: wrapper's public key first, recipient's
    // second. The order is part of the format and is NOT symmetric — getting
    // it backwards here would make every unwrap fail, which is the desired
    // failure direction for a mistake of this kind.
    std::uint8_t aad_buf[2 * kPublicKeyBytes];
    std::memcpy(aad_buf, wrapper_public.data(), kPublicKeyBytes);
    std::memcpy(aad_buf + kPublicKeyBytes, recipient.public_key.data(), kPublicKeyBytes);

    SealedBlob blob;
    blob.nonce = wrapped.nonce;
    blob.ciphertext.assign(wrapped.ciphertext.begin(), wrapped.ciphertext.end());

    SecureBytes  plaintext;
    const Status s = open(kek, blob, ByteView{aad_buf, sizeof aad_buf}, plaintext);
    if (!is_ok(s)) { return s; }
    if (plaintext.size() != kContentKeyBytes) { return Status::Internal; }

    return out.import(plaintext.data(), plaintext.size());
}

// --- passphrase KDF --------------------------------------------------------

void kdf_parameters(KdfProfile profile, unsigned long long& opslimit,
                    std::size_t& memlimit_bytes) noexcept {
    switch (profile) {
        case KdfProfile::DesktopModerate:
            // libsodium's MODERATE preset verbatim: ops=3, mem=256 MiB.
            opslimit       = crypto_pwhash_OPSLIMIT_MODERATE;
            memlimit_bytes = crypto_pwhash_MEMLIMIT_MODERATE;
            return;
        case KdfProfile::ArchivalSensitive:
            // libsodium's SENSITIVE preset verbatim: ops=4, mem=1 GiB.
            opslimit       = crypto_pwhash_OPSLIMIT_SENSITIVE;
            memlimit_bytes = crypto_pwhash_MEMLIMIT_SENSITIVE;
            return;
        case KdfProfile::MobileInteractive:
            break;
    }
    // Default, and the only mobile-legal profile. memlimit is INTERACTIVE's
    // 64 MiB — the largest preset that fits ADR-0002 decision 3's 2 GB floor
    // — and opslimit is 3 rather than INTERACTIVE's 2, following libsodium's
    // documented procedure ("set memlimit to the amount of memory you want to
    // reserve... then set opslimit to 3"). See crypto.h's "KDF PARAMETERS",
    // which also records that this is a deliberate deviation from
    // security-model.md §5's written 256 MiB.
    opslimit       = 3;
    memlimit_bytes = crypto_pwhash_MEMLIMIT_INTERACTIVE;
}

Status derive_key_from_passphrase(ByteView passphrase, const Salt& salt, KdfProfile profile,
                                  ContentKey& out) {
    out.reset();

    const Status init_status = require_init();
    if (!is_ok(init_status)) { return init_status; }

    if (passphrase.data == nullptr) { return Status::InvalidArgument; }
    if (passphrase.size < kMinPassphraseBytes) { return Status::CryptoPassphraseTooShort; }

    unsigned long long opslimit = 0;
    std::size_t        memlimit = 0;
    kdf_parameters(profile, opslimit, memlimit);

    std::uint8_t scratch[kContentKeyBytes];

    // crypto_pwhash_ALG_ARGON2ID13 EXPLICITLY, never ALG_DEFAULT: the
    // default changed from Argon2i to Argon2id at libsodium 1.0.15, and a
    // derived key whose algorithm depends on the linked version is a key
    // that silently stops deriving after a dependency roll. See crypto.h.
    const int rc = crypto_pwhash(scratch, kContentKeyBytes,
                                 reinterpret_cast<const char*>(passphrase.data),
                                 static_cast<unsigned long long>(passphrase.size), salt.data(),
                                 opslimit, memlimit, crypto_pwhash_ALG_ARGON2ID13);

    if (rc != 0) {
        // The documented failure is "out of memory". On a 2 GB floor device
        // asking for 64 MiB of contiguous Argon2 state, this is a condition
        // to handle, not an impossibility to assert away.
        secure_zero(scratch, sizeof scratch);
        return Status::CryptoKdfFailed;
    }

    const Status s = out.import(scratch, sizeof scratch);
    secure_zero(scratch, sizeof scratch);
    if (!is_ok(s)) { out.reset(); }
    return s;
}

}  // namespace uapp::crypto
