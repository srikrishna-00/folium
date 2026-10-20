/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/capi/crypto_capi_test.c -- security/crypto, exercised from C.
 *
 * C99, same -Werror posture as the other capi test files. Compiled only
 * when UAPP_CRYPTO_ENABLED (see tests/CMakeLists.txt), since uapp_crypto.h's
 * functions do not exist in a library built without libsodium.
 *
 * The HKDF and Argon2id golden values below are copied VERBATIM from
 * tests/crypto/crypto_test.cpp (kHkdfParent/kHkdfUserRootKey/
 * kHkdfDocumentKey/kHkdfWorkspaceWrap, kKdfSalt/kKdfPassphrase/
 * kKdfGoldenMobileInteractive) -- the whole point of this file is to recheck
 * the SAME externally-verified vectors the C++ suite already checks, now
 * reached ONLY through the C ABI. See that file's own comments for where
 * each value came from (RFC 5869 Test Case 1 cross-check, an independent
 * Python HKDF-SHA256/Argon2id implementation).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uapp/capi/uapp_capi.h"
#include "uapp/capi/uapp_crypto.h"

#include "capi_test_harness.h"

static void hex_to_bytes(const char* hex, uint8_t* out, size_t out_len) {
    size_t i;
    for (i = 0; i < out_len; ++i) {
        unsigned byte;
        sscanf(hex + 2u * i, "%2x", &byte);
        out[i] = (uint8_t)byte;
    }
}

/* ===========================================================================
 * slice:
 * =========================================================================== */

static void test_slice_init_and_limits(void) {
    /* main() already called uapp_crypto_init() before any test case ran --
     * this matters because ctest invokes this binary once PER FILTER
     * ("slice:" and "boundary:" are two separate process runs), so a case
     * cannot rely on a DIFFERENT case in the same filter group having run
     * first. Re-calling it here only proves idempotence. */
    REQUIRE_OK(uapp_crypto_init());
    CHECK(uapp_crypto_is_initialised() == UAPP_TRUE);
    CHECK(uapp_crypto_library_version() != NULL);

    uapp_crypto_limits_t limits;
    limits.struct_size = (uint32_t)sizeof limits;
    REQUIRE_OK(uapp_crypto_get_limits(&limits));
    CHECK(limits.public_key_bytes == 32u);
    CHECK(limits.content_key_bytes == 32u);
    CHECK(limits.nonce_bytes == 24u);
    CHECK(limits.tag_bytes == 16u);
    CHECK(limits.salt_bytes == 16u);
    CHECK(limits.wrapped_key_bytes == 48u);
    CHECK(limits.min_passphrase_bytes > 0u);
}

static void test_slice_hkdf_matches_published_vectors(void) {
    static const char* kHkdfParent =
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

    uint8_t parent_bytes[32];
    hex_to_bytes(kHkdfParent, parent_bytes, sizeof parent_bytes);

    uapp_crypto_content_key_t* parent = NULL;
    REQUIRE_OK(uapp_crypto_content_key_import(parent_bytes, &parent));

    {
        /* label = "uapp/keys/v1/user-root-key", info = "" */
        static const char* kExpected =
            "b1f72154966ab04ef6418a60f2d94f3783fb87eefdaf08566d92e84a23790714";
        uint8_t expected[32];
        hex_to_bytes(kExpected, expected, sizeof expected);

        uapp_crypto_content_key_t* derived = NULL;
        REQUIRE_OK(uapp_crypto_content_key_derive_subkey(
            parent, "uapp/keys/v1/user-root-key", strlen("uapp/keys/v1/user-root-key"), "", 0,
            &derived));

        /* There is no raw-byte accessor for a content key (by design), so
         * the comparison is indirect: seal a fixed plaintext under `derived`
         * and under a key imported from `expected`, with a fixed nonce
         * supplied by... there is no nonce-override either (also by
         * design). So the real cross-check is wrap/unwrap based: import
         * `expected` as a second handle and confirm uapp_crypto_seal()
         * followed by uapp_crypto_open() with EACH key against the OTHER's
         * ciphertext fails (different keys), and a seal/open round trip
         * with `derived` alone succeeds -- proving `derived` is a real,
         * usable 32-byte key without ever reading its bytes back, which is
         * the strongest statement this boundary is designed to let a test
         * make. */
        uapp_crypto_content_key_t* reference = NULL;
        REQUIRE_OK(uapp_crypto_content_key_import(expected, &reference));

        const char* plaintext    = "derive_subkey cross-check";
        uint8_t      nonce_a[24], nonce_b[24];
        uint8_t      ct_a[64], ct_b[64];
        size_t       ct_a_len = 0, ct_b_len = 0;

        REQUIRE_OK(uapp_crypto_seal(derived, plaintext, strlen(plaintext), NULL, 0, nonce_a, ct_a,
                                    sizeof ct_a, &ct_a_len));
        REQUIRE_OK(uapp_crypto_seal(reference, plaintext, strlen(plaintext), NULL, 0, nonce_b,
                                    ct_b, sizeof ct_b, &ct_b_len));

        /* If derive_subkey and the independent reference implementation
         * agree, `derived` and `reference` hold the SAME 32 bytes, so
         * `reference` must be able to OPEN what `derived` sealed. */
        uint8_t recovered[64];
        size_t  recovered_len = 0;
        CHECK_OK(uapp_crypto_open(reference, nonce_a, ct_a, ct_a_len, NULL, 0, recovered,
                                  sizeof recovered, &recovered_len));
        CHECK(recovered_len == strlen(plaintext));
        CHECK(memcmp(recovered, plaintext, recovered_len) == 0);

        uapp_crypto_content_key_close(&derived);
        uapp_crypto_content_key_close(&reference);
    }

    uapp_crypto_content_key_close(&parent);
}

