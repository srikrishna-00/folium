# ---------------------------------------------------------------------------
# cmake/UappPdfium.cmake — define the imported PDFium target, or explain why
# there isn't one.
#
# SP-1 (`docs/spikes/SP-1-findings.md` §5) says PDFium is an out-of-tree
# artefact build consumed "from our CMake as an IMPORTED target". That is the
# shape implemented here. What differs from SP-1, and is NOT hidden:
#
#   * the artefact is a PREBUILT binary from bblanchon/pdfium-binaries,
#     pinned by tag and SHA-256, not one we built hermetically from source;
#   * it is SHARED, not STATIC, because this release publishes no static
#     archive for macOS (third_party/pdfium/VERSION records the check).
#
# Both are bootstrap compromises with an owner and an exit condition; see
# third_party/pdfium/README.md, "What is still owed".
#
# THIS FILE NEVER TOUCHES THE NETWORK. It looks at the filesystem and nothing
# else, so `cmake -S . -B build` works offline — the same rule
# tests/support/test_framework.h applies to test frameworks. Vendoring is an
# explicit prior step: third_party/pdfium/fetch_pdfium.sh.
#
# Contract with the rest of the build:
#   UAPP_PDFIUM_FOUND  — TRUE iff uapp::pdfium was defined.
#   uapp::pdfium       — IMPORTED target carrying includes + the library.
# Nothing here calls message(FATAL_ERROR): whether a missing PDFium is fatal
# is the caller's policy decision (see the root CMakeLists), not this file's.
# ---------------------------------------------------------------------------

set(UAPP_PDFIUM_FOUND FALSE)
set(UAPP_PDFIUM_ROOT "${CMAKE_SOURCE_DIR}/third_party/pdfium")

# Which vendored platform directory applies to this build. Deliberately a
# short, closed list: an unrecognised host gets "no PDFium" and a clear
# message, never a guess at a directory name.
set(UAPP_PDFIUM_PLATFORM "")
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(UAPP_PDFIUM_PLATFORM "mac-arm64")
elseif(APPLE)
    set(UAPP_PDFIUM_PLATFORM "mac-x64")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(UAPP_PDFIUM_PLATFORM "linux-arm64")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(UAPP_PDFIUM_PLATFORM "linux-x64")
endif()

if(UAPP_PDFIUM_PLATFORM STREQUAL "")
    set(UAPP_PDFIUM_REASON
        "no vendored PDFium platform is defined for ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}")
    return()
endif()

set(UAPP_PDFIUM_DIR "${UAPP_PDFIUM_ROOT}/${UAPP_PDFIUM_PLATFORM}")

# find_library / find_path with NO_DEFAULT_PATH: we want *our* pinned copy or
# nothing. Silently falling back to a system-wide libpdfium of unknown vintage
# would defeat the whole point of pinning a SHA-256, and would make the
# licence inventory describe a build we do not ship.
find_path(UAPP_PDFIUM_INCLUDE_DIR
    NAMES fpdfview.h
    PATHS "${UAPP_PDFIUM_DIR}/include"
    NO_DEFAULT_PATH)

find_library(UAPP_PDFIUM_LIBRARY
    NAMES pdfium
    PATHS "${UAPP_PDFIUM_DIR}/lib"
    NO_DEFAULT_PATH)

if(NOT UAPP_PDFIUM_INCLUDE_DIR OR NOT UAPP_PDFIUM_LIBRARY)
    set(UAPP_PDFIUM_REASON
        "third_party/pdfium/${UAPP_PDFIUM_PLATFORM} is missing or incomplete "
        "(run third_party/pdfium/fetch_pdfium.sh ${UAPP_PDFIUM_PLATFORM})")
    string(REPLACE ";" "" UAPP_PDFIUM_REASON "${UAPP_PDFIUM_REASON}")
    return()
endif()

# --- the macOS install-name problem ----------------------------------------
# The distributed dylib's install name is the RELATIVE path "./libpdfium.dylib"
# (verify: `otool -D third_party/pdfium/mac-arm64/lib/libpdfium.dylib`). Linking
# against it verbatim bakes that relative path into every consumer, so the
# loader would resolve it against the process's CURRENT WORKING DIRECTORY —
# meaning the tests pass or fail depending on where ctest happened to be
# invoked from. That is not a runtime path we are willing to ship or to test
# against.
#
# So: copy the dylib into the build tree once, at configure time, rewrite its
# id to @rpath/, and give consumers an rpath pointing at that copy. The
# vendored file under third_party/ is never modified — it must keep matching
# the SHA-256 in VERSION.
set(UAPP_PDFIUM_LINK_TARGET "${UAPP_PDFIUM_LIBRARY}")

