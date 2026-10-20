// SPDX-License-Identifier: Apache-2.0
//
// core/capi/src/capi_crypto.cpp — the C99 face of security/crypto.
//
// Compiled only when UAPP_CRYPTO_ENABLED (i.e. only when security/crypto
// itself is built, which requires libsodium). uapp_crypto_is_initialised()
// before init() succeeds returns UAPP_FALSE honestly; there is no separate
// "is this surface available" query the way uapp_pdf_is_available() exists,
// because unlike PDF this surface has no optional-build story of its own in
// a build that already resolved UAPP_CRYPTO_ENABLED true — it either exists
// unconditionally or the whole translation unit is absent, exactly like
// uapp_notes.h.
//
// House rules: same as capi_pdf.cpp's file header.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

#include "capi_internal.h"
#include "uapp/capi/uapp_capi.h"
#include "uapp/capi/uapp_crypto.h"
#include "uapp/crypto/crypto.h"
#include "uapp/status.h"

namespace {

using uapp::Status;
using uapp::capi::to_c_status;
using uapp::crypto::ByteView;
using uapp::crypto::ContentKey;
using uapp::crypto::DeviceKeyPair;
using uapp::crypto::KdfProfile;
using uapp::crypto::Salt;
using uapp::crypto::SealedBlob;
using uapp::crypto::WrappedKey;

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

}  // namespace

// --- handle definitions ------------------------------------------------------

struct uapp_crypto_keypair final {
    std::uint32_t  magic = uapp::capi::kKeypairMagic;
    DeviceKeyPair  pair;
};

struct uapp_crypto_content_key final {
    std::uint32_t magic = uapp::capi::kContentKeyMagic;
    ContentKey    key;
};

// ---------------------------------------------------------------------------
// Exported surface
// ---------------------------------------------------------------------------

extern "C" {

uapp_status_t uapp_crypto_get_limits(uapp_crypto_limits_t* out_limits) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_limits == nullptr || out_limits->struct_size < sizeof(uapp_crypto_limits_t)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        out_limits->public_key_bytes     = static_cast<std::uint32_t>(uapp::crypto::kPublicKeyBytes);
        out_limits->secret_key_bytes     = static_cast<std::uint32_t>(uapp::crypto::kSecretKeyBytes);
        out_limits->content_key_bytes    = static_cast<std::uint32_t>(uapp::crypto::kContentKeyBytes);
        out_limits->nonce_bytes          = static_cast<std::uint32_t>(uapp::crypto::kNonceBytes);
        out_limits->tag_bytes            = static_cast<std::uint32_t>(uapp::crypto::kTagBytes);
        out_limits->salt_bytes           = static_cast<std::uint32_t>(uapp::crypto::kSaltBytes);
        out_limits->wrapped_key_bytes    = static_cast<std::uint32_t>(uapp::crypto::kContentKeyBytes +
                                                                       uapp::crypto::kTagBytes);
        out_limits->max_plaintext_bytes  = static_cast<std::uint64_t>(uapp::crypto::kMaxPlaintextBytes);
        out_limits->max_aad_bytes        = static_cast<std::uint64_t>(uapp::crypto::kMaxAadBytes);
        out_limits->min_passphrase_bytes = static_cast<std::uint32_t>(uapp::crypto::kMinPassphraseBytes);
        out_limits->max_kdf_label_bytes  = static_cast<std::uint32_t>(uapp::crypto::kMaxKdfLabelBytes);
        out_limits->max_kdf_info_bytes   = static_cast<std::uint32_t>(uapp::crypto::kMaxKdfInfoBytes);
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_crypto_init(void) {
    UAPP_CAPI_GUARD_BEGIN
        return to_c_status(uapp::crypto::init());
    UAPP_CAPI_GUARD_END
}

uapp_bool_t uapp_crypto_is_initialised(void) {
    UAPP_CAPI_GUARD_BEGIN
        return uapp::crypto::is_initialised() ? UAPP_TRUE : UAPP_FALSE;
    UAPP_CAPI_GUARD_END_FALLBACK(UAPP_FALSE)
}

const char* uapp_crypto_library_version(void) {
    UAPP_CAPI_GUARD_BEGIN
        return uapp::crypto::library_version();
    UAPP_CAPI_GUARD_END_FALLBACK("uninitialised")
}

uapp_status_t uapp_crypto_generate_salt(uint8_t out_salt[16]) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_salt == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        Salt salt{};
        const Status status = uapp::crypto::generate_salt(salt);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        std::memcpy(out_salt, salt.data(), salt.size());
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- keypair -----------------------------------------------------------------

uapp_status_t uapp_crypto_keypair_generate(uapp_crypto_keypair_t** out_keypair) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_keypair == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        std::unique_ptr<uapp_crypto_keypair> handle(new (std::nothrow) uapp_crypto_keypair());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        const Status status = uapp::crypto::generate_device_keypair(handle->pair);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        *out_keypair = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

void uapp_crypto_keypair_close(uapp_crypto_keypair_t** keypair) {
    UAPP_CAPI_GUARD_BEGIN
        if (keypair == nullptr || *keypair == nullptr) {
            return;
        }
        if (!handle_valid(*keypair, uapp::capi::kKeypairMagic)) {
            return;
        }
        (*keypair)->magic = 0u;
        delete *keypair;
        *keypair = nullptr;
    UAPP_CAPI_GUARD_END_VOID
}

uapp_status_t uapp_crypto_keypair_public_key(const uapp_crypto_keypair_t* keypair,
                                             uint8_t out_public_key[32]) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_public_key == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(keypair, uapp::capi::kKeypairMagic)) {
            return UAPP_STATUS_CRYPTO_KEY_INVALID;
        }
        std::memcpy(out_public_key, keypair->pair.public_key.data(),
                    keypair->pair.public_key.size());
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- content key ---------------------------------------------------------

