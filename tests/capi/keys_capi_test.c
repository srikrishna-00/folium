/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/capi/keys_capi_test.c -- security/keys, exercised from C.
 *
 * C99, same -Werror posture as the other capi test files. Compiled only
 * when UAPP_CRYPTO_ENABLED, since uapp_keys.h's functions do not exist in a
 * library built without libsodium (security/keys links security/crypto).
 *
 * One slice case drives the WHOLE hierarchy end to end on real content --
 * recovery secret -> URK -> workspace key -> document key -> AEAD -- which
 * is the case that fails if any key in the design were accidentally
 * per-device or nondeterministic, mirroring tests/keys/key_hierarchy_test.cpp's
 * own "keys.chain" case, now reached only through the C ABI.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uapp/capi/uapp_capi.h"
#include "uapp/capi/uapp_crypto.h"
#include "uapp/capi/uapp_keys.h"

#include "capi_test_harness.h"

/* ===========================================================================
 * slice:
 * =========================================================================== */

static void test_slice_limits(void) {
    uapp_keys_limits_t limits;
    limits.struct_size = (uint32_t)sizeof limits;
    REQUIRE_OK(uapp_keys_get_limits(&limits));
    CHECK(limits.recovery_secret_bytes == 32u);
    CHECK(limits.wrapped_key_bytes == 48u);
    CHECK(limits.max_scope_id_bytes > 0u);
    CHECK(limits.record_format_version >= 1u);
}

static void test_slice_whole_chain_recovery_to_document_content(void) {
    /* recovery secret -> URK */
    uapp_keys_recovery_secret_t* secret = NULL;
    REQUIRE_OK(uapp_keys_generate_recovery_secret(&secret));

    uapp_keys_urk_t* urk = NULL;
    REQUIRE_OK(uapp_keys_derive_user_root_key(secret, &urk));

    /* URK -> workspace key, wrapped and unwrapped */
    uapp_keys_wsk_t* wsk = NULL;
    REQUIRE_OK(uapp_keys_generate_workspace_key(&wsk));

    const char* workspace_id = "workspace-0001";
    uapp_keys_urk_wrapped_workspace_key_record_t wsk_record;
    REQUIRE_OK(uapp_keys_wrap_workspace_key(urk, workspace_id, strlen(workspace_id), wsk,
                                            &wsk_record));

    uapp_keys_wsk_t* wsk_unwrapped = NULL;
    REQUIRE_OK(uapp_keys_unwrap_workspace_key(urk, workspace_id, strlen(workspace_id),
                                              &wsk_record, &wsk_unwrapped));

    /* workspace key -> document key, DERIVED, never stored. Two devices
     * (here: the original `wsk` and the `wsk_unwrapped` recovered from the
     * record) must compute the SAME document key with no exchange. */
    const char* document_id = "page_0001";
    uapp_keys_document_key_t* doc_key_a = NULL;
    uapp_keys_document_key_t* doc_key_b = NULL;
    REQUIRE_OK(uapp_keys_derive_document_key(wsk, document_id, strlen(document_id), &doc_key_a));
    REQUIRE_OK(uapp_keys_derive_document_key(wsk_unwrapped, document_id, strlen(document_id),
                                             &doc_key_b));

    /* Prove doc_key_a == doc_key_b indirectly, same technique
     * crypto_capi_test.c uses: seal under one, open under the other. */
    const char* content = "real document content, end to end through the ABI";
    uint8_t      nonce[24];
    uint8_t      ciphertext[96];
    size_t       ciphertext_len = 0;
    REQUIRE_OK(uapp_keys_seal_document_content(doc_key_a, document_id, strlen(document_id),
                                               content, strlen(content), nonce, ciphertext,
                                               sizeof ciphertext, &ciphertext_len));

    uint8_t recovered[96];
    size_t  recovered_len = 0;
    CHECK_OK(uapp_keys_open_document_content(doc_key_b, document_id, strlen(document_id), nonce,
                                             ciphertext, ciphertext_len, recovered,
                                             sizeof recovered, &recovered_len));
    CHECK(recovered_len == strlen(content));
    CHECK(memcmp(recovered, content, recovered_len) == 0);

    uapp_keys_document_key_close(&doc_key_a);
    uapp_keys_document_key_close(&doc_key_b);
    uapp_keys_wsk_close(&wsk);
    uapp_keys_wsk_close(&wsk_unwrapped);
    uapp_keys_urk_close(&urk);
    uapp_keys_recovery_secret_close(&secret);
}

