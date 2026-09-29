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

if(UAPP_ENABLE_SANITIZERS)
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
