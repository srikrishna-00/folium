cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Folium — native core build
#
# NAMING, recorded here because a public reader will ask: the CMake project
# name, the C++ namespace (`uapp::`) and the C ABI prefix (`UAPP_*`) are all
# "uapp" — the project's internal engineering codename from before it had a
# public name. "Folium" is the product/repository brand (see the root
# README.md and the logo in docs/assets/). The two are deliberately NOT
# unified: core/capi is a versioned, frozen C99 ABI that several commits'
# worth of work has gone into keeping stable (see docs/adr/0003 and every
# ABI-version-bump commit in docs/changelog.md) — a project-wide find/replace
# of `uapp` -> `folium` would touch that frozen surface for zero functional
# benefit and no user of the ABI asked for it. If this project ever does
# rename its internals, that is its own ADR, not a side effect of branding.
#
# What this file actually builds: every module listed in README.md's
# "What exists today" table — core/notes, core/blocks, core/search,
# core/database, core/conversion, file-engine/detection, pdf-engine/reader,
# ocr/engine, security/crypto, security/keys, ai/provider + ai/local, and
# core/capi over all of them. See docs/changelog.md for the full, ordered
# history of what each one does and does not do.
#
# Language level is C++17 per ADR-0001 ("Implement the core in C++17,
# restricted to a disciplined subset, and export a pure C99 ABI").
#
# C is enabled as a first-class language, not because any production code is
# written in C — none is — but because core/capi's whole promise is that its
# headers are consumable from C99, and the only way to keep that promise honest
# is to compile something with a C compiler. tests/capi is that something.
# ---------------------------------------------------------------------------