uapp_status_t uapp_crypto_content_key_generate(uapp_crypto_content_key_t** out_key) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_key == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        std::unique_ptr<uapp_crypto_content_key> handle(
            new (std::nothrow) uapp_crypto_content_key());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        const Status status = uapp::crypto::generate_content_key(handle->key);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        *out_key = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_crypto_content_key_import(const uint8_t key[32],
                                             uapp_crypto_content_key_t** out_key) {
    UAPP_CAPI_GUARD_BEGIN
        if (key == nullptr || out_key == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        std::unique_ptr<uapp_crypto_content_key> handle(
            new (std::nothrow) uapp_crypto_content_key());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        const Status status =
            handle->key.import(key, static_cast<std::size_t>(uapp::crypto::kContentKeyBytes));
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        *out_key = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

void uapp_crypto_content_key_close(uapp_crypto_content_key_t** key) {
    UAPP_CAPI_GUARD_BEGIN
        if (key == nullptr || *key == nullptr) {
            return;
        }
        if (!handle_valid(*key, uapp::capi::kContentKeyMagic)) {
            return;
        }
        (*key)->key.reset();
        (*key)->magic = 0u;
        delete *key;
        *key = nullptr;
    UAPP_CAPI_GUARD_END_VOID
}

uapp_status_t uapp_crypto_content_key_derive_subkey(const uapp_crypto_content_key_t* parent,
                                                    const void* label, size_t label_len,
                                                    const void* info, size_t info_len,
                                                    uapp_crypto_content_key_t** out_subkey) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_subkey == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(parent, uapp::capi::kContentKeyMagic)) {
            return UAPP_STATUS_CRYPTO_KEY_INVALID;
        }
        if ((label == nullptr && label_len != 0u) || (info == nullptr && info_len != 0u)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        std::unique_ptr<uapp_crypto_content_key> handle(
            new (std::nothrow) uapp_crypto_content_key());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }

        const ByteView label_view{static_cast<const std::uint8_t*>(label), label_len};
        const ByteView info_view{static_cast<const std::uint8_t*>(info), info_len};
        const Status   status =
            uapp::crypto::derive_subkey(parent->key, label_view, info_view, handle->key);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        *out_subkey = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_crypto_content_key_derive_from_passphrase(
    const void* passphrase, size_t passphrase_len, const uint8_t salt[16],
    uapp_crypto_kdf_profile_t profile, uapp_crypto_content_key_t** out_key) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_key == nullptr || salt == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (passphrase == nullptr && passphrase_len != 0u) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        std::unique_ptr<uapp_crypto_content_key> handle(
            new (std::nothrow) uapp_crypto_content_key());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }

        Salt salt_array{};
        std::memcpy(salt_array.data(), salt, salt_array.size());

        const ByteView passphrase_view{static_cast<const std::uint8_t*>(passphrase),
                                       passphrase_len};
        const Status status = uapp::crypto::derive_key_from_passphrase(
            passphrase_view, salt_array, to_cpp_profile(profile), handle->key);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        *out_key = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- authenticated encryption ---------------------------------------------

size_t uapp_crypto_seal_required_buffer_bytes(size_t plaintext_len) {
    UAPP_CAPI_GUARD_BEGIN
        if (plaintext_len > uapp::crypto::kMaxPlaintextBytes) {
            return 0u;
        }
        const size_t required = plaintext_len + uapp::crypto::kTagBytes;
        if (required < plaintext_len) {  // overflow
            return 0u;
        }
        return required;
    UAPP_CAPI_GUARD_END_FALLBACK(0u)
}

