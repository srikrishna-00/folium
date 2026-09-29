/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/capi/note_capi_test.c — the C ABI, exercised from C.
 *
 * This file is C99, compiled by a C compiler with -std=c99 -Wall -Wextra
 * -Wpedantic -Wconversion -Werror. That is not incidental: it is the test.
 * architecture.md section 8 rule 5 says the capi headers "must compile clean
 * under a C compiler, not just a C++ one", and the only way to know that is to
 * do it. If a C++-ism lands in uapp_capi.h — a default argument, a `bool`, an
 * anonymous struct member, a `//` comment in a C89 context, a cast that only
 * C++ accepts — this file stops building.
 *
 * It includes the two PUBLIC headers and nothing else from the project. No
 * test_framework.h (that is C++), no core header, no CMake-injected path into
 * src/. Whatever is not reachable from here is not reachable from Kotlin or
 * Swift either.
 *
 * Two groups, run as two separate CTest entries:
 *
 *   "slice: ..."     the full vertical slice through the ABI —
 *                    create, store, open, edit, save, search, delete, count,
 *                    persistence across a close/reopen, and two independent
 *                    service handles proving there is no shared global state.
 *
 *   "boundary: ..."  what happens when the caller is wrong. Every one of these
 *                    would be a native crash in a careless C API, and every one
 *                    of them is exactly what a generated JNI wrapper does on a
 *                    bad day. They must all return a status.
 *
 * The harness is ~40 lines at the bottom of the file. It is C because it has
 * to be, and small because anything larger would be a second thing to trust.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uapp/capi/uapp_capi.h"
#include "uapp/capi/uapp_notes.h"

/* --- tiny C test harness --------------------------------------------------
 * Declared first so the cases can use it; defined at the bottom. */

static int g_checks_failed;  /* failures inside the case currently running */
static int g_cases_passed;
static int g_cases_failed;

static void fail_at(const char* file, int line, const char* what, const char* detail) {
    ++g_checks_failed;
    fprintf(stderr, "    FAIL %s:%d\n      expected: %s\n", file, line, what);
    if (detail != NULL && detail[0] != '\0') {
        fprintf(stderr, "      %s\n", detail);
    }
}

