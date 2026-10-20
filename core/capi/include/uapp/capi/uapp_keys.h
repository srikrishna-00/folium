/* SPDX-License-Identifier: Apache-2.0 */
/*
 * uapp/capi/uapp_keys.h -- the key HIERARCHY surface of libuapp_capi, over
 * security/keys/include/uapp/keys/key_hierarchy.h.
 *
 * Same ABI 0.4 bump as uapp_crypto.h, same deferred-until-the-hierarchy-
 * exists reasoning -- see uapp_crypto.h's header and
 * key_hierarchy.h's own NOT IMPLEMENTED item 8.
 *
 * ===========================================================================
 * FOUR DISTINCT HANDLE TYPES, NOT ONE GENERIC KEY HANDLE
 * ===========================================================================
 * key_hierarchy.h keeps RecoverySecret, UserRootKey, WorkspaceKey and
 * DocumentKey as four distinct C++ types over one carrier, specifically
 * because "the C++ type system is the only thing that stops a WSK being
 * passed where a URK is expected" -- a mistake that would "work" (all four
 * are 32-byte AEAD keys) and would silently produce wrong document keys for
 * an entire workspace. A C ABI has NO type system at all, so this property
 * matters MORE here, not less: four distinct opaque handle types, four
 * distinct magic cookies, so passing a WSK handle to a function expecting a
 * URK handle is a checked runtime error (UAPP_STATUS_CRYPTO_KEY_INVALID)
 * instead of silent corruption.
 *
 * ===========================================================================
 * DEVICE KEYPAIRS ARE uapp_crypto_keypair_t, REUSED, NOT REDEFINED
 * ===========================================================================
 * wrap_urk_for_device()/unwrap_urk_from_device() take the SAME
 * crypto::DeviceKeyPair uapp_crypto.h's wrap/unwrap-content-key functions do,
 * so this header reuses uapp_crypto_keypair_t rather than minting a second,
 * incompatible device-key handle type. Include uapp_crypto.h before this
 * header.
 *
 * ERRORS, OWNERSHIP, THREADING: uapp_capi.h's rules, unchanged. Record
 * structs below are FLAT, FIXED-SIZE, no heap -- mirroring key_hierarchy.h's
 * own "no heap allocation" design for the same reason: these are meant to be
 * written byte-for-byte into a caller's own storage.
 */

#ifndef UAPP_CAPI_UAPP_KEYS_H
#define UAPP_CAPI_UAPP_KEYS_H

#include <stddef.h>
#include <stdint.h>

#include "uapp/capi/uapp_capi.h"
#include "uapp/capi/uapp_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- sizes, read at runtime -------------------------------------------- */

typedef struct uapp_keys_limits {
    uint32_t struct_size;          /* in: sizeof(uapp_keys_limits_t) */
    uint32_t recovery_secret_bytes; /* out: == 32 */
    uint32_t wrapped_key_bytes;      /* out: == 48 */
    uint32_t max_scope_id_bytes;     /* out: longest workspace/document id */
    uint8_t  record_format_version;  /* out: the only version this build writes/reads */
} uapp_keys_limits_t;

UAPP_CAPI_API uapp_status_t uapp_keys_get_limits(uapp_keys_limits_t* out_limits);

/* --- plaintext record metadata, mirrors key_hierarchy.h's KeyRole/WrapKind.
 *
 * Plaintext on every record, checked against the caller's expectation on
 * every unwrap, and TRUSTED FOR NOTHING -- same rule as the C++ layer. */
typedef enum uapp_keys_role {
    UAPP_KEYS_ROLE_USER_ROOT  = 1,
    UAPP_KEYS_ROLE_WORKSPACE  = 2,

    UAPP_KEYS_ROLE_FORCE_INT32 = 0x7FFFFFFF
} uapp_keys_role_t;

typedef enum uapp_keys_wrap_kind {
    UAPP_KEYS_WRAP_KIND_DEVICE_WRAPPING_KEY  = 1,
    UAPP_KEYS_WRAP_KIND_RECOVERY_PASSPHRASE  = 2,
    UAPP_KEYS_WRAP_KIND_USER_ROOT_KEY        = 3,

    UAPP_KEYS_WRAP_KIND_FORCE_INT32 = 0x7FFFFFFF
} uapp_keys_wrap_kind_t;

/* --- the four handle types ------------------------------------------------- */

typedef struct uapp_keys_recovery_secret uapp_keys_recovery_secret_t;
typedef struct uapp_keys_urk             uapp_keys_urk_t;
typedef struct uapp_keys_wsk             uapp_keys_wsk_t;
typedef struct uapp_keys_document_key    uapp_keys_document_key_t;

