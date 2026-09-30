add_library(uapp_core_blocks STATIC
    src/block.cpp
    src/document_service.cpp
    src/page.cpp)
add_library(uapp::core_blocks ALIAS uapp_core_blocks)

target_include_directories(uapp_core_blocks PUBLIC include)
target_link_libraries(uapp_core_blocks
    PUBLIC  uapp::core_common
    PRIVATE uapp::compiler_options)

# Same guard rail as core/notes (PHASE0_SUMMARY.md H-3): core/ must never
# reach for SQLite. uapp_core_blocks deliberately does NOT link uapp::sqlite3,
# so an accidental `#include <sqlite3.h>` here fails to compile rather than
# quietly working.
#
# It also does NOT link uapp::core_notes. The block model supersedes the flat
# note (docs/adr/0003-page-supersedes-note.md) and must not grow a dependency
# on the thing it replaces.