static void test_slice_passphrase_derivation_matches_published_vector(void) {
    static const char* kSaltHex =
        "000102030405060708090a0b0c0d0e0f";
    static const char* kPassphrase = "correct horse battery staple";
    static const char* kExpectedHex =
        "0d1a3c6523c8f06e4e0af9c515aa5b5448cfebd6838f2d52c3d8b6ef8ddc3c2e";

    uint8_t salt[16];
    hex_to_bytes(kSaltHex, salt, sizeof salt);
    uint8_t expected[32];
    hex_to_bytes(kExpectedHex, expected, sizeof expected);

    uapp_crypto_content_key_t* derived = NULL;
    REQUIRE_OK(uapp_crypto_content_key_derive_from_passphrase(
        kPassphrase, strlen(kPassphrase), salt, UAPP_CRYPTO_KDF_PROFILE_MOBILE_INTERACTIVE,
        &derived));

    uapp_crypto_content_key_t* reference = NULL;
    REQUIRE_OK(uapp_crypto_content_key_import(expected, &reference));

    /* Same indirect cross-check as the HKDF case above: if derivation
     * matches the published golden, `reference` can open what `derived`
     * sealed. */
    const char* plaintext = "passphrase derivation cross-check";
    uint8_t      nonce[24];
    uint8_t      ciphertext[80];
    size_t       ciphertext_len = 0;
    REQUIRE_OK(uapp_crypto_seal(derived, plaintext, strlen(plaintext), NULL, 0, nonce, ciphertext,
                                sizeof ciphertext, &ciphertext_len));

    uint8_t recovered[80];
    size_t  recovered_len = 0;
    CHECK_OK(uapp_crypto_open(reference, nonce, ciphertext, ciphertext_len, NULL, 0, recovered,
                              sizeof recovered, &recovered_len));
    CHECK(recovered_len == strlen(plaintext));
    CHECK(memcmp(recovered, plaintext, recovered_len) == 0);

    uapp_crypto_content_key_close(&derived);
    uapp_crypto_content_key_close(&reference);
}

static void test_slice_seal_open_round_trip(void) {
    uapp_crypto_content_key_t* key = NULL;
    REQUIRE_OK(uapp_crypto_content_key_generate(&key));

    const char* plaintext = "a real document body, sealed for real";
    const char* aad       = "doc-id-0001";
    uint8_t      nonce[24];
    uint8_t      ciphertext[128];
    size_t       ciphertext_len = 0;

    REQUIRE_OK(uapp_crypto_seal(key, plaintext, strlen(plaintext), aad, strlen(aad), nonce,
                                ciphertext, sizeof ciphertext, &ciphertext_len));
    CHECK(ciphertext_len == strlen(plaintext) + 16u);

    uint8_t recovered[128];
    size_t  recovered_len = 0;
    REQUIRE_OK(uapp_crypto_open(key, nonce, ciphertext, ciphertext_len, aad, strlen(aad),
                                recovered, sizeof recovered, &recovered_len));
    CHECK(recovered_len == strlen(plaintext));
    CHECK(memcmp(recovered, plaintext, recovered_len) == 0);

    uapp_crypto_content_key_close(&key);
}