uapp_status_t uapp_crypto_seal(const uapp_crypto_content_key_t* key, const void* plaintext,
                               size_t plaintext_len, const void* aad, size_t aad_len,
                               uint8_t out_nonce[24], void* out_ciphertext,
                               size_t ciphertext_capacity, size_t* out_ciphertext_len) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_nonce == nullptr || out_ciphertext == nullptr || out_ciphertext_len == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(key, uapp::capi::kContentKeyMagic)) {
            return UAPP_STATUS_CRYPTO_KEY_INVALID;
        }
        if ((plaintext == nullptr && plaintext_len != 0u) ||
            (aad == nullptr && aad_len != 0u)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const size_t required = uapp_crypto_seal_required_buffer_bytes(plaintext_len);
        if (required == 0u && plaintext_len != 0u) {
            return UAPP_STATUS_CRYPTO_MESSAGE_TOO_LARGE;
        }
        if (ciphertext_capacity < required) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        const ByteView plaintext_view{static_cast<const std::uint8_t*>(plaintext), plaintext_len};
        const ByteView aad_view{static_cast<const std::uint8_t*>(aad), aad_len};
        SealedBlob     blob;
        const Status   status = uapp::crypto::seal(key->key, plaintext_view, aad_view, blob);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }

        std::memcpy(out_nonce, blob.nonce.data(), blob.nonce.size());
        std::memcpy(out_ciphertext, blob.ciphertext.data(), blob.ciphertext.size());
        *out_ciphertext_len = blob.ciphertext.size();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_crypto_open(const uapp_crypto_content_key_t* key, const uint8_t nonce[24],
                               const void* ciphertext, size_t ciphertext_len, const void* aad,
                               size_t aad_len, void* out_plaintext, size_t plaintext_capacity,
                               size_t* out_plaintext_len) {
    UAPP_CAPI_GUARD_BEGIN
        if (nonce == nullptr || out_plaintext_len == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        *out_plaintext_len = 0u;
        if (!handle_valid(key, uapp::capi::kContentKeyMagic)) {
            return UAPP_STATUS_CRYPTO_KEY_INVALID;
        }
        if ((ciphertext == nullptr && ciphertext_len != 0u) ||
            (aad == nullptr && aad_len != 0u)) {
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

        const ByteView       aad_view{static_cast<const std::uint8_t*>(aad), aad_len};
        uapp::crypto::SecureBytes recovered;
        const Status         status = uapp::crypto::open(key->key, blob, aad_view, recovered);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }

        // Copy out of guarded memory into the caller's plain buffer — see
        // this header's own "CROSSING THE SECURE-MEMORY BOUNDARY" note. The
        // SecureBytes destructor zeroes `recovered`'s storage regardless of
        // which path this function returns on.
        std::memcpy(out_plaintext, recovered.data(), recovered.size());
        *out_plaintext_len = recovered.size();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- key wrapping between devices -------------------------------------------

uapp_status_t uapp_crypto_wrap_content_key(const uapp_crypto_keypair_t* wrapper,
                                           const uint8_t recipient_public[32],
                                           const uapp_crypto_content_key_t* cek,
                                           uapp_crypto_wrapped_key_t* out_wrapped) {
    UAPP_CAPI_GUARD_BEGIN
        if (recipient_public == nullptr || out_wrapped == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(wrapper, uapp::capi::kKeypairMagic) ||
            !handle_valid(cek, uapp::capi::kContentKeyMagic)) {
            return UAPP_STATUS_CRYPTO_KEY_INVALID;
        }

        uapp::crypto::PublicKey recipient{};
        std::memcpy(recipient.data(), recipient_public, recipient.size());

        WrappedKey   wrapped;
        const Status status =
            uapp::crypto::wrap_content_key(wrapper->pair, recipient, cek->key, wrapped);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }

        std::memcpy(out_wrapped->nonce, wrapped.nonce.data(), wrapped.nonce.size());
        std::memcpy(out_wrapped->ciphertext, wrapped.ciphertext.data(),
                    wrapped.ciphertext.size());
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_crypto_unwrap_content_key(const uapp_crypto_keypair_t* recipient,
                                             const uint8_t wrapper_public[32],
                                             const uapp_crypto_wrapped_key_t* wrapped,
                                             uapp_crypto_content_key_t** out_cek) {
    UAPP_CAPI_GUARD_BEGIN
        if (wrapper_public == nullptr || wrapped == nullptr || out_cek == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(recipient, uapp::capi::kKeypairMagic)) {
            return UAPP_STATUS_CRYPTO_KEY_INVALID;
        }

        uapp::crypto::PublicKey wrapper{};
        std::memcpy(wrapper.data(), wrapper_public, wrapper.size());

        WrappedKey cpp_wrapped;
        std::memcpy(cpp_wrapped.nonce.data(), wrapped->nonce, cpp_wrapped.nonce.size());
        std::memcpy(cpp_wrapped.ciphertext.data(), wrapped->ciphertext,
                    cpp_wrapped.ciphertext.size());

        std::unique_ptr<uapp_crypto_content_key> handle(
            new (std::nothrow) uapp_crypto_content_key());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }

        const Status status = uapp::crypto::unwrap_content_key(recipient->pair, wrapper,
                                                                cpp_wrapped, handle->key);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        *out_cek = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

}  // extern "C"
