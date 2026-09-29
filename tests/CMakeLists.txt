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
