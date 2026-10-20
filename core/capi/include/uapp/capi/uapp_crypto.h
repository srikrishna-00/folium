/* SPDX-License-Identifier: Apache-2.0 */
/*
 * uapp/capi/uapp_crypto.h -- the PRIMITIVES surface of libuapp_capi, over
 * security/crypto/include/uapp/crypto/crypto.h.
 *
 * Deferred since ABI 0.1 on security/crypto/crypto.h's own NOT IMPLEMENTED
 * item 6: "the shape of these functions will change when the key HIERARCHY
 * above them exists." That hierarchy (security/keys, see uapp_keys.h) has
 * landed, so this surface is minted now, in the same ABI bump as uapp_keys.h.
 *
 * ===========================================================================
 * THIS SURFACE IS OPTIONAL. CHECK uapp_crypto_is_available() BEFORE USE.
 * ===========================================================================
 * security/crypto is built only when a libsodium is found (UAPP_ENABLE_CRYPTO,
 * AUTO by default -- see the root CMakeLists.txt and
 * third_party/libsodium/README.md). A library built without it does not
 * export any uapp_crypto_* or uapp_keys_* symbol declared in this header or
 * uapp_keys.h.
 *
 * ===========================================================================
 * WHAT THIS BOUNDARY DELIBERATELY DOES NOT EXPOSE
 * ===========================================================================
 * There is NO accessor for a key's raw bytes, anywhere in this header.
 * uapp_crypto_keypair_t exposes only its PUBLIC half (uapp_crypto_keypair_public_key).
 * uapp_crypto_content_key_t is consumed by seal/open/wrap/unwrap and never
 * read. This is not an oversight waiting for a "give me the bytes" function
 * to be added later -- crypto.h's ContentKey deliberately has no
 * mutable_bytes() and no const-bytes accessor exposed to this boundary for
 * the same reason the C++ type does not expose one to ITS callers: a raw
 * writable or readable pointer into key storage is how half-initialised or
 * leaked keys happen. A binding that needs the bytes is asking the wrong
 * question of this ABI by design.
 *
 * ===========================================================================
 * CROSSING THE SECURE-MEMORY BOUNDARY: uapp_crypto_open() IS WHERE IT HAPPENS
 * ===========================================================================
 * crypto::open() in the C++ layer returns recovered plaintext into a
 * SecureBytes -- guarded, mlock()ed pages (secure_bytes.h). This function
 * necessarily copies that plaintext into the CALLER'S buffer to hand it back
 * across the C boundary, which is an ordinary heap or stack buffer on the
 * other side. That copy is NOT guarded, and this header says so rather than
 * implying a guarantee it cannot keep: a C, Kotlin or Swift caller that holds
 * recovered plaintext for any length of time is responsible for its own
 * zeroing/guarding on that side of the boundary. This module's own internal
 * scratch copy is zeroed before returning, every time, success or failure.
 *
 * ERRORS, OWNERSHIP, THREADING: uapp_capi.h's rules, unchanged.
 */

#ifndef UAPP_CAPI_UAPP_CRYPTO_H
#define UAPP_CAPI_UAPP_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#include "uapp/capi/uapp_capi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- sizes, read at runtime, not hard-coded by a binding ------------------- */

typedef struct uapp_crypto_limits {
    uint32_t struct_size;            /* in: sizeof(uapp_crypto_limits_t) */
    uint32_t public_key_bytes;       /* out: == 32 */
    uint32_t secret_key_bytes;       /* out: == 32, informational -- never read back */
    uint32_t content_key_bytes;      /* out: == 32 */
    uint32_t nonce_bytes;            /* out: == 24 */
    uint32_t tag_bytes;              /* out: == 16 */
    uint32_t salt_bytes;             /* out: == 16 */
    uint32_t wrapped_key_bytes;      /* out: content_key_bytes + tag_bytes, == 48 */
    uint64_t max_plaintext_bytes;    /* out: largest seal() input */
    uint64_t max_aad_bytes;          /* out: largest associated-data input */
    uint32_t min_passphrase_bytes;   /* out: floor for derive_key_from_passphrase() */
    uint32_t max_kdf_label_bytes;    /* out: derive_subkey() label cap */
    uint32_t max_kdf_info_bytes;     /* out: derive_subkey() info cap */
} uapp_crypto_limits_t;

