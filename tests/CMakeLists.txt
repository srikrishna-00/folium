# tests/ — see tests/support/test_framework.h for why there is no third-party
# test framework here yet.

add_executable(uapp_notes_tests
    support/test_main.cpp
    core/note_id_test.cpp
    storage/fts_query_test.cpp
    storage/note_store_test.cpp)

target_include_directories(uapp_notes_tests PRIVATE support)

# uapp::storage_sqlite links uapp::core_notes PUBLIC, so naming core_notes
# again here would just make the linker warn about a duplicate.
target_link_libraries(uapp_notes_tests
    PRIVATE uapp::storage_sqlite uapp::compiler_options)

add_test(NAME uapp.notes.unit COMMAND uapp_notes_tests)

# ---------------------------------------------------------------------------
# core/blocks — the Notion-like document model (spec section 14).
#
# A SEPARATE executable from uapp_notes_tests, deliberately: it links
# uapp::core_blocks and uapp::storage_sqlite and never names uapp::core_notes,
# so "the block model does not depend on the flat note it supersedes"
# (docs/adr/0003-page-supersedes-note.md) is a property the build graph
# checks rather than one review has to remember. storage/sqlite does still
# drag core_notes in transitively — that is the adapter target's business,
# not core/blocks'.
#
# Two CTest entries over one binary, split by name filter, for the same
# reason the capi tests are split: "blocks.tree failed" and
# "blocks.store failed" mean very different things.
# ---------------------------------------------------------------------------
add_executable(uapp_blocks_tests
    support/test_main.cpp
    core/blocks_test.cpp
    storage/block_store_test.cpp)

target_include_directories(uapp_blocks_tests PRIVATE support)

# uapp::storage_sqlite links uapp::core_blocks PUBLIC, so naming it again
# here only makes the linker warn about a duplicate.
target_link_libraries(uapp_blocks_tests
    PRIVATE uapp::storage_sqlite uapp::compiler_options)

add_test(NAME uapp.blocks.unit  COMMAND uapp_blocks_tests)
add_test(NAME uapp.blocks.store COMMAND uapp_blocks_tests "blocks.store")

set_tests_properties(uapp.blocks.unit uapp.blocks.store PROPERTIES TIMEOUT 120)

# The CLI demo is the integration test: it drives the full slice against a
# real on-disk database in a fresh directory and fails loudly at the first
# broken step. Running it here means "the slice works end to end" is verified
# by CI, not by someone remembering to try it.
add_test(NAME uapp.notes.cli_demo
         COMMAND ${CMAKE_COMMAND}
                 -DUAPP_CLI=$<TARGET_FILE:uapp_notes_cli>
                 -DUAPP_WORKDIR=${CMAKE_CURRENT_BINARY_DIR}/cli_demo_run
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_cli_demo.cmake)

