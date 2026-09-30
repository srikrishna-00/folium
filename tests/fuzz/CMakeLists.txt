# ---------------------------------------------------------------------------
# tests/fuzz/ — libFuzzer targets (testing-strategy.md §6).
#
# Reached only from the root CMakeLists.txt under UAPP_ENABLE_FUZZING, which
# is OFF by default. Nothing here is compiled, configured or even parsed in a
# normal build, and no target here is registered with ctest: a fuzz target has
# no pass/fail, it has a time budget.
#
# ONE TARGET, DELIBERATELY. testing-strategy.md §6.1 lists eighteen; this is
# `fuzz_detect`, the only one whose subject exists yet. The other seventeen
# name parsers (PDF, TIFF, XML, CBOR sync deltas) that are still empty Phase 0
# directories — adding stub targets for them would turn a real gate into a
# checklist of harnesses that fuzz nothing.
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
# ---------------------------------------------------------------------------
