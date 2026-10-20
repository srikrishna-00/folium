// SPDX-License-Identifier: Apache-2.0
//
// tools/loadtest/loadtest_util.h — the measurement primitives shared by every
// load-test binary in this directory.
//
// WHAT THIS IS. The project's own performance strategy (docs/performance-
// strategy.md §6) says micro-benchmarks belong in benchmarks/ under Google
// Benchmark, and device macro-benchmarks belong on reserved hardware. Neither
// exists yet, and neither answers the question this harness answers: "as the
// data volume grows from 10 to 100 000, does each engine stay smooth, and when
// it stops being smooth, does it fail with a real Status or fall over?"
//
// So this is deliberately NOT a micro-benchmark framework. It does not do
// warm-up iterations, statistical repetition or p95 estimation, because the
// thing under test is a SCALE CURVE, not a single operation's distribution.
// Each row it emits is one (module, phase, scale) point measured once, on real
// data, against a real on-disk SQLite file or a real Tesseract.
//
// THREE MEASUREMENTS PER POINT, which is what §6 asks a harness to record:
//   * wall clock       — steady_clock, total and divided per operation
//   * peak RSS         — getrusage(RUSAGE_SELF).ru_maxrss, which is a
//                        high-water mark for the whole process and therefore
//                        monotonic: it answers "did this module ever need more
//                        than the ADR-0002 Tier D ceiling of 220 MB?"
//   * current RSS      — needed because peak RSS cannot show a LEAK. A leak is
//                        a floor that rises; the peak would hide it. This is
//                        the measurement the OCR repeated-call test is built
//                        on (ocr_loadtest.cpp).
//
// ADR-0001 forbids <iostream>, so output goes through <cstdio>, exactly as
// tools/notes_cli does.
//
// OUTPUT FORMAT is one CSV row per measurement on stdout, flushed as it goes,
// so an interrupted run still leaves every point it already reached. Human
// commentary goes to stderr so the two streams can be separated with a pipe.

#ifndef UAPP_TOOLS_LOADTEST_LOADTEST_UTIL_H
#define UAPP_TOOLS_LOADTEST_LOADTEST_UTIL_H

#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/resource.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include "uapp/status.h"

namespace uapp::loadtest {

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

/// steady_clock, not system_clock: a load test must not be perturbed by NTP.
class Timer final {
public:
    Timer() noexcept : start_(std::chrono::steady_clock::now()) {}

    void reset() noexcept { start_ = std::chrono::steady_clock::now(); }

    [[nodiscard]] double ms() const noexcept {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(now - start_).count();
    }

private:
    std::chrono::steady_clock::time_point start_;
};

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------

/// Process-lifetime high-water mark of resident set size, in MiB.
///
/// PORTABILITY NOTE, and it matters: `ru_maxrss` is in BYTES on macOS/BSD and
/// in KIBIBYTES on Linux. Getting this wrong is a 1024x reporting error, which
/// is why it is written once, here, and not at each call site.
[[nodiscard]] inline double peak_rss_mib() noexcept {
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) { return -1.0; }
#if defined(__APPLE__) || defined(__FreeBSD__)
    return static_cast<double>(ru.ru_maxrss) / (1024.0 * 1024.0);
#else
    return static_cast<double>(ru.ru_maxrss) / 1024.0;
#endif
}

/// Current (not peak) resident set size in MiB, or -1 if unavailable.
///
/// This is the leak detector. `peak_rss_mib()` is monotonic, so a steadily
/// growing allocation floor looks identical to a single early spike. Sampling
/// the CURRENT value across repeated identical operations is the only way to
/// tell "this module reuses its buffers" from "this module leaks one buffer
/// per call".
[[nodiscard]] inline double current_rss_mib() noexcept {
#if defined(__APPLE__)
    mach_task_basic_info        info{};
    mach_msg_type_number_t      count = MACH_TASK_BASIC_INFO_COUNT;
    const kern_return_t         kr =
        task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count);
    if (kr != KERN_SUCCESS) { return -1.0; }
    return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
#else
    std::FILE* f = std::fopen("/proc/self/statm", "re");
    if (f == nullptr) { return -1.0; }
    long long total_pages = 0;
    long long rss_pages   = 0;
    const int n = std::fscanf(f, "%lld %lld", &total_pages, &rss_pages);
    std::fclose(f);
    if (n != 2) { return -1.0; }
    const double page = 4096.0;
    return static_cast<double>(rss_pages) * page / (1024.0 * 1024.0);
#endif
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

/// Printed once per process, before any row.
inline void emit_csv_header() {
    std::printf("module,phase,scale,ops,total_ms,per_op_us,peak_rss_mib,verdict,detail\n");
    std::fflush(stdout);
}

/// One measured point. `verdict` is one of ok / limit / slow / FAIL, so a
/// reader can grep for trouble without reading the numbers.
inline void emit_row(const char* module, const char* phase, long long scale, long long ops,
                     double total_ms, const char* verdict, const char* detail) {
    const double per_op_us =
        ops > 0 ? (total_ms * 1000.0) / static_cast<double>(ops) : 0.0;
    std::printf("%s,%s,%lld,%lld,%.3f,%.3f,%.2f,%s,%s\n", module, phase, scale, ops,
                total_ms, per_op_us, peak_rss_mib(), verdict, detail);
    std::fflush(stdout);
}

/// Progress/commentary. stderr, so it never pollutes the CSV.
__attribute__((format(printf, 1, 2))) inline void note(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

/// Format a Status as "code(message)" for the CSV `detail` column. Commas are
/// not possible in either field (both are fixed identifiers), so no quoting is
/// needed, and the file stays greppable.
[[nodiscard]] inline std::string status_text(Status s) {
    std::string out = status_code(s);
    out += "|";
    out += status_message(s);
    return out;
}

/// A fatal problem in the harness itself (not in the code under test).
[[noreturn]] inline void die(const char* what, Status s) {
    std::fprintf(stderr, "loadtest: FATAL %s: %s (%s)\n", what, status_code(s),
                 status_message(s));
    std::exit(1);
}

// ---------------------------------------------------------------------------
// Scale points
// ---------------------------------------------------------------------------

/// The canonical ladder. Every module walks as much of this as its own
/// documented kMax* ceiling allows; where the ceiling bites first, that is the
/// result, and the module's runner says so with verdict=limit.
inline constexpr long long kScales[]  = {10, 100, 1000, 10000, 100000};
inline constexpr std::size_t kScaleCount = sizeof(kScales) / sizeof(kScales[0]);

}  // namespace uapp::loadtest

#endif  // UAPP_TOOLS_LOADTEST_LOADTEST_UTIL_H
