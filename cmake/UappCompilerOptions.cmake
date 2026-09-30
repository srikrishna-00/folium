# ---------------------------------------------------------------------------
# Shared compiler posture for all first-party targets.
#
# Encodes the restricted C++17 subset from ADR-0001:
#   * no exceptions  -> errors are returned as uapp::Status, never thrown
#   * no RTTI        -> no dynamic_cast / typeid
#   * loud warnings  -> spec section 38 ("Run: compiler warnings, static
#                       analyzers, sanitizers, memory checks")
#
# Consume via: target_link_libraries(<tgt> PRIVATE uapp::compiler_options)
# It is an INTERFACE library so the flags cannot silently leak into
# third-party code we do not control.
#
# C AND C++. Since core/capi landed, first-party code is compiled in two
# languages: the ABI implementation is C++, and tests/capi is C so that the
# public headers are *proved* to work with a C compiler at C99 rather than
# assumed to (architecture.md section 8 rule 5). The warning posture is
# identical for both. The C++-only flags are gated on COMPILE_LANGUAGE:CXX
# rather than split out into a second options target, because a second target
# is a second thing to forget to update and the two would drift. Without the
# gate, clang emits "-fno-rtti is valid for C++ but not for C" for every C
# file, and -Werror then turns the C-conformance check into a build failure
# for entirely the wrong reason.
# ---------------------------------------------------------------------------

add_library(uapp_compiler_options INTERFACE)
add_library(uapp::compiler_options ALIAS uapp_compiler_options)

if(MSVC)
    # Applies to both languages.
    target_compile_options(uapp_compiler_options INTERFACE /W4 /permissive-)
    # Exceptions and RTTI are C++ concepts; /GR- and /EHs-c- are C++-only
    # switches and _HAS_EXCEPTIONS is a C++ standard-library knob.
    target_compile_options(uapp_compiler_options INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:/EHs-c->
        $<$<COMPILE_LANGUAGE:CXX>:/GR->)
    target_compile_definitions(uapp_compiler_options INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:_HAS_EXCEPTIONS=0>)
    if(UAPP_WARNINGS_AS_ERRORS)
        target_compile_options(uapp_compiler_options INTERFACE /WX)
    endif()
else()
    # Valid for C and C++ alike — the whole warning posture applies to both.
    target_compile_options(uapp_compiler_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wcast-qual
        -Wdouble-promotion
        -Wformat=2
        -Wundef
        -Wunused)

    # C++-only. Passing these to the C compiler is a warning, and under
    # -Werror a build failure.
    target_compile_options(uapp_compiler_options INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
        $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
        $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
        # ADR-0001 "Forbidden" list, enforced by the toolchain rather than by
        # code review alone. Still unconditional for every C++ translation
        # unit in the project, core/capi's included.
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)

    if(UAPP_WARNINGS_AS_ERRORS)
        target_compile_options(uapp_compiler_options INTERFACE -Werror)
    endif()
endif()

if(UAPP_ENABLE_SANITIZERS AND NOT UAPP_ENABLE_FUZZING)
    if(MSVC)
        message(WARNING "uapp: UAPP_ENABLE_SANITIZERS is not wired up for MSVC; ignoring.")
    else()
        target_compile_options(uapp_compiler_options INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=undefined)
        target_link_options(uapp_compiler_options INTERFACE
            -fsanitize=address,undefined)
    endif()
endif()

# ---------------------------------------------------------------------------
# UAPP_ENABLE_FUZZING — a THIRD build configuration, not a variant of the
# other two (testing-strategy.md §6.2: "libFuzzer + ASan + UBSan as the
# default configuration").
#
# WHY IT IS ITS OWN CONFIGURATION AND NOT A FLAG ON THE NORMAL BUILD
# ---------------------------------------------------------------------------
# libFuzzer supplies `main`. Every executable linked with `-fsanitize=fuzzer`
# therefore stops being a program you can run and becomes a fuzzer driver, so
# the flag cannot simply be added to the shared posture: it would break
# uapp_notes_cli and all four test binaries. What IS safe to apply project
# -wide is `fuzzer-no-link`, which adds the SanitizerCoverage instrumentation
# libFuzzer steers by and leaves `main` alone. The fuzz target alone then adds
# the real `-fsanitize=fuzzer` at LINK time (see tests/fuzz/CMakeLists.txt).
#
# Instrumenting the library and not just the harness is the whole point: a
# fuzz target built against an uninstrumented uapp::file_detection is a random
# byte generator with no feedback, which will not find anything past the first
# magic-number branch.
#
# UAPP_ENABLE_SANITIZERS is suppressed above when this is on rather than
# combined with it, because both would put -fsanitize=address,undefined on the
# command line twice — harmless on clang today, but "two options that must
# agree" is a trap for whoever changes one of them next.
#
# NOT IMPLEMENTED: MemorySanitizer. §6.2 requires it to run separately
# because it needs an instrumented libc++ and cannot be combined with ASan;
# that needs a libc++ build this repo does not have.
# ---------------------------------------------------------------------------
if(UAPP_ENABLE_FUZZING)
    if(MSVC)
        message(FATAL_ERROR
            "uapp: UAPP_ENABLE_FUZZING requires a clang-family compiler with libFuzzer; "
            "MSVC is not supported.")
    endif()

    # Fail at CONFIGURE time with a usable message rather than at link time
    # with a missing-archive error. Apple's Command Line Tools clang accepts
    # -fsanitize=fuzzer on the command line but does NOT ship
    # libclang_rt.fuzzer_osx.a, so a compile-only probe would pass and the
    # link would fail hundreds of files later; this probe links.
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer,address,undefined")
    check_cxx_source_compiles([[
        #include <cstddef>
        #include <cstdint>
        extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }
    ]] UAPP_HAVE_LIBFUZZER)
    unset(CMAKE_REQUIRED_FLAGS)

    if(NOT UAPP_HAVE_LIBFUZZER)
        message(FATAL_ERROR
            "uapp: UAPP_ENABLE_FUZZING=ON but this toolchain cannot link a libFuzzer target.\n"
            "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n"
            "            ${CMAKE_CXX_COMPILER}\n"
            "  On macOS this is expected with Apple's Command Line Tools clang: it accepts\n"
            "  -fsanitize=fuzzer but omits libclang_rt.fuzzer_osx.a from the runtime set.\n"
            "  Use an upstream LLVM instead, e.g.:\n"
            "      brew install llvm\n"
            "      cmake -S . -B build-fuzz -DUAPP_ENABLE_FUZZING=ON \\\n"
            "            -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang \\\n"
            "            -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++")
    endif()

    target_compile_options(uapp_compiler_options INTERFACE
        -fsanitize=fuzzer-no-link,address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=undefined)
    target_link_options(uapp_compiler_options INTERFACE
        -fsanitize=address,undefined)
endif()
