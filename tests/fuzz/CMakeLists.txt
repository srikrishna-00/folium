# ---------------------------------------------------------------------------
# tests/fuzz/ — libFuzzer targets (testing-strategy.md §6).
#
# Reached only from the root CMakeLists.txt under UAPP_ENABLE_FUZZING, which
# is OFF by default. Nothing here is compiled, configured or even parsed in a
# normal build, and no target here is registered with ctest: a fuzz target has
# no pass/fail, it has a time budget.
#
# TWO TARGETS, DELIBERATELY. testing-strategy.md §6.1 lists eighteen; these
# are the only two whose subject exists: `fuzz_detect` over
# file-engine/detection, and `fuzz_pdf_reader` over pdf-engine/reader. The
# rest name parsers (TIFF, XML, CBOR sync deltas, OOXML) that are still empty
# Phase 0 directories — adding stub targets for them would turn a real gate
# into a checklist of harnesses that fuzz nothing.
# ---------------------------------------------------------------------------

add_executable(fuzz_file_detection fuzz_file_detection.cpp)

target_link_libraries(fuzz_file_detection
    PRIVATE uapp::file_detection uapp::compiler_options)

# The library and the harness are both already compiled with
# `-fsanitize=fuzzer-no-link,address,undefined` from uapp::compiler_options,
# which is what gives libFuzzer its coverage feedback. What is added HERE, and
# only here, is the fuzzer runtime itself — it provides `main`, so it must not
# reach any other executable in the project.
target_compile_options(fuzz_file_detection PRIVATE -fsanitize=fuzzer)
target_link_options(fuzz_file_detection PRIVATE -fsanitize=fuzzer)

# ---------------------------------------------------------------------------
# fuzz_pdf_reader — testing-strategy.md §6.1's first-listed target, covering
# both `fuzz_pdf_parse` and `fuzz_pdf_render` in one binary (see the header
# comment in fuzz_pdf_reader.cpp for why they are not split).
#
# CONDITIONAL, on exactly the same variable tests/CMakeLists.txt uses for the
# PDF unit tests. pdf-engine/ is only added when a vendored PDFium was found
# (root CMakeLists), so on a machine that has not run
# third_party/pdfium/fetch_pdfium.sh this target silently does not exist and
# the fuzz build still produces fuzz_file_detection. That is the AUTO
# contract, not a special case for fuzzing: a missing third-party binary must
# never turn into a configure error for someone who did not ask for PDF.
# ---------------------------------------------------------------------------
if(UAPP_PDF_ENABLED)
    add_executable(fuzz_pdf_reader fuzz_pdf_reader.cpp)

    # uapp::pdf_reader only. Not uapp::file_detection, not uapp::storage — the
    # same link-line guard rail tests/CMakeLists.txt applies to uapp_pdf_tests,
    # for the same reason: "the PDF reader is bytes in, pixels out, with no
    # storage and no document model behind it" should be checked by the build
    # graph. It also proves the header boundary under the fuzz configuration —
    # this harness includes only uapp/pdf/document.h and would fail to compile
    # if a PDFium type had leaked into the public interface.
    target_link_libraries(fuzz_pdf_reader
        PRIVATE uapp::pdf_reader uapp::compiler_options)

    target_compile_options(fuzz_pdf_reader PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_pdf_reader PRIVATE -fsanitize=fuzzer)
else()
    message(STATUS
        "uapp: fuzz_pdf_reader        = OFF (no vendored PDFium; "
        "run third_party/pdfium/fetch_pdfium.sh)")
endif()

# The seed corpus lives with the other fixtures (§6.2: "Seed corpora checked
# into tests/fixtures/corpus/<target>/"), not next to this file, so the
# eventual CI step that syncs and minimises corpora has one directory to look
# at. It is passed on the command line rather than compiled in, because
# libFuzzer WRITES to its corpus directory and a long run must be pointed at
# scratch space instead.

# ---------------------------------------------------------------------------
# HOW TO RUN IT
#
#   # configure (macOS needs an upstream LLVM; Apple clang ships no libFuzzer)
#   cmake -S . -B build-fuzz -DUAPP_ENABLE_FUZZING=ON \
#         -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang \
#         -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++
#   cmake --build build-fuzz -j
#
#   # 60-second smoke run (the per-PR cadence, §6.2)
#   mkdir -p /tmp/uapp-fuzz-work
#   ./build-fuzz/bin/fuzz_file_detection -max_total_time=60 -print_final_stats=1 \
#       -artifact_prefix=/tmp/uapp-fuzz-artifacts/ \
#       /tmp/uapp-fuzz-work tests/fixtures/corpus/fuzz_file_detection/
#
#   # replay one crash reproducer
#   ./build-fuzz/bin/fuzz_file_detection /tmp/uapp-fuzz-artifacts/crash-<sha1>
#
#   # minimise a run's survivors back down before checking any of them in
#   ./build-fuzz/bin/fuzz_file_detection -merge=1 \
#       tests/fixtures/corpus/fuzz_file_detection/ /tmp/uapp-fuzz-work/
#
# The FIRST positional directory is WRITTEN to: libFuzzer saves every input
# that reaches new coverage into it, and the directories after it are read
# -only. Hence the scratch directory first and the checked-in seed set second
# — the other order would add a few hundred files to the working tree on
# every smoke run.
#
# One length note: libFuzzer's default `-max_len` is 4096, which never reaches
# the 8 KiB text probe or the 64 KiB EOCD backward scan. A run that means to
# cover those needs `-max_len=131072`.
#
# ---------------------------------------------------------------------------
# ...AND THE PDF ONE
#
#   mkdir -p /tmp/uapp-fuzz-pdf-work
#   ./build-fuzz/bin/fuzz_pdf_reader -max_total_time=120 -print_final_stats=1 \
#       -timeout=25 -rss_limit_mb=2048 \
#       -artifact_prefix=/tmp/uapp-fuzz-artifacts/ \
#       /tmp/uapp-fuzz-pdf-work tests/fixtures/corpus/fuzz_pdf_reader/
#
# `-timeout` and `-rss_limit_mb` are not decoration here the way they would be
# for the detector: §6.2 says "timeout and OOM are findings, not noise — a PDF
# that takes 30 s to parse is a DoS", and a PDF parser is the one component in
# this repo where an input genuinely can ask for unbounded work or unbounded
# memory. Leaving the limits at their defaults would let a decompression bomb
# read as a slow iteration instead of as the finding it is.
#
# Expect a MUCH lower exec/s than fuzz_file_detection — hundreds, not tens of
# thousands. Opening and rasterising a PDF is several orders of magnitude more
# work than sniffing a magic number, and the vendored libpdfium is a prebuilt
# dylib with no coverage instrumentation, so libFuzzer's feedback comes from
# the wrapper alone. Both points are spelled out in fuzz_pdf_reader.cpp.
# ---------------------------------------------------------------------------
