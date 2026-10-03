add_library(uapp_core_search STATIC
    src/search_result.cpp)
add_library(uapp::core_search ALIAS uapp_core_search)

target_include_directories(uapp_core_search PUBLIC include)
target_link_libraries(uapp_core_search
    PUBLIC  uapp::core_common
    PRIVATE uapp::compiler_options)

# Guard rails, enforced by the link line rather than by review:
#
#  * NO uapp::sqlite3 — PHASE0_SUMMARY.md H-3. An accidental
#    `#include <sqlite3.h>` here fails to compile instead of quietly working.
#  * NO uapp::core_notes and NO uapp::core_blocks. The unified search layer
#    describes hits in terms of the shared 16-byte ObjectId and a source-kind
#    tag, NOT in terms of either domain's value types. If it linked them, the
#    next source kind (PDF page, OCR region, file name) would add another
#    domain dependency and core/search would end up linked against the whole
#    product. See core/search/include/uapp/search/search_result.h.
