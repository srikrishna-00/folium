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

option(UAPP_BUILD_TESTS       "Build unit tests"                         ON)
option(UAPP_BUILD_TOOLS       "Build developer CLI / harness executables" ON)
option(UAPP_ENABLE_SANITIZERS "Build with Address+UB sanitizers"         OFF)
option(UAPP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"       ON)

include(cmake/UappCompilerOptions.cmake)

# --- Third-party -----------------------------------------------------------
# SQLite (public domain — see THIRD_PARTY_NOTICES). For this slice we link the
# platform-provided libsqlite3, which on every supported dev host ships with
# FTS5 compiled in; the configure step below asserts that rather than assuming.
# Vendoring the SQLite amalgamation + SQLCipher for reproducible mobile builds
# is tracked for a later Phase 1 task, not done here.
include(cmake/FindSQLite3Fts5.cmake)

add_subdirectory(core)
add_subdirectory(storage)

# file-engine/ — spec section 8/9. Detection only so far. It is added after
# core/ because it links uapp::core_common for Status and ObjectId, and it is
# added BEFORE core/capi because it is a peer domain engine, not part of the
# ABI composition root: nothing in core/capi references it yet.
add_subdirectory(file-engine)

# core/capi is added HERE rather than from core/CMakeLists.txt, and the order
# is load-bearing: libuapp_capi is the composition root that binds the abstract
# NoteStore port to the concrete SQLite adapter, so uapp::storage_sqlite must
# already be a defined target when its link line is evaluated.
add_subdirectory(core/capi)

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
message(STATUS "uapp: warnings-as-errors  = ${UAPP_WARNINGS_AS_ERRORS}")
