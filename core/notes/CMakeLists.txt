add_library(uapp_core_notes STATIC
    src/note.cpp
    src/note_service.cpp
    src/ports.cpp)
add_library(uapp::core_notes ALIAS uapp_core_notes)

target_include_directories(uapp_core_notes PUBLIC include)
target_link_libraries(uapp_core_notes
    PUBLIC  uapp::core_common
    PRIVATE uapp::compiler_options)

# Guard rail for PHASE0_SUMMARY.md H-3: core/ must never reach for SQLite.
# core/notes deliberately does NOT link uapp::sqlite3, so an accidental
# `#include <sqlite3.h>` here fails to compile rather than quietly working.
