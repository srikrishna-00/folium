add_library(uapp_ai_local STATIC
    src/local_heuristic_provider.cpp
    src/null_provider.cpp
    src/text_analysis.cpp)
add_library(uapp::ai_local ALIAS uapp_ai_local)

target_include_directories(uapp_ai_local PUBLIC include)

target_link_libraries(uapp_ai_local
    PUBLIC  uapp::ai_provider   # the interface it implements
    PRIVATE uapp::compiler_options)
# uapp::core_common arrives transitively and PUBLIC-ly through ai_provider.

# A SEPARATE target from uapp::ai_provider, deliberately. The dependency runs
# one way only -- uapp::ai_provider does not know this library exists -- so
# "the abstraction is provider-independent" (spec §17: "do not hard-code the
# product to one AI provider") is a property of the build graph rather than of
# this comment. If ai/provider ever reached for LocalHeuristicProvider as a
# default, the link would become circular and the build would fail.
#
# Guard rails, same as ai/provider's and for the same reasons:
#
#  * NO uapp::security_crypto / uapp::security_keys. Nothing here decrypts.
#  * NO uapp::sqlite3 / uapp::storage_sqlite. This provider is a pure function
#    of the text it is handed: no model file to load, no cache to consult, no
#    index to write. `nm -u` on the built archive shows no sqlite3_* and no
#    sodium_* / crypto_* import.
#  * NO uapp::core_blocks / core_notes / core_database / core_search. It takes
#    text, not a Page.
#  * NO network library. Everything here runs on-device, which is the only
#    reason its descriptor can declare Locality::OnDevice and skip the consent
#    boundary.
#  * NO third-party dependency at all, which is why this module (unlike
#    pdf-engine and security/crypto) needs no UAPP_*_ENABLED gate: there is no
#    build configuration in which it is absent, so nothing downstream has to
#    report "skipped" for code that was never compiled.
