cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Universal AI Productivity Platform — native core build
#
# Phase 1: this file wires up the vertical slice
#   CREATE NOTE -> STORE LOCALLY -> SEARCH NOTE -> OPEN NOTE -> EDIT -> SAVE
# (spec section 40, "FIRST TASK"), the block document model that supersedes it
# (spec section 14), and file-engine/detection (spec section 8/9). The
# remaining monorepo directories (pdf-engine/, ocr/, scanner/, ai/, ...) are
# still empty Phase 0 skeletons and are deliberately NOT added here yet.
#
# Language level is C++17 per ADR-0001 ("Implement the core in C++17,
# restricted to a disciplined subset, and export a pure C99 ABI"). The second
# half of that sentence is now real: core/capi exports it.
#
# C is enabled as a first-class language, not because any production code is
# written in C — none is — but because core/capi's whole promise is that its
# headers are consumable from C99, and the only way to keep that promise honest
# is to compile something with a C compiler. tests/capi is that something.
# ---------------------------------------------------------------------------

project(uapp
    VERSION 0.0.1
    DESCRIPTION "AI-Native Universal Productivity, Document, PDF, Scanner & File Platform"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict C99, no GNU extensions. architecture.md section 8 rule 5 requires the
# capi headers to compile clean under -std=c99 -Wall -Wextra -Wpedantic with a
# C compiler; leaving extensions on would let a GNU-ism through here and break
# the first consumer whose toolchain is stricter than ours.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Fuzzing is a SEPARATE build configuration, not a switch on this one — see
# cmake/UappCompilerOptions.cmake for why (libFuzzer supplies `main`). Default
# OFF; with it off, nothing below this line differs from before it existed.
#
#   cmake -S . -B build-fuzz -DUAPP_ENABLE_FUZZING=ON \
#         -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang \
#         -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++
#   cmake --build build-fuzz -j
#   ./build-fuzz/bin/fuzz_file_detection -max_total_time=60 \
#         tests/fixtures/corpus/fuzz_file_detection/
#
# (The explicit compiler is a macOS necessity, not a preference: Apple's
# Command Line Tools clang ships no libFuzzer runtime. The configure step
# says so if it is missing.)
option(UAPP_ENABLE_FUZZING    "Build libFuzzer fuzz targets (ASan+UBSan)" OFF)

# A fuzz build is not a test build. When fuzzing is on these default OFF so
# `cmake --build build-fuzz` produces the library under test and the harness
# and nothing else: the four test binaries and the CLI all have their own
# `main`, none of them is what is being fuzzed, and instrumenting SQLite to
# build them wastes minutes per iteration. Both remain overridable.
set(uapp_aux_default ON)
if(UAPP_ENABLE_FUZZING)
    set(uapp_aux_default OFF)
endif()

option(UAPP_BUILD_TESTS       "Build unit tests"                         ${uapp_aux_default})
option(UAPP_BUILD_TOOLS       "Build developer CLI / harness executables" ${uapp_aux_default})
option(UAPP_ENABLE_SANITIZERS "Build with Address+UB sanitizers"         OFF)
option(UAPP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"       ON)

include(cmake/UappCompilerOptions.cmake)

# --- Third-party -----------------------------------------------------------
# SQLite (public domain — see THIRD_PARTY_NOTICES). For this slice we link the
# platform-provided libsqlite3, which on every supported dev host ships with
# FTS5 compiled in; the configure step below asserts that rather than assuming.
# Vendoring the SQLite amalgamation + SQLCipher for reproducible mobile builds
# is tracked for a later Phase 1 task, not done here.
#
# Skipped entirely in a fuzz build: the persistence stack is not on the
# untrusted-input path being fuzzed, and probing for a system SQLite with FTS5
# would make the fuzz configuration fail for a reason that has nothing to do
# with fuzzing.
if(NOT UAPP_ENABLE_FUZZING)
    include(cmake/FindSQLite3Fts5.cmake)
endif()

add_subdirectory(core)

if(NOT UAPP_ENABLE_FUZZING)
    add_subdirectory(storage)
endif()

# file-engine/ — spec section 8/9. Detection only so far. It is added after
# core/ because it links uapp::core_common for Status and ObjectId, and it is
# added BEFORE core/capi because it is a peer domain engine, not part of the
# ABI composition root: nothing in core/capi references it yet.
add_subdirectory(file-engine)

# core/capi is added HERE rather than from core/CMakeLists.txt, and the order
# is load-bearing: libuapp_capi is the composition root that binds the abstract
# NoteStore port to the concrete SQLite adapter, so uapp::storage_sqlite must
# already be a defined target when its link line is evaluated.
if(NOT UAPP_ENABLE_FUZZING)
    add_subdirectory(core/capi)
endif()

# tests/fuzz/ is added OUTSIDE the UAPP_BUILD_TESTS block on purpose. A fuzz
# target is not a unit test — it has no pass/fail, it is never added to ctest,
# and it is driven by a corpus rather than by assertions — so tying it to the
# test switch would mean "turn tests off" silently turns fuzzing off too.
if(UAPP_ENABLE_FUZZING)
    add_subdirectory(tests/fuzz)
endif()

if(UAPP_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(UAPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "uapp: build type          = ${CMAKE_BUILD_TYPE}")
message(STATUS "uapp: tests               = ${UAPP_BUILD_TESTS}")
message(STATUS "uapp: tools               = ${UAPP_BUILD_TOOLS}")
message(STATUS "uapp: sanitizers          = ${UAPP_ENABLE_SANITIZERS}")
message(STATUS "uapp: fuzzing             = ${UAPP_ENABLE_FUZZING}")
message(STATUS "uapp: warnings-as-errors  = ${UAPP_WARNINGS_AS_ERRORS}")