static void test_slice_urk_wrapped_for_a_device_and_recovered(void) {
    uapp_keys_recovery_secret_t* secret = NULL;
    uapp_keys_urk_t*             urk    = NULL;
    REQUIRE_OK(uapp_keys_generate_recovery_secret(&secret));
    REQUIRE_OK(uapp_keys_derive_user_root_key(secret, &urk));

    uapp_crypto_keypair_t* wrapper_device   = NULL;
    uapp_crypto_keypair_t* recipient_device = NULL;
    REQUIRE_OK(uapp_crypto_keypair_generate(&wrapper_device));
    REQUIRE_OK(uapp_crypto_keypair_generate(&recipient_device));

    uint8_t recipient_public[32];
    uint8_t wrapper_public[32];
    REQUIRE_OK(uapp_crypto_keypair_public_key(recipient_device, recipient_public));
    REQUIRE_OK(uapp_crypto_keypair_public_key(wrapper_device, wrapper_public));

    uapp_keys_device_wrapped_urk_record_t record;
    REQUIRE_OK(
        uapp_keys_wrap_urk_for_device(wrapper_device, recipient_public, urk, &record));

    uapp_keys_urk_t* recovered_urk = NULL;
    REQUIRE_OK(uapp_keys_unwrap_urk_from_device(recipient_device, wrapper_public, &record,
                                                &recovered_urk));

    /* Prove it is really the same URK: derive the same workspace key edge
     * from both and confirm a wrap/unwrap round trip works from either. */
    uapp_keys_wsk_t* wsk = NULL;
    REQUIRE_OK(uapp_keys_generate_workspace_key(&wsk));
    const char* workspace_id = "cross-device-workspace";
    uapp_keys_urk_wrapped_workspace_key_record_t wsk_record;
    REQUIRE_OK(
        uapp_keys_wrap_workspace_key(urk, workspace_id, strlen(workspace_id), wsk, &wsk_record));

    uapp_keys_wsk_t* wsk_from_recovered = NULL;
    CHECK_OK(uapp_keys_unwrap_workspace_key(recovered_urk, workspace_id, strlen(workspace_id),
                                            &wsk_record, &wsk_from_recovered));
    if (wsk_from_recovered != NULL) {
        uapp_keys_wsk_close(&wsk_from_recovered);
    }

    uapp_keys_wsk_close(&wsk);
    uapp_keys_urk_close(&recovered_urk);
    uapp_keys_urk_close(&urk);
    uapp_keys_recovery_secret_close(&secret);
    uapp_crypto_keypair_close(&wrapper_device);
    uapp_crypto_keypair_close(&recipient_device);
}

static void test_slice_urk_wrapped_under_passphrase_and_recovered(void) {
    uapp_keys_recovery_secret_t* secret = NULL;
    uapp_keys_urk_t*             urk    = NULL;
    REQUIRE_OK(uapp_keys_generate_recovery_secret(&secret));
    REQUIRE_OK(uapp_keys_derive_user_root_key(secret, &urk));

    const char* passphrase = "a reasonably long passphrase for testing";
    uapp_keys_passphrase_wrapped_urk_record_t record;
    REQUIRE_OK(uapp_keys_wrap_urk_under_passphrase(
        passphrase, strlen(passphrase), UAPP_CRYPTO_KDF_PROFILE_MOBILE_INTERACTIVE, urk,
        &record));

    uapp_keys_urk_t* recovered = NULL;
    REQUIRE_OK(
        uapp_keys_unwrap_urk_with_passphrase(passphrase, strlen(passphrase), &record, &recovered));

    uapp_keys_urk_close(&recovered);
    uapp_keys_urk_close(&urk);
    uapp_keys_recovery_secret_close(&secret);
}