static void test_slice_wrap_unwrap_between_two_devices(void) {
    uapp_crypto_keypair_t* wrapper   = NULL;
    uapp_crypto_keypair_t* recipient = NULL;
    REQUIRE_OK(uapp_crypto_keypair_generate(&wrapper));
    REQUIRE_OK(uapp_crypto_keypair_generate(&recipient));

    uint8_t recipient_public[32];
    REQUIRE_OK(uapp_crypto_keypair_public_key(recipient, recipient_public));
    uint8_t wrapper_public[32];
    REQUIRE_OK(uapp_crypto_keypair_public_key(wrapper, wrapper_public));

    uapp_crypto_content_key_t* cek = NULL;
    REQUIRE_OK(uapp_crypto_content_key_generate(&cek));

    uapp_crypto_wrapped_key_t wrapped;
    REQUIRE_OK(uapp_crypto_wrap_content_key(wrapper, recipient_public, cek, &wrapped));

    uapp_crypto_content_key_t* unwrapped = NULL;
    REQUIRE_OK(
        uapp_crypto_unwrap_content_key(recipient, wrapper_public, &wrapped, &unwrapped));

    /* Prove `unwrapped` really is `cek`: seal under one, open under the
     * other. */
    const char* plaintext = "shared between devices";
    uint8_t      nonce[24];
    uint8_t      ciphertext[64];
    size_t       ciphertext_len = 0;
    REQUIRE_OK(uapp_crypto_seal(cek, plaintext, strlen(plaintext), NULL, 0, nonce, ciphertext,
                                sizeof ciphertext, &ciphertext_len));
    uint8_t recovered[64];
    size_t  recovered_len = 0;
    CHECK_OK(uapp_crypto_open(unwrapped, nonce, ciphertext, ciphertext_len, NULL, 0, recovered,
                              sizeof recovered, &recovered_len));
    CHECK(recovered_len == strlen(plaintext));
    CHECK(memcmp(recovered, plaintext, recovered_len) == 0);

    uapp_crypto_content_key_close(&cek);
    uapp_crypto_content_key_close(&unwrapped);
    uapp_crypto_keypair_close(&wrapper);
    uapp_crypto_keypair_close(&recipient);
}

/* ===========================================================================
 * boundary:
 * =========================================================================== */

static void test_boundary_tampered_ciphertext_is_auth_failed(void) {
    uapp_crypto_content_key_t* key = NULL;
    REQUIRE_OK(uapp_crypto_content_key_generate(&key));

    const char* plaintext = "tamper me";
    uint8_t      nonce[24];
    uint8_t      ciphertext[64];
    size_t       ciphertext_len = 0;
    REQUIRE_OK(uapp_crypto_seal(key, plaintext, strlen(plaintext), NULL, 0, nonce, ciphertext,
                                sizeof ciphertext, &ciphertext_len));

    ciphertext[0] ^= 0x01u; /* flip one bit */

    uint8_t recovered[64];
    size_t  recovered_len = 1234u; /* poison, to prove it gets reset to 0 */
    CHECK_STATUS(uapp_crypto_open(key, nonce, ciphertext, ciphertext_len, NULL, 0, recovered,
                                  sizeof recovered, &recovered_len),
                UAPP_STATUS_CRYPTO_AUTH_FAILED);
    CHECK(recovered_len == 0u);

    uapp_crypto_content_key_close(&key);
}

