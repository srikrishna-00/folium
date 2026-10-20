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
# ---------------------------------------------------------------------------
# uapp::compiler_options_exceptions_ok — the ONE sanctioned exception to
# ADR-0001's "no exceptions" rule, and it exists for exactly one reason:
# cpp-httplib (third_party/httplib) genuinely cannot be compiled under
# -fno-exceptions. This was verified empirically, not assumed — compiling a
# trivial `httplib::Server` translation unit with -fno-exceptions produces
# hard errors at real, load-bearing call sites, not a style choice:
#   httplib.h:2408/2413  Server::serve_guarded's own try/catch, which is how
#                        httplib turns a handler exception into a 500
#                        response instead of crashing the process — losing
#                        this would be a regression, not a simplification.
#   httplib.h:8364, 12479, 12504, 12872, 15312, 18114, 23551
#                        throw/rethrow in argument validation and range-
#                        header parsing.
#
# This target is IDENTICAL to uapp::compiler_options (same warnings, same
# -fno-rtti — confirmed separately that httplib compiles cleanly under
# -fno-rtti alone) except it omits -fno-exceptions.
#
# THE RULE THIS TARGET EXISTS UNDER, NOT JUST THE FLAGS: it may be linked
# ONLY by a small translation unit whose entire job is to wrap httplib (or
# any other exception-throwing third party) behind a plain, exception-free
# C++ interface — e.g. api/src/transport/http_server.cpp,
# ai/remote/src/http_client.cpp. That wrapper's own public functions must
# never let a C++ exception escape across its boundary: every httplib call
# it makes is inside its own try/catch, converted to a bool/Status return,
# exactly the same "exception-to-status conversion at the boundary"
# philosophy core/capi/src/capi_internal.h's UAPP_CAPI_GUARD_BEGIN/END macros
# already apply at the ABI boundary — just written out by hand here because
# this is one file, not a whole module of exported C functions. No route
# handler, no business-logic file, and nothing under core/, security/, ai/
# (other than the one http_client.cpp wrapper) may link this target.
# ---------------------------------------------------------------------------
add_library(uapp_compiler_options_exceptions_ok INTERFACE)
add_library(uapp::compiler_options_exceptions_ok ALIAS uapp_compiler_options_exceptions_ok)

if(MSVC)
    target_compile_options(uapp_compiler_options_exceptions_ok INTERFACE /W4 /permissive-)
    target_compile_options(uapp_compiler_options_exceptions_ok INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:/GR->)
    if(UAPP_WARNINGS_AS_ERRORS)
        target_compile_options(uapp_compiler_options_exceptions_ok INTERFACE /WX)
    endif()
else()
    target_compile_options(uapp_compiler_options_exceptions_ok INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
        -Wcast-qual -Wdouble-promotion -Wformat=2 -Wundef -Wunused)
    target_compile_options(uapp_compiler_options_exceptions_ok INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
        $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
        $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
    if(UAPP_WARNINGS_AS_ERRORS)
        target_compile_options(uapp_compiler_options_exceptions_ok INTERFACE -Werror)
    endif()
endif()

# Sanitizers apply here too, and matter MORE here than on most targets: the
# one file allowed to link this target (http_server.cpp) is the one place
# in the whole project parsing raw bytes off a real network socket — exactly
# the untrusted-input surface ASan/UBSan exist to catch. Mirrors
# uapp::compiler_options' own block below verbatim; kept as a second copy
# rather than refactored into a shared macro because these two targets'
# divergence (exceptions on/off) is the entire reason this file exists, and
# a shared helper would obscure that at the one place a reader most needs it
# spelled out.
if(UAPP_ENABLE_SANITIZERS AND NOT UAPP_ENABLE_FUZZING AND NOT MSVC)
    target_compile_options(uapp_compiler_options_exceptions_ok INTERFACE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=undefined)
    target_link_options(uapp_compiler_options_exceptions_ok INTERFACE
        -fsanitize=address,undefined)
endif()

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
