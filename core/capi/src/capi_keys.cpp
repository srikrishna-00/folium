// SPDX-License-Identifier: Apache-2.0
//
// core/capi/src/capi_keys.cpp — the C99 face of security/keys.
//
// Compiled only when UAPP_CRYPTO_ENABLED (security/keys links
// security/crypto, which needs libsodium). House rules: same as
// capi_crypto.cpp's file header.
//
// NOTE ON THE CRYPTO HANDLE TYPES THIS FILE REACHES INTO: uapp_crypto_keypair
// and uapp_crypto_content_key are DEFINED in capi_crypto.cpp, not declared in
// any shared private header, because nothing outside capi_crypto.cpp has
// needed their layout until now. This file forward-declares them identically
// rather than duplicating capi_crypto.cpp's struct — see the definitions
// below, kept byte-for-byte in sync with capi_crypto.cpp by comment
// cross-reference. If a third file ever needs them, THAT is the signal to
// move both definitions into a shared capi_crypto_internal.h; two files
// agreeing by comment is a judgement call documented here, not an oversight.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

#include "capi_internal.h"
#include "uapp/capi/uapp_capi.h"
#include "uapp/capi/uapp_crypto.h"
#include "uapp/capi/uapp_keys.h"
#include "uapp/crypto/crypto.h"
#include "uapp/keys/key_hierarchy.h"
#include "uapp/status.h"

// Mirrors capi_crypto.cpp's definitions exactly — see this file's header
// comment for why these are duplicated rather than shared.
struct uapp_crypto_keypair final {
    std::uint32_t                magic = uapp::capi::kKeypairMagic;
    uapp::crypto::DeviceKeyPair  pair;
};

struct uapp_crypto_content_key final {
    std::uint32_t          magic = uapp::capi::kContentKeyMagic;
    uapp::crypto::ContentKey key;
};

namespace {

using uapp::Status;
using uapp::capi::to_c_status;
using uapp::crypto::ByteView;
using uapp::crypto::KdfProfile;
using uapp::crypto::Nonce;
using uapp::crypto::PublicKey;
using uapp::crypto::SealedBlob;
using uapp::crypto::WrappedKey;
using uapp::keys::DeviceWrappedUrkRecord;
using uapp::keys::DocumentKey;
using uapp::keys::KeyRole;
using uapp::keys::PassphraseWrappedUrkRecord;
using uapp::keys::RecoverySecret;
using uapp::keys::UrkWrappedWorkspaceKeyRecord;
using uapp::keys::UserRootKey;
using uapp::keys::WorkspaceKey;
using uapp::keys::WrapKind;

template <typename Handle>
[[nodiscard]] bool handle_valid(const Handle* handle, std::uint32_t expected) noexcept {
    return handle != nullptr && handle->magic == expected;
}

[[nodiscard]] KdfProfile to_cpp_profile(uapp_crypto_kdf_profile_t p) noexcept {
    switch (p) {
        case UAPP_CRYPTO_KDF_PROFILE_DESKTOP_MODERATE:   return KdfProfile::DesktopModerate;
        case UAPP_CRYPTO_KDF_PROFILE_ARCHIVAL_SENSITIVE: return KdfProfile::ArchivalSensitive;
        case UAPP_CRYPTO_KDF_PROFILE_MOBILE_INTERACTIVE:
        case UAPP_CRYPTO_KDF_PROFILE_FORCE_INT32:
        default:
            return KdfProfile::MobileInteractive;
    }
}

void to_cpp_wrapped(const std::uint8_t nonce[24], const std::uint8_t ciphertext[48],
                    WrappedKey& out) noexcept {
    std::memcpy(out.nonce.data(), nonce, out.nonce.size());
    std::memcpy(out.ciphertext.data(), ciphertext, out.ciphertext.size());
}

void from_cpp_wrapped(const WrappedKey& in, std::uint8_t out_nonce[24],
                      std::uint8_t out_ciphertext[48]) noexcept {
    std::memcpy(out_nonce, in.nonce.data(), in.nonce.size());
    std::memcpy(out_ciphertext, in.ciphertext.data(), in.ciphertext.size());
}

}  // namespace

// --- handle definitions ------------------------------------------------------

struct uapp_keys_recovery_secret final {
    std::uint32_t   magic = uapp::capi::kRecoverySecretMagic;
    RecoverySecret  secret;
};