static void test_boundary_wrong_key_is_also_auth_failed(void) {
    /* The oracle rule: wrong key and tampered ciphertext must be the SAME
     * status, so a caller cannot distinguish them. */
    uapp_crypto_content_key_t* key_a = NULL;
    uapp_crypto_content_key_t* key_b = NULL;
    REQUIRE_OK(uapp_crypto_content_key_generate(&key_a));
    REQUIRE_OK(uapp_crypto_content_key_generate(&key_b));

    const char* plaintext = "wrong key test";
    uint8_t      nonce[24];
    uint8_t      ciphertext[64];
    size_t       ciphertext_len = 0;
    REQUIRE_OK(uapp_crypto_seal(key_a, plaintext, strlen(plaintext), NULL, 0, nonce, ciphertext,
                                sizeof ciphertext, &ciphertext_len));

    uint8_t recovered[64];
    size_t  recovered_len = 0;
    CHECK_STATUS(uapp_crypto_open(key_b, nonce, ciphertext, ciphertext_len, NULL, 0, recovered,
                                  sizeof recovered, &recovered_len),
                UAPP_STATUS_CRYPTO_AUTH_FAILED);

    uapp_crypto_content_key_close(&key_a);
    uapp_crypto_content_key_close(&key_b);
}

static void test_boundary_null_and_invalid_handles(void) {
    uint8_t public_key[32];
    CHECK_STATUS(uapp_crypto_keypair_public_key(NULL, public_key), UAPP_STATUS_CRYPTO_KEY_INVALID);

    uapp_crypto_content_key_t* out = NULL;
    CHECK_STATUS(uapp_crypto_content_key_derive_subkey(NULL, "x", 1, "", 0, &out),
                UAPP_STATUS_CRYPTO_KEY_INVALID);
    CHECK(out == NULL);

    CHECK_STATUS(uapp_crypto_seal(NULL, "a", 1, NULL, 0, (uint8_t[24]){0}, (uint8_t[32]){0}, 32,
                                  &(size_t){0}),
                UAPP_STATUS_CRYPTO_KEY_INVALID);
}

static void test_boundary_struct_size_versioning(void) {
    uapp_crypto_limits_t limits;
    limits.struct_size = 1u;
    CHECK_STATUS(uapp_crypto_get_limits(&limits), UAPP_STATUS_INVALID_ARGUMENT);
}

static void test_boundary_double_close_is_a_noop(void) {
    uapp_crypto_content_key_t* key = NULL;
    REQUIRE_OK(uapp_crypto_content_key_generate(&key));
    uapp_crypto_content_key_close(&key);
    CHECK(key == NULL);
    uapp_crypto_content_key_close(&key); /* must not crash */
}

/* ===========================================================================
 * Harness
 * =========================================================================== */

static const uapp_test_case_t k_cases[] = {
    {"slice: init, is_initialised, library_version, limits", test_slice_init_and_limits},
    {"slice: derive_subkey matches a published HKDF-SHA256 vector",
     test_slice_hkdf_matches_published_vectors},
    {"slice: passphrase derivation matches a published Argon2id vector",
     test_slice_passphrase_derivation_matches_published_vector},
    {"slice: seal/open round trip", test_slice_seal_open_round_trip},
    {"slice: wrap/unwrap a content key between two devices",
     test_slice_wrap_unwrap_between_two_devices},

    {"boundary: a tampered ciphertext is CRYPTO_AUTH_FAILED",
     test_boundary_tampered_ciphertext_is_auth_failed},
    {"boundary: the wrong key is also CRYPTO_AUTH_FAILED (same oracle rule)",
     test_boundary_wrong_key_is_also_auth_failed},
    {"boundary: null/invalid handles are rejected, never dereferenced",
     test_boundary_null_and_invalid_handles},
    {"boundary: a stale struct_size is rejected", test_boundary_struct_size_versioning},
    {"boundary: double-close is a no-op, not a crash", test_boundary_double_close_is_a_noop}};

int main(int argc, char** argv) {
    const char* filter = (argc > 1) ? argv[1] : NULL;

    /* Called here, not inside a "slice:" case: ctest invokes this binary
     * once per filter group ("slice:" and "boundary:" are two separate
     * process runs), so a "boundary:" case cannot rely on a "slice:" case
     * in a DIFFERENT process having already called this. */
    const uapp_status_t init_status = uapp_crypto_init();
    if (init_status != UAPP_STATUS_OK) {
        fprintf(stderr, "uapp_crypto_init() failed: %s\n", uapp_status_code(init_status));
        return 1;
    }

    return uapp_run_test_cases("uapp crypto C ABI tests", k_cases,
                               sizeof k_cases / sizeof k_cases[0], filter);
}
