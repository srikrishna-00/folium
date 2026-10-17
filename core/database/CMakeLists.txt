add_library(uapp_core_database STATIC
    src/database.cpp
    src/database_store.cpp
    src/property.cpp
    src/view.cpp)
add_library(uapp::core_database ALIAS uapp_core_database)

target_include_directories(uapp_core_database PUBLIC include)

target_link_libraries(uapp_core_database
    PUBLIC  uapp::core_blocks   # blocks::PageId — a Database is anchored to a Page
    PRIVATE uapp::compiler_options)
# uapp::core_common arrives transitively and PUBLIC-ly through core_blocks
# (Status, ObjectId), so naming it again here would only make the linker warn
# about a duplicate.

# Guard rails, enforced by the link line rather than by review:
#
#  * NO uapp::sqlite3 and NO uapp::storage_sqlite — PHASE0_SUMMARY.md H-3.
#    This module is the one in the tree where that rule is easiest to break:
#    its whole subject matter is tables, rows, filters and ORDER BY, and the
#    obvious shortcut for "filter 10 000 rows" is to reach for a database
#    handle. It must not. core/database defines what a view MEANS
#    (database::evaluate_table_view); storage/sqlite decides how to answer one
#    quickly. An accidental `#include <sqlite3.h>` here fails to compile
#    instead of quietly working, and `nm -u` on the built archive shows no
#    sqlite3_* import.
#  * NO uapp::core_notes. core/database is built on the block model that
#    supersedes the flat note (docs/adr/0003-page-supersedes-note.md).
#  * NO uapp::core_search. Spec section 15 lists "Search" under a database's
#    supported operations, and that is NOT IMPLEMENTED here precisely so that
#    this dependency does not appear: text search over property values belongs
#    in the unified index (spec section 16), which already owns tokenisation
#    and ranking. See core/database/include/uapp/database/view.h.
#  * NO uapp::capi. There is no C ABI for this module yet (see
#    docs/changelog.md), and the dependency would run the wrong way if there
#    were: capi wraps core, never the reverse.