project(uapp
    VERSION 0.0.1
    DESCRIPTION "Folium — an AI-native document, knowledge, and file engine"
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

# --- PDF (spec section 11) -------------------------------------------------
# UAPP_ENABLE_PDF is a TRISTATE spelled as a string rather than an option(),
# because the three cases genuinely differ and collapsing them loses
# information:
#
#   AUTO (default) — build pdf-engine/ iff a vendored PDFium is present.
#                    Someone who has not run third_party/pdfium/fetch_pdfium.sh
#                    gets exactly the build they had before this module
#                    existed, plus one STATUS line saying why PDF is off.
#                    Nothing they rely on breaks and nothing pretends PDF works.
#   ON             — build it, and FAIL CONFIGURE if PDFium is missing. An
#                    explicit request must never be silently downgraded: CI
#                    passes ON so that a broken vendor step is a red build,
#                    not a green build with the PDF tests quietly absent.
#   OFF            — do not build it even if PDFium is vendored.
#
# The dependency is a PREBUILT binary pinned by SHA-256 (third_party/pdfium),
# NOT the hermetic from-source build docs/spikes/SP-1-findings.md §5 requires
# before shipping. See third_party/pdfium/README.md, "What is still owed".
set(UAPP_ENABLE_PDF "AUTO" CACHE STRING
    "Build pdf-engine/ against the vendored PDFium: ON, OFF or AUTO (detect)")
set_property(CACHE UAPP_ENABLE_PDF PROPERTY STRINGS AUTO ON OFF)

# --- Cryptography (spec sections 23/38, ADR-0002 decision 1) ---------------
# UAPP_ENABLE_CRYPTO is the same TRISTATE as UAPP_ENABLE_PDF, with the same
# three meanings, for the same reasons — AUTO detects, ON requires and fails
# configure if absent, OFF skips even when present. The contract is copied
# deliberately: a second dependency switch that behaved differently from the
# first would be a trap, and the pdf tristate's comment block above argues
# every case already.
#
# What is DIFFERENT from PDFium is where the dependency comes from.
# libsodium is a standard, packaged, dependency-free C99 library found via
# pkg-config on the system (cmake/UappSodium.cmake), NOT a prebuilt binary
# vendored under third_party/ and pinned by SHA-256. That inversion of
# UappPdfium.cmake's NO_DEFAULT_PATH rule is argued at length in
# third_party/libsodium/README.md, "Why this looks different from
# third_party/pdfium" — the short version is that libsodium's entire
# observable behaviour is checked against three EXTERNAL published test
# vectors in tests/crypto, so a surprise copy is a red test rather than a
# silent behaviour change, which is not true of a PDF parser.
#
# The acquisition step is still one explicit human/CI command and CMake still
# never touches the network:
#     brew install libsodium          # macOS
#     apt install libsodium-dev       # Debian/Ubuntu CI
#
# This gate covers the PRIMITIVES layer only. E2EE itself — the key
# hierarchy, device enrolment, rotation, sealed sync payloads — is NOT
# IMPLEMENTED; see security/crypto/include/uapp/crypto/crypto.h.
set(UAPP_ENABLE_CRYPTO "AUTO" CACHE STRING
    "Build security/crypto against libsodium: ON, OFF or AUTO (detect)")
set_property(CACHE UAPP_ENABLE_CRYPTO PROPERTY STRINGS AUTO ON OFF)

# --- OCR (spec section 13) --------------------------------------------------
# UAPP_ENABLE_OCR is the same TRISTATE as UAPP_ENABLE_PDF and
# UAPP_ENABLE_CRYPTO, same three meanings. Like libsodium and unlike PDFium,
# Tesseract is a standard packaged dependency found via pkg-config/find_library
# on the system (cmake/UappTesseract.cmake) rather than a prebuilt binary
# vendored under third_party/ and pinned by SHA-256 — there is no upstream
# equivalent of bblanchon/pdfium-binaries to pin. See
# cmake/UappTesseract.cmake's own header comment for why it follows
# UappSodium.cmake's shape rather than UappPdfium.cmake's, and
# docs/spikes/SP-10-findings.md for the full dependency vetting (license,
# the Homebrew build's libcurl/libarchive linkage and why a shipping build
# must disable both, mobile build story).
#
# The acquisition step is still one explicit human/CI command and CMake still
# never touches the network:
#     brew install tesseract                              # macOS
#     apt install libtesseract-dev libleptonica-dev        # Debian/Ubuntu CI
#
# This gate covers printed-text recognition only; see
# ocr/engine/include/uapp/ocr/engine.h for the full scoping argument (tables,
# forms, receipts/business cards, handwriting and multi-language are all
# NOT IMPLEMENTED).
set(UAPP_ENABLE_OCR "AUTO" CACHE STRING
    "Build ocr/engine against a system Tesseract: ON, OFF or AUTO (detect)")
set_property(CACHE UAPP_ENABLE_OCR PROPERTY STRINGS AUTO ON OFF)

option(UAPP_BUILD_TESTS       "Build unit tests"                         ${uapp_aux_default})
option(UAPP_BUILD_TOOLS       "Build developer CLI / harness executables" ${uapp_aux_default})
option(UAPP_ENABLE_SANITIZERS "Build with Address+UB sanitizers"         OFF)
option(UAPP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"       ON)

# core/sync/spike — the Fugue CRDT correctness spike owed by ADR-0002. It is
# RESEARCH CODE: it is never linked by a product target, and it follows
# UAPP_BUILD_TESTS because its only consumer is tests/sync_spike. Spelled as
# its own switch so that a release build can be verified to exclude it, and so
# that turning it off is a one-flag answer to "is any of this shipping?".
option(UAPP_BUILD_SYNC_SPIKE  "Build the standalone Fugue CRDT correctness spike (research only)"
       ${UAPP_BUILD_TESTS})

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

# --- security/crypto — the libsodium-backed primitives layer ---------------
# Resolve the tristate into UAPP_CRYPTO_ENABLED (a plain bool the rest of the
# build and tests/ branch on), exactly as UAPP_PDF_ENABLED works below.
#
# Added here, straight after core/, because security/crypto links
# uapp::core_common PUBLIC-ly (crypto.h returns uapp::Status) and nothing
# else. It is added BEFORE storage/ and file-engine/ to make the direction
# visible: crypto depends on core/common and on nothing else in the tree,
# and no module in the tree depends on crypto yet — the E2EE integration
# those modules will need is NOT IMPLEMENTED.
#
# Skipped under UAPP_ENABLE_FUZZING, like storage/ and core/capi. That is a
# judgement call worth recording rather than leaving implicit: crypto::open()
# IS an untrusted-input entry point (spec §28 — a synced blob is untrusted
# even though it used to be ours), so a libFuzzer target over it would be
# legitimate. It is not written. The reason it is not is that the attack
# surface is ~15 lines of length arithmetic in front of a library we did not
# write and must not fuzz on its behalf; spec §29's required-fuzz-target list
# names the format parsers, and inventing an extra one here would spend fuzz
# minutes on libsodium rather than on the PDF reader. Recorded as NOT
# IMPLEMENTED, not as unnecessary.
set(UAPP_CRYPTO_ENABLED FALSE)
if(NOT UAPP_ENABLE_CRYPTO STREQUAL "OFF" AND NOT UAPP_ENABLE_FUZZING)
    include(cmake/UappSodium.cmake)

    if(UAPP_SODIUM_FOUND)
        set(UAPP_CRYPTO_ENABLED TRUE)
    elseif(UAPP_ENABLE_CRYPTO STREQUAL "ON")
        message(FATAL_ERROR
            "uapp: UAPP_ENABLE_CRYPTO=ON but libsodium was not found.\n"
            "  Reason: ${UAPP_SODIUM_REASON}\n"
            "  Install it (LOCAL DEV BOOTSTRAP — see third_party/libsodium/README.md):\n"
            "      brew install libsodium        # macOS\n"
            "      apt install libsodium-dev     # Debian/Ubuntu\n"
            "  Or configure with -DUAPP_ENABLE_CRYPTO=OFF to build without it.")
    endif()
endif()

if(UAPP_CRYPTO_ENABLED)
    add_subdirectory(security)
endif()

# --- ai/ — the provider-independent AI abstraction (spec §17) ---------------
# Added here, straight after security/, for the same reason security/crypto is
# added straight after core/: ai/provider links uapp::core_common PUBLIC-ly
# (Status) and NOTHING else — no SQLite, no libsodium, no document model, no
# network. Adding it before storage/ and file-engine/ makes the direction
# visible: the AI layer depends on core/common alone, and nothing in the tree
# depends on the AI layer yet. The E2EE integration it will eventually need
# (ai-architecture.md §13.2 step 7, decrypting consented ranges) is the
# CALLER's job through a resolver callback, which is why this module sits
# beside security/ rather than on top of it.
#
# UNCONDITIONAL, unlike pdf-engine and security/crypto. Both of those are gated
# because they need a third-party binary a fresh clone may not have; this
# module has no third-party dependency at all, so there is no configuration in
# which it is absent and no reason for anything downstream to report "skipped"
# for code that was never compiled.
#
# NOT skipped under UAPP_ENABLE_FUZZING, unlike storage/ and core/capi. This IS
# an untrusted-input path — a provider is handed a user document, a synced page
# or an OCR result. No fuzz target is written for it in this slice, which is a
# judgement call worth recording rather than leaving implicit: spec §29's
# required-target list names the format parsers, and the highest-value harness
# here (the field scanners and the sentence splitter over arbitrary bytes) is a
# follow-up. Recorded as NOT IMPLEMENTED in docs/ai-architecture.md §14, not as
# unnecessary.
add_subdirectory(ai)

# --- ocr/ — printed-text recognition (spec section 13) ---------------------
# Resolve the tristate into UAPP_OCR_ENABLED (a plain bool the rest of the
# build and tests/ can branch on), exactly as UAPP_PDF_ENABLED and
# UAPP_CRYPTO_ENABLED work above.
#
# Added here, straight after ai/, for the same "depends on core/common alone"
# reason: ocr/engine links uapp::core_common PUBLIC-ly (Status) and
# uapp::tesseract PRIVATE-ly, nothing else — no SQLite, no libsodium, no
# document model, no network (see ocr/engine/CMakeLists.txt). The eventual
# "OCR output feeds ai/'s entity extraction" and "OCR a rendered PDF page"
# integrations are a CALLER's job, which is why this sits beside ai/ and
# pdf-engine rather than depending on either.
#
# NOT skipped under UAPP_ENABLE_FUZZING, unlike storage/ and core/capi, and
# like pdf-engine: ocr::Engine::recognise() IS the untrusted-input path spec
# section 29 cares about (a crafted image, not just a crafted document), so
# under fuzzing this is a library under test, not an extra cost. No fuzz
# target is written for it in this slice — recorded as NOT IMPLEMENTED in
# docs/file-format-support.md, not as unnecessary, matching the same
# judgement call ai/'s CMakeLists.txt already recorded for its own
# untrusted-input surface.
set(UAPP_OCR_ENABLED FALSE)
if(NOT UAPP_ENABLE_OCR STREQUAL "OFF")
    include(cmake/UappTesseract.cmake)

    if(UAPP_TESSERACT_FOUND)
        set(UAPP_OCR_ENABLED TRUE)
    elseif(UAPP_ENABLE_OCR STREQUAL "ON")
        message(FATAL_ERROR
            "uapp: UAPP_ENABLE_OCR=ON but libtesseract was not found.\n"
            "  Reason: ${UAPP_TESSERACT_REASON}\n"
            "  Install it (LOCAL DEV BOOTSTRAP — see third_party/tesseract/README.md):\n"
            "      brew install tesseract                           # macOS\n"
            "      apt install libtesseract-dev libleptonica-dev    # Debian/Ubuntu\n"
            "  Or configure with -DUAPP_ENABLE_OCR=OFF to build without it.")
    endif()
endif()

if(UAPP_OCR_ENABLED)
    add_subdirectory(ocr)
endif()

if(NOT UAPP_ENABLE_FUZZING)
    add_subdirectory(storage)
endif()

# file-engine/ — spec section 8/9. Detection only so far. It is added after
# core/ because it links uapp::core_common for Status and ObjectId, and it is
# added BEFORE core/capi because it is a peer domain engine, not part of the
# ABI composition root: nothing in core/capi references it yet.
add_subdirectory(file-engine)

# --- pdf-engine/ — spec section 11, VIEWING ONLY ---------------------------
# Resolve the tristate into UAPP_PDF_ENABLED (a plain bool the rest of the
# build and tests/ can branch on).
#
# NOT skipped under UAPP_ENABLE_FUZZING, unlike storage/ and core/capi. Those
# two are off the untrusted-input path and only cost fuzz build time; the PDF
# reader IS the untrusted-input path — spec §29 names the PDF parser first in
# its required-fuzz-targets list — so under fuzzing this is a *library under
# test*, not an extra. `tests/fuzz/fuzz_pdf_reader` is built iff this resolves
# TRUE, which keeps the AUTO contract intact: a fuzz build on a machine with
# no vendored PDFium still configures and still builds fuzz_file_detection,
# it just has one fewer target and says so.
set(UAPP_PDF_ENABLED FALSE)
if(NOT UAPP_ENABLE_PDF STREQUAL "OFF")
    include(cmake/UappPdfium.cmake)

    if(UAPP_PDFIUM_FOUND)
        set(UAPP_PDF_ENABLED TRUE)
    elseif(UAPP_ENABLE_PDF STREQUAL "ON")
        message(FATAL_ERROR
            "uapp: UAPP_ENABLE_PDF=ON but no vendored PDFium was found.\n"
            "  Reason: ${UAPP_PDFIUM_REASON}\n"
            "  Vendor the pinned prebuilt (bootstrap — see third_party/pdfium/README.md):\n"
            "      third_party/pdfium/fetch_pdfium.sh\n"
            "  Or configure with -DUAPP_ENABLE_PDF=OFF to build without PDF support.")
    endif()
endif()

if(UAPP_PDF_ENABLED)
    add_subdirectory(pdf-engine)
endif()

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

# The CRDT spike is added here rather than from core/CMakeLists.txt on purpose:
# core/ is the portable domain layer, and an experiment that no product target
# links is not part of it. See core/sync/spike/CMakeLists.txt.
if(UAPP_BUILD_SYNC_SPIKE)
    add_subdirectory(core/sync/spike)
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

if(UAPP_CRYPTO_ENABLED)
    message(STATUS "uapp: security/crypto     = ON  (libsodium ${UAPP_SODIUM_VERSION}, "
                   "${UAPP_SODIUM_LIBRARY} — LOCAL DEV BOOTSTRAP, see third_party/libsodium/README.md)")
else()
    message(STATUS "uapp: security/crypto     = OFF (${UAPP_ENABLE_CRYPTO})")
    if(UAPP_SODIUM_REASON)
        message(STATUS "uapp:                       ${UAPP_SODIUM_REASON}")
    endif()
endif()

if(UAPP_PDF_ENABLED)
    message(STATUS "uapp: pdf-engine          = ON  (PDFium ${UAPP_PDFIUM_VERSION}, "
                   "${UAPP_PDFIUM_PLATFORM}, PREBUILT bootstrap — see third_party/pdfium/README.md)")
else()
    message(STATUS "uapp: pdf-engine          = OFF (${UAPP_ENABLE_PDF})")
    if(UAPP_PDFIUM_REASON)
        message(STATUS "uapp:                       ${UAPP_PDFIUM_REASON}")
    endif()
endif()

if(UAPP_OCR_ENABLED)
    message(STATUS "uapp: ocr/engine          = ON  (Tesseract ${UAPP_TESSERACT_VERSION}, "
                   "${UAPP_TESSERACT_LIBRARY} — LOCAL DEV BOOTSTRAP, see third_party/tesseract/README.md)")
    if(UAPP_TESSERACT_TESSDATA)
        message(STATUS "uapp:                       tessdata for tests: ${UAPP_TESSERACT_TESSDATA}")
    else()
        message(STATUS "uapp:                       no tessdata found — recognition tests will skip")
    endif()
else()
    message(STATUS "uapp: ocr/engine          = OFF (${UAPP_ENABLE_OCR})")
    if(UAPP_TESSERACT_REASON)
        message(STATUS "uapp:                       ${UAPP_TESSERACT_REASON}")
    endif()
endif()
