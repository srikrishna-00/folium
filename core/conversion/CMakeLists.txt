add_library(uapp_core_conversion STATIC
    src/fidelity.cpp
    src/markdown.cpp
    src/markdown_syntax.cpp)
add_library(uapp::core_conversion ALIAS uapp_core_conversion)

target_include_directories(uapp_core_conversion PUBLIC include)

# PRIVATE include path for the internal lexical layer (src/markdown_syntax.h).
# It is deliberately NOT under include/: the escape/unescape pair and the
# block-start classifier are an implementation detail shared by the writer and
# the reader, and nothing outside this library may depend on them.
target_include_directories(uapp_core_conversion PRIVATE src)

target_link_libraries(uapp_core_conversion
    PUBLIC  uapp::core_blocks   # the IR *is* blocks::Page / blocks::Block
    PRIVATE uapp::compiler_options)
# uapp::core_common arrives transitively and PUBLIC-ly through core_blocks
# (Status, ObjectId, Clock, IdSource), so naming it again here would only make
# the linker warn about a duplicate.

# Guard rails, enforced by the link line rather than by review:
#
#  * NO uapp::sqlite3 and NO uapp::storage_sqlite — PHASE0_SUMMARY.md H-3, and
#    something stronger than that for this module in particular: a conversion
#    is a PURE IN-MEMORY TRANSFORMATION of a value the caller already holds.
#    If it could reach a database handle, "export this page" would become a
#    function that might do I/O, and the converter would need a transaction
#    story it has no business having. An accidental `#include <sqlite3.h>`
#    here fails to compile instead of quietly working, and `nm -u` on the
#    built archive shows no sqlite3_* import.
#  * NO uapp::core_notes. core/conversion operates on the block model that
#    supersedes the flat note (docs/adr/0003-page-supersedes-note.md); it must
#    not grow a dependency on the thing that model replaces.
#  * NO uapp::capi. There is no C ABI for this module yet (see
#    docs/changelog.md), and the dependency would run the wrong way if there
#    were: capi wraps core, never the reverse.
