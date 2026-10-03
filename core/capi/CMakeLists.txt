# core/capi — libuapp_capi, the stable C99 ABI.
#
# architecture.md §4 names this "the ONLY supported boundary": everything above
# it (uapp-kotlin over JNI, UAPPKit over Swift/ObjC) is a hand-written binding
# that calls these symbols and nothing else. PHASE0_SUMMARY.md H-2 is the
# decision this target implements — split the spec's single "platform bridge"
# into a versioned C ABI plus idiomatic per-platform bindings, so no C++ ABI
# ever crosses JNI or Swift interop.
#
# STATIC for now. The shared build the JNI .so and the iOS framework will want
# (UAPP_CAPI_BUILD_SHARED + a version script closing the export set + the
# ABI-diff CI job from api-design.md §10 rule 5) is a later task; the
# visibility annotations in the public header are already in place for it.
#
# ONE translation unit per public header, and that mapping is deliberate: a
# binding reads uapp_blocks.h and finds capi_blocks.cpp, and the shared-library
# export set (when it lands) is a per-surface decision rather than a hunt
# through one large file.
add_library(uapp_capi STATIC
    src/capi_common.cpp
    src/capi_notes.cpp
    src/capi_blocks.cpp
    src/capi_search.cpp)
add_library(uapp::capi ALIAS uapp_capi)

target_include_directories(uapp_capi
    PUBLIC  include
    PRIVATE src)

# This target is the COMPOSITION ROOT for all three slices: it is where the
# abstract NoteStore, BlockStore and SearchIndex ports are bound to their
# concrete SQLite adapters, and it is the only reason a link to storage/sqlite
# appears above core/. (That is also why nothing here links uapp::core_blocks
# or uapp::core_search by name — storage/sqlite links both PUBLIC, so the
# headers and the archives arrive through it, exactly as core_notes already
# did.) It is PRIVATE,
# so the dependency stops here — nothing that links uapp::capi inherits a path
# to sqlite3, and PHASE0_SUMMARY.md H-3 stays true.
#
# When core/api (the API facade in the accepted layering) exists, the wiring
# moves down into it and this becomes a link to uapp::core_api instead.
#
# Every dependency is PRIVATE on purpose: nothing a consumer of this ABI needs
# is expressed in C++. They still link transitively (CMake keeps PRIVATE deps
# of a static archive in the link closure), but no C++ include path and no
# C++-only compile flag reaches them — which is what lets tests/capi build with
# a C compiler at -std=c99 -Wpedantic -Werror.
target_link_libraries(uapp_capi
    PRIVATE uapp::storage_sqlite uapp::compiler_options)

# The public headers must not need a C++ compiler, a C++ standard library, or
# any of this project's private include paths. They pull in <stddef.h> and
# <stdint.h> and nothing else.
