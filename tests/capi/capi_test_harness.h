/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/capi/capi_test_harness.h -- the ~70-line C test harness, shared.
 *
 * WHY THIS FILE EXISTS, AND WHY note_capi_test.c DOES NOT USE IT
 *
 * tests/capi/note_capi_test.c carries this harness inline, with the comment
 * "small because anything larger would be a second thing to trust". That was
 * right when there was one C test file. With three, keeping it inline would
 * mean three copies of the macros that decide whether a test passed -- and
 * three places for them to drift.
 *
 * So it is extracted here, for the two NEW files. note_capi_test.c is left
 * exactly as it was ON PURPOSE: it is a passing suite guarding a frozen ABI
 * surface, and rewriting it to share a header would be a non-functional edit
 * to the one file whose job is to prove the note boundary did not move. The
 * duplication is the cheaper of the two risks, and it is recorded here rather
 * than left for a reviewer to notice.
 *
 * Still C99, still no C++ anywhere, still includes only the PUBLIC capi
 * header. Everything is `static`, so each test translation unit gets its own
 * copy with no linker involvement -- which is also why there is no .c file to
 * go with this .h.
 */

#ifndef UAPP_TESTS_CAPI_CAPI_TEST_HARNESS_H
#define UAPP_TESTS_CAPI_CAPI_TEST_HARNESS_H

#include <stdio.h>
#include <string.h>

#include "uapp/capi/uapp_capi.h"

static int g_checks_failed; /* failures inside the case currently running */
static int g_cases_passed;
static int g_cases_failed;

static void fail_at(const char* file, int line, const char* what, const char* detail) {
    ++g_checks_failed;
    fprintf(stderr, "    FAIL %s:%d\n      expected: %s\n", file, line, what);
    if (detail != NULL && detail[0] != '\0') {
        fprintf(stderr, "      %s\n", detail);
    }
}

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) { fail_at(__FILE__, __LINE__, #cond, ""); } \
    } while (0)

/* Assert an exact status, printing both stable code strings on mismatch --
 * "got STORE_WRITE_FAILED, wanted NOT_FOUND" beats "got 303, wanted 101". */
#define CHECK_STATUS(expr, expected)                                      \
    do {                                                                  \
        const uapp_status_t actual_ = (expr);                             \
        if (actual_ != (expected)) {                                      \
            char detail_[160];                                            \
            snprintf(detail_, sizeof detail_, "got %s, wanted %s",        \
                     uapp_status_code(actual_),                           \
                     uapp_status_code((uapp_status_t)(expected)));        \
            fail_at(__FILE__, __LINE__, #expr " == " #expected, detail_); \
        }                                                                 \
    } while (0)

#define CHECK_OK(expr) CHECK_STATUS((expr), UAPP_STATUS_OK)

/* Fatal: a broken precondition must not cascade into a wall of noise. */
#define REQUIRE_OK(expr)                                \
    do {                                                \
        const uapp_status_t r_ = (expr);                \
        if (r_ != UAPP_STATUS_OK) {                     \
            fail_at(__FILE__, __LINE__, #expr " == OK", \
                    uapp_status_code(r_));              \
            return;                                     \
        }                                               \
    } while (0)

/* Assert a borrowed string equals a literal, printing what was actually
 * there. Never dereferences NULL -- a NULL where a string was promised is
 * itself a failure, and must not take the test process down with it. */
#define CHECK_STR(actual, expected)                                            \
    do {                                                                       \
        const char* a_ = (actual);                                             \
        if (a_ == NULL) {                                                      \
            fail_at(__FILE__, __LINE__, #actual " != NULL", "got NULL");       \
        } else if (strcmp(a_, (expected)) != 0) {                              \
            char detail_[256];                                                 \
            snprintf(detail_, sizeof detail_, "got \"%s\", wanted \"%s\"",     \
                     a_, (expected));                                          \
            fail_at(__FILE__, __LINE__, #actual " == " #expected, detail_);    \
        }                                                                      \
    } while (0)

typedef struct uapp_test_case {
    const char* name;
    void (*fn)(void);
} uapp_test_case_t;

/*
 * Run the cases whose name contains `filter` (NULL runs all). Returns a
 * process exit code. Running NOTHING is a failure: a filter that matches no
 * case is a CTest entry that silently tests nothing, which is worse than a
 * red build.
 */
static int uapp_run_test_cases(const char* suite,
                               const uapp_test_case_t* cases,
                               size_t count,
                               const char* filter) {
    size_t i;
    int skipped = 0;

    printf("running %s (abi %s)%s%s\n", suite, uapp_abi_version_string(),
           filter != NULL ? " matching " : "", filter != NULL ? filter : "");

    for (i = 0; i < count; ++i) {
        if (filter != NULL && strstr(cases[i].name, filter) == NULL) {
            ++skipped;
            continue;
        }
        g_checks_failed = 0;
        cases[i].fn();
        if (g_checks_failed == 0) {
            printf("  ok   %s\n", cases[i].name);
            ++g_cases_passed;
        } else {
            printf("  FAIL %s (%d assertion failure%s)\n", cases[i].name, g_checks_failed,
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
        fputs("error: no test cases ran -- check the filter\n", stderr);
        return 1;
    }
    return g_cases_failed == 0 ? 0 : 1;
}

#endif /* UAPP_TESTS_CAPI_CAPI_TEST_HARNESS_H */