struct uapp_keys_urk final {
    std::uint32_t magic = uapp::capi::kUserRootKeyMagic;
    UserRootKey   urk;
};

struct uapp_keys_wsk final {
    std::uint32_t magic = uapp::capi::kWorkspaceKeyMagic;
    WorkspaceKey  wsk;
};

struct uapp_keys_document_key final {
    std::uint32_t magic = uapp::capi::kDocumentKeyMagic;
    DocumentKey   key;
};

// ---------------------------------------------------------------------------
// Exported surface
// ---------------------------------------------------------------------------

extern "C" {

uapp_status_t uapp_keys_get_limits(uapp_keys_limits_t* out_limits) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_limits == nullptr || out_limits->struct_size < sizeof(uapp_keys_limits_t)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        out_limits->recovery_secret_bytes =
            static_cast<std::uint32_t>(uapp::keys::kRecoverySecretBytes);
        out_limits->wrapped_key_bytes = static_cast<std::uint32_t>(uapp::keys::kWrappedKeyBytes);
        out_limits->max_scope_id_bytes =
            static_cast<std::uint32_t>(uapp::keys::kMaxScopeIdBytes);
        out_limits->record_format_version = uapp::keys::kRecordFormatVersion;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_keys_generate_recovery_secret(uapp_keys_recovery_secret_t** out_secret) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_secret == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        std::unique_ptr<uapp_keys_recovery_secret> handle(
            new (std::nothrow) uapp_keys_recovery_secret());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        const Status status = uapp::keys::generate_recovery_secret(handle->secret);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        *out_secret = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

void uapp_keys_recovery_secret_close(uapp_keys_recovery_secret_t** secret) {
    UAPP_CAPI_GUARD_BEGIN
        if (secret == nullptr || *secret == nullptr) { return; }
        if (!handle_valid(*secret, uapp::capi::kRecoverySecretMagic)) { return; }
        (*secret)->magic = 0u;
        delete *secret;
        *secret = nullptr;
    UAPP_CAPI_GUARD_END_VOID
}

void uapp_keys_urk_close(uapp_keys_urk_t** urk) {
    UAPP_CAPI_GUARD_BEGIN
        if (urk == nullptr || *urk == nullptr) { return; }
        if (!handle_valid(*urk, uapp::capi::kUserRootKeyMagic)) { return; }
        (*urk)->magic = 0u;
        delete *urk;
        *urk = nullptr;
    UAPP_CAPI_GUARD_END_VOID
}

void uapp_keys_wsk_close(uapp_keys_wsk_t** wsk) {
    UAPP_CAPI_GUARD_BEGIN
        if (wsk == nullptr || *wsk == nullptr) { return; }
        if (!handle_valid(*wsk, uapp::capi::kWorkspaceKeyMagic)) { return; }
        (*wsk)->magic = 0u;
        delete *wsk;
        *wsk = nullptr;
    UAPP_CAPI_GUARD_END_VOID
}

void uapp_keys_document_key_close(uapp_keys_document_key_t** key) {
    UAPP_CAPI_GUARD_BEGIN
        if (key == nullptr || *key == nullptr) { return; }
        if (!handle_valid(*key, uapp::capi::kDocumentKeyMagic)) { return; }
        (*key)->magic = 0u;
        delete *key;
        *key = nullptr;
    UAPP_CAPI_GUARD_END_VOID
}

uapp_status_t uapp_keys_derive_user_root_key(const uapp_keys_recovery_secret_t* secret,
                                             uapp_keys_urk_t** out_urk) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_urk == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(secret, uapp::capi::kRecoverySecretMagic)) {
            return UAPP_STATUS_CRYPTO_KEY_INVALID;
        }
        std::unique_ptr<uapp_keys_urk> handle(new (std::nothrow) uapp_keys_urk());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        const Status status = uapp::keys::derive_user_root_key(secret->secret, handle->urk);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        *out_urk = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_keys_generate_workspace_key(uapp_keys_wsk_t** out_wsk) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_wsk == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        std::unique_ptr<uapp_keys_wsk> handle(new (std::nothrow) uapp_keys_wsk());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        const Status status = uapp::keys::generate_workspace_key(handle->wsk);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        *out_wsk = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- URK <-> device bundles --------------------------------------------------

