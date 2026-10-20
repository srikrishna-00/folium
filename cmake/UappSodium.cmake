# ---------------------------------------------------------------------------
# cmake/UappSodium.cmake — define the imported libsodium target, or explain
# why there isn't one.
#
# This is the FIRST external dependency in the project that is not a prebuilt
# binary we vendored ourselves, so the acquisition strategy differs from
# cmake/UappPdfium.cmake on purpose. The argument is written out in
# third_party/libsodium/README.md, "Why this looks different from
# third_party/pdfium"; the short version:
#
#   * libsodium is a standard, packaged, dependency-free C99 library. Every
#     dev host and CI image can install it with one command, and it ships a
#     pkg-config file. PDFium can do none of those things.
#   * So this file looks at SYSTEM paths (via pkg-config first), where
#     UappPdfium.cmake deliberately refuses to. The protection PDFium's
#     NO_DEFAULT_PATH rule buys — "never link an unpinned parser of unknown
#     vintage" — is worth much less for a library whose entire observable
#     behaviour is checked against three EXTERNAL published test vectors in
#     tests/crypto. A surprise libsodium is a red test, not a silent
#     behaviour change.
#
# THIS FILE NEVER TOUCHES THE NETWORK, which is the one rule it shares
# unchanged with UappPdfium.cmake and tests/support/test_framework.h:
# `cmake -S . -B build` must work offline. There is no FetchContent and no
# file(DOWNLOAD). Acquisition is an explicit prior step (`brew install
# libsodium`, `apt install libsodium-dev`).
#
# Contract with the rest of the build:
#   UAPP_SODIUM_FOUND    — TRUE iff uapp::sodium was defined.
#   uapp::sodium         — IMPORTED target carrying includes + the library.
#   UAPP_SODIUM_VERSION  — version string, or "unknown".
#   UAPP_SODIUM_REASON   — why it was not found, for the caller's message.
# Nothing here calls message(FATAL_ERROR): whether a missing libsodium is
# fatal is the root CMakeLists' policy decision, not this file's. Same
# division of responsibility as UappPdfium.cmake.
# ---------------------------------------------------------------------------

set(UAPP_SODIUM_FOUND FALSE)
set(UAPP_SODIUM_VERSION "unknown")
set(UAPP_SODIUM_REASON "")

# --- route 1: pkg-config ----------------------------------------------------
# Preferred because libsodium.pc is installed by upstream's own `make install`
# and by every distro package, and because it is the only route that reports
# a VERSION. We want the version: crypto_pwhash_ALG_DEFAULT changed meaning
# at 1.0.15 (Argon2i -> Argon2id), and although security/crypto pins
# ALG_ARGON2ID13 explicitly rather than relying on the default, knowing which
# libsodium is linked is a prerequisite for any CVE response at all
# (security-model.md §12).
#
# QUIET, not REQUIRED: a host with no pkg-config at all should fall through to
# route 2 rather than fail configure. That is the AUTO contract.
find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
    pkg_check_modules(UAPP_SODIUM_PC QUIET libsodium)
endif()

if(UAPP_SODIUM_PC_FOUND)
    # pkg_check_modules gives -L/-l fragments; resolve them to a real path so
    # the imported target has an IMPORTED_LOCATION and the configure log can
    # print exactly which file got linked. "libsodium was found" is not a
    # useful build-log line; the path is.
    find_library(UAPP_SODIUM_LIBRARY
        NAMES sodium
        HINTS ${UAPP_SODIUM_PC_LIBRARY_DIRS})
    find_path(UAPP_SODIUM_INCLUDE_DIR
        NAMES sodium.h
        HINTS ${UAPP_SODIUM_PC_INCLUDE_DIRS})
    set(UAPP_SODIUM_VERSION "${UAPP_SODIUM_PC_VERSION}")
else()
    # --- route 2: plain find_library / find_path ---------------------------
    # For hosts without pkg-config (which includes this dev host — macOS
    # ships none and Homebrew does not install one as a dependency of
    # libsodium) and for a future cross-compile toolchain file that points
    # CMAKE_PREFIX_PATH at a libsodium we built ourselves.
    find_library(UAPP_SODIUM_LIBRARY NAMES sodium)
    find_path(UAPP_SODIUM_INCLUDE_DIR NAMES sodium.h)
endif()

if(NOT UAPP_SODIUM_LIBRARY OR NOT UAPP_SODIUM_INCLUDE_DIR)
    set(UAPP_SODIUM_REASON
        "libsodium was not found (install it: `brew install libsodium` on macOS, "
        "`apt install libsodium-dev` on Debian/Ubuntu, or point CMAKE_PREFIX_PATH "
        "at a built one). See third_party/libsodium/README.md.")
    # The list above is a CMake list, so it joins with ';' unless collapsed.
    # Same trick UappPdfium.cmake uses for the same reason — and note that no
    # ';' may appear in the message text itself, or it would be eaten here.
    string(REPLACE ";" "" UAPP_SODIUM_REASON "${UAPP_SODIUM_REASON}")
    return()
endif()

# --- the version, read out of the installed header -------------------------
# Route 1 supplies a version; route 2 does not, and "unknown" is a bad answer
# to a question security-model.md §12's CVE process has to be able to ask.
# libsodium ships SODIUM_VERSION_STRING in <sodium/version.h>, so read it
# from the headers we are actually about to compile against.
#
# Reading the HEADER rather than trusting third_party/libsodium/VERSION is
# the point: the file in third_party/ records what we intended to pin, and
# this records what the build actually found. When the two disagree, the
# configure line and the runtime assertion in tests/crypto say so. (The
# runtime check in crypto::library_version() is still the authoritative one,
# because a header can be newer than the library beside it.)
if(UAPP_SODIUM_VERSION STREQUAL "unknown" AND EXISTS "${UAPP_SODIUM_INCLUDE_DIR}/sodium/version.h")
    file(READ "${UAPP_SODIUM_INCLUDE_DIR}/sodium/version.h" uapp_sodium_version_header)
    string(REGEX MATCH "SODIUM_VERSION_STRING[ \t]+\"([0-9][^\"]*)\"" _m
           "${uapp_sodium_version_header}")
    if(CMAKE_MATCH_1)
        set(UAPP_SODIUM_VERSION "${CMAKE_MATCH_1}")
    endif()
endif()

add_library(uapp_sodium UNKNOWN IMPORTED GLOBAL)
add_library(uapp::sodium ALIAS uapp_sodium)

# UNKNOWN rather than SHARED or STATIC, deliberately. find_library may hand
# back either a .dylib/.so or a .a depending on what the host installed and
# on CMAKE_FIND_LIBRARY_SUFFIXES, and claiming one when it is the other makes
# CMake generate a wrong link line. UNKNOWN tells CMake "link this file, I am
# not asserting its kind". The shipping mobile build WILL be static (see
# third_party/libsodium/README.md item 4) and can declare STATIC then,
# because it will own the artefact.
#
# INTERFACE_INCLUDE_DIRECTORIES on an IMPORTED target is treated as SYSTEM by
# CMake, which is load-bearing for the same reason it is for PDFium: a third
# party's headers are not written to survive our -Wconversion
# -Wold-style-cast -Werror posture, and our posture is not something to relax
# for them. sodium.h in particular uses old-style casts inside macros.
set_target_properties(uapp_sodium PROPERTIES
    IMPORTED_LOCATION             "${UAPP_SODIUM_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${UAPP_SODIUM_INCLUDE_DIR}")

set(UAPP_SODIUM_FOUND TRUE)
