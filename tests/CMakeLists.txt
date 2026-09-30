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