uapp_status_t uapp_keys_wrap_urk_for_device(const uapp_crypto_keypair_t* wrapper,
                                            const uint8_t recipient_public[32],
                                            const uapp_keys_urk_t* urk,
                                            uapp_keys_device_wrapped_urk_record_t* out_record) {
    UAPP_CAPI_GUARD_BEGIN
        if (recipient_public == nullptr || out_record == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(wrapper, uapp::capi::kKeypairMagic) ||
            !handle_valid(urk, uapp::capi::kUserRootKeyMagic)) {
            return UAPP_STATUS_CRYPTO_KEY_INVALID;
        }

        PublicKey recipient{};
        std::memcpy(recipient.data(), recipient_public, recipient.size());

        DeviceWrappedUrkRecord record;
        const Status status =
            uapp::keys::wrap_urk_for_device(wrapper->pair, recipient, urk->urk, record);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }

        out_record->format_version = record.format_version;
        out_record->role           = static_cast<std::uint8_t>(record.role);
        out_record->wrap_kind      = static_cast<std::uint8_t>(record.wrap_kind);
        out_record->_reserved      = 0u;
        std::memcpy(out_record->wrapper_public, record.wrapper_public.data(),
                    record.wrapper_public.size());
        std::memcpy(out_record->recipient_public, record.recipient_public.data(),
                    record.recipient_public.size());
        from_cpp_wrapped(record.wrapped, out_record->wrapped_nonce, out_record->wrapped_ciphertext);
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_keys_unwrap_urk_from_device(
    const uapp_crypto_keypair_t* recipient, const uint8_t expected_wrapper_public[32],
    const uapp_keys_device_wrapped_urk_record_t* record, uapp_keys_urk_t** out_urk) {
    UAPP_CAPI_GUARD_BEGIN
        if (expected_wrapper_public == nullptr || record == nullptr || out_urk == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(recipient, uapp::capi::kKeypairMagic)) {
            return UAPP_STATUS_CRYPTO_KEY_INVALID;
        }

        PublicKey expected{};
        std::memcpy(expected.data(), expected_wrapper_public, expected.size());

        DeviceWrappedUrkRecord cpp_record;
        cpp_record.format_version = record->format_version;
        cpp_record.role           = static_cast<KeyRole>(record->role);
        cpp_record.wrap_kind      = static_cast<WrapKind>(record->wrap_kind);
        std::memcpy(cpp_record.wrapper_public.data(), record->wrapper_public,
                    cpp_record.wrapper_public.size());
        std::memcpy(cpp_record.recipient_public.data(), record->recipient_public,
                    cpp_record.recipient_public.size());
        to_cpp_wrapped(record->wrapped_nonce, record->wrapped_ciphertext, cpp_record.wrapped);

        std::unique_ptr<uapp_keys_urk> handle(new (std::nothrow) uapp_keys_urk());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        const Status status = uapp::keys::unwrap_urk_from_device(recipient->pair, expected,
                                                                  cpp_record, handle->urk);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        *out_urk = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- URK under a recovery passphrase -----------------------------------------

uapp_status_t uapp_keys_wrap_urk_under_passphrase(
    const void* passphrase, size_t passphrase_len, uapp_crypto_kdf_profile_t profile,
    const uapp_keys_urk_t* urk, uapp_keys_passphrase_wrapped_urk_record_t* out_record) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_record == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (passphrase == nullptr && passphrase_len != 0u) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(urk, uapp::capi::kUserRootKeyMagic)) {
            return UAPP_STATUS_CRYPTO_KEY_INVALID;
        }

        const ByteView passphrase_view{static_cast<const std::uint8_t*>(passphrase),
                                       passphrase_len};
        PassphraseWrappedUrkRecord record;
        const Status status = uapp::keys::wrap_urk_under_passphrase(
            passphrase_view, to_cpp_profile(profile), urk->urk, record);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }

        out_record->format_version = record.format_version;
        out_record->role           = static_cast<std::uint8_t>(record.role);
        out_record->wrap_kind      = static_cast<std::uint8_t>(record.wrap_kind);
        out_record->_reserved      = 0u;
        std::memcpy(out_record->salt, record.salt.data(), record.salt.size());
        out_record->kdf_profile = static_cast<std::int32_t>(record.kdf_profile);
        std::memcpy(out_record->nonce, record.nonce.data(), record.nonce.size());
        std::memcpy(out_record->ciphertext, record.ciphertext.data(), record.ciphertext.size());
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_keys_unwrap_urk_with_passphrase(
    const void* passphrase, size_t passphrase_len,
    const uapp_keys_passphrase_wrapped_urk_record_t* record, uapp_keys_urk_t** out_urk) {
    UAPP_CAPI_GUARD_BEGIN
        if (record == nullptr || out_urk == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (passphrase == nullptr && passphrase_len != 0u) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        PassphraseWrappedUrkRecord cpp_record;
        cpp_record.format_version = record->format_version;
        cpp_record.role           = static_cast<KeyRole>(record->role);
        cpp_record.wrap_kind      = static_cast<WrapKind>(record->wrap_kind);
        std::memcpy(cpp_record.salt.data(), record->salt, cpp_record.salt.size());
        cpp_record.kdf_profile = to_cpp_profile(
            static_cast<uapp_crypto_kdf_profile_t>(record->kdf_profile));
        std::memcpy(cpp_record.nonce.data(), record->nonce, cpp_record.nonce.size());
        std::memcpy(cpp_record.ciphertext.data(), record->ciphertext,
                    cpp_record.ciphertext.size());

        const ByteView passphrase_view{static_cast<const std::uint8_t*>(passphrase),
                                       passphrase_len};
        std::unique_ptr<uapp_keys_urk> handle(new (std::nothrow) uapp_keys_urk());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        const Status status =
            uapp::keys::unwrap_urk_with_passphrase(passphrase_view, cpp_record, handle->urk);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        *out_urk = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- workspace keys under the URK -------------------------------------------

uapp_status_t uapp_keys_wrap_workspace_key(
    const uapp_keys_urk_t* urk, const void* workspace_id, size_t workspace_id_len,
    const uapp_keys_wsk_t* wsk, uapp_keys_urk_wrapped_workspace_key_record_t* out_record) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_record == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (workspace_id == nullptr && workspace_id_len != 0u) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(urk, uapp::capi::kUserRootKeyMagic) ||
            !handle_valid(wsk, uapp::capi::kWorkspaceKeyMagic)) {
            return UAPP_STATUS_CRYPTO_KEY_INVALID;
        }

        const ByteView id_view{static_cast<const std::uint8_t*>(workspace_id), workspace_id_len};
        UrkWrappedWorkspaceKeyRecord record;
        const Status status =
            uapp::keys::wrap_workspace_key(urk->urk, id_view, wsk->wsk, record);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }

        out_record->format_version = record.format_version;
        out_record->role           = static_cast<std::uint8_t>(record.role);
        out_record->wrap_kind      = static_cast<std::uint8_t>(record.wrap_kind);
        out_record->_reserved      = 0u;
        std::memcpy(out_record->nonce, record.nonce.data(), record.nonce.size());
        std::memcpy(out_record->ciphertext, record.ciphertext.data(), record.ciphertext.size());
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_keys_unwrap_workspace_key(
    const uapp_keys_urk_t* urk, const void* workspace_id, size_t workspace_id_len,
    const uapp_keys_urk_wrapped_workspace_key_record_t* record, uapp_keys_wsk_t** out_wsk) {
    UAPP_CAPI_GUARD_BEGIN
        if (record == nullptr || out_wsk == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (workspace_id == nullptr && workspace_id_len != 0u) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(urk, uapp::capi::kUserRootKeyMagic)) {
            return UAPP_STATUS_CRYPTO_KEY_INVALID;
        }

        UrkWrappedWorkspaceKeyRecord cpp_record;
        cpp_record.format_version = record->format_version;
        cpp_record.role           = static_cast<KeyRole>(record->role);
        cpp_record.wrap_kind      = static_cast<WrapKind>(record->wrap_kind);
        std::memcpy(cpp_record.nonce.data(), record->nonce, cpp_record.nonce.size());
        std::memcpy(cpp_record.ciphertext.data(), record->ciphertext,
                    cpp_record.ciphertext.size());

        const ByteView id_view{static_cast<const std::uint8_t*>(workspace_id), workspace_id_len};
        std::unique_ptr<uapp_keys_wsk> handle(new (std::nothrow) uapp_keys_wsk());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        const Status status =
            uapp::keys::unwrap_workspace_key(urk->urk, id_view, cpp_record, handle->wsk);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        *out_wsk = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- document keys -----------------------------------------------------------

uapp_status_t uapp_keys_derive_document_key(const uapp_keys_wsk_t* wsk, const void* document_id,
                                            size_t document_id_len,
                                            uapp_keys_document_key_t** out_key) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_key == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (document_id == nullptr && document_id_len != 0u) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(wsk, uapp::capi::kWorkspaceKeyMagic)) {
            return UAPP_STATUS_CRYPTO_KEY_INVALID;
        }

        const ByteView id_view{static_cast<const std::uint8_t*>(document_id), document_id_len};
        std::unique_ptr<uapp_keys_document_key> handle(
            new (std::nothrow) uapp_keys_document_key());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        const Status status = uapp::keys::derive_document_key(wsk->wsk, id_view, handle->key);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        *out_key = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- content, at the bottom of the chain -------------------------------------