/* ===========================================================================
 * boundary:
 * =========================================================================== */

static void test_boundary_wrong_device_is_auth_failed(void) {
    uapp_keys_recovery_secret_t* secret = NULL;
    uapp_keys_urk_t*             urk    = NULL;
    REQUIRE_OK(uapp_keys_generate_recovery_secret(&secret));
    REQUIRE_OK(uapp_keys_derive_user_root_key(secret, &urk));

    uapp_crypto_keypair_t* wrapper   = NULL;
    uapp_crypto_keypair_t* recipient = NULL;
    uapp_crypto_keypair_t* impostor  = NULL;
    REQUIRE_OK(uapp_crypto_keypair_generate(&wrapper));
    REQUIRE_OK(uapp_crypto_keypair_generate(&recipient));
    REQUIRE_OK(uapp_crypto_keypair_generate(&impostor));

    uint8_t recipient_public[32];
    uint8_t wrapper_public[32];
    REQUIRE_OK(uapp_crypto_keypair_public_key(recipient, recipient_public));
    REQUIRE_OK(uapp_crypto_keypair_public_key(wrapper, wrapper_public));

    uapp_keys_device_wrapped_urk_record_t record;
    REQUIRE_OK(uapp_keys_wrap_urk_for_device(wrapper, recipient_public, urk, &record));

    uapp_keys_urk_t* out = NULL;
    /* impostor does not hold recipient's secret key */
    CHECK_STATUS(uapp_keys_unwrap_urk_from_device(impostor, wrapper_public, &record, &out),
                UAPP_STATUS_CRYPTO_AUTH_FAILED);
    CHECK(out == NULL);

    uapp_keys_urk_close(&urk);
    uapp_keys_recovery_secret_close(&secret);
    uapp_crypto_keypair_close(&wrapper);
    uapp_crypto_keypair_close(&recipient);
    uapp_crypto_keypair_close(&impostor);
}

static void test_boundary_wrong_workspace_id_is_auth_failed(void) {
    uapp_keys_recovery_secret_t* secret = NULL;
    uapp_keys_urk_t*             urk    = NULL;
    uapp_keys_wsk_t*             wsk    = NULL;
    REQUIRE_OK(uapp_keys_generate_recovery_secret(&secret));
    REQUIRE_OK(uapp_keys_derive_user_root_key(secret, &urk));
    REQUIRE_OK(uapp_keys_generate_workspace_key(&wsk));

    const char* workspace_id       = "real-workspace";
    const char* different_id       = "different-workspace";
    uapp_keys_urk_wrapped_workspace_key_record_t record;
    REQUIRE_OK(
        uapp_keys_wrap_workspace_key(urk, workspace_id, strlen(workspace_id), wsk, &record));

    uapp_keys_wsk_t* out = NULL;
    CHECK_STATUS(
        uapp_keys_unwrap_workspace_key(urk, different_id, strlen(different_id), &record, &out),
        UAPP_STATUS_CRYPTO_AUTH_FAILED);
    CHECK(out == NULL);

    uapp_keys_wsk_close(&wsk);
    uapp_keys_urk_close(&urk);
    uapp_keys_recovery_secret_close(&secret);
}