UAPP_CAPI_API uapp_status_t uapp_keys_generate_recovery_secret(
    uapp_keys_recovery_secret_t** out_secret);
UAPP_CAPI_API void uapp_keys_recovery_secret_close(uapp_keys_recovery_secret_t** secret);

UAPP_CAPI_API void uapp_keys_urk_close(uapp_keys_urk_t** urk);
UAPP_CAPI_API void uapp_keys_wsk_close(uapp_keys_wsk_t** wsk);
UAPP_CAPI_API void uapp_keys_document_key_close(uapp_keys_document_key_t** key);

/*
 * Derive the URK from the recovery secret. DETERMINISTIC -- this is the
 * recovery path: a new device with the 24-word phrase re-derives the URK
 * and then unwraps workspace keys from server-stored records, with no other
 * device present.
 */
UAPP_CAPI_API uapp_status_t uapp_keys_derive_user_root_key(
    const uapp_keys_recovery_secret_t* secret, uapp_keys_urk_t** out_urk);

/* A fresh workspace key. GENERATED, not derived, so §5A.3 rotation can
 * replace it without rotating the URK (and therefore the recovery phrase). */
UAPP_CAPI_API uapp_status_t uapp_keys_generate_workspace_key(uapp_keys_wsk_t** out_wsk);

/* --- URK <-> device bundles ------------------------------------------------- */

typedef struct uapp_keys_device_wrapped_urk_record {
    uint8_t  format_version;        /* set to uapp_keys_limits_t::record_format_version to write */
    uint8_t  role;                  /* uapp_keys_role_t, always USER_ROOT */
    uint8_t  wrap_kind;             /* uapp_keys_wrap_kind_t, always DEVICE_WRAPPING_KEY */
    uint8_t  _reserved;             /* explicit padding; always 0 */
    uint8_t  wrapper_public[32];    /* the enrolled device that sealed it */
    uint8_t  recipient_public[32];  /* the device that can open it */
    uint8_t  wrapped_nonce[24];
    uint8_t  wrapped_ciphertext[48];
} uapp_keys_device_wrapped_urk_record_t;

/*
 * Wrap `urk` so only `recipient_public`'s holder can unwrap it, authenticated
 * as coming from `wrapper`. Call ONLY after an out-of-band short-code
 * comparison has established that `recipient_public` really belongs to the
 * device in front of the user -- this function has no way to check that
 * itself (see key_hierarchy.h NOT IMPLEMENTED item 3).
 */
UAPP_CAPI_API uapp_status_t uapp_keys_wrap_urk_for_device(
    const uapp_crypto_keypair_t* wrapper, const uint8_t recipient_public[32],
    const uapp_keys_urk_t* urk, uapp_keys_device_wrapped_urk_record_t* out_record);

/*
 * Unwrap a URK from a device bundle. `expected_wrapper_public` MUST come
 * from the caller's own trusted device list, never from the record: a
 * record whose own `wrapper_public` field disagrees with it fails before any
 * key material is touched. Every form of "this did not authenticate" is
 * UAPP_STATUS_CRYPTO_AUTH_FAILED (never a more specific code), matching
 * uapp_crypto_open()'s oracle-avoidance rule.
 *
 * Returns UAPP_STATUS_KEY_RECORD_VERSION_UNSUPPORTED,
 * UAPP_STATUS_KEY_RECORD_ROLE_MISMATCH (both checked first, over plaintext
 * metadata), UAPP_STATUS_CRYPTO_KEY_INVALID, or
 * UAPP_STATUS_CRYPTO_AUTH_FAILED.
 */
UAPP_CAPI_API uapp_status_t uapp_keys_unwrap_urk_from_device(
    const uapp_crypto_keypair_t* recipient, const uint8_t expected_wrapper_public[32],
    const uapp_keys_device_wrapped_urk_record_t* record, uapp_keys_urk_t** out_urk);

/* --- URK under a recovery passphrase ---------------------------------------
 *
 * FLAGGED FOR PRODUCT REVIEW in key_hierarchy.h: a passphrase-wrapped URK is
 * strictly weaker than the 24-word phrase and must not be presented to a
 * user as an equivalent recovery method. Implemented here because the
 * record format must exist before the data model freezes, not because that
 * policy question is settled -- see this header's own NOT clause in
 * uapp_keys.h's parent comment.
 */

typedef struct uapp_keys_passphrase_wrapped_urk_record {
    uint8_t  format_version;
    uint8_t  role;        /* always USER_ROOT */
    uint8_t  wrap_kind;   /* always RECOVERY_PASSPHRASE */
    uint8_t  _reserved;
    uint8_t  salt[16];
    int32_t  kdf_profile; /* uapp_crypto_kdf_profile_t -- part of the key's identity, stored */
    uint8_t  nonce[24];
    uint8_t  ciphertext[48];
} uapp_keys_passphrase_wrapped_urk_record_t;