size_t uapp_keys_seal_document_content_required_buffer_bytes(size_t plaintext_len) {
    UAPP_CAPI_GUARD_BEGIN
        return uapp_crypto_seal_required_buffer_bytes(plaintext_len);
    UAPP_CAPI_GUARD_END_FALLBACK(0u)
}

uapp_status_t uapp_keys_seal_document_content(const uapp_keys_document_key_t* key,
                                              const void* document_id, size_t document_id_len,
                                              const void* plaintext, size_t plaintext_len,
                                              uint8_t out_nonce[24], void* out_ciphertext,
                                              size_t ciphertext_capacity,
                                              size_t* out_ciphertext_len) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_nonce == nullptr || out_ciphertext == nullptr || out_ciphertext_len == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(key, uapp::capi::kDocumentKeyMagic)) {
            return UAPP_STATUS_CRYPTO_KEY_INVALID;
        }
        if ((document_id == nullptr && document_id_len != 0u) ||
            (plaintext == nullptr && plaintext_len != 0u)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const size_t required = uapp_crypto_seal_required_buffer_bytes(plaintext_len);
        if (required == 0u && plaintext_len != 0u) {
            return UAPP_STATUS_CRYPTO_MESSAGE_TOO_LARGE;
        }
        if (ciphertext_capacity < required) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        const ByteView id_view{static_cast<const std::uint8_t*>(document_id), document_id_len};
        const ByteView plaintext_view{static_cast<const std::uint8_t*>(plaintext), plaintext_len};
        SealedBlob     blob;
        const Status   status =
            uapp::keys::seal_document_content(key->key, id_view, plaintext_view, blob);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }

        std::memcpy(out_nonce, blob.nonce.data(), blob.nonce.size());
        std::memcpy(out_ciphertext, blob.ciphertext.data(), blob.ciphertext.size());
        *out_ciphertext_len = blob.ciphertext.size();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_keys_open_document_content(const uapp_keys_document_key_t* key,
                                              const void* document_id, size_t document_id_len,
                                              const uint8_t nonce[24], const void* ciphertext,
                                              size_t ciphertext_len, void* out_plaintext,
                                              size_t plaintext_capacity,
                                              size_t* out_plaintext_len) {
    UAPP_CAPI_GUARD_BEGIN
        if (nonce == nullptr || out_plaintext_len == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        *out_plaintext_len = 0u;
        if (!handle_valid(key, uapp::capi::kDocumentKeyMagic)) {
            return UAPP_STATUS_CRYPTO_KEY_INVALID;
        }
        if ((document_id == nullptr && document_id_len != 0u) ||
            (ciphertext == nullptr && ciphertext_len != 0u)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (ciphertext_len < uapp::crypto::kTagBytes) {
            return UAPP_STATUS_CRYPTO_SEALED_BLOB_BAD;
        }
        if (plaintext_capacity < ciphertext_len - uapp::crypto::kTagBytes) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        SealedBlob blob;
        std::memcpy(blob.nonce.data(), nonce, blob.nonce.size());
        blob.ciphertext.assign(static_cast<const std::uint8_t*>(ciphertext),
                               static_cast<const std::uint8_t*>(ciphertext) + ciphertext_len);

        const ByteView       id_view{static_cast<const std::uint8_t*>(document_id),
                                     document_id_len};
        uapp::crypto::SecureBytes recovered;
        const Status         status =
            uapp::keys::open_document_content(key->key, id_view, blob, recovered);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }

        std::memcpy(out_plaintext, recovered.data(), recovered.size());
        *out_plaintext_len = recovered.size();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

}  // extern "C"