#define CHECK(cond)                                            \
    do {                                                       \
        if (!(cond)) { fail_at(__FILE__, __LINE__, #cond, ""); } \
    } while (0)

/* Assert an exact status, printing both stable code strings on mismatch —
 * "got STORE_WRITE_FAILED, wanted NOT_FOUND" beats "got 303, wanted 101". */
#define CHECK_STATUS(expr, expected)                                        \
    do {                                                                    \
        const uapp_status_t actual_ = (expr);                               \
        if (actual_ != (expected)) {                                        \
            char detail_[160];                                              \
            snprintf(detail_, sizeof detail_, "got %s, wanted %s",          \
                     uapp_status_code(actual_),                             \
                     uapp_status_code((uapp_status_t)(expected)));          \
            fail_at(__FILE__, __LINE__, #expr " == " #expected, detail_);   \
        }                                                                   \
    } while (0)

#define CHECK_OK(expr) CHECK_STATUS((expr), UAPP_STATUS_OK)

/* Fatal: a broken precondition must not cascade into a wall of noise. */
#define REQUIRE_OK(expr)                                  \
    do {                                                  \
        const uapp_status_t r_ = (expr);                  \
        if (r_ != UAPP_STATUS_OK) {                       \
            fail_at(__FILE__, __LINE__, #expr " == OK",   \
                    uapp_status_code(r_));                \
            return;                                       \
        }                                                 \
    } while (0)

/* --- shared helpers -------------------------------------------------------- */

/* Open an in-memory service. Each call gets its OWN private database:
 * ":memory:" is per-connection in SQLite, which is what makes the
 * independent-handles case below meaningful rather than accidental. */
static uapp_status_t open_memory_service(uapp_note_service_t** out) {
    uapp_note_service_config_t config;
    const uapp_status_t init = uapp_note_service_config_init(&config);
    if (init != UAPP_STATUS_OK) {
        return init;
    }
    config.database_path = ":memory:";
    return uapp_note_service_open(&config, out);
}

static uapp_status_t open_file_service(const char* path, uapp_note_service_t** out) {
    uapp_note_service_config_t config;
    const uapp_status_t init = uapp_note_service_config_init(&config);
    if (init != UAPP_STATUS_OK) {
        return init;
    }
    config.database_path = path;
    return uapp_note_service_open(&config, out);
}

/* ===========================================================================
 * ABI versioning (spec section 30: the interface must be versioned)
 * =========================================================================== */

static void test_abi_version(void) {
    CHECK(uapp_abi_version_major() == UAPP_ABI_VERSION_MAJOR);
    CHECK(uapp_abi_version_minor() == UAPP_ABI_VERSION_MINOR);
    CHECK(uapp_abi_version_string() != NULL);
    CHECK(strcmp(uapp_abi_version_string(), UAPP_ABI_VERSION_STRING) == 0);

    /* The check a binding runs in JNI_OnLoad. */
    CHECK(UAPP_ABI_ASSERT_COMPATIBLE() == UAPP_TRUE);

    /* A caller built against an older minor of the same major still runs. */
    CHECK(uapp_abi_is_compatible(UAPP_ABI_VERSION_MAJOR, 0) == UAPP_TRUE);
    /* A caller from the future does not: it may reference symbols we lack. */
    CHECK(uapp_abi_is_compatible(UAPP_ABI_VERSION_MAJOR, UAPP_ABI_VERSION_MINOR + 1u)
          == UAPP_FALSE);
    /* A different major never does. */
    CHECK(uapp_abi_is_compatible(UAPP_ABI_VERSION_MAJOR + 1u, 0) == UAPP_FALSE);
}

static void test_status_strings(void) {
    /* Never NULL, never empty — a binding attaches these to crash reports. */
    CHECK(strcmp(uapp_status_code(UAPP_STATUS_OK), "OK") == 0);
    CHECK(strcmp(uapp_status_code(UAPP_STATUS_NOT_FOUND), "NOT_FOUND") == 0);
    CHECK(strcmp(uapp_status_code(UAPP_STATUS_STORE_WRITE_FAILED), "STORE_WRITE_FAILED") == 0);
    /* The 9xx block is C-ABI-only and has no uapp::Status counterpart. */
    CHECK(strcmp(uapp_status_code(UAPP_STATUS_OUT_OF_MEMORY), "OUT_OF_MEMORY") == 0);
    CHECK(strcmp(uapp_status_code(UAPP_STATUS_UNEXPECTED), "UNEXPECTED") == 0);

    CHECK(uapp_status_message(UAPP_STATUS_NOT_FOUND) != NULL);
    CHECK(uapp_status_message(UAPP_STATUS_OUT_OF_MEMORY)[0] != '\0');
    CHECK(uapp_status_message(UAPP_STATUS_UNEXPECTED)[0] != '\0');

    CHECK(uapp_status_is_ok(UAPP_STATUS_OK) == UAPP_TRUE);
    CHECK(uapp_status_is_ok(UAPP_STATUS_NOT_FOUND) == UAPP_FALSE);

    /* The enum is pinned to 32 bits on both sides of the boundary. */
    CHECK(sizeof(uapp_status_t) == 4u);
    CHECK(sizeof(uapp_bool_t) == 4u);

    /* An integer nobody defined must still produce a string, not a crash —
     * and must not be undefined behaviour to read, which is what
     * UAPP_STATUS_FORCE_INT32 buys. UBSan is what caught the version of this
     * enum that lacked it. */
    CHECK(strcmp(uapp_status_code((uapp_status_t)424242), "UNKNOWN_STATUS") == 0);
    CHECK(uapp_status_message((uapp_status_t)424242) != NULL);
    CHECK(uapp_status_is_ok((uapp_status_t)424242) == UAPP_FALSE);
}

static void test_limits(void) {
    uapp_note_limits_t limits;
    memset(&limits, 0, sizeof limits);
    limits.struct_size = (uint32_t)sizeof limits;
    REQUIRE_OK(uapp_note_get_limits(&limits));

    CHECK(limits.id_hex_length == 32u);
    CHECK(limits.max_title_bytes > 0u);
    CHECK(limits.max_body_bytes > limits.max_title_bytes);
    CHECK(limits.max_query_bytes > 0u);
    CHECK(limits.default_search_limit > 0u);
    CHECK(limits.default_search_limit <= limits.max_search_limit);
}

/* ===========================================================================
 * The slice: CREATE -> STORE -> SEARCH -> OPEN -> EDIT -> SAVE (+ DELETE, COUNT)
 * =========================================================================== */

static void test_slice_full_round_trip(void) {
    uapp_note_service_t* service = NULL;
    uapp_note_t* created = NULL;
    uapp_note_t* reopened = NULL;
    uapp_search_results_t* results = NULL;
    const char* borrowed = NULL;
    char* owned_id = NULL;
    size_t count = 0;
    size_t hits = 0;
    int64_t created_at = 0;
    int64_t modified_at = 0;
    int64_t modified_after_save = 0;

    REQUIRE_OK(open_memory_service(&service));

    /* --- COUNT on an empty store ----------------------------------------- */
    REQUIRE_OK(uapp_note_service_count(service, &count));
    CHECK(count == 0u);

    /* --- CREATE + STORE --------------------------------------------------- */
    REQUIRE_OK(uapp_note_service_create(service, "Amortised analysis",
                                        "Bounds the average cost per operation.", &created));
    CHECK(created != NULL);

    REQUIRE_OK(uapp_note_service_count(service, &count));
    CHECK(count == 1u);

    /* Borrowed reads (rule B). */
    REQUIRE_OK(uapp_note_get_id_hex(created, &borrowed));
    CHECK(strlen(borrowed) == 32u);
    /* Take an owned copy (rule O) so the id survives freeing the handle. */
    REQUIRE_OK(uapp_note_copy_id_hex(created, &owned_id));
    CHECK(owned_id != NULL);
    CHECK(strcmp(owned_id, borrowed) == 0);

    REQUIRE_OK(uapp_note_get_title(created, &borrowed));
    CHECK(strcmp(borrowed, "Amortised analysis") == 0);
    REQUIRE_OK(uapp_note_get_body(created, &borrowed));
    CHECK(strcmp(borrowed, "Bounds the average cost per operation.") == 0);

    REQUIRE_OK(uapp_note_get_timestamps(created, &created_at, &modified_at));
    CHECK(created_at > 0);
    CHECK(modified_at >= created_at);

    /* The handle is a snapshot; drop it and work from the persisted id. */
    uapp_note_free(&created);
    CHECK(created == NULL); /* the destructor nulls the caller's pointer */

    /* --- SEARCH ----------------------------------------------------------- */
    REQUIRE_OK(uapp_note_service_search(service, "amortised", 0u, &results));
    REQUIRE_OK(uapp_search_results_count(results, &hits));
    CHECK(hits == 1u);
    if (hits == 1u) {
        double rank = 1.0;
        REQUIRE_OK(uapp_search_results_get_id_hex(results, 0u, &borrowed));
        CHECK(strcmp(borrowed, owned_id) == 0);
        REQUIRE_OK(uapp_search_results_get_title(results, 0u, &borrowed));
        CHECK(strcmp(borrowed, "Amortised analysis") == 0);
        REQUIRE_OK(uapp_search_results_get_snippet(results, 0u, &borrowed));
        CHECK(borrowed[0] != '\0');
        REQUIRE_OK(uapp_search_results_get_rank(results, 0u, &rank));
        CHECK(rank <= 0.0); /* BM25 convention: lower is better, and negative */
    }
    uapp_search_results_free(&results);
    CHECK(results == NULL);

    /* A query that matches nothing is OK with zero rows, NOT a NOT_FOUND. */
    REQUIRE_OK(uapp_note_service_search(service, "quicksort", 0u, &results));
    REQUIRE_OK(uapp_search_results_count(results, &hits));
    CHECK(hits == 0u);
    uapp_search_results_free(&results);

    /* --- OPEN ------------------------------------------------------------- */
    REQUIRE_OK(uapp_note_service_open_note(service, owned_id, &reopened));
    REQUIRE_OK(uapp_note_get_title(reopened, &borrowed));
    CHECK(strcmp(borrowed, "Amortised analysis") == 0);

    /* --- EDIT (on the caller's snapshot; nothing persisted yet) ----------- */
    REQUIRE_OK(uapp_note_set_title(reopened, "Amortised analysis (revised)"));
    REQUIRE_OK(uapp_note_set_body(reopened, "Aggregate, accounting and potential methods."));
    REQUIRE_OK(uapp_note_get_title(reopened, &borrowed));
    CHECK(strcmp(borrowed, "Amortised analysis (revised)") == 0);
    {
        size_t title_len = 0;
        size_t body_len = 0;
        REQUIRE_OK(uapp_note_get_title_length(reopened, &title_len));
        REQUIRE_OK(uapp_note_get_body_length(reopened, &body_len));
        CHECK(title_len == strlen("Amortised analysis (revised)"));
        CHECK(body_len == strlen("Aggregate, accounting and potential methods."));
    }

    /* The edit must NOT be visible in the store until save. */
    {
        uapp_note_t* unsaved_view = NULL;
        REQUIRE_OK(uapp_note_service_open_note(service, owned_id, &unsaved_view));
        REQUIRE_OK(uapp_note_get_title(unsaved_view, &borrowed));
        CHECK(strcmp(borrowed, "Amortised analysis") == 0);
        uapp_note_free(&unsaved_view);
    }

    /* --- SAVE ------------------------------------------------------------- */
    REQUIRE_OK(uapp_note_service_save(service, reopened));
    /* The handle is refreshed in place with what was actually persisted. */
    REQUIRE_OK(uapp_note_get_timestamps(reopened, NULL, &modified_after_save));
    CHECK(modified_after_save >= modified_at);

    /* And the store agrees. */
    {
        uapp_note_t* saved_view = NULL;
        REQUIRE_OK(uapp_note_service_open_note(service, owned_id, &saved_view));
        REQUIRE_OK(uapp_note_get_title(saved_view, &borrowed));
        CHECK(strcmp(borrowed, "Amortised analysis (revised)") == 0);
        REQUIRE_OK(uapp_note_get_body(saved_view, &borrowed));
        CHECK(strcmp(borrowed, "Aggregate, accounting and potential methods.") == 0);
        uapp_note_free(&saved_view);
    }

    /* The search index moved with the content: new term hits, old term does not. */
    REQUIRE_OK(uapp_note_service_search(service, "potential", 0u, &results));
    REQUIRE_OK(uapp_search_results_count(results, &hits));
    CHECK(hits == 1u);
    uapp_search_results_free(&results);

    REQUIRE_OK(uapp_note_service_search(service, "average", 0u, &results));
    REQUIRE_OK(uapp_search_results_count(results, &hits));
    CHECK(hits == 0u);
    uapp_search_results_free(&results);

    uapp_note_free(&reopened);

    /* --- DELETE ----------------------------------------------------------- */
    REQUIRE_OK(uapp_note_service_delete(service, owned_id));
    REQUIRE_OK(uapp_note_service_count(service, &count));
    CHECK(count == 0u);
    CHECK_STATUS(uapp_note_service_open_note(service, owned_id, &reopened),
                 UAPP_STATUS_NOT_FOUND);
    CHECK(reopened == NULL);
    /* Deleting twice is NOT_FOUND, not a crash and not a silent success. */
    CHECK_STATUS(uapp_note_service_delete(service, owned_id), UAPP_STATUS_NOT_FOUND);
    /* The index went with it. */
    REQUIRE_OK(uapp_note_service_search(service, "potential", 0u, &results));
    REQUIRE_OK(uapp_search_results_count(results, &hits));
    CHECK(hits == 0u);
    uapp_search_results_free(&results);

    uapp_free_string(&owned_id);
    CHECK(owned_id == NULL);
    uapp_note_service_close(&service);
    CHECK(service == NULL);
}

static void test_slice_utf8_and_empty(void) {
    uapp_note_service_t* service = NULL;
    uapp_note_t* note = NULL;
    const char* borrowed = NULL;
    char* owned = NULL;

    REQUIRE_OK(open_memory_service(&service));

    /* Bytes in, identical bytes out — the ABI must not transcode anything. */
    REQUIRE_OK(uapp_note_service_create(service, "\xE0\xA4\xA8\xE0\xA5\x8B\xE0\xA4\x9F",
                                        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xF0\x9F\x93\x9D",
                                        &note));
    REQUIRE_OK(uapp_note_get_title(note, &borrowed));
    CHECK(strcmp(borrowed, "\xE0\xA4\xA8\xE0\xA5\x8B\xE0\xA4\x9F") == 0);
    REQUIRE_OK(uapp_note_copy_body(note, &owned));
    CHECK(strcmp(owned, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xF0\x9F\x93\x9D") == 0);
    uapp_free_string(&owned);

    /* Empty is a legal title and a legal body; only NULL is an error. */
    REQUIRE_OK(uapp_note_set_title(note, ""));
    REQUIRE_OK(uapp_note_set_body(note, ""));
    REQUIRE_OK(uapp_note_service_save(service, note));
    REQUIRE_OK(uapp_note_get_title(note, &borrowed));
    CHECK(borrowed[0] == '\0');

    uapp_note_free(&note);
    uapp_note_service_close(&service);
}

static void test_slice_persists_across_close(void) {
    /* The one case that needs a real file: prove the bytes survive the handle,
     * not just the process's memory. */
    const char* path = UAPP_CAPI_TEST_DB_PATH;
    uapp_note_service_t* service = NULL;
    uapp_note_t* note = NULL;
    char* id = NULL;
    const char* borrowed = NULL;
    size_t count = 0;

    remove(path);

    REQUIRE_OK(open_file_service(path, &service));
    REQUIRE_OK(uapp_note_service_create(service, "Durable", "Written to disk.", &note));
    REQUIRE_OK(uapp_note_copy_id_hex(note, &id));
    uapp_note_free(&note);
    uapp_note_service_close(&service);

    REQUIRE_OK(open_file_service(path, &service));
    REQUIRE_OK(uapp_note_service_count(service, &count));
    CHECK(count == 1u);
    REQUIRE_OK(uapp_note_service_open_note(service, id, &note));
    REQUIRE_OK(uapp_note_get_body(note, &borrowed));
    CHECK(strcmp(borrowed, "Written to disk.") == 0);

    uapp_note_free(&note);
    uapp_note_service_close(&service);
    uapp_free_string(&id);
    remove(path);
}

/*
 * Requirement: multiple independent handles in one process. Claiming it is not
 * enough; this is the proof.
 *
 * Two services, two private in-memory databases, alive at the same time. If
 * anything behind the ABI were a singleton — a cached connection, a global
 * service pointer, a process-wide id table — these assertions would fail.
 */
static void test_slice_independent_services(void) {
    uapp_note_service_t* alpha = NULL;
    uapp_note_service_t* beta = NULL;
    uapp_note_t* in_alpha = NULL;
    uapp_note_t* in_beta = NULL;
    uapp_note_t* leaked = NULL;
    uapp_search_results_t* results = NULL;
    char* alpha_id = NULL;
    char* beta_id = NULL;
    size_t count = 0;
    size_t hits = 0;

    REQUIRE_OK(open_memory_service(&alpha));
    REQUIRE_OK(open_memory_service(&beta));
    CHECK(alpha != beta);

    REQUIRE_OK(uapp_note_service_create(alpha, "Alpha only", "kangaroo", &in_alpha));
    REQUIRE_OK(uapp_note_copy_id_hex(in_alpha, &alpha_id));

    /* Beta must not have seen that write at all. */
    REQUIRE_OK(uapp_note_service_count(alpha, &count));
    CHECK(count == 1u);
    REQUIRE_OK(uapp_note_service_count(beta, &count));
    CHECK(count == 0u);
    CHECK_STATUS(uapp_note_service_open_note(beta, alpha_id, &leaked), UAPP_STATUS_NOT_FOUND);
    CHECK(leaked == NULL);
    REQUIRE_OK(uapp_note_service_search(beta, "kangaroo", 0u, &results));
    REQUIRE_OK(uapp_search_results_count(results, &hits));
    CHECK(hits == 0u);
    uapp_search_results_free(&results);

    /* Symmetrically, and with both services interleaved. */
    REQUIRE_OK(uapp_note_service_create(beta, "Beta only", "platypus", &in_beta));
    REQUIRE_OK(uapp_note_copy_id_hex(in_beta, &beta_id));
    CHECK(strcmp(alpha_id, beta_id) != 0);

    CHECK_STATUS(uapp_note_service_open_note(alpha, beta_id, &leaked), UAPP_STATUS_NOT_FOUND);
    REQUIRE_OK(uapp_note_service_search(alpha, "platypus", 0u, &results));
    REQUIRE_OK(uapp_search_results_count(results, &hits));
    CHECK(hits == 0u);
    uapp_search_results_free(&results);
    REQUIRE_OK(uapp_note_service_search(alpha, "kangaroo", 0u, &results));
    REQUIRE_OK(uapp_search_results_count(results, &hits));
    CHECK(hits == 1u);
    uapp_search_results_free(&results);

    REQUIRE_OK(uapp_note_service_count(alpha, &count));
    CHECK(count == 1u);
    REQUIRE_OK(uapp_note_service_count(beta, &count));
    CHECK(count == 1u);

    /* Closing one must not disturb the other. */
    uapp_note_service_close(&alpha);
    REQUIRE_OK(uapp_note_service_count(beta, &count));
    CHECK(count == 1u);

    /* A note handle is a self-contained snapshot: it outlives its service. */
    {
        const char* borrowed = NULL;
        REQUIRE_OK(uapp_note_get_title(in_alpha, &borrowed));
        CHECK(strcmp(borrowed, "Alpha only") == 0);
    }
    /* But saving it through the CLOSED service is impossible — the caller's
     * pointer is NULL, so this is a clean rejection rather than a crash. */
    CHECK_STATUS(uapp_note_service_save(alpha, in_alpha), UAPP_STATUS_INVALID_ARGUMENT);

    uapp_note_free(&in_alpha);
    uapp_note_free(&in_beta);
    uapp_free_string(&alpha_id);
    uapp_free_string(&beta_id);
    uapp_note_service_close(&beta);
}

static void test_slice_search_limit(void) {
    uapp_note_service_t* service = NULL;
    uapp_search_results_t* results = NULL;
    size_t hits = 0;
    int i;

    REQUIRE_OK(open_memory_service(&service));
    for (i = 0; i < 5; ++i) {
        uapp_note_t* note = NULL;
        REQUIRE_OK(uapp_note_service_create(service, "Topic", "recursion everywhere", &note));
        uapp_note_free(&note);
    }

    REQUIRE_OK(uapp_note_service_search(service, "recursion", 0u, &results)); /* 0 = default */
    REQUIRE_OK(uapp_search_results_count(results, &hits));
    CHECK(hits == 5u);
    uapp_search_results_free(&results);

    REQUIRE_OK(uapp_note_service_search(service, "recursion", 2u, &results));
    REQUIRE_OK(uapp_search_results_count(results, &hits));
    CHECK(hits == 2u);
    uapp_search_results_free(&results);

    /* Above the maximum is CLAMPED, not rejected. */
    REQUIRE_OK(uapp_note_service_search(service, "recursion", (size_t)1000000, &results));
    REQUIRE_OK(uapp_search_results_count(results, &hits));
    CHECK(hits == 5u);
    uapp_search_results_free(&results);

    uapp_note_service_close(&service);
}

/* ===========================================================================
 * Boundary safety. Every one of these is a native crash in a careless C API.
 * =========================================================================== */

static void test_boundary_null_handles(void) {
    uapp_note_service_t* service = NULL;
    uapp_note_t* note = NULL;
    uapp_search_results_t* results = NULL;
    size_t count = 123;
    const char* borrowed = NULL;
    char* owned = NULL;
    int64_t when = 0;
    double rank = 0.0;

    /* A NULL service handle on every entry point that takes one. */
    CHECK_STATUS(uapp_note_service_create(NULL, "t", "b", &note), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(note == NULL);
    CHECK_STATUS(uapp_note_service_open_note(NULL, "00000000000000000000000000000000", &note),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_service_save(NULL, NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_service_delete(NULL, "00000000000000000000000000000000"),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_service_count(NULL, &count), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(count == 123u); /* a failed call must not touch the out-parameter */
    CHECK_STATUS(uapp_note_service_search(NULL, "q", 0u, &results), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(results == NULL);

    /* A NULL note handle on every note accessor. */
    CHECK_STATUS(uapp_note_get_id_hex(NULL, &borrowed), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_get_title(NULL, &borrowed), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_get_body(NULL, &borrowed), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_get_title_length(NULL, &count), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_get_body_length(NULL, &count), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_get_timestamps(NULL, &when, &when), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_copy_id_hex(NULL, &owned), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(owned == NULL);
    CHECK_STATUS(uapp_note_copy_title(NULL, &owned), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_copy_body(NULL, &owned), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_set_title(NULL, "t"), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_set_body(NULL, "b"), UAPP_STATUS_INVALID_ARGUMENT);

    /* A NULL results handle on every row accessor. */
    CHECK_STATUS(uapp_search_results_count(NULL, &count), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_search_results_get_id_hex(NULL, 0u, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_search_results_get_title(NULL, 0u, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_search_results_get_snippet(NULL, 0u, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_search_results_get_rank(NULL, 0u, &rank), UAPP_STATUS_INVALID_ARGUMENT);

    /* Destructors are total: NULL, and a pointer to NULL, are both fine. */
    uapp_note_service_close(NULL);
    uapp_note_service_close(&service);
    uapp_note_free(NULL);
    uapp_note_free(&note);
    uapp_search_results_free(NULL);
    uapp_search_results_free(&results);
    uapp_free_string(NULL);
    uapp_free_string(&owned);
}

static void test_boundary_null_strings_and_out_params(void) {
    uapp_note_service_t* service = NULL;
    uapp_note_t* note = NULL;

    REQUIRE_OK(open_memory_service(&service));

    /* NULL where a string is expected. Note that "" is legal and NULL is not:
     * an empty note is a real thing, an absent pointer is a caller bug. */
    CHECK_STATUS(uapp_note_service_create(service, NULL, "b", &note),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(note == NULL);
    CHECK_STATUS(uapp_note_service_create(service, "t", NULL, &note),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_service_open_note(service, NULL, &note),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_service_delete(service, NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_service_search(service, NULL, 0u, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);

    /* NULL where an out-parameter is expected. */
    CHECK_STATUS(uapp_note_service_create(service, "t", "b", NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_service_open_note(service, "00000000000000000000000000000000", NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_service_count(service, NULL), UAPP_STATUS_INVALID_ARGUMENT);

    REQUIRE_OK(uapp_note_service_create(service, "t", "b", &note));
    CHECK_STATUS(uapp_note_get_title(note, NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_get_body(note, NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_get_id_hex(note, NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_copy_title(note, NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_set_title(note, NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_set_body(note, NULL), UAPP_STATUS_INVALID_ARGUMENT);
    /* Both timestamp out-parameters are optional, so this one IS legal. */
    CHECK_OK(uapp_note_get_timestamps(note, NULL, NULL));

    /* The out-parameter rule, stated as a test. A FAILING call must leave the
     * caller's variable exactly as it found it. The obvious-looking
     * alternative — null the handle on failure so the caller "sees something
     * safe" — would silently drop the live handle held here, which is a leak
     * the caller has no way to observe. */
    {
        uapp_note_t* const live = note;
        CHECK_STATUS(uapp_note_service_create(service, NULL, "b", &note),
                     UAPP_STATUS_INVALID_ARGUMENT);
        CHECK(note == live);
    }

    uapp_note_free(&note);
    uapp_note_service_close(&service);
}

/*
 * Use after close.
 *
 * The destructors take a pointer TO the handle and NULL it, so the ordinary
 * mistake — keep using the variable you just closed — degrades to a clean
 * INVALID_ARGUMENT instead of a use-after-free. That is the whole reason for
 * the double-pointer signature, and this is the case that proves it.
 *
 * What this does NOT claim: that calling through a SEPARATE copy of the
 * pointer you saved before closing is detected. That is use-after-free and no
 * C API can catch it. The header says so.
 */
static void test_boundary_use_after_close(void) {
    uapp_note_service_t* service = NULL;
    uapp_note_t* note = NULL;
    uapp_search_results_t* results = NULL;
    size_t count = 7;
    const char* borrowed = "untouched";
    char* owned = NULL;

    REQUIRE_OK(open_memory_service(&service));
    REQUIRE_OK(uapp_note_service_create(service, "Closing time", "body", &note));
    REQUIRE_OK(uapp_note_service_search(service, "closing", 0u, &results));

    uapp_note_service_close(&service);
    CHECK(service == NULL);
    /* Closing twice is a no-op, not a double free. */
    uapp_note_service_close(&service);
    CHECK(service == NULL);

    /* Every operation on the closed service is now a defined rejection. */
    CHECK_STATUS(uapp_note_service_count(service, &count), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(count == 7u);
    CHECK_STATUS(uapp_note_service_save(service, note), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_service_delete(service, "00000000000000000000000000000000"),
                 UAPP_STATUS_INVALID_ARGUMENT);
    /* A fresh variable: `note` below is still a live handle, and now that a
     * failed call leaves out-parameters untouched, reusing it here would be a
     * leak in this test rather than a bug in the library. (It was exactly
     * that, until macOS `leaks` said so.) */
    {
        uapp_note_t* fresh = NULL;
        CHECK_STATUS(uapp_note_service_create(service, "t", "b", &fresh),
                     UAPP_STATUS_INVALID_ARGUMENT);
        CHECK(fresh == NULL);
    }

    /* Same discipline for the other two handle types. */
    uapp_note_free(&note);
    CHECK(note == NULL);
    uapp_note_free(&note);
    CHECK_STATUS(uapp_note_get_title(note, &borrowed), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(strcmp(borrowed, "untouched") == 0);
    CHECK_STATUS(uapp_note_copy_title(note, &owned), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(owned == NULL);

    uapp_search_results_free(&results);
    CHECK(results == NULL);
    uapp_search_results_free(&results);
    CHECK_STATUS(uapp_search_results_count(results, &count), UAPP_STATUS_INVALID_ARGUMENT);
}

/*
 * A handle of the wrong type.
 *
 * This is not a contrived scenario: it is what a generated binding does when
 * two `long` handle fields get swapped, and on the Kotlin/Swift side both are
 * just opaque integers with nothing to distinguish them. Every handle carries
 * a distinct magic cookie as its first member precisely so this is caught
 * instead of dereferenced.
 */
static void test_boundary_wrong_handle_type(void) {
    uapp_note_service_t* service = NULL;
    uapp_note_t* note = NULL;
    uapp_search_results_t* results = NULL;
    size_t count = 0;
    const char* borrowed = NULL;

    REQUIRE_OK(open_memory_service(&service));
    REQUIRE_OK(uapp_note_service_create(service, "Mixed up", "body", &note));
    REQUIRE_OK(uapp_note_service_search(service, "mixed", 0u, &results));

    /* A live note handle passed where a service is expected, and vice versa. */
    CHECK_STATUS(uapp_note_service_count((uapp_note_service_t*)note, &count),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_get_title((const uapp_note_t*)service, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_search_results_count((const uapp_search_results_t*)note, &count),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_get_title((const uapp_note_t*)results, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);

    uapp_search_results_free(&results);
    uapp_note_free(&note);
    uapp_note_service_close(&service);
}

static void test_boundary_bad_ids_and_validation(void) {
    uapp_note_service_t* service = NULL;
    uapp_note_t* note = NULL;
    char* oversized = NULL;
    const char* borrowed = NULL;
    uapp_note_limits_t limits;

    memset(&limits, 0, sizeof limits);
    limits.struct_size = (uint32_t)sizeof limits;
    REQUIRE_OK(uapp_note_get_limits(&limits));
    REQUIRE_OK(open_memory_service(&service));

    /* A malformed id is told apart from an id that is simply absent. */
    CHECK_STATUS(uapp_note_service_open_note(service, "", &note),
                 UAPP_STATUS_NOTE_ID_MALFORMED);
    CHECK_STATUS(uapp_note_service_open_note(service, "not-hex", &note),
                 UAPP_STATUS_NOTE_ID_MALFORMED);
    CHECK_STATUS(uapp_note_service_open_note(service, "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", &note),
                 UAPP_STATUS_NOTE_ID_MALFORMED);
    CHECK_STATUS(uapp_note_service_open_note(service, "0123456789abcdef", &note),
                 UAPP_STATUS_NOTE_ID_MALFORMED);
    CHECK_STATUS(uapp_note_service_delete(service, "not-hex"), UAPP_STATUS_NOTE_ID_MALFORMED);
    /* Well-formed but absent. */
    CHECK_STATUS(uapp_note_service_open_note(service, "0123456789abcdef0123456789abcdef", &note),
                 UAPP_STATUS_NOT_FOUND);
    CHECK(note == NULL);

    /* Oversized title: rejected at create AND at edit, with the same code. */
    oversized = (char*)malloc(limits.max_title_bytes + 8u);
    REQUIRE_OK(oversized != NULL ? UAPP_STATUS_OK : UAPP_STATUS_OUT_OF_MEMORY);
    memset(oversized, 'x', limits.max_title_bytes + 7u);
    oversized[limits.max_title_bytes + 7u] = '\0';

    CHECK_STATUS(uapp_note_service_create(service, oversized, "b", &note),
                 UAPP_STATUS_NOTE_TITLE_TOO_LONG);
    CHECK(note == NULL);

    REQUIRE_OK(uapp_note_service_create(service, "ok", "b", &note));
    CHECK_STATUS(uapp_note_set_title(note, oversized), UAPP_STATUS_NOTE_TITLE_TOO_LONG);
    /* A rejected edit leaves the handle exactly as it was. */
    REQUIRE_OK(uapp_note_get_title(note, &borrowed));
    CHECK(strcmp(borrowed, "ok") == 0);
    free(oversized);

    /* A query with no searchable terms is its own status, distinct from
     * "searched and found nothing" — the UI must say different things. */
    {
        uapp_search_results_t* results = NULL;
        CHECK_STATUS(uapp_note_service_search(service, "!!! ???", 0u, &results),
                     UAPP_STATUS_SEARCH_QUERY_EMPTY);
        CHECK(results == NULL);
        CHECK_STATUS(uapp_note_service_search(service, "", 0u, &results),
                     UAPP_STATUS_SEARCH_QUERY_EMPTY);
    }

    uapp_note_free(&note);
    uapp_note_service_close(&service);
}

static void test_boundary_config_and_struct_size(void) {
    uapp_note_service_config_t config;
    uapp_note_service_t* service = NULL;
    uapp_note_limits_t limits;

    CHECK_STATUS(uapp_note_service_config_init(NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_service_open(NULL, &service), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(service == NULL);
    REQUIRE_OK(uapp_note_service_config_init(&config));
    CHECK(config.struct_size == (uint32_t)sizeof config);
    CHECK_STATUS(uapp_note_service_open(&config, NULL), UAPP_STATUS_INVALID_ARGUMENT);

    /* config_init leaves database_path NULL on purpose: there is no sensible
     * default for it, and inventing one would put a database somewhere the
     * caller did not ask for. */
    CHECK_STATUS(uapp_note_service_open(&config, &service), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(service == NULL);

    /* A struct_size smaller than this build's is rejected: honouring it would
     * mean writing past the end of the caller's object. */
    config.database_path = ":memory:";
    config.struct_size = 0u;
    CHECK_STATUS(uapp_note_service_open(&config, &service), UAPP_STATUS_INVALID_ARGUMENT);
    config.struct_size = (uint32_t)(sizeof config - 1u);
    CHECK_STATUS(uapp_note_service_open(&config, &service), UAPP_STATUS_INVALID_ARGUMENT);
    /* A caller from the future, with a LARGER struct, is accepted. */
    config.struct_size = (uint32_t)(sizeof config + 64u);
    REQUIRE_OK(uapp_note_service_open(&config, &service));
    uapp_note_service_close(&service);

    /* Negative timeouts are nonsense, not a clamp. */
    REQUIRE_OK(uapp_note_service_config_init(&config));
    config.database_path = ":memory:";
    config.busy_timeout_ms = -1;
    CHECK_STATUS(uapp_note_service_open(&config, &service), UAPP_STATUS_INVALID_ARGUMENT);

    /* Same rule on the limits struct. */
    CHECK_STATUS(uapp_note_get_limits(NULL), UAPP_STATUS_INVALID_ARGUMENT);
    memset(&limits, 0, sizeof limits);
    limits.struct_size = 0u;
    CHECK_STATUS(uapp_note_get_limits(&limits), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(limits.max_title_bytes == 0u); /* nothing written on rejection */
}

static void test_boundary_unopenable_database(void) {
    uapp_note_service_t* service = NULL;
    uapp_note_service_config_t config;

    REQUIRE_OK(uapp_note_service_config_init(&config));
    config.database_path = "/nonexistent-directory-uapp-capi/notes.db";
    /* A real failure from deep in the adapter must arrive as a status, with
     * the code intact, rather than as an abort. */
    CHECK_STATUS(uapp_note_service_open(&config, &service), UAPP_STATUS_STORE_OPEN_FAILED);
    CHECK(service == NULL);

    REQUIRE_OK(uapp_note_service_config_init(&config));
    config.database_path = "";
    CHECK_STATUS(uapp_note_service_open(&config, &service), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(service == NULL);
}

static void test_boundary_search_index_bounds(void) {
    uapp_note_service_t* service = NULL;
    uapp_note_t* note = NULL;
    uapp_search_results_t* results = NULL;
    const char* borrowed = NULL;
    double rank = 0.0;
    size_t hits = 0;

    REQUIRE_OK(open_memory_service(&service));
    REQUIRE_OK(uapp_note_service_create(service, "Indexed", "hippopotamus", &note));
    uapp_note_free(&note);
    REQUIRE_OK(uapp_note_service_search(service, "hippopotamus", 0u, &results));
    REQUIRE_OK(uapp_search_results_count(results, &hits));
    CHECK(hits == 1u);

    /* One past the end, and a wildly out-of-range index. A loop-bound bug is a
     * caller error (INVALID_ARGUMENT), not a missing note (NOT_FOUND). */
    CHECK_STATUS(uapp_search_results_get_id_hex(results, 1u, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_search_results_get_title(results, 99u, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_search_results_get_snippet(results, (size_t)-1, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_search_results_get_rank(results, 1u, &rank),
                 UAPP_STATUS_INVALID_ARGUMENT);
    /* NULL out-parameter with a valid index. */
    CHECK_STATUS(uapp_search_results_get_title(results, 0u, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_search_results_count(results, NULL), UAPP_STATUS_INVALID_ARGUMENT);

    /* An empty result set has no valid index at all. */
    uapp_search_results_free(&results);
    REQUIRE_OK(uapp_note_service_search(service, "rhinoceros", 0u, &results));
    CHECK_STATUS(uapp_search_results_get_title(results, 0u, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);

    uapp_search_results_free(&results);
    uapp_note_service_close(&service);
}

static void test_boundary_save_after_delete(void) {
    uapp_note_service_t* service = NULL;
    uapp_note_t* note = NULL;
    char* id = NULL;

    REQUIRE_OK(open_memory_service(&service));
    REQUIRE_OK(uapp_note_service_create(service, "Doomed", "body", &note));
    REQUIRE_OK(uapp_note_copy_id_hex(note, &id));
    REQUIRE_OK(uapp_note_service_delete(service, id));

    /* The handle is still a perfectly valid snapshot of a note that no longer
     * exists. Saving it must NOT resurrect the row — save never creates. */
    REQUIRE_OK(uapp_note_set_title(note, "Back from the dead"));
    CHECK_STATUS(uapp_note_service_save(service, note), UAPP_STATUS_NOT_FOUND);
    {
        size_t count = 99;
        REQUIRE_OK(uapp_note_service_count(service, &count));
        CHECK(count == 0u);
    }

    uapp_free_string(&id);
    uapp_note_free(&note);
    uapp_note_service_close(&service);
}

/* ===========================================================================
 * Harness
 * =========================================================================== */

typedef struct test_case {
    const char* name;
    void (*fn)(void);
} test_case_t;

static const test_case_t k_cases[] = {
    {"slice: abi version is exported and negotiable", test_abi_version},
    {"slice: status codes and messages are total", test_status_strings},
    {"slice: limits are readable at runtime", test_limits},
    {"slice: create store search open edit save delete count", test_slice_full_round_trip},
    {"slice: utf-8 round trip and empty strings", test_slice_utf8_and_empty},
    {"slice: notes persist across close and reopen", test_slice_persists_across_close},
    {"slice: two independent services do not see each other", test_slice_independent_services},
    {"slice: search limit defaults and clamps", test_slice_search_limit},

    {"boundary: null handles are rejected everywhere", test_boundary_null_handles},
    {"boundary: null strings and null out-params", test_boundary_null_strings_and_out_params},
    {"boundary: use after close is a status, not a crash", test_boundary_use_after_close},
    {"boundary: a wrong-type handle is rejected", test_boundary_wrong_handle_type},
    {"boundary: malformed ids and oversized input", test_boundary_bad_ids_and_validation},
    {"boundary: config and struct_size versioning", test_boundary_config_and_struct_size},
    {"boundary: an unopenable database reports a status", test_boundary_unopenable_database},
    {"boundary: search result index bounds", test_boundary_search_index_bounds},
    {"boundary: save after delete does not resurrect", test_boundary_save_after_delete}
};

int main(int argc, char** argv) {
    const char* filter = (argc > 1) ? argv[1] : NULL;
    size_t i;
    int skipped = 0;

    printf("running uapp C ABI tests (abi %s)%s%s\n",
           uapp_abi_version_string(),
           filter != NULL ? " matching " : "",
           filter != NULL ? filter : "");

    for (i = 0; i < sizeof k_cases / sizeof k_cases[0]; ++i) {
        if (filter != NULL && strstr(k_cases[i].name, filter) == NULL) {
            ++skipped;
            continue;
        }
        g_checks_failed = 0;
        k_cases[i].fn();
        if (g_checks_failed == 0) {
            printf("  ok   %s\n", k_cases[i].name);
            ++g_cases_passed;
        } else {
            printf("  FAIL %s (%d assertion failure%s)\n", k_cases[i].name, g_checks_failed,
                   g_checks_failed == 1 ? "" : "s");
            ++g_cases_failed;
        }
    }

    printf("\n%d passed, %d failed", g_cases_passed, g_cases_failed);
    if (skipped > 0) {
        printf(", %d filtered out", skipped);
    }
    printf("\n");

    if (g_cases_passed == 0 && g_cases_failed == 0) {
        fputs("error: no test cases ran — check the filter\n", stderr);
        return 1;
    }
    return g_cases_failed == 0 ? 0 : 1;
}