UAPP_CAPI_API uapp_status_t uapp_crypto_get_limits(uapp_crypto_limits_t* out_limits);

/* --- lifecycle --------------------------------------------------------------
 *
 * MUST be called, and must succeed, before any other uapp_crypto_* or
 * uapp_keys_* function; every one of them returns UAPP_STATUS_CRYPTO_NOT_INITIALISED
 * otherwise. Idempotent and thread-safe to call more than once (crypto::init()
 * documents both), but call it exactly once at process start.
 */
UAPP_CAPI_API uapp_status_t uapp_crypto_init(void);
UAPP_CAPI_API uapp_bool_t    uapp_crypto_is_initialised(void);

/* Borrowed, static-storage, never freed -- same rule as uapp_status_code().
 * Never NULL; "uninitialised" before uapp_crypto_init() succeeds. */
UAPP_CAPI_API const char* uapp_crypto_library_version(void);

/* --- Argon2id cost profile, mirrors crypto::KdfProfile exactly -------------
 *
 * MobileInteractive is the ONLY profile legal on a path that also runs on a
 * phone -- see crypto.h's "KDF PARAMETERS" note for the full argument. There
 * is no default value on purpose: a caller must say which cost it is paying.
 */
typedef enum uapp_crypto_kdf_profile {
    UAPP_CRYPTO_KDF_PROFILE_MOBILE_INTERACTIVE = 0,
    UAPP_CRYPTO_KDF_PROFILE_DESKTOP_MODERATE    = 1,
    UAPP_CRYPTO_KDF_PROFILE_ARCHIVAL_SENSITIVE  = 2,

    UAPP_CRYPTO_KDF_PROFILE_FORCE_INT32 = 0x7FFFFFFF
} uapp_crypto_kdf_profile_t;

/* --- a fresh, uniform-random Argon2id salt ----------------------------------
 *
 * MUST be generated fresh for every new passphrase derivation and then
 * stored and reused for every later re-derivation of the SAME key -- a salt
 * is not a secret, it is a uniqueness device, and crypto.h is explicit that
 * reusing one across two different purposes makes their derived keys
 * comparable. `out` must be exactly 16 bytes (uapp_crypto_limits_t::salt_bytes).
 */
UAPP_CAPI_API uapp_status_t uapp_crypto_generate_salt(uint8_t out_salt[16]);

/* --- device keypair handle --------------------------------------------------
 *
 * Wraps crypto::DeviceKeyPair. ONLY the public half is ever readable through
 * this handle -- see the file header's "WHAT THIS BOUNDARY DELIBERATELY DOES
 * NOT EXPOSE". The secret half is consumed internally by
 * uapp_crypto_wrap_content_key()/uapp_crypto_unwrap_content_key() and never
 * leaves the handle.
 */
typedef struct uapp_crypto_keypair uapp_crypto_keypair_t;

UAPP_CAPI_API uapp_status_t uapp_crypto_keypair_generate(uapp_crypto_keypair_t** out_keypair);
UAPP_CAPI_API void          uapp_crypto_keypair_close(uapp_crypto_keypair_t** keypair);

/* Copies the 32-byte public key out. `out_public_key` must have room for
 * exactly uapp_crypto_limits_t::public_key_bytes (32) bytes. */
UAPP_CAPI_API uapp_status_t uapp_crypto_keypair_public_key(const uapp_crypto_keypair_t* keypair,
                                                           uint8_t out_public_key[32]);

