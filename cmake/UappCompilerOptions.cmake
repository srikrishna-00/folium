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
# ---------------------------------------------------------------------------

add_library(uapp_compiler_options INTERFACE)
add_library(uapp::compiler_options ALIAS uapp_compiler_options)

if(MSVC)
    target_compile_options(uapp_compiler_options INTERFACE
        /W4 /permissive- /EHs-c- /GR-)
    target_compile_definitions(uapp_compiler_options INTERFACE _HAS_EXCEPTIONS=0)
    if(UAPP_WARNINGS_AS_ERRORS)
        target_compile_options(uapp_compiler_options INTERFACE /WX)
    endif()
else()
    target_compile_options(uapp_compiler_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wcast-qual
        -Wold-style-cast
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wdouble-promotion
        -Wformat=2
        -Wundef
        -Wunused
        # ADR-0001 "Forbidden" list, enforced by the toolchain rather than by
        # code review alone.
        -fno-exceptions
        -fno-rtti)
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