/* Wraps `urk` under an Argon2id-derived key, generating a fresh salt
 * internally (the record carries it; the caller never generates one). */
UAPP_CAPI_API uapp_status_t uapp_keys_wrap_urk_under_passphrase(
    const void* passphrase, size_t passphrase_len, uapp_crypto_kdf_profile_t profile,
    const uapp_keys_urk_t* urk, uapp_keys_passphrase_wrapped_urk_record_t* out_record);

/* A wrong passphrase is UAPP_STATUS_CRYPTO_AUTH_FAILED -- the same code as a
 * tampered record, deliberately (same oracle rule as everywhere else in this
 * surface). */
UAPP_CAPI_API uapp_status_t uapp_keys_unwrap_urk_with_passphrase(
    const void* passphrase, size_t passphrase_len,
    const uapp_keys_passphrase_wrapped_urk_record_t* record, uapp_keys_urk_t** out_urk);

/* --- workspace keys under the URK ------------------------------------------- */

typedef struct uapp_keys_urk_wrapped_workspace_key_record {
    uint8_t  format_version;
    uint8_t  role;        /* always WORKSPACE */
    uint8_t  wrap_kind;   /* always USER_ROOT_KEY */
    uint8_t  _reserved;
    uint8_t  nonce[24];
    uint8_t  ciphertext[48];
} uapp_keys_urk_wrapped_workspace_key_record_t;

/* `workspace_id` is bound into the derivation AND the record's AAD; it is
 * NOT stored in the record -- the caller owns the mapping from id to
 * record, same as the C++ layer requires. */
UAPP_CAPI_API uapp_status_t uapp_keys_wrap_workspace_key(
    const uapp_keys_urk_t* urk, const void* workspace_id, size_t workspace_id_len,
    const uapp_keys_wsk_t* wsk, uapp_keys_urk_wrapped_workspace_key_record_t* out_record);

/* Passing a different `workspace_id` than the wrap used is
 * UAPP_STATUS_CRYPTO_AUTH_FAILED, not a "wrong key" code -- the id is in the
 * AAD, so a mismatch looks exactly like tampering, which it functionally
 * is. */
UAPP_CAPI_API uapp_status_t uapp_keys_unwrap_workspace_key(
    const uapp_keys_urk_t* urk, const void* workspace_id, size_t workspace_id_len,
    const uapp_keys_urk_wrapped_workspace_key_record_t* record, uapp_keys_wsk_t** out_wsk);

/* --- document keys: derived, never stored ----------------------------------- */

/* Derives `document_id`'s key from `wsk`. DETERMINISTIC and storage-free --
 * two devices holding the WSK compute byte-identical document keys with no
 * exchange. An empty `document_id` is rejected
 * (UAPP_STATUS_KEY_SCOPE_ID_INVALID) rather than normalised: it would give
 * every document in the workspace the same key. */
UAPP_CAPI_API uapp_status_t uapp_keys_derive_document_key(
    const uapp_keys_wsk_t* wsk, const void* document_id, size_t document_id_len,
    uapp_keys_document_key_t** out_key);

/* --- content, at the bottom of the chain ------------------------------------ */

/* Same sizing rule as uapp_crypto_seal_required_buffer_bytes() -- the AEAD
 * construction underneath is identical, this just binds document_id into
 * the AAD on the caller's behalf so no call site can forget to. */
UAPP_CAPI_API size_t uapp_keys_seal_document_content_required_buffer_bytes(
    size_t plaintext_len);

UAPP_CAPI_API uapp_status_t uapp_keys_seal_document_content(
    const uapp_keys_document_key_t* key, const void* document_id, size_t document_id_len,
    const void* plaintext, size_t plaintext_len, uint8_t out_nonce[24], void* out_ciphertext,
    size_t ciphertext_capacity, size_t* out_ciphertext_len);

/* Requires the SAME `document_id` used to seal -- a different one is
 * UAPP_STATUS_CRYPTO_AUTH_FAILED, same reasoning as unwrap_workspace_key()
 * above. See uapp_crypto.h's "CROSSING THE SECURE-MEMORY BOUNDARY" note:
 * applies here identically. */
UAPP_CAPI_API uapp_status_t uapp_keys_open_document_content(
    const uapp_keys_document_key_t* key, const void* document_id, size_t document_id_len,
    const uint8_t nonce[24], const void* ciphertext, size_t ciphertext_len,
    void* out_plaintext, size_t plaintext_capacity, size_t* out_plaintext_len);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* UAPP_CAPI_UAPP_KEYS_H */