/* --- content-encryption-key handle ------------------------------------------
 *
 * Wraps crypto::ContentKey. Consumed by seal/open/wrap/unwrap; never read.
 */
typedef struct uapp_crypto_content_key uapp_crypto_content_key_t;

UAPP_CAPI_API uapp_status_t uapp_crypto_content_key_generate(
    uapp_crypto_content_key_t** out_key);

/*
 * Load a CHOSEN 32-byte key. THE ONLY WAY TO GET A SPECIFIC KEY IN, mirroring
 * crypto::ContentKey::import()'s own restriction to its two legitimate
 * callers: a future keystore adapter unwrapping a key from Keychain/Keystore,
 * and a test loading a published test vector. There is deliberately no
 * matching export/read accessor anywhere in this header -- see the file
 * header's "WHAT THIS BOUNDARY DELIBERATELY DOES NOT EXPOSE".
 */
UAPP_CAPI_API uapp_status_t uapp_crypto_content_key_import(const uint8_t key[32],
                                                            uapp_crypto_content_key_t** out_key);

UAPP_CAPI_API void uapp_crypto_content_key_close(uapp_crypto_content_key_t** key);

/*
 * Deterministic subkey derivation (HKDF-SHA256). `label` MUST be non-empty;
 * `info` may be empty for an edge with exactly one child. Both are capped at
 * uapp_crypto_limits_t::max_kdf_label_bytes / max_kdf_info_bytes.
 *
 * `out_subkey` is written ONLY on UAPP_STATUS_OK.
 */
UAPP_CAPI_API uapp_status_t uapp_crypto_content_key_derive_subkey(
    const uapp_crypto_content_key_t* parent, const void* label, size_t label_len,
    const void* info, size_t info_len, uapp_crypto_content_key_t** out_subkey);

/*
 * Argon2id derivation from a passphrase and a salt generated by
 * uapp_crypto_generate_salt(). `profile` has no default -- see the enum's own
 * comment. `salt` must be exactly 16 bytes.
 *
 * Returns UAPP_STATUS_CRYPTO_PASSPHRASE_TOO_SHORT for a passphrase under
 * uapp_crypto_limits_t::min_passphrase_bytes, UAPP_STATUS_CRYPTO_KDF_FAILED if
 * Argon2id could not allocate its working memory (expected and handleable on
 * a floor-tier device, not a reason to abort).
 */
UAPP_CAPI_API uapp_status_t uapp_crypto_content_key_derive_from_passphrase(
    const void* passphrase, size_t passphrase_len, const uint8_t salt[16],
    uapp_crypto_kdf_profile_t profile, uapp_crypto_content_key_t** out_key);

/* --- authenticated encryption ----------------------------------------------- */

/*
 * Minimum ciphertext buffer size for sealing `plaintext_len` bytes, i.e.
 * plaintext_len + uapp_crypto_limits_t::tag_bytes. Returns 0 if
 * plaintext_len exceeds max_plaintext_bytes or would overflow -- 0 ALWAYS
 * means "do not allocate this", never "zero bytes needed".
 */
UAPP_CAPI_API size_t uapp_crypto_seal_required_buffer_bytes(size_t plaintext_len);

/*
 * Seal `plaintext` under `key`, binding `aad` (may be NULL/0 for none), with
 * a fresh random nonce generated INSIDE this call -- there is no way to
 * supply, influence or reuse a nonce, by design (crypto.h's "NONCE STRATEGY").
 *
 * `out_nonce` must have room for exactly uapp_crypto_limits_t::nonce_bytes
 * (24) bytes. `out_ciphertext`/`ciphertext_capacity` must be sized with
 * uapp_crypto_seal_required_buffer_bytes() first; `out_ciphertext_len`
 * receives the number of bytes actually written (always exactly
 * plaintext_len + tag_bytes on success).
 *
 * On any failure, out_nonce/out_ciphertext/out_ciphertext_len are left
 * untouched -- never partially filled.
 *
 * Returns UAPP_STATUS_OK, UAPP_STATUS_CRYPTO_NOT_INITIALISED,
 * UAPP_STATUS_CRYPTO_KEY_INVALID, UAPP_STATUS_CRYPTO_MESSAGE_TOO_LARGE,
 * UAPP_STATUS_INVALID_ARGUMENT (null data with nonzero size, or ciphertext
 * buffer too small), UAPP_STATUS_INTERNAL.
 */