set_tests_properties(uapp.notes.unit uapp.notes.cli_demo PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# core/search + storage/sqlite — the UNIFIED search layer (spec section 16).
#
# A separate executable, and the one test binary that deliberately links BOTH
# domains: proving that one query returns correctly-typed hits from notes AND
# from pages/blocks is only possible with both stores in the same process,
# writing to the same database file. (It names uapp::storage_sqlite only;
# core_notes, core_blocks and core_search all arrive through it PUBLIC-ly.)
#
# Unlike the per-store suites, every case here needs a REAL FILE: two
# ":memory:" opens are two PRIVATE databases, so a note store and a block
# store on ":memory:" could never see each other's index rows. The scratch
# files live in a TempDir that removes itself, so nothing is written outside
# the system temp directory and nothing is left behind.
#
# Four CTest entries over one binary, split by name filter, because the four
# failure modes mean different things: "cross-type search is wrong", "an edit
# left a stale hit behind", "a delete left a hit behind", and "a hostile query
# got through". The last three are the ways a search index rots silently.
# ---------------------------------------------------------------------------
add_executable(uapp_search_tests
    support/test_main.cpp
    search/unified_search_test.cpp)

target_include_directories(uapp_search_tests PRIVATE support)

target_link_libraries(uapp_search_tests
    PRIVATE uapp::storage_sqlite uapp::compiler_options)

add_test(NAME uapp.search.unit    COMMAND uapp_search_tests)
add_test(NAME uapp.search.edits   COMMAND uapp_search_tests "editing")
add_test(NAME uapp.search.deletes COMMAND uapp_search_tests "deleting")
add_test(NAME uapp.search.hostile COMMAND uapp_search_tests "hostile")

set_tests_properties(uapp.search.unit uapp.search.edits uapp.search.deletes
                     uapp.search.hostile PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# core/sync/spike — the Fugue CRDT correctness spike (ADR-0002, SP-6 R-1..R-3).
#
# A separate executable, and the MOST isolated one in the tree: it links only
# uapp::sync_fugue_spike, which links nothing but uapp::compiler_options. It
# cannot reach uapp::core_common, core/blocks, storage/sqlite or the C ABI, so
# "the CRDT spike is standalone research code and is not wired into the
# product" is checked by the link line rather than by review. If a source under
# core/sync/spike ever includes a core header, this target fails to build.
#
# Conditional on UAPP_BUILD_SYNC_SPIKE for the same reason the PDF suite is
# conditional on a vendored PDFium: with the spike off, `ctest` runs exactly
# the suite it ran before the spike existed, and no placeholder reports
# "skipped" for a module that was never compiled.
#
# Five CTest entries over one binary, split by name filter, because the five
# failure modes mean five different things:
#   .basic           — the algorithm does not even do single-replica editing
#   .rga_control     — the DEFECTIVE control stopped reproducing the published
#                      RGA anomaly, which would silently void the P4 result
#   .noninterleaving — P4, the property the algorithm was chosen for
#   .properties      — P1/P2/P3 plus the differential and specification oracles
#   .buffer          — out-of-causal-order delivery and the bounded buffer
#
# The property cases are randomised; UAPP_FUGUE_TRIALS raises the trial count
# for a soak run without a rebuild. The default is sized for the PR path.
# ---------------------------------------------------------------------------
if(TARGET uapp::sync_fugue_spike)
    add_executable(uapp_sync_spike_tests
        support/test_main.cpp
        sync_spike/fugue_spike_test.cpp)

    target_include_directories(uapp_sync_spike_tests PRIVATE support sync_spike)

    target_link_libraries(uapp_sync_spike_tests
        PRIVATE uapp::sync_fugue_spike uapp::compiler_options)

    add_test(NAME uapp.sync_spike.basic           COMMAND uapp_sync_spike_tests "fugue.")
    add_test(NAME uapp.sync_spike.rga_control     COMMAND uapp_sync_spike_tests "rga.control")
    add_test(NAME uapp.sync_spike.noninterleaving COMMAND uapp_sync_spike_tests "noninterleaving.")
    add_test(NAME uapp.sync_spike.properties      COMMAND uapp_sync_spike_tests "property.")
    add_test(NAME uapp.sync_spike.buffer          COMMAND uapp_sync_spike_tests "property.noncausal")

    set_tests_properties(uapp.sync_spike.basic uapp.sync_spike.rga_control
                         uapp.sync_spike.noninterleaving uapp.sync_spike.properties
                         uapp.sync_spike.buffer PROPERTIES TIMEOUT 600)
endif()

# ---------------------------------------------------------------------------
# file-engine/detection — spec section 8/9 file-type detection.
#
# A third separate executable, and the most isolated of them: it links ONLY
# uapp::file_detection, which in turn links only uapp::core_common. It cannot
# reach SQLite, core/notes or core/blocks, so "detection is a pure function
# of bytes with no storage and no document model behind it" is checked by the
# build graph rather than by review. If a detection source ever needs a
# database handle, this link line fails.
#
# Three CTest entries over one binary, split by name filter, because the
# three failure modes mean different things: "a signature is wrong",
# "a hostile file got through", and "a capability claim drifted from
# file-format-support.md" are three different bugs with three different
# owners.
# ---------------------------------------------------------------------------
add_executable(uapp_file_engine_tests
    support/test_main.cpp
    file_engine/detection_test.cpp
    file_engine/file_value_test.cpp)

target_include_directories(uapp_file_engine_tests PRIVATE support file_engine)

target_link_libraries(uapp_file_engine_tests
    PRIVATE uapp::file_detection uapp::compiler_options)

add_test(NAME uapp.file_engine.detect    COMMAND uapp_file_engine_tests "detect.")
add_test(NAME uapp.file_engine.malformed COMMAND uapp_file_engine_tests "malformed")
add_test(NAME uapp.file_engine.unit      COMMAND uapp_file_engine_tests)

set_tests_properties(uapp.file_engine.detect uapp.file_engine.malformed
                     uapp.file_engine.unit PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# The C ABI, exercised from C.
#
# A separate executable, and separate ON PURPOSE: this one is compiled by the C
# compiler at -std=c99 under the same -Wall -Wextra -Wpedantic -Wconversion
# -Werror posture, and it links ONLY uapp::capi. It cannot reach a core header,
# because uapp::capi exposes none. Whatever this file cannot do, Kotlin and
# Swift cannot do either — which makes it a contract test for the boundary
# rather than merely another unit test.
#
# It also is the C99-conformance check architecture.md section 8 rule 5 asks
# for ("a CI job compiles the capi headers standalone with a C compiler"). A
# C++-ism landing in a public header breaks this build.
# ---------------------------------------------------------------------------
add_executable(uapp_capi_tests capi/note_capi_test.c)

target_link_libraries(uapp_capi_tests PRIVATE uapp::capi uapp::compiler_options)

# Every source here is C, so CMake would choose the C driver to link — which
# does not pull in the C++ runtime that uapp::capi's implementation needs. Say
# so explicitly rather than adding a stub .cpp to trick it into doing the right
# thing. (A shared libuapp_capi would not need this; a static one does.)
set_target_properties(uapp_capi_tests PROPERTIES LINKER_LANGUAGE CXX)

# The one case needing a real file on disk — proving notes survive a
# close/reopen — gets its path from here rather than hard-coding one, so the
# test never writes outside the build tree.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/capi_test_run")
target_compile_definitions(uapp_capi_tests PRIVATE
    UAPP_CAPI_TEST_DB_PATH="${CMAKE_CURRENT_BINARY_DIR}/capi_test_run/capi_notes.db")

# Two CTest entries over one binary, split by the name filter. The split is
# meaningful: "slice" is "the feature works through the ABI", "boundary" is
# "the ABI does not crash when the caller is wrong". A failure in one says
# something quite different from a failure in the other.
add_test(NAME uapp.capi.slice    COMMAND uapp_capi_tests "slice:")
add_test(NAME uapp.capi.boundary COMMAND uapp_capi_tests "boundary:")

set_tests_properties(uapp.capi.slice uapp.capi.boundary PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# The PAGE/BLOCK C ABI, exercised from C (ABI 0.2, uapp/capi/uapp_blocks.h).
#
# A SEPARATE executable from uapp_capi_tests, deliberately, and for the reason
# the note/block C++ suites are split: "the note ABI broke" and "the document
# ABI broke" mean very different things to whoever is holding the pager.
#
# Same posture: C compiler, -std=c99, the project warning set with -Werror,
# linking ONLY uapp::capi. It includes uapp_blocks.h, uapp_capi.h and
# uapp_notes.h -- the last one on purpose, so the wrong-handle-type case can
# pass a live NOTE handle where a page is expected, which is exactly the
# mistake a generated binding makes once one wrapper owns several opaque
# `long` handles.
# ---------------------------------------------------------------------------
add_executable(uapp_blocks_capi_tests capi/blocks_capi_test.c)

target_include_directories(uapp_blocks_capi_tests PRIVATE capi)

target_link_libraries(uapp_blocks_capi_tests PRIVATE uapp::capi uapp::compiler_options)

# Every source here is C, so CMake would choose the C driver to link — which
# does not pull in the C++ runtime that uapp::capi's implementation needs.
set_target_properties(uapp_blocks_capi_tests PROPERTIES LINKER_LANGUAGE CXX)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/capi_blocks_test_run")
target_compile_definitions(uapp_blocks_capi_tests PRIVATE
    UAPP_CAPI_BLOCKS_TEST_DB_PATH="${CMAKE_CURRENT_BINARY_DIR}/capi_blocks_test_run/pages.db"
    UAPP_CAPI_BLOCKS_TEST_RO_DB_PATH="${CMAKE_CURRENT_BINARY_DIR}/capi_blocks_test_run/pages_ro.db")

add_test(NAME uapp.capi.blocks.slice    COMMAND uapp_blocks_capi_tests "slice:")
add_test(NAME uapp.capi.blocks.boundary COMMAND uapp_blocks_capi_tests "boundary:")

set_tests_properties(uapp.capi.blocks.slice uapp.capi.blocks.boundary
                     PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# The UNIFIED SEARCH C ABI, exercised from C (ABI 0.2, uapp/capi/uapp_search.h).
#
# A third C executable, and the one that deliberately drives ALL THREE public
# surfaces at once: the property under test — one query string returning a
# note AND a block from a page, each row typed and carrying the ids needed to
# open it — only exists with a note service, a document service and a search
# index alive in one process on one database file. That is precisely what a
# mobile binding will do, and no single-surface test can prove it.
#
# Every case here needs a REAL FILE, for the same reason the C++ search suite
# does: two ":memory:" opens are two PRIVATE databases, so the three handles
# could never see each other's index rows and the symptom would be zero
# results with no error. Each case gets its own scratch path inside the build
# tree, so nothing is written outside it and nothing is left behind.
# ---------------------------------------------------------------------------
add_executable(uapp_search_capi_tests capi/search_capi_test.c)

target_include_directories(uapp_search_capi_tests PRIVATE capi)

target_link_libraries(uapp_search_capi_tests PRIVATE uapp::capi uapp::compiler_options)

set_target_properties(uapp_search_capi_tests PROPERTIES LINKER_LANGUAGE CXX)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/capi_search_test_run")
set(UAPP_CAPI_SEARCH_RUN_DIR "${CMAKE_CURRENT_BINARY_DIR}/capi_search_test_run")
target_compile_definitions(uapp_search_capi_tests PRIVATE
    UAPP_CAPI_SEARCH_TEST_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/unified.db"
    UAPP_CAPI_SEARCH_TEST_EDITS_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/edits.db"
    UAPP_CAPI_SEARCH_TEST_LIMIT_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/limit.db"
    UAPP_CAPI_SEARCH_TEST_ALPHA_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/alpha.db"
    UAPP_CAPI_SEARCH_TEST_BETA_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/beta.db"
    UAPP_CAPI_SEARCH_TEST_OUTLIVE_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/outlive.db"
    UAPP_CAPI_SEARCH_TEST_CONFIG_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/config.db"
    UAPP_CAPI_SEARCH_TEST_QUERY_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/query.db"
    UAPP_CAPI_SEARCH_TEST_BOUNDS_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/bounds.db"
    UAPP_CAPI_SEARCH_TEST_CLOSE_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/close.db"
    UAPP_CAPI_SEARCH_TEST_MIXUP_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/mixup.db")

add_test(NAME uapp.capi.search.slice    COMMAND uapp_search_capi_tests "slice:")
add_test(NAME uapp.capi.search.boundary COMMAND uapp_search_capi_tests "boundary:")

set_tests_properties(uapp.capi.search.slice uapp.capi.search.boundary
                     PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# FILE DETECTION C ABI, exercised from C (ABI 0.3, uapp/capi/uapp_file.h).
#
# A FOURTH C executable. Unconditional, like file-engine/detection itself:
# this surface has no third-party dependency, so there is no build
# configuration in which it is absent. It links ONLY uapp::capi and includes
# only uapp_file.h/uapp_capi.h, matching uapp_file.h's own claim that this is
# the one surface in the ABI with no handle and no state — no scratch
# database path needed, unlike the three test executables above it.
# ---------------------------------------------------------------------------
add_executable(uapp_file_capi_tests capi/file_capi_test.c)

target_include_directories(uapp_file_capi_tests PRIVATE capi)

target_link_libraries(uapp_file_capi_tests PRIVATE uapp::capi uapp::compiler_options)

set_target_properties(uapp_file_capi_tests PROPERTIES LINKER_LANGUAGE CXX)

add_test(NAME uapp.capi.file.slice    COMMAND uapp_file_capi_tests "slice:")
add_test(NAME uapp.capi.file.boundary COMMAND uapp_file_capi_tests "boundary:")

set_tests_properties(uapp.capi.file.slice uapp.capi.file.boundary
                     PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# pdf-engine/reader — spec section 11, the viewing slice only.
#
# Conditional, and that is the point of the condition: PDF support depends on
# a vendored third-party binary (third_party/pdfium) that a fresh clone does
# not have. With it absent, UAPP_PDF_ENABLED is FALSE, this block does not
# execute, and `ctest` runs exactly the suite it ran before pdf-engine
# existed. No skipped-test placeholder is registered, because a test that
# reports "skipped" for a module that was never compiled is a test result
# about nothing.
#
# A FIFTH separate executable. It links ONLY uapp::pdf_reader, which links
# uapp::core_common and (privately) uapp::pdfium — so it cannot reach SQLite,
# core/notes, core/blocks or file-engine/detection, and "the PDF reader is
# bytes in, pixels out, with no storage and no document model behind it" is
# checked by the link line rather than by review.
#
# It also proves the header boundary: this file includes only
# uapp/pdf/document.h and never a PDFium header, so if FPDF_* ever leaked
# into the public interface, this target would need an include path it has
# not been given and the build would fail.
#
# Three CTest entries over one binary, split by name filter, because the
# three failure modes have three different meanings: "the PDF API is wrong",
# "the pixels are wrong", and "a hostile file got through".
# ---------------------------------------------------------------------------
if(UAPP_PDF_ENABLED)
    add_executable(uapp_pdf_tests
        support/test_main.cpp
        pdf_engine/pdf_reader_test.cpp)

    target_include_directories(uapp_pdf_tests PRIVATE support pdf_engine)

    target_link_libraries(uapp_pdf_tests
        PRIVATE uapp::pdf_reader uapp::compiler_options)

    add_test(NAME uapp.pdf.read      COMMAND uapp_pdf_tests "pdf.read")
    add_test(NAME uapp.pdf.render    COMMAND uapp_pdf_tests "pdf.render")
    add_test(NAME uapp.pdf.malformed COMMAND uapp_pdf_tests "pdf.malformed")

    set_tests_properties(uapp.pdf.read uapp.pdf.render uapp.pdf.malformed
                         PROPERTIES TIMEOUT 120)

    # ------------------------------------------------------------------------
    # PDF C ABI, exercised from C (ABI 0.3, uapp/capi/uapp_pdf.h).
    #
    # A SIXTH executable, conditional for the same reason uapp_pdf_tests above
    # is: uapp_pdf.h's functions do not exist in a library built without a
    # vendored PDFium, so this file is not even compiled when UAPP_PDF_ENABLED
    # is false, rather than compiled and skipped at runtime.
    # ------------------------------------------------------------------------
    add_executable(uapp_pdf_capi_tests capi/pdf_capi_test.c)

    target_include_directories(uapp_pdf_capi_tests PRIVATE capi)

    target_link_libraries(uapp_pdf_capi_tests PRIVATE uapp::capi uapp::compiler_options)

    set_target_properties(uapp_pdf_capi_tests PROPERTIES LINKER_LANGUAGE CXX)

    add_test(NAME uapp.capi.pdf.slice    COMMAND uapp_pdf_capi_tests "slice:")
    add_test(NAME uapp.capi.pdf.boundary COMMAND uapp_pdf_capi_tests "boundary:")

    set_tests_properties(uapp.capi.pdf.slice uapp.capi.pdf.boundary
                         PROPERTIES TIMEOUT 120)
endif()
