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
# capi_file.cpp is unconditional: file-engine/detection has no third-party
# dependency, so its ABI surface is always built, unlike capi_pdf.cpp below.
add_library(uapp_capi STATIC
    src/capi_common.cpp
    src/capi_notes.cpp
    src/capi_blocks.cpp
    src/capi_search.cpp
    src/capi_file.cpp)
add_library(uapp::capi ALIAS uapp_capi)

target_include_directories(uapp_capi
    PUBLIC  include
    PRIVATE src)

# capi_pdf.cpp exists only when pdf-engine/ itself was built (UAPP_PDF_ENABLED,
# resolved by the root CMakeLists.txt from UAPP_ENABLE_PDF). uapp_pdf_is_available()
# — the one symbol that must always exist so a binding can ask the question —
# lives in capi_common.cpp instead, gated at the PREPROCESSOR level by the
# UAPP_PDF_ENABLED compile definition below, not by which .cpp files are in
# this target.
if(UAPP_PDF_ENABLED)
    target_sources(uapp_capi PRIVATE src/capi_pdf.cpp)
    target_link_libraries(uapp_capi PRIVATE uapp::pdf_reader)
    target_compile_definitions(uapp_capi PRIVATE UAPP_PDF_ENABLED=1)
else()
    target_compile_definitions(uapp_capi PRIVATE UAPP_PDF_ENABLED=0)
endif()

# capi_crypto.cpp/capi_keys.cpp exist only when security/crypto and
# security/keys themselves were built (UAPP_CRYPTO_ENABLED, resolved by the
# root CMakeLists.txt from UAPP_ENABLE_CRYPTO). Unlike PDF, there is no
# runtime "is this surface available" query to keep working either way: a
# caller that built against a header exposing uapp_crypto_* in a library that
# was built without it gets a link error against this static archive, same
# as any other symbol that is simply absent — crypto has no story, the way
# PDF does, of shipping the SURFACE unconditionally while varying whether the
# underlying engine is present.
if(UAPP_CRYPTO_ENABLED)
    target_sources(uapp_capi PRIVATE src/capi_crypto.cpp src/capi_keys.cpp)
    target_link_libraries(uapp_capi PRIVATE uapp::security_crypto uapp::security_keys)
endif()

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
    PRIVATE uapp::storage_sqlite uapp::file_detection uapp::compiler_options)

# The public headers must not need a C++ compiler, a C++ standard library, or
# any of this project's private include paths. They pull in <stddef.h> and
# <stdint.h> and nothing else.