UAPP_CAPI_API uapp_status_t uapp_crypto_seal(const uapp_crypto_content_key_t* key,
                                             const void* plaintext, size_t plaintext_len,
                                             const void* aad, size_t aad_len,
                                             uint8_t out_nonce[24], void* out_ciphertext,
                                             size_t ciphertext_capacity,
                                             size_t* out_ciphertext_len);

/*
 * Open a blob sealed by uapp_crypto_seal() (or crypto::seal()). `nonce` is
 * exactly the 24 bytes uapp_crypto_seal() produced; `aad` must be the SAME
 * bytes bound at seal time -- a mismatch is CryptoAuthFailed, not a
 * different code (see "FAILURE BEHAVIOUR" in crypto.h: wrong key, flipped
 * bit, truncated ciphertext, substituted nonce and mismatched AAD are all
 * the SAME status, deliberately, so this boundary is not a decryption
 * oracle).
 *
 * `out_plaintext`/`plaintext_capacity` must be at least `ciphertext_len` -
 * tag_bytes. See this header's own "CROSSING THE SECURE-MEMORY BOUNDARY"
 * note: the bytes landing in `out_plaintext` are NOT guarded memory on this
 * side of the boundary.
 *
 * On ANY failure, out_plaintext is left untouched and out_plaintext_len is
 * set to 0 -- no partial, unauthenticated plaintext is ever handed back.
 */
UAPP_CAPI_API uapp_status_t uapp_crypto_open(const uapp_crypto_content_key_t* key,
                                             const uint8_t nonce[24], const void* ciphertext,
                                             size_t ciphertext_len, const void* aad,
                                             size_t aad_len, void* out_plaintext,
                                             size_t plaintext_capacity,
                                             size_t* out_plaintext_len);

/* --- key wrapping between two devices --------------------------------------- */

/* Fixed-size wrapped key: mirrors crypto::WrappedKey exactly, no heap. */
typedef struct uapp_crypto_wrapped_key {
    uint8_t nonce[24];
    uint8_t ciphertext[48]; /* content_key_bytes (32) + tag_bytes (16) */
} uapp_crypto_wrapped_key_t;

/*
 * Wrap `cek` so that only the holder of `recipient_public` can unwrap it,
 * authenticated as coming from `wrapper`. Returns
 * UAPP_STATUS_CRYPTO_KEY_INVALID if `recipient_public` is not a usable X25519
 * point (all-zero or low-order).
 */
UAPP_CAPI_API uapp_status_t uapp_crypto_wrap_content_key(
    const uapp_crypto_keypair_t* wrapper, const uint8_t recipient_public[32],
    const uapp_crypto_content_key_t* cek, uapp_crypto_wrapped_key_t* out_wrapped);

/*
 * Unwrap a CEK sealed by `wrapper_public` to `recipient`. A mismatch of any
 * kind -- wrong recipient, wrong wrapper, tampered bytes -- is
 * UAPP_STATUS_CRYPTO_AUTH_FAILED, the same oracle-avoidance rule as
 * uapp_crypto_open().
 *
 * `out_cek` is written ONLY on UAPP_STATUS_OK.
 */
UAPP_CAPI_API uapp_status_t uapp_crypto_unwrap_content_key(
    const uapp_crypto_keypair_t* recipient, const uint8_t wrapper_public[32],
    const uapp_crypto_wrapped_key_t* wrapped, uapp_crypto_content_key_t** out_cek);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* UAPP_CAPI_UAPP_CRYPTO_H */
