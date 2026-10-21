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

# ---------------------------------------------------------------------------
# core/blocks — the Notion-like document model (spec section 14).
#
# A SEPARATE executable from uapp_notes_tests, deliberately: it links
# uapp::core_blocks and uapp::storage_sqlite and never names uapp::core_notes,
# so "the block model does not depend on the flat note it supersedes"
# (docs/adr/0003-page-supersedes-note.md) is a property the build graph
# checks rather than one review has to remember. storage/sqlite does still
# drag core_notes in transitively — that is the adapter target's business,
# not core/blocks'.
#
# Two CTest entries over one binary, split by name filter, for the same
# reason the capi tests are split: "blocks.tree failed" and
# "blocks.store failed" mean very different things.
# ---------------------------------------------------------------------------
add_executable(uapp_blocks_tests
    support/test_main.cpp
    core/blocks_test.cpp
    storage/block_store_test.cpp)

target_include_directories(uapp_blocks_tests PRIVATE support)

# uapp::storage_sqlite links uapp::core_blocks PUBLIC, so naming it again
# here only makes the linker warn about a duplicate.
target_link_libraries(uapp_blocks_tests
    PRIVATE uapp::storage_sqlite uapp::compiler_options)

add_test(NAME uapp.blocks.unit  COMMAND uapp_blocks_tests)
add_test(NAME uapp.blocks.store COMMAND uapp_blocks_tests "blocks.store")

set_tests_properties(uapp.blocks.unit uapp.blocks.store PROPERTIES TIMEOUT 120)

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

# ---------------------------------------------------------------------------
# core/search + storage/sqlite — the UNIFIED search layer (spec section 16).
#
# A separate executable, and the one test binary that deliberately links BOTH
# domains: proving that one query returns correctly-typed hits from notes AND
# from pages/blocks is only possible with both stores in the same process,
# writing to the same database file. (It names uapp::storage_sqlite only;
# core_notes, core_blocks and core_search all arrive through it PUBLIC-ly.)
#
# Unlike the per-store suites, every case here needs a REAL FILE: two
# ":memory:" opens are two PRIVATE databases, so a note store and a block
# store on ":memory:" could never see each other's index rows. The scratch
# files live in a TempDir that removes itself, so nothing is written outside
# the system temp directory and nothing is left behind.
#
# Four CTest entries over one binary, split by name filter, because the four
# failure modes mean different things: "cross-type search is wrong", "an edit
# left a stale hit behind", "a delete left a hit behind", and "a hostile query
# got through". The last three are the ways a search index rots silently.
# ---------------------------------------------------------------------------
add_executable(uapp_search_tests
    support/test_main.cpp
    search/unified_search_test.cpp)

target_include_directories(uapp_search_tests PRIVATE support)

target_link_libraries(uapp_search_tests
    PRIVATE uapp::storage_sqlite uapp::compiler_options)

add_test(NAME uapp.search.unit    COMMAND uapp_search_tests)
add_test(NAME uapp.search.edits   COMMAND uapp_search_tests "editing")
add_test(NAME uapp.search.deletes COMMAND uapp_search_tests "deleting")
add_test(NAME uapp.search.hostile COMMAND uapp_search_tests "hostile")

# ---------------------------------------------------------------------------
# api/ — uapp-server's route handlers (spec: squishy-roaming-comet plan, M1+).
#
# Guarded on UAPP_BUILD_SERVER because uapp_server_routes only exists when
# api/ is added from the root CMakeLists.txt; a tests-on/server-off configure
# must not fail looking for a target that was never created.
#
# Calls handlers directly against the plain transport::Request/Response
# types — no listening socket, no httplib.h in this binary at all (that
# boundary is the whole point of api/src/transport/http_server.h).
# ---------------------------------------------------------------------------
if(UAPP_BUILD_SERVER AND NOT UAPP_ENABLE_FUZZING)
    add_executable(uapp_api_tests
        support/test_main.cpp
        api/health_test.cpp)

    target_include_directories(uapp_api_tests PRIVATE support)

    target_link_libraries(uapp_api_tests
        PRIVATE uapp_server_routes uapp::compiler_options)

    add_test(NAME uapp.api.health COMMAND uapp_api_tests)
endif()

