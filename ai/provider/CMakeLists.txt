add_library(uapp_ai_provider STATIC
    src/ai_types.cpp
    src/consent.cpp
    src/provider.cpp
    src/registry.cpp)
add_library(uapp::ai_provider ALIAS uapp_ai_provider)

target_include_directories(uapp_ai_provider PUBLIC include)

target_link_libraries(uapp_ai_provider
    PUBLIC  uapp::core_common    # uapp::Status, and the -fno-exceptions/-fno-rtti posture
    PRIVATE uapp::compiler_options)

# Guard rails, enforced by the link line rather than by review. This module is
# the one in the tree where the FIRST of these is easiest to break, and where
# breaking it would matter most:
#
#  * NO uapp::security_crypto and NO uapp::security_keys. The consent boundary
#    here is a TYPE SYSTEM boundary, not a cryptographic one: nothing in this
#    module decrypts, and the caller's PlaintextResolverFn is where a real
#    client will call into the key hierarchy. Keeping crypto off the link line
#    means (a) the boundary can be reasoned about and tested without a key
#    hierarchy, and (b) the shortcut "just decrypt it here, we already have
#    the document key" fails to compile rather than quietly working. The two
#    places that genuinely need crypto -- a cryptographic digest of the
#    consented byte ranges, and a hash-chained persisted consent/audit record
#    -- are NOT IMPLEMENTED and are recorded as such in
#    ai/provider/include/uapp/ai/consent.h and docs/ai-architecture.md §14.
#    When they land they belong in a SEPARATE module above this one (ai/policy),
#    not in here.
#  * NO uapp::sqlite3 and NO uapp::storage_sqlite. There is no AI persistence:
#    no response cache, no embedding cache, no consent-record table, no vector
#    store. All four are real future needs (ai-architecture.md §10, §13.3) and
#    all four belong to storage, which decides HOW to store what this module
#    decides the MEANING of. `nm -u` on the built archive shows no sqlite3_*
#    and no sodium_* / crypto_* import.
#  * NO uapp::core_blocks, core_notes, core_database, core_search,
#    file_detection or pdf_reader. A provider takes text and returns text,
#    vectors or typed fields. It does not know what a Page is. The RAG
#    pipeline (spec §18, NOT IMPLEMENTED) is what will connect documents to
#    this interface, and it will sit ABOVE both -- which is only possible if
#    this layer has no opinion about the document model.
#  * NO uapp::capi. There is no C ABI for this module yet, and the dependency
#    would run the wrong way if there were.
#  * NO network library of any kind, because there is no cloud provider. This
#    environment has no network access to a real AI provider API, so an
#    adapter written here could not be honestly tested -- see
#    docs/ai-architecture.md §14.