if(APPLE)
    get_filename_component(uapp_pdfium_soname "${UAPP_PDFIUM_LIBRARY}" NAME)
    set(uapp_pdfium_stage "${CMAKE_BINARY_DIR}/pdfium-lib")
    file(MAKE_DIRECTORY "${uapp_pdfium_stage}")
    file(COPY "${UAPP_PDFIUM_LIBRARY}" DESTINATION "${uapp_pdfium_stage}")
    set(UAPP_PDFIUM_LINK_TARGET "${uapp_pdfium_stage}/${uapp_pdfium_soname}")

    find_program(UAPP_INSTALL_NAME_TOOL install_name_tool)
    if(UAPP_INSTALL_NAME_TOOL)
        execute_process(
            COMMAND "${UAPP_INSTALL_NAME_TOOL}" -id "@rpath/${uapp_pdfium_soname}"
                    "${UAPP_PDFIUM_LINK_TARGET}"
            RESULT_VARIABLE uapp_pdfium_rename_rc
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT uapp_pdfium_rename_rc EQUAL 0)
            message(WARNING
                "uapp: install_name_tool could not rewrite the PDFium install name; "
                "pdf tests may fail to load libpdfium at runtime.")
        endif()
    else()
        message(WARNING
            "uapp: install_name_tool not found; the vendored libpdfium keeps its "
            "relative install name and consumers will only load it from the right cwd.")
    endif()
    set(UAPP_PDFIUM_RPATH "${uapp_pdfium_stage}")
else()
    get_filename_component(UAPP_PDFIUM_RPATH "${UAPP_PDFIUM_LIBRARY}" DIRECTORY)
endif()

add_library(uapp_pdfium SHARED IMPORTED GLOBAL)
add_library(uapp::pdfium ALIAS uapp_pdfium)

# `include/cpp` carries fpdf_scopers.h / fpdf_deleters.h. We do not use them
# (they are std::unique_ptr wrappers and our RAII is our own, in
# pdf-engine/reader), but the directory is part of the package's documented
# interface and omitting it would be a surprise for the next consumer.
#
# Imported targets' interface includes are treated as SYSTEM by CMake, which
# is load-bearing here: PDFium's headers are not written to survive our
# -Wold-style-cast -Wconversion -Werror posture, and our posture is not
# something to relax for a third party's header.
set_target_properties(uapp_pdfium PROPERTIES
    IMPORTED_LOCATION             "${UAPP_PDFIUM_LINK_TARGET}"
    INTERFACE_INCLUDE_DIRECTORIES "${UAPP_PDFIUM_INCLUDE_DIR};${UAPP_PDFIUM_INCLUDE_DIR}/cpp")

# The rpath goes on CMAKE_BUILD_RPATH rather than on the imported target's
# INTERFACE_LINK_OPTIONS. Both work, but link options are not de-duplicated by
# CMake, and a static library that links PDFium privately still forwards them
# under $<LINK_ONLY:>, so an executable that reaches PDFium through more than
# one path gets `-Wl,-rpath,<same dir>` more than once and the linker warns.
# A warning that appears only in some link lines is exactly the kind of noise
# people learn to ignore.
#
# This is a BUILD rpath, for running tests and tools out of the build tree. It
# is deliberately not an install rpath: how PDFium is located in a shipped
# app bundle or APK is a packaging decision that does not exist yet, and
# guessing at it here would bake a wrong answer into the first packaging
# attempt.
if(NOT MSVC)
    list(APPEND CMAKE_BUILD_RPATH "${UAPP_PDFIUM_RPATH}")
    list(REMOVE_DUPLICATES CMAKE_BUILD_RPATH)
    # include() does not open a scope, so this lands in the caller's (the root
    # CMakeLists') directory scope and is inherited by every add_subdirectory
    # that follows. That ordering is why UappPdfium.cmake is included before
    # add_subdirectory(pdf-engine) and add_subdirectory(tests).
endif()

# Read the version back out of the payload rather than trusting the copy in
# this file: if someone hand-edits a vendored tree, the build should say so.
set(UAPP_PDFIUM_VERSION "unknown")
if(EXISTS "${UAPP_PDFIUM_DIR}/VERSION")
    file(READ "${UAPP_PDFIUM_DIR}/VERSION" uapp_pdfium_version_file)
    string(REGEX MATCH "MAJOR=([0-9]+)" _m "${uapp_pdfium_version_file}")
    set(_major "${CMAKE_MATCH_1}")
    string(REGEX MATCH "MINOR=([0-9]+)" _m "${uapp_pdfium_version_file}")
    set(_minor "${CMAKE_MATCH_1}")
    string(REGEX MATCH "BUILD=([0-9]+)" _m "${uapp_pdfium_version_file}")
    set(_build "${CMAKE_MATCH_1}")
    string(REGEX MATCH "PATCH=([0-9]+)" _m "${uapp_pdfium_version_file}")
    set(_patch "${CMAKE_MATCH_1}")
    set(UAPP_PDFIUM_VERSION "${_major}.${_minor}.${_build}.${_patch}")
endif()

set(UAPP_PDFIUM_FOUND TRUE)
set(UAPP_PDFIUM_REASON "")