set_tests_properties(uapp.search.unit uapp.search.edits uapp.search.deletes
                     uapp.search.hostile PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# core/conversion — Page/Block <-> Markdown (spec section 10).
#
# A separate executable, unconditional like core/search's: this module has NO
# third-party dependency, so there is no build configuration in which it is
# absent and nothing here reports "skipped" for code that was compiled.
#
# It links ONLY uapp::core_conversion, which brings uapp::core_blocks and
# uapp::core_common with it and NOTHING ELSE. It therefore cannot reach
# SQLite, core/notes, core/search, file-engine or the C ABI, so "a conversion
# is a pure in-memory transformation of a value the caller already holds" is
# checked by the link line rather than by review. If a conversion source ever
# needed a database handle, this target would fail to link.
#
# It also reaches into core/conversion/src for markdown_syntax.h, which is NOT
# a public header. That is deliberate: the escape/unescape pair is an
# implementation detail with an invariant, and the invariant deserves direct
# tests rather than coverage inferred from the public API on top of it.
#
# Three CTest entries over one binary, split by name filter, because the three
# failure modes mean three different things:
#   .escape     — the escaper and the unescaper are no longer inverses, which
#                 is silent data corruption on round-trip and invalidates
#                 every other case in the suite
#   .roundtrip  — a block type or a nesting shape no longer survives the cycle
#   .fidelity   — a documented lossy case changed behaviour or stopped being
#                 reported, i.e. the converter started lying about what it did
#   .bounds     — untrusted input got past a limit
# ---------------------------------------------------------------------------
add_executable(uapp_conversion_tests
    support/test_main.cpp
    conversion/markdown_syntax_test.cpp
    conversion/markdown_test.cpp)

target_include_directories(uapp_conversion_tests
    PRIVATE support "${CMAKE_SOURCE_DIR}/core/conversion/src")

target_link_libraries(uapp_conversion_tests
    PRIVATE uapp::core_conversion uapp::compiler_options)

add_test(NAME uapp.conversion.escape    COMMAND uapp_conversion_tests "conversion.escape")
add_test(NAME uapp.conversion.syntax    COMMAND uapp_conversion_tests "conversion.syntax")
add_test(NAME uapp.conversion.roundtrip COMMAND uapp_conversion_tests "conversion.roundtrip")
add_test(NAME uapp.conversion.fidelity  COMMAND uapp_conversion_tests "conversion.fidelity")
add_test(NAME uapp.conversion.bounds    COMMAND uapp_conversion_tests "conversion.bounds")

set_tests_properties(uapp.conversion.escape uapp.conversion.syntax
                     uapp.conversion.roundtrip uapp.conversion.fidelity
                     uapp.conversion.bounds PROPERTIES TIMEOUT 300)

# ---------------------------------------------------------------------------
# core/database + storage/sqlite — the property-typed database system
# (spec section 15).
#
# A separate executable, and it links uapp::storage_sqlite (which brings
# uapp::core_database with it PUBLIC-ly) for the same reason uapp_blocks_tests
# does: the suite has to drive BOTH the pure domain layer and the real
# adapter, because the most valuable cases here are DIFFERENTIAL -- they run
# database::evaluate_table_view and the adapter's compiled SQL over the same
# rows and require the identical sequence. One process, one database file.
#
# It never names uapp::core_notes, so "the database system is built on the
# block model that supersedes the flat note" (ADR-0003) stays a property of
# the build graph. storage/sqlite does still drag core_notes in transitively;
# that is the adapter target's business, not core/database's.
#
# Every case needs a REAL FILE rather than ":memory:", unlike the blocks
# suite. A database is anchored to a `page` row (ADR-0004) and the `page`
# table is owned by the BLOCK store, so the host page is created through a
# block store on the same file -- and two ":memory:" opens are two PRIVATE
# databases, in which the page would be invisible. The scratch files live in a
# TempDir that removes itself.
#
# Five CTest entries over one binary, split by name filter, because the five
# failure modes mean five different things:
#   .property     — the typed property system or its schema validation broke,
#                   which invalidates every case above it
#   .view         — the MEANING of a Table view changed (operator legality,
#                   filter semantics, sort keys, absent-value ordering)
#   .store        — persistence, the schema round-trip or saved views broke
#   .differential — the compiled SQL and the reference evaluator DISAGREE,
#                   i.e. a view returns a different answer depending on which
#                   code path answered it. This is the one that cannot be
#                   found by reading either implementation alone.
#   .bounds       — untrusted input got past a limit
# ---------------------------------------------------------------------------
add_executable(uapp_database_tests
    support/test_main.cpp
    database/property_test.cpp
    database/view_test.cpp
    database/database_store_test.cpp)

target_include_directories(uapp_database_tests PRIVATE support database)

target_link_libraries(uapp_database_tests
    PRIVATE uapp::storage_sqlite uapp::compiler_options)

add_test(NAME uapp.database.property     COMMAND uapp_database_tests "database.property")
add_test(NAME uapp.database.view         COMMAND uapp_database_tests "database.view")
add_test(NAME uapp.database.store        COMMAND uapp_database_tests "database.store")
add_test(NAME uapp.database.differential COMMAND uapp_database_tests "database.differential")
add_test(NAME uapp.database.bounds       COMMAND uapp_database_tests "database.bounds")

set_tests_properties(uapp.database.property uapp.database.view uapp.database.store
                     uapp.database.differential uapp.database.bounds
                     PROPERTIES TIMEOUT 300)

# ---------------------------------------------------------------------------
# ai/provider + ai/local — the provider-independent AI abstraction (spec §17)
# and the one real on-device implementation behind it.
#
# A separate executable, unconditional like core/conversion's: this module has
# NO third-party dependency, so there is no build configuration in which it is
# absent and nothing here reports "skipped" for code that was compiled.
#
# It links ONLY uapp::ai_local, which brings uapp::ai_provider and
# uapp::core_common with it and NOTHING ELSE. So this binary cannot reach
# SQLite, libsodium, security/crypto, security/keys, core/blocks, core/notes,
# core/database, core/search, file-engine, pdf-engine or the C ABI. Two claims
# are therefore checked by the link line rather than by review:
#
#   1. "a provider takes text and returns text, vectors or typed fields; it
#      does not know what a Page is" — if an AI source ever needed the document
#      model or a database handle, this target would fail to link.
#   2. "the consent boundary in ai/provider/consent.h is a TYPE boundary, not
#      a cryptographic one" — nothing here decrypts, and the shortcut of
#      reaching into security/keys to do it would break this link line. The
#      cryptographic half (a real digest of the consented ranges, a
#      hash-chained persisted consent record) is NOT IMPLEMENTED and belongs
#      in a module above this one. See docs/ai-architecture.md §14.
#
# It also reaches into ai/local/src for text_analysis.h, which is NOT a public
# header, for the same reason the conversion suite reaches for
# markdown_syntax.h: the sentence splitter, the TF-IDF ranker, the field
# scanners and the hash embedder have invariants that deserve direct tests
# rather than coverage inferred from the public API above them.
#
# Four CTest entries over one binary, split by name filter, because the four
# failure modes mean four very different things:
#   .consent  — THE SECURITY PROPERTY. Content reached, or could have reached,
#               an off-device provider without a consent grant, or a grant was
#               honoured that should not have been (wrong provider, expired,
#               revoked, replayed). ADR-0002 decision 1. A failure here is not
#               a bug in a feature, it is the privacy promise not holding.
#   .contract — the abstraction's own guarantees: a provider's declared
#               capabilities no longer match its behaviour, untrusted input got
#               past a bound, or a provider's contract-violating output was
#               passed through instead of being rejected.
#   .local    — the on-device provider's OUTPUT QUALITY changed: the summariser
#               stopped selecting by information content (i.e. became
#               truncation), the classifier started guessing instead of
#               abstaining, or a field scanner began accepting a near-miss.
#   .registry — provider selection or the failover rules broke, including the
#               two that are policy rather than plumbing: no silent
#               substitution for a pinned model, and no failover across a
#               locality boundary.
# ---------------------------------------------------------------------------
add_executable(uapp_ai_tests
    support/test_main.cpp
    ai/consent_test.cpp
    ai/provider_contract_test.cpp
    ai/local_heuristic_test.cpp
    ai/registry_test.cpp)

target_include_directories(uapp_ai_tests
    PRIVATE support ai "${CMAKE_SOURCE_DIR}/ai/local/src")

target_link_libraries(uapp_ai_tests
    PRIVATE uapp::ai_local uapp::compiler_options)

add_test(NAME uapp.ai.consent  COMMAND uapp_ai_tests "ai.consent")
add_test(NAME uapp.ai.contract COMMAND uapp_ai_tests "ai.contract")
add_test(NAME uapp.ai.local    COMMAND uapp_ai_tests "ai.local")
add_test(NAME uapp.ai.text     COMMAND uapp_ai_tests "ai.text")
add_test(NAME uapp.ai.registry COMMAND uapp_ai_tests "ai.registry")

# TIMEOUT 300 rather than 120: three cases build a 1 MiB input on purpose
# (the oversized-input bound, the at-the-cap boundary, and the pathological
# all-numbers extraction), and under ASan+UBSan a megabyte of byte-at-a-time
# scanning is several times slower than in a plain Debug build. A timeout that
# only fails in the sanitizer configuration is worse than a generous one.
set_tests_properties(uapp.ai.consent uapp.ai.contract uapp.ai.local uapp.ai.text
                     uapp.ai.registry PROPERTIES TIMEOUT 300)

# ---------------------------------------------------------------------------
# core/sync/spike — the Fugue CRDT correctness spike (ADR-0002, SP-6 R-1..R-3).
#
# A separate executable, and the MOST isolated one in the tree: it links only
# uapp::sync_fugue_spike, which links nothing but uapp::compiler_options. It
# cannot reach uapp::core_common, core/blocks, storage/sqlite or the C ABI, so
# "the CRDT spike is standalone research code and is not wired into the
# product" is checked by the link line rather than by review. If a source under
# core/sync/spike ever includes a core header, this target fails to build.
#
# Conditional on UAPP_BUILD_SYNC_SPIKE for the same reason the PDF suite is
# conditional on a vendored PDFium: with the spike off, `ctest` runs exactly
# the suite it ran before the spike existed, and no placeholder reports
# "skipped" for a module that was never compiled.
#
# Five CTest entries over one binary, split by name filter, because the five
# failure modes mean five different things:
#   .basic           — the algorithm does not even do single-replica editing
#   .rga_control     — the DEFECTIVE control stopped reproducing the published
#                      RGA anomaly, which would silently void the P4 result
#   .noninterleaving — P4, the property the algorithm was chosen for
#   .properties      — P1/P2/P3 plus the differential and specification oracles
#   .buffer          — out-of-causal-order delivery and the bounded buffer
#
# The property cases are randomised; UAPP_FUGUE_TRIALS raises the trial count
# for a soak run without a rebuild. The default is sized for the PR path.
# ---------------------------------------------------------------------------
if(TARGET uapp::sync_fugue_spike)
    add_executable(uapp_sync_spike_tests
        support/test_main.cpp
        sync_spike/fugue_spike_test.cpp)

    target_include_directories(uapp_sync_spike_tests PRIVATE support sync_spike)

    target_link_libraries(uapp_sync_spike_tests
        PRIVATE uapp::sync_fugue_spike uapp::compiler_options)

    add_test(NAME uapp.sync_spike.basic           COMMAND uapp_sync_spike_tests "fugue.")
    add_test(NAME uapp.sync_spike.rga_control     COMMAND uapp_sync_spike_tests "rga.control")
    add_test(NAME uapp.sync_spike.noninterleaving COMMAND uapp_sync_spike_tests "noninterleaving.")
    add_test(NAME uapp.sync_spike.properties      COMMAND uapp_sync_spike_tests "property.")
    add_test(NAME uapp.sync_spike.buffer          COMMAND uapp_sync_spike_tests "property.noncausal")

    set_tests_properties(uapp.sync_spike.basic uapp.sync_spike.rga_control
                         uapp.sync_spike.noninterleaving uapp.sync_spike.properties
                         uapp.sync_spike.buffer PROPERTIES TIMEOUT 600)
endif()

# ---------------------------------------------------------------------------
# file-engine/detection — spec section 8/9 file-type detection.
#
# A third separate executable, and the most isolated of them: it links ONLY
# uapp::file_detection, which in turn links only uapp::core_common. It cannot
# reach SQLite, core/notes or core/blocks, so "detection is a pure function
# of bytes with no storage and no document model behind it" is checked by the
# build graph rather than by review. If a detection source ever needs a
# database handle, this link line fails.
#
# Three CTest entries over one binary, split by name filter, because the
# three failure modes mean different things: "a signature is wrong",
# "a hostile file got through", and "a capability claim drifted from
# file-format-support.md" are three different bugs with three different
# owners.
# ---------------------------------------------------------------------------
add_executable(uapp_file_engine_tests
    support/test_main.cpp
    file_engine/detection_test.cpp
    file_engine/file_value_test.cpp)

target_include_directories(uapp_file_engine_tests PRIVATE support file_engine)

target_link_libraries(uapp_file_engine_tests
    PRIVATE uapp::file_detection uapp::compiler_options)

add_test(NAME uapp.file_engine.detect    COMMAND uapp_file_engine_tests "detect.")
add_test(NAME uapp.file_engine.malformed COMMAND uapp_file_engine_tests "malformed")
add_test(NAME uapp.file_engine.unit      COMMAND uapp_file_engine_tests)

set_tests_properties(uapp.file_engine.detect uapp.file_engine.malformed
                     uapp.file_engine.unit PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# The C ABI, exercised from C.
#
# A separate executable, and separate ON PURPOSE: this one is compiled by the C
# compiler at -std=c99 under the same -Wall -Wextra -Wpedantic -Wconversion
# -Werror posture, and it links ONLY uapp::capi. It cannot reach a core header,
# because uapp::capi exposes none. Whatever this file cannot do, Kotlin and
# Swift cannot do either — which makes it a contract test for the boundary
# rather than merely another unit test.
#
# It also is the C99-conformance check architecture.md section 8 rule 5 asks
# for ("a CI job compiles the capi headers standalone with a C compiler"). A
# C++-ism landing in a public header breaks this build.
# ---------------------------------------------------------------------------
add_executable(uapp_capi_tests capi/note_capi_test.c)

target_link_libraries(uapp_capi_tests PRIVATE uapp::capi uapp::compiler_options)

# Every source here is C, so CMake would choose the C driver to link — which
# does not pull in the C++ runtime that uapp::capi's implementation needs. Say
# so explicitly rather than adding a stub .cpp to trick it into doing the right
# thing. (A shared libuapp_capi would not need this; a static one does.)
set_target_properties(uapp_capi_tests PROPERTIES LINKER_LANGUAGE CXX)

# The one case needing a real file on disk — proving notes survive a
# close/reopen — gets its path from here rather than hard-coding one, so the
# test never writes outside the build tree.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/capi_test_run")
target_compile_definitions(uapp_capi_tests PRIVATE
    UAPP_CAPI_TEST_DB_PATH="${CMAKE_CURRENT_BINARY_DIR}/capi_test_run/capi_notes.db")

# Two CTest entries over one binary, split by the name filter. The split is
# meaningful: "slice" is "the feature works through the ABI", "boundary" is
# "the ABI does not crash when the caller is wrong". A failure in one says
# something quite different from a failure in the other.
add_test(NAME uapp.capi.slice    COMMAND uapp_capi_tests "slice:")
add_test(NAME uapp.capi.boundary COMMAND uapp_capi_tests "boundary:")

set_tests_properties(uapp.capi.slice uapp.capi.boundary PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# The PAGE/BLOCK C ABI, exercised from C (ABI 0.2, uapp/capi/uapp_blocks.h).
#
# A SEPARATE executable from uapp_capi_tests, deliberately, and for the reason
# the note/block C++ suites are split: "the note ABI broke" and "the document
# ABI broke" mean very different things to whoever is holding the pager.
#
# Same posture: C compiler, -std=c99, the project warning set with -Werror,
# linking ONLY uapp::capi. It includes uapp_blocks.h, uapp_capi.h and
# uapp_notes.h -- the last one on purpose, so the wrong-handle-type case can
# pass a live NOTE handle where a page is expected, which is exactly the
# mistake a generated binding makes once one wrapper owns several opaque
# `long` handles.
# ---------------------------------------------------------------------------
add_executable(uapp_blocks_capi_tests capi/blocks_capi_test.c)

target_include_directories(uapp_blocks_capi_tests PRIVATE capi)

target_link_libraries(uapp_blocks_capi_tests PRIVATE uapp::capi uapp::compiler_options)

# Every source here is C, so CMake would choose the C driver to link — which
# does not pull in the C++ runtime that uapp::capi's implementation needs.
set_target_properties(uapp_blocks_capi_tests PROPERTIES LINKER_LANGUAGE CXX)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/capi_blocks_test_run")
target_compile_definitions(uapp_blocks_capi_tests PRIVATE
    UAPP_CAPI_BLOCKS_TEST_DB_PATH="${CMAKE_CURRENT_BINARY_DIR}/capi_blocks_test_run/pages.db"
    UAPP_CAPI_BLOCKS_TEST_RO_DB_PATH="${CMAKE_CURRENT_BINARY_DIR}/capi_blocks_test_run/pages_ro.db")

add_test(NAME uapp.capi.blocks.slice    COMMAND uapp_blocks_capi_tests "slice:")
add_test(NAME uapp.capi.blocks.boundary COMMAND uapp_blocks_capi_tests "boundary:")

set_tests_properties(uapp.capi.blocks.slice uapp.capi.blocks.boundary
                     PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# The UNIFIED SEARCH C ABI, exercised from C (ABI 0.2, uapp/capi/uapp_search.h).
#
# A third C executable, and the one that deliberately drives ALL THREE public
# surfaces at once: the property under test — one query string returning a
# note AND a block from a page, each row typed and carrying the ids needed to
# open it — only exists with a note service, a document service and a search
# index alive in one process on one database file. That is precisely what a
# mobile binding will do, and no single-surface test can prove it.
#
# Every case here needs a REAL FILE, for the same reason the C++ search suite
# does: two ":memory:" opens are two PRIVATE databases, so the three handles
# could never see each other's index rows and the symptom would be zero
# results with no error. Each case gets its own scratch path inside the build
# tree, so nothing is written outside it and nothing is left behind.
# ---------------------------------------------------------------------------
add_executable(uapp_search_capi_tests capi/search_capi_test.c)

target_include_directories(uapp_search_capi_tests PRIVATE capi)

target_link_libraries(uapp_search_capi_tests PRIVATE uapp::capi uapp::compiler_options)

set_target_properties(uapp_search_capi_tests PROPERTIES LINKER_LANGUAGE CXX)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/capi_search_test_run")
set(UAPP_CAPI_SEARCH_RUN_DIR "${CMAKE_CURRENT_BINARY_DIR}/capi_search_test_run")
target_compile_definitions(uapp_search_capi_tests PRIVATE
    UAPP_CAPI_SEARCH_TEST_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/unified.db"
    UAPP_CAPI_SEARCH_TEST_EDITS_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/edits.db"
    UAPP_CAPI_SEARCH_TEST_LIMIT_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/limit.db"
    UAPP_CAPI_SEARCH_TEST_ALPHA_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/alpha.db"
    UAPP_CAPI_SEARCH_TEST_BETA_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/beta.db"
    UAPP_CAPI_SEARCH_TEST_OUTLIVE_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/outlive.db"
    UAPP_CAPI_SEARCH_TEST_CONFIG_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/config.db"
    UAPP_CAPI_SEARCH_TEST_QUERY_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/query.db"
    UAPP_CAPI_SEARCH_TEST_BOUNDS_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/bounds.db"
    UAPP_CAPI_SEARCH_TEST_CLOSE_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/close.db"
    UAPP_CAPI_SEARCH_TEST_MIXUP_DB_PATH="${UAPP_CAPI_SEARCH_RUN_DIR}/mixup.db")

add_test(NAME uapp.capi.search.slice    COMMAND uapp_search_capi_tests "slice:")
add_test(NAME uapp.capi.search.boundary COMMAND uapp_search_capi_tests "boundary:")

set_tests_properties(uapp.capi.search.slice uapp.capi.search.boundary
                     PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# FILE DETECTION C ABI, exercised from C (ABI 0.3, uapp/capi/uapp_file.h).
#
# A FOURTH C executable. Unconditional, like file-engine/detection itself:
# this surface has no third-party dependency, so there is no build
# configuration in which it is absent. It links ONLY uapp::capi and includes
# only uapp_file.h/uapp_capi.h, matching uapp_file.h's own claim that this is
# the one surface in the ABI with no handle and no state — no scratch
# database path needed, unlike the three test executables above it.
# ---------------------------------------------------------------------------
add_executable(uapp_file_capi_tests capi/file_capi_test.c)

target_include_directories(uapp_file_capi_tests PRIVATE capi)

target_link_libraries(uapp_file_capi_tests PRIVATE uapp::capi uapp::compiler_options)

set_target_properties(uapp_file_capi_tests PROPERTIES LINKER_LANGUAGE CXX)

add_test(NAME uapp.capi.file.slice    COMMAND uapp_file_capi_tests "slice:")
add_test(NAME uapp.capi.file.boundary COMMAND uapp_file_capi_tests "boundary:")

set_tests_properties(uapp.capi.file.slice uapp.capi.file.boundary
                     PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# pdf-engine/writer — the PDF WRITING surface (images -> PDF).
#
# UNCONDITIONAL, unlike pdf-engine/reader just below: uapp::pdf_writer has no
# PDFium dependency at all (see pdf-engine/writer/CMakeLists.txt), so a build
# with no vendored PDFium must still get real coverage for it, not a silent
# gap. The stronger claim — that a real PDF reader can open what this module
# writes — is proven separately, in uapp_pdf_tests' "pdf.roundtrip" case
# below, because THAT needs PDFium and so can only run when it is present.
# ---------------------------------------------------------------------------
add_executable(uapp_pdf_writer_tests
    support/test_main.cpp
    pdf_engine/pdf_writer_test.cpp)

target_include_directories(uapp_pdf_writer_tests PRIVATE support pdf_engine)

target_link_libraries(uapp_pdf_writer_tests
    PRIVATE uapp::pdf_writer uapp::compiler_options)

add_test(NAME uapp.pdf_writer.build   COMMAND uapp_pdf_writer_tests "writer.build")
add_test(NAME uapp.pdf_writer.decode  COMMAND uapp_pdf_writer_tests "writer.decode")
add_test(NAME uapp.pdf_writer.reject  COMMAND uapp_pdf_writer_tests "writer.reject")

set_tests_properties(uapp.pdf_writer.build uapp.pdf_writer.decode uapp.pdf_writer.reject
                     PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# pdf-engine/reader — spec section 11, the viewing slice only.
#
# Conditional, and that is the point of the condition: PDF support depends on
# a vendored third-party binary (third_party/pdfium) that a fresh clone does
# not have. With it absent, UAPP_PDF_ENABLED is FALSE, this block does not
# execute, and `ctest` runs exactly the suite it ran before pdf-engine
# existed. No skipped-test placeholder is registered, because a test that
# reports "skipped" for a module that was never compiled is a test result
# about nothing.
#
# A FIFTH separate executable. It links ONLY uapp::pdf_reader, which links
# uapp::core_common and (privately) uapp::pdfium — so it cannot reach SQLite,
# core/notes, core/blocks or file-engine/detection, and "the PDF reader is
# bytes in, pixels out, with no storage and no document model behind it" is
# checked by the link line rather than by review.
#
# It also proves the header boundary: this file includes only
# uapp/pdf/document.h and never a PDFium header, so if FPDF_* ever leaked
# into the public interface, this target would need an include path it has
# not been given and the build would fail.
#
# Four CTest entries over one binary now, split by name filter, because the
# four failure modes have four different meanings: "the PDF API is wrong",
# "the pixels are wrong", "a hostile file got through", and (the newest)
# "our own writer's output does not actually open in a real PDF reader".
# That fourth case is why this binary ALSO links uapp::pdf_writer now — it
# is the one place in the whole test suite that can check both halves of
# round-trip correctness in one process: write with pdf-engine/writer, open
# and render with pdf-engine/reader, same real PDFium underneath the latter.
# ---------------------------------------------------------------------------
if(UAPP_PDF_ENABLED)
    add_executable(uapp_pdf_tests
        support/test_main.cpp
        pdf_engine/pdf_reader_test.cpp)

    target_include_directories(uapp_pdf_tests PRIVATE support pdf_engine)

    target_link_libraries(uapp_pdf_tests
        PRIVATE uapp::pdf_reader uapp::pdf_writer uapp::compiler_options)

    add_test(NAME uapp.pdf.read      COMMAND uapp_pdf_tests "pdf.read")
    add_test(NAME uapp.pdf.render    COMMAND uapp_pdf_tests "pdf.render")
    add_test(NAME uapp.pdf.roundtrip COMMAND uapp_pdf_tests "pdf.roundtrip")
    add_test(NAME uapp.pdf.malformed COMMAND uapp_pdf_tests "pdf.malformed")

    set_tests_properties(uapp.pdf.read uapp.pdf.render uapp.pdf.roundtrip uapp.pdf.malformed
                         PROPERTIES TIMEOUT 120)

    # ------------------------------------------------------------------------
    # PDF C ABI, exercised from C (ABI 0.3, uapp/capi/uapp_pdf.h).
    #
    # A SIXTH executable, conditional for the same reason uapp_pdf_tests above
    # is: uapp_pdf.h's functions do not exist in a library built without a
    # vendored PDFium, so this file is not even compiled when UAPP_PDF_ENABLED
    # is false, rather than compiled and skipped at runtime.
    # ------------------------------------------------------------------------
    add_executable(uapp_pdf_capi_tests capi/pdf_capi_test.c)

    target_include_directories(uapp_pdf_capi_tests PRIVATE capi)

    target_link_libraries(uapp_pdf_capi_tests PRIVATE uapp::capi uapp::compiler_options)

    set_target_properties(uapp_pdf_capi_tests PROPERTIES LINKER_LANGUAGE CXX)

    add_test(NAME uapp.capi.pdf.slice    COMMAND uapp_pdf_capi_tests "slice:")
    add_test(NAME uapp.capi.pdf.boundary COMMAND uapp_pdf_capi_tests "boundary:")

    set_tests_properties(uapp.capi.pdf.slice uapp.capi.pdf.boundary
                         PROPERTIES TIMEOUT 120)
endif()

# ---------------------------------------------------------------------------
# security/crypto — the libsodium-backed cryptographic primitives
# (spec §23/§38, ADR-0002 decision 1).
#
# Conditional on UAPP_CRYPTO_ENABLED for the same reason uapp_pdf_tests is
# conditional on UAPP_PDF_ENABLED: with libsodium absent the module is not
# compiled at all, so these cases cannot be compiled-and-skipped. No
# skipped-test placeholder is registered — a test that reports "skipped" for
# a module that was never built is a test result about nothing.
#
# A SEVENTH separate executable. It links ONLY uapp::security_crypto, which
# links uapp::core_common PUBLIC and uapp::sodium PRIVATE — so it cannot
# reach SQLite, core/notes, core/blocks, core/database, file-engine or
# pdf-engine, and "the crypto module encrypts bytes and knows nothing about
# the document model" is checked by the link line rather than by review.
#
# It also proves the HEADER boundary, which is the more valuable half: this
# file includes only uapp/crypto/*.h and never <sodium.h>, and it is not
# given libsodium's include path. If a libsodium type or macro ever leaked
# into security/crypto's public interface, this target would stop compiling.
# That is why the hex parser in crypto_test.cpp is hand-written instead of
# calling sodium_hex2bin, which would have been one line.
#
# Six CTest entries over one binary, split by name filter, because the six
# failure modes mean six different things:
#   .init     — libsodium is missing, mis-versioned or failed to initialise;
#               everything below it is meaningless if this fails
#   .vectors  — we disagree with an EXTERNAL published test vector
#               (draft-irtf-cfrg-xchacha-03 A.3.1, RFC 7748 §6.1). This is
#               the entry that says "our ciphertext is not what the standard
#               says it should be", i.e. the only one that can catch a
#               wrong-construction bug a round trip would hide.
#   .aead     — encrypt/decrypt, or one of the ways decryption must fail
#               (wrong key, tampered, truncated, wrong nonce, wrong AAD)
#               without crashing and without returning plaintext
#   .wrap     — key wrapping between device keypairs, including that the
#               wrong device and the wrong sender are both rejected
#   .kdf      — Argon2id determinism, parameter separation, and the cost
#               parameters chosen for the 2 GB device floor
#   .nonce    — the nonce/key uniqueness sampling. A failure here means the
#               CSPRNG path is broken, which invalidates .aead and .wrap even
#               though both would still pass
#   .memory   — SecureBytes: guarded allocation, zeroing, move ownership
#
# TIMEOUT 300 rather than the 120 used elsewhere. .kdf runs Argon2id five
# times, including once at DesktopModerate's 256 MiB, and under
# ASan+UBSan a memory-hard function is several times slower than in a plain
# Debug build. A timeout that only fails in the sanitizer configuration is
# worse than a generous one.
# ---------------------------------------------------------------------------
if(UAPP_CRYPTO_ENABLED)
    add_executable(uapp_crypto_tests
        support/test_main.cpp
        crypto/crypto_test.cpp)

    target_include_directories(uapp_crypto_tests PRIVATE support crypto)

    target_link_libraries(uapp_crypto_tests
        PRIVATE uapp::security_crypto uapp::compiler_options)

    # UAPP_SODIUM_VERSION_EXPECTED: what UappSodium.cmake's configure-time
    # probe found on THIS machine, not the third_party/libsodium/VERSION pin
    # hardcoded for the macOS Homebrew bootstrap. Real-world finding, not a
    # guess: the pin is 1.0.22 (this project's own macOS dev host); Ubuntu's
    # packaged libsodium is 1.0.18-2, confirmed by actually building inside
    # an `ubuntu:latest` container. Asserting "always 1.0.22" would make this
    # test fail on every non-pinned platform for a reason that has nothing to
    # do with a real drift — which is a worse test, not a stricter one, since
    # it stops distinguishing "the libsodium on THIS machine is not what THIS
    # machine's own CMake run found" (a real bug: something changed between
    # configure and link) from "this machine was never the pinned one in the
    # first place" (expected, every CI platform except the one third_party/
    # libsodium/VERSION documents). crypto_test.cpp now checks the former.
    target_compile_definitions(uapp_crypto_tests PRIVATE
        UAPP_SODIUM_VERSION_EXPECTED="${UAPP_SODIUM_VERSION}")

    add_test(NAME uapp.crypto.init    COMMAND uapp_crypto_tests "crypto.init")
    add_test(NAME uapp.crypto.vectors COMMAND uapp_crypto_tests "crypto.vectors")
    add_test(NAME uapp.crypto.aead    COMMAND uapp_crypto_tests "crypto.aead")
    add_test(NAME uapp.crypto.wrap    COMMAND uapp_crypto_tests "crypto.wrap")
    add_test(NAME uapp.crypto.kdf     COMMAND uapp_crypto_tests "crypto.kdf")
    add_test(NAME uapp.crypto.hkdf    COMMAND uapp_crypto_tests "crypto.hkdf")
    add_test(NAME uapp.crypto.nonce   COMMAND uapp_crypto_tests "crypto.nonce")
    add_test(NAME uapp.crypto.memory  COMMAND uapp_crypto_tests "crypto.memory")

    set_tests_properties(uapp.crypto.init uapp.crypto.vectors uapp.crypto.aead
                         uapp.crypto.wrap uapp.crypto.kdf uapp.crypto.hkdf
                         uapp.crypto.nonce uapp.crypto.memory
                         PROPERTIES TIMEOUT 300)
endif()

# ---------------------------------------------------------------------------
# security/keys — the key HIERARCHY (security-model.md section 5A.1,
# ADR-0002 decision 1).
#
# An EIGHTH separate executable, under the same UAPP_CRYPTO_ENABLED guard as
# uapp_crypto_tests and for the same reason: with libsodium absent neither
# module is compiled, so these cases cannot be compiled-and-skipped.
#
# It links ONLY uapp::security_keys, which brings uapp::security_crypto in
# PUBLIC-ly and libsodium not at all. So this binary cannot reach SQLite,
# core/notes, core/blocks, core/database, file-engine, pdf-engine OR a raw
# libsodium call, and "the key hierarchy is a function over keys and ids that
# knows nothing about storage or the document model" is checked by the link
# line rather than by review. It is deliberately a SEPARATE binary from
# uapp_crypto_tests rather than two more files in one: "the primitives are
# wrong" and "the hierarchy over them is wrong" are different failures, and
# tests/crypto proves a header boundary (no <sodium.h>) that would be
# diluted by mixing the two.
#
# Six CTest entries over one binary, split by name filter, because the six
# failure modes mean six different things:
#   .root       — the recovery secret and the URK derived from it. Everything
#                 below is meaningless if this edge is wrong, and a change
#                 here invalidates every existing user's recovery phrase
#   .device     — the URK wrapped to another of the user's devices (the
#                 "DEVICE KEY BUNDLE"), including that a third device, a
#                 wrong sender and a tampered or relabelled record are all
#                 rejected
#   .recovery   — the URK under an Argon2id-derived key, including a wrong
#                 passphrase, a tampered salt and a downgraded KDF profile.
#                 FLAGGED FOR REVIEW as an additional path, not the root of
#                 trust (security/keys/include/uapp/keys/key_hierarchy.h)
#   .workspace  — the workspace key wrapped under the URK, and that the
#                 workspace id is really bound into the record
#   .document   — per-document keys DERIVED, not stored (the section 5A.1
#                 scheme), including the edge-confusion check that proves the
#                 domain-separation labels reach the derivation
#   .chain      — the whole hierarchy on real content: recovery phrase ->
#                 URK -> workspace key -> document key -> AEAD, read back by
#                 a second device, by the phrase alone, and by the
#                 passphrase-wrapped URK. This is the entry that fails if any
#                 key in the design is accidentally per-device or
#                 nondeterministic
#
# TIMEOUT 300, matching uapp_crypto_tests: .recovery and .chain run Argon2id
# several times, and under ASan+UBSan a memory-hard function is several times
# slower than in a plain Debug build.
# ---------------------------------------------------------------------------
if(UAPP_CRYPTO_ENABLED)
    add_executable(uapp_keys_tests
        support/test_main.cpp
        keys/key_hierarchy_test.cpp)

    target_include_directories(uapp_keys_tests PRIVATE support keys)

    target_link_libraries(uapp_keys_tests
        PRIVATE uapp::security_keys uapp::compiler_options)

    add_test(NAME uapp.keys.root      COMMAND uapp_keys_tests "keys.root")
    add_test(NAME uapp.keys.device    COMMAND uapp_keys_tests "keys.device")
    add_test(NAME uapp.keys.recovery  COMMAND uapp_keys_tests "keys.recovery")
    add_test(NAME uapp.keys.workspace COMMAND uapp_keys_tests "keys.workspace")
    add_test(NAME uapp.keys.document  COMMAND uapp_keys_tests "keys.document")
    add_test(NAME uapp.keys.chain     COMMAND uapp_keys_tests "keys.chain")

    set_tests_properties(uapp.keys.root uapp.keys.device uapp.keys.recovery
                         uapp.keys.workspace uapp.keys.document uapp.keys.chain
                         PROPERTIES TIMEOUT 300)

    # ------------------------------------------------------------------------
    # security/crypto + security/keys C ABI, exercised from C (ABI 0.4,
    # uapp/capi/uapp_crypto.h + uapp/capi/uapp_keys.h).
    #
    # Same pattern as uapp_pdf_capi_tests above: two more executables,
    # conditional for the same reason — the ABI functions do not exist in a
    # library built without libsodium, so these files are not even compiled
    # when UAPP_CRYPTO_ENABLED is false.
    #
    # crypto_capi_test.c's HKDF and Argon2id cases re-check the SAME
    # externally-verified golden values crypto_test.cpp above does (copied
    # verbatim, see that file's own comment for provenance), now reached
    # ONLY through the C ABI — the M2 verification step this pair of
    # executables exists to satisfy.
    # ------------------------------------------------------------------------
    add_executable(uapp_crypto_capi_tests capi/crypto_capi_test.c)
    target_include_directories(uapp_crypto_capi_tests PRIVATE capi)
    target_link_libraries(uapp_crypto_capi_tests PRIVATE uapp::capi uapp::compiler_options)
    set_target_properties(uapp_crypto_capi_tests PROPERTIES LINKER_LANGUAGE CXX)

    add_test(NAME uapp.capi.crypto.slice    COMMAND uapp_crypto_capi_tests "slice:")
    add_test(NAME uapp.capi.crypto.boundary COMMAND uapp_crypto_capi_tests "boundary:")

    set_tests_properties(uapp.capi.crypto.slice uapp.capi.crypto.boundary
                         PROPERTIES TIMEOUT 120)

    add_executable(uapp_keys_capi_tests capi/keys_capi_test.c)
    target_include_directories(uapp_keys_capi_tests PRIVATE capi)
    target_link_libraries(uapp_keys_capi_tests PRIVATE uapp::capi uapp::compiler_options)
    set_target_properties(uapp_keys_capi_tests PROPERTIES LINKER_LANGUAGE CXX)

    add_test(NAME uapp.capi.keys.slice    COMMAND uapp_keys_capi_tests "slice:")
    add_test(NAME uapp.capi.keys.boundary COMMAND uapp_keys_capi_tests "boundary:")

    set_tests_properties(uapp.capi.keys.slice uapp.capi.keys.boundary
                         PROPERTIES TIMEOUT 120)
endif()

# ---------------------------------------------------------------------------
# ocr/engine — printed-text recognition against a real Tesseract (spec
# section 13).
#
# Conditional on UAPP_OCR_ENABLED for the same reason uapp_pdf_tests is
# conditional on UAPP_PDF_ENABLED and uapp_crypto_tests on
# UAPP_CRYPTO_ENABLED: with no system Tesseract found, ocr/engine is not
# compiled at all, so these cases cannot be compiled-and-skipped. No
# skipped-test placeholder is registered.
#
# A separate executable. It links ONLY uapp::ocr_engine, which links
# uapp::core_common PUBLIC and uapp::tesseract PRIVATE — so it cannot reach
# SQLite, core/notes, core/blocks, core/database, file-engine, pdf-engine or
# ai/, and "recognition is pixels in, TextBlocks out, with no document model
# or storage behind it" is checked by the link line rather than by review. It
# also proves the header boundary this module cares about most: this file
# includes only uapp/ocr/engine.h and never <tesseract/capi.h>.
#
# UAPP_OCR_TEST_TESSDATA is handed to the test target as a compile
# definition, sourced from UAPP_TESSERACT_TESSDATA (cmake/UappTesseract.cmake
# — a tessdata directory located on THIS host, never compiled into
# ocr/engine itself, which has no default path by design; see
# Options::tessdata_path). If no tessdata was found at configure time the
# macro is defined empty and the "ocr.read" recognition cases skip
# themselves at runtime with a clear message, rather than failing for a
# reason that has nothing to do with the module under test — the library
# still links and the input-validation cases ("ocr.bounds", most of
# "ocr.open") still run regardless, because they need no model at all.
#
# Three CTest entries over one binary, split by name filter, because the
# three failure modes mean different things:
#   .read    — real recognition against the embedded fixture image (spec
#              section 13's actual OCR claim: clear text is read correctly,
#              with a real bounding box and confidence; a blank or noise
#              image degrades gracefully rather than fabricating a result)
#   .bounds  — the image-geometry validation spec section 38 requires
#              (oversized dimensions, oversized total pixel count, a buffer
#              too small for the claimed geometry), needing no Tesseract
#              model at all
#   .open    — Engine::open()'s own input validation (language tag syntax,
#              a language this build has no pack for, a bad tessdata path,
#              out-of-range dpi/confidence) plus the handle-lifecycle cases
#              (recognise() before open, close() idempotence, a negative
#              page index, a null pixel pointer)
# ---------------------------------------------------------------------------
if(UAPP_OCR_ENABLED)
    add_executable(uapp_ocr_tests
        support/test_main.cpp
        ocr/ocr_engine_test.cpp)

    target_include_directories(uapp_ocr_tests PRIVATE support ocr)

    target_link_libraries(uapp_ocr_tests
        PRIVATE uapp::ocr_engine uapp::compiler_options)

    target_compile_definitions(uapp_ocr_tests PRIVATE
        UAPP_OCR_TEST_TESSDATA="${UAPP_TESSERACT_TESSDATA}")

    add_test(NAME uapp.ocr.read   COMMAND uapp_ocr_tests "ocr.read")
    add_test(NAME uapp.ocr.bounds COMMAND uapp_ocr_tests "ocr.bounds")
    add_test(NAME uapp.ocr.open   COMMAND uapp_ocr_tests "ocr.open")

    set_tests_properties(uapp.ocr.read uapp.ocr.bounds uapp.ocr.open
                         PROPERTIES TIMEOUT 120)
endif()
