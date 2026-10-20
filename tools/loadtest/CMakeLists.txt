# tools/loadtest — the scale/stress harness.
#
# THREE EXECUTABLES, NOT ONE, AND THE LINK LINE IS THE REASON. This project
# enforces module boundaries with target_link_libraries rather than with code
# review: ocr/engine reaches core_common and Tesseract and nothing else;
# security/crypto reaches libsodium and nothing else. A single
# "uapp-loadtest" linking SQLite, libsodium and Tesseract together would make
# this directory the one place in the tree where that isolation does not hold.
#
# Splitting also happens to be the only correct way to measure memory. Peak
# RSS from getrusage() is a per-PROCESS high-water mark, so a 19-megapixel OCR
# run and a 100 000-row SQLite run in one process report the max of the two and
# attribute it to neither.
#
# crypto and OCR follow the same configure-time gates as their libraries
# (UAPP_CRYPTO_ENABLED / UAPP_OCR_ENABLED), so a build without libsodium or
# without Tesseract still builds the storage harness.

# --- storage: notes, blocks, unified search, database, conversion -----------
add_executable(uapp_loadtest_storage storage_loadtest.cpp)

# core_notes, core_blocks and core_database arrive transitively via
# storage_sqlite's PUBLIC link, exactly as tools/notes_cli relies on.
target_link_libraries(uapp_loadtest_storage
    PRIVATE uapp::storage_sqlite
            uapp::core_search
            uapp::core_conversion
            uapp::compiler_options)

target_include_directories(uapp_loadtest_storage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(uapp_loadtest_storage PROPERTIES
    OUTPUT_NAME "uapp-loadtest-storage")

# --- crypto + keys ----------------------------------------------------------
if(UAPP_CRYPTO_ENABLED)
    add_executable(uapp_loadtest_crypto crypto_loadtest.cpp)

    # security_keys links security_crypto PUBLIC, so both headers are visible.
    target_link_libraries(uapp_loadtest_crypto
        PRIVATE uapp::security_keys uapp::compiler_options)

    target_include_directories(uapp_loadtest_crypto PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    set_target_properties(uapp_loadtest_crypto PROPERTIES
        OUTPUT_NAME "uapp-loadtest-crypto")
endif()

# --- OCR --------------------------------------------------------------------
#
# This target reaches into tests/ocr for ocr_fixture.h, which is the one
# place in the repository that holds a Gray8 image with VERIFIED ground truth
# (generated from source, checked against the real tesseract(1) CLI before
# being embedded). The harness tiles it to reach 19 megapixels, so every pixel
# it feeds the recogniser is text a human has already confirmed is readable,
# and correctness stays checkable at every scale rather than only speed.
#
# This is a narrow, deliberate exception to "tools/ does not depend on
# tests/": the file is a header-only constexpr byte array with no target
# behind it, nothing links tests/, and the alternative — inventing pixels, or
# checking a 19 MB binary into the repository — is worse. If a vendored
# tessdata_fast fixture corpus ever lands under a shared fixtures/ directory
# (owed by docs/spikes/SP-10-findings.md), both this and tests/ocr should move
# to it.
if(UAPP_OCR_ENABLED)
    add_executable(uapp_loadtest_ocr ocr_loadtest.cpp)

    target_link_libraries(uapp_loadtest_ocr
        PRIVATE uapp::ocr_engine uapp::compiler_options)

    target_include_directories(uapp_loadtest_ocr PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/tests/ocr)

    # Same sourcing as tests/ocr: a tessdata directory found on THIS host at
    # configure time, never compiled into ocr/engine, which has no default
    # path by design. Empty means the harness refuses to run and says why
    # rather than reporting a zero.
    target_compile_definitions(uapp_loadtest_ocr PRIVATE
        UAPP_LOADTEST_TESSDATA="${UAPP_TESSERACT_TESSDATA}")

    set_target_properties(uapp_loadtest_ocr PROPERTIES
        OUTPUT_NAME "uapp-loadtest-ocr")
endif()