static void test_boundary_wrong_passphrase_is_auth_failed(void) {
    uapp_keys_recovery_secret_t* secret = NULL;
    uapp_keys_urk_t*             urk    = NULL;
    REQUIRE_OK(uapp_keys_generate_recovery_secret(&secret));
    REQUIRE_OK(uapp_keys_derive_user_root_key(secret, &urk));

    const char* passphrase = "the real passphrase";
    const char* wrong       = "a wrong passphrase";
    uapp_keys_passphrase_wrapped_urk_record_t record;
    REQUIRE_OK(uapp_keys_wrap_urk_under_passphrase(
        passphrase, strlen(passphrase), UAPP_CRYPTO_KDF_PROFILE_MOBILE_INTERACTIVE, urk,
        &record));

    uapp_keys_urk_t* out = NULL;
    CHECK_STATUS(uapp_keys_unwrap_urk_with_passphrase(wrong, strlen(wrong), &record, &out),
                UAPP_STATUS_CRYPTO_AUTH_FAILED);
    CHECK(out == NULL);

    uapp_keys_urk_close(&urk);
    uapp_keys_recovery_secret_close(&secret);
}

static void test_boundary_empty_document_id_is_rejected(void) {
    uapp_keys_wsk_t* wsk = NULL;
    REQUIRE_OK(uapp_keys_generate_workspace_key(&wsk));

    uapp_keys_document_key_t* out = NULL;
    CHECK_STATUS(uapp_keys_derive_document_key(wsk, "", 0, &out), UAPP_STATUS_KEY_SCOPE_ID_INVALID);
    CHECK(out == NULL);

    uapp_keys_wsk_close(&wsk);
}

static void test_boundary_null_and_invalid_handles(void) {
    uapp_keys_urk_t* out = NULL;
    CHECK_STATUS(uapp_keys_derive_user_root_key(NULL, &out), UAPP_STATUS_CRYPTO_KEY_INVALID);
    CHECK(out == NULL);
}

static void test_boundary_struct_size_versioning(void) {
    uapp_keys_limits_t limits;
    limits.struct_size = 1u;
    CHECK_STATUS(uapp_keys_get_limits(&limits), UAPP_STATUS_INVALID_ARGUMENT);
}

/* ===========================================================================
 * Harness
 * =========================================================================== */

static const uapp_test_case_t k_cases[] = {
    {"slice: limits", test_slice_limits},
    {"slice: recovery secret -> URK -> WSK -> document key -> AEAD, end to end",
     test_slice_whole_chain_recovery_to_document_content},
    {"slice: URK wrapped for a device and recovered on it",
     test_slice_urk_wrapped_for_a_device_and_recovered},
    {"slice: URK wrapped under a passphrase and recovered with it",
     test_slice_urk_wrapped_under_passphrase_and_recovered},

    {"boundary: a device without the recipient's secret key is CRYPTO_AUTH_FAILED",
     test_boundary_wrong_device_is_auth_failed},
    {"boundary: the wrong workspace id is CRYPTO_AUTH_FAILED",
     test_boundary_wrong_workspace_id_is_auth_failed},
    {"boundary: the wrong passphrase is CRYPTO_AUTH_FAILED",
     test_boundary_wrong_passphrase_is_auth_failed},
    {"boundary: an empty document id is KEY_SCOPE_ID_INVALID",
     test_boundary_empty_document_id_is_rejected},
    {"boundary: null/invalid handles are rejected, never dereferenced",
     test_boundary_null_and_invalid_handles},
    {"boundary: a stale struct_size is rejected", test_boundary_struct_size_versioning}};

int main(int argc, char** argv) {
    const char* filter = (argc > 1) ? argv[1] : NULL;

    /* uapp_keys_* calls crypto::init()'d primitives internally but never
     * calls init() itself -- same as the C++ layer, which documents
     * CryptoNotInitialised as the caller's responsibility to avoid. This
     * binary is its own process (separate from uapp_crypto_capi_tests), so
     * it must call this once itself rather than relying on another
     * executable having already done so. REQUIRE_OK is not used here: its
     * bare `return;` is only valid inside the `void` test functions below,
     * not in `main`. */
    const uapp_status_t init_status = uapp_crypto_init();
    if (init_status != UAPP_STATUS_OK) {
        fprintf(stderr, "uapp_crypto_init() failed: %s\n", uapp_status_code(init_status));
        return 1;
    }

    return uapp_run_test_cases("uapp keys C ABI tests", k_cases,
                               sizeof k_cases / sizeof k_cases[0], filter);
}
