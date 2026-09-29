// SPDX-License-Identifier: Apache-2.0
//
// tests/support/test_framework.h — a ~120-line test harness.
//
// WHY NOT Catch2 / doctest / GoogleTest (the task asked for a justification):
//
//  1. ADR-0001 compiles the whole core with -fno-exceptions. Catch2 and
//     GoogleTest both require exceptions for their assertion and fixture
//     machinery; adopting either would mean either re-enabling exceptions for
//     test binaries (so tests would stop exercising the configuration we
//     actually ship) or fighting the framework. doctest can build without
//     exceptions but loses REQUIRE's early-exit semantics.
//  2. Spec section 35 requires a license/redistribution analysis and a
//     THIRD_PARTY_NOTICES entry before any new dependency lands. That is a
//     real review, not a FetchContent line, and it is not worth spending on
//     the bootstrap slice.
//  3. FetchContent makes `cmake ..` need the network. A build that cannot be
//     configured offline is a bad default for a project whose first principle
//     is offline-first.
//
// This is explicitly a bootstrap decision. When the suite outgrows it --
// parameterised cases, fixtures, XML output for CI -- revisit with a proper
// ADR. What is here covers: named cases, a registry, non-fatal CHECK, fatal
// REQUIRE, an optional name-substring filter, and a pass/fail count.

#ifndef UAPP_TESTS_SUPPORT_TEST_FRAMEWORK_H
#define UAPP_TESTS_SUPPORT_TEST_FRAMEWORK_H

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace uapp::test {

struct CaseResult final {
    int  failures = 0;
    bool aborted  = false;  ///< a REQUIRE failed and stopped the case early
};

using CaseFn = void (*)(CaseResult&);

struct TestCase final {
    const char* name;
    CaseFn      fn;
};

/// The registry. A function-local static so registration order across
/// translation units cannot bite us.
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar final {
    Registrar(const char* name, CaseFn fn) { registry().push_back(TestCase{name, fn}); }
};

inline void report_failure(CaseResult& r, const char* file, int line,
                           const char* expr, const std::string& detail) {
    ++r.failures;
    std::fprintf(stderr, "    FAIL %s:%d\n      expected: %s\n", file, line, expr);
    if (!detail.empty()) {
        std::fprintf(stderr, "      %s\n", detail.c_str());
    }
}

/// Run every registered case whose name contains `filter` (null = all).
/// Returns 0 if all passed.
inline int run_all(const char* filter) {
    int passed = 0;
    int failed = 0;
    int skipped = 0;

    for (const TestCase& tc : registry()) {
        if (filter != nullptr && std::strstr(tc.name, filter) == nullptr) {
            ++skipped;
            continue;
        }
        CaseResult r;
        tc.fn(r);
        if (r.failures == 0) {
            std::printf("  ok   %s\n", tc.name);
            ++passed;
        } else {
            std::printf("  FAIL %s (%d assertion failure%s)\n",
                        tc.name, r.failures, r.failures == 1 ? "" : "s");
            ++failed;
        }
    }

    std::printf("\n%d passed, %d failed", passed, failed);
    if (skipped > 0) { std::printf(", %d filtered out", skipped); }
    std::printf("\n");

    if (passed == 0 && failed == 0) {
        std::fputs("error: no test cases ran — check the filter\n", stderr);
        return 1;
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace uapp::test

// --- macros ----------------------------------------------------------------

#define UAPP_TEST_CONCAT_INNER(a, b) a##b
#define UAPP_TEST_CONCAT(a, b) UAPP_TEST_CONCAT_INNER(a, b)

/// Define a test case:  UAPP_TEST("note_id.parse rejects short input") { ... }
/// The CaseResult is available inside the body as `uapp_test_result`.
#define UAPP_TEST(name_literal)                                                       \
    static void UAPP_TEST_CONCAT(uapp_test_fn_, __LINE__)(::uapp::test::CaseResult&);  \
    static const ::uapp::test::Registrar UAPP_TEST_CONCAT(uapp_test_reg_, __LINE__)(   \
        name_literal, &UAPP_TEST_CONCAT(uapp_test_fn_, __LINE__));                     \
    static void UAPP_TEST_CONCAT(uapp_test_fn_, __LINE__)(                             \
        ::uapp::test::CaseResult& uapp_test_result)

/// Non-fatal: records the failure and keeps going.
#define UAPP_CHECK(cond)                                                              \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ::uapp::test::report_failure(uapp_test_result, __FILE__, __LINE__,         \
                                         #cond, std::string());                        \
        }                                                                             \
    } while (false)

#define UAPP_CHECK_MSG(cond, detail)                                                  \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ::uapp::test::report_failure(uapp_test_result, __FILE__, __LINE__,         \
                                         #cond, std::string(detail));                  \
        }                                                                             \
    } while (false)

/// Fatal: records the failure and returns from the case, so that a broken
/// precondition does not cascade into a wall of misleading errors.
#define UAPP_REQUIRE(cond)                                                            \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ::uapp::test::report_failure(uapp_test_result, __FILE__, __LINE__,         \
                                         #cond, std::string());                        \
            uapp_test_result.aborted = true;                                           \
            return;                                                                    \
        }                                                                             \
    } while (false)

/// Assert an exact uapp::Status, printing both codes on mismatch. Fatal.
#define UAPP_REQUIRE_STATUS(expr, expected)                                           \
    do {                                                                              \
        const ::uapp::Status uapp_actual_status_ = (expr);                             \
        if (uapp_actual_status_ != (expected)) {                                        \
            ::uapp::test::report_failure(                                               \
                uapp_test_result, __FILE__, __LINE__, #expr " == " #expected,           \
                std::string("got ") + ::uapp::status_code(uapp_actual_status_) +        \
                    ", wanted " + ::uapp::status_code(expected));                       \
            uapp_test_result.aborted = true;                                            \
            return;                                                                     \
        }                                                                               \
    } while (false)

#define UAPP_REQUIRE_OK(expr) UAPP_REQUIRE_STATUS(expr, ::uapp::Status::Ok)

#endif  // UAPP_TESTS_SUPPORT_TEST_FRAMEWORK_H
