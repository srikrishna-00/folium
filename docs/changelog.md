# Changelog

All notable changes to this project are documented here.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Per spec §5, nothing is recorded here as complete unless it is implemented and tested.
Planning and architecture work is recorded as such.

---

## [Unreleased]

### Added — Phase 1 bootstrap: the note vertical slice (first production code)

Implements exactly the slice spec §40 names as the task after architecture approval:

```text
CREATE NOTE -> STORE LOCALLY -> SEARCH NOTE -> OPEN NOTE -> EDIT NOTE -> SAVE NOTE
```

Nothing else from Phase 1 is done. This is the seed, not the phase.

- **Buildable CMake project at the repo root.** C++17 per ADR-0001. Top-level
  `CMakeLists.txt` plus `cmake/UappCompilerOptions.cmake`, which encodes the ADR's
  restricted subset as toolchain flags — `-fno-exceptions -fno-rtti`, `-Wall -Wextra
  -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast -Werror`. Options:
  `UAPP_BUILD_TESTS`, `UAPP_BUILD_TOOLS`, `UAPP_ENABLE_SANITIZERS`, `UAPP_WARNINGS_AS_ERRORS`.
- **`cmake/FindSQLite3Fts5.cmake`** — locates SQLite *and compiles-and-runs a probe* proving
  FTS5 is present, failing configuration otherwise. An FTS5-less libsqlite3 would not fail at
  link time; it would fail on a user's device the first time anyone searched.
- **`core/common`** (`uapp::core_common`) — `uapp::Status`, a flat append-only error enum with
  both a stable machine-readable code (`NOTE_NOT_FOUND`) and a human-readable message, per §33.
  Every fallible call in the core returns it and is `[[nodiscard]]`.
- **`core/notes`** (`uapp::core_notes`) — the portable domain. `NoteId` (16-byte opaque id,
  validating hex parse), `Note`, validation with hard size limits, the `NoteStore` persistence
  **port**, `Clock`/`IdGenerator` ports, and `NoteService` — the public API implementing the
  six slice steps. Links no database library at all, so PHASE0_SUMMARY.md H-3 ("nothing outside
  `storage/sqlite` includes `sqlite3.h`") is enforced by the build graph, not by convention.
- **`storage/sqlite`** (`uapp::storage_sqlite`) — the SQLite adapter. RAII wrappers
  (`Connection`/`Statement`/`Transaction`, rollback-on-destruction) so no early return can leak
  a statement or double-free one; WAL + `foreign_keys=ON` + busy timeout; bind helpers that
  check every `size_t`→`int` narrowing before it reaches the C API; content and FTS5 index
  written in **one transaction** per database-design.md §3.9; schema-version check that refuses
  a database written by a newer build.
- **`storage/sqlite/fts_query`** — converts raw user input into a safe FTS5 `MATCH` expression
  via an allow-list (word bytes only, each term double-quoted, ANDed, last term prefix-matched).
  Binding user text straight to `MATCH` is not SQL injection but *is* injection into FTS5's
  query language; this is where §38's "validate external input" applies to search.
- **`tools/notes_cli`** (`uapp-notes`) — developer harness. `demo` drives the whole slice
  against a real on-disk database and verifies every step, exiting non-zero on the first
  failure; `create`/`search`/`open`/`edit`/`delete`/`count` allow manual use.
- **`tests/`** — 42 unit/integration cases plus a CLI integration test, wired into CTest as
  `uapp.notes.unit` and `uapp.notes.cli_demo`. Coverage includes create, persist-and-reload
  across a closed connection, FTS hit *and* miss, prefix match, edit-then-reload, index
  freshness after edit and delete, UTF-8 round-trip, and eleven error-path cases (unknown id,
  nil id, save-after-delete, oversized title/body/query, read-only database write rejection,
  unopenable path, empty path, entropy-source failure, duplicate id, hostile search input).

**Verification.** Debug, Release, and ASan+UBSan configurations all build with zero warnings
under `-Werror`; `ctest` is 2/2 and the unit binary reports **42 passed, 0 failed** in each.

### Added — the C ABI (`libuapp_capi`): the platform-bridge boundary

Infrastructure, not a feature. The note slice already worked and was tested; this gives it a
C-callable face so the Android (JNI) and iOS (Swift/ObjC) bindings have something to attach to.
No note behaviour changed — `core/notes` and `storage/sqlite` are untouched.

Implements PHASE0_SUMMARY.md **H-2** (split the spec §4 "platform bridge" into a versioned C
ABI plus hand-written idiomatic bindings, so a C++ ABI never crosses JNI or Swift interop) and
the ABI rules in `api-design.md` §10 / `mobile-architecture.md` §2.

- **`core/capi`** (`uapp::capi`) — two pure C99 public headers. `uapp/capi/uapp_capi.h` carries
  the ABI version, the status enum and the string-ownership rules; `uapp/capi/uapp_notes.h`
  carries the note surface. Opaque handles only (`uapp_note_service_t*`, `uapp_note_t*`,
  `uapp_search_results_t*`); no C++ type, no STL container and no C++ ABI crosses the line. The
  implementation is C++17 under the same `-fno-exceptions -fno-rtti -Werror` posture as
  everything else. It is also the composition root that binds the abstract `NoteStore` port to
  the SQLite adapter — a PRIVATE link, so nothing above it inherits a path to `sqlite3.h`.
- **Versioned per spec §30.** `UAPP_ABI_VERSION_MAJOR/MINOR/STRING` plus real exported
  accessors and `uapp_abi_is_compatible()`, so a binding can interrogate the library it
  actually loaded rather than the header it compiled against. Structs crossing the boundary
  carry a leading `uint32_t struct_size`; a caller with a smaller struct is rejected, one with
  a larger struct is served. **Major is 0 deliberately** — the machinery is complete and
  enforced, the surface is not yet frozen. The freeze and the ABI-diff CI job over the exported
  symbol list (`api-design.md` §10 rule 5) land with the first shipped release.
- **Every fallible function returns `uapp_status_t`**, a C mirror of `uapp::Status` whose
  numeric values are asserted identical to the C++ enum's — code by code, at compile time — so
  a renumbering in the core is a build failure rather than a shipped client reporting the wrong
  error a release later. Two C-ABI-only codes in a 9xx block: `OUT_OF_MEMORY` and `UNEXPECTED`.
  The only `void` functions are the destructors and the pure version/string lookups.
- **Nothing unwinds across the boundary.** Every exported body is wrapped in a guard that
  converts an escaping exception into `UAPP_STATUS_UNEXPECTED`. Under the shipping
  `-fno-exceptions` build it compiles to nothing; it exists because the flag is per translation
  unit, and a future engine linked in with exceptions on must not unwind through a JNI frame.
- **String ownership is a two-rule contract, and the function names carry it.** `uapp_*_get_*`
  borrows from the handle (freeing it is a bug; it dies with the handle). `uapp_*_copy_*`
  transfers ownership and **must** be released with `uapp_free_string()`, never `free()` — the
  host and the core need not share an allocator. Status and version strings are static storage.
- **Handle hygiene.** Every destructor takes a *pointer to* the handle and NULLs it, so
  double-release is a no-op and the ordinary use-after-close mistake degrades to
  `INVALID_ARGUMENT` instead of a native crash. Each handle type carries a distinct magic
  cookie, so a NULL, zeroed or **wrong-type** handle is rejected rather than dereferenced. This
  is defence in depth, not a memory-safety claim, and the header says so.
- **No hidden global state.** No `uapp_ctx_t` singleton, no thread-local last-error slot, no
  process-wide registry. A service handle owns its connection, clock and id generator; any
  number may coexist and they share nothing. Proved, not asserted — see the tests.
- **`tests/capi/note_capi_test.c`** — **a C file**, compiled by the C compiler at `-std=c99`
  with `-Wall -Wextra -Wpedantic -Wconversion -Werror`, including only the two public headers
  and linking only `uapp::capi`. It is simultaneously the functional test and the
  C99-conformance check `architecture.md` §8 rule 5 asks for: a C++-ism in a public header
  breaks the build. 17 cases, wired into CTest as `uapp.capi.slice` and `uapp.capi.boundary`.
  Coverage: the full slice (create/store/search/open/edit/save/delete/count), persistence
  across close-and-reopen, UTF-8 round trip, search-limit default and clamp, **two simultaneous
  independent services proving isolation in both directions**, and boundary safety — null
  handles on every entry point, null strings, null out-parameters, use-after-close,
  double-close, wrong-type handles, malformed ids, oversized input, out-of-range result
  indices, `struct_size` versioning, and an unopenable database.
- **Build.** The project now enables C as a first-class language (`CMAKE_C_STANDARD 99`,
  extensions off). `cmake/UappCompilerOptions.cmake` and `core/common/CMakeLists.txt` gate the
  C++-only flags (`-fno-exceptions`, `-fno-rtti`, `-Wold-style-cast`, …) on
  `COMPILE_LANGUAGE:CXX`, so one options target still governs both languages and the warning
  posture is identical for each.

**Deliberate deviations from the `api-design.md` §10 sketch**, recorded so they are not mistaken
for oversights. That sketch describes the eventual full ABI; this is the note slice only. No
`uapp_init`/`uapp_ctx_t` — a process-wide context is exactly the hidden global state a
multi-handle design must avoid, and nothing here needs one. No `uapp_last_error()` TLS slot —
errors are self-describing codes with static message strings, which suffices until the
`uapp_error_t` cause chain exists. No FlatBuffers payloads — a flat title+body note has no
structured payload to carry; they arrive with the block tree. No job/progress/cancellation
protocol — every operation here is a local SQLite call and none can block indefinitely, so it
would be scaffolding around nothing.

**Verification.** Built from scratch in Debug, Release, and Debug+ASan/UBSan: **zero warnings
under `-Werror` in all three**, and `ctest` **4/4 passed** in each. The C ABI binary reports
**17 passed, 0 failed**; the pre-existing C++ suite still reports **42 passed, 0 failed**,
unchanged. The public headers additionally compile clean standalone under
`-std=c99 -pedantic-errors`, `-std=c11` and `-std=c17`, and as C++11/C++17 with
`-Wold-style-cast -fno-exceptions -fno-rtti` (the posture an ObjC++ iOS bridge would use).
macOS `leaks` reports **0 leaks** for the C ABI test binary.

Two defects were found by tooling during this work and fixed; both are recorded because neither
is visible by inspection. **UBSan** caught that reading an out-of-range `uapp_status_t` is
undefined behaviour — and a C caller can pass any `int` — fixed by pinning the enum with a
`UAPP_STATUS_FORCE_INT32` sentinel, which also guarantees the type is 32 bits wide on every
toolchain instead of implementation-defined. **`leaks`** caught that nulling an out-parameter on
a failure path silently discards whatever handle the caller's variable was holding; the ABI now
matches `NoteService`'s own contract — **out-parameters are never written on failure** — and a
test asserts exactly that.

### Test framework decision

No third-party test framework. Catch2 and GoogleTest both need exceptions, which ADR-0001
disables core-wide — adopting either would mean testing a configuration we do not ship.
`FetchContent` would also make `cmake` require the network, a poor default for an offline-first
product, and §35 requires a real license/redistribution review plus a `THIRD_PARTY_NOTICES`
entry before any dependency lands. `tests/support/test_framework.h` is ~120 lines providing
named cases, a registry, fatal/non-fatal assertions, a name filter and a pass/fail count.
Recorded as a bootstrap decision to revisit (with an ADR) when the suite needs fixtures or
CI-consumable output.

The C ABI test carries its own ~40-line harness for a further reason: it is a **C** file, and
`tests/support/test_framework.h` is C++. Including it would defeat the point of the file, which
is that only the public C headers are reachable from it. The two harnesses print the same
format on purpose, so the output of the two suites reads identically.

### Known gaps — deliberately out of scope for this slice, recorded so they are not forgotten

- **NOT IMPLEMENTED: end-to-end encryption.** ADR-0002 decision 1 makes E2EE a **v1.0
  requirement**. This slice stores note titles and bodies as plaintext in an unencrypted SQLite
  file. SQLCipher integration and the key hierarchy in `security-model.md` must land before any
  build carries real user data.
- **NOT IMPLEMENTED: sync, CRDT, multi-device.** Single-device only. The Fugue CRDT is Phase 13
  and needs its own correctness-proof spike first (ADR-0002 amendment); no CRDT work was started.
- **Partially implemented: the C ABI (`libuapp_capi`).** *(Was: NOT IMPLEMENTED. Narrowed —
  `core/capi` now exists, is tested from C, and is the supported boundary for the note slice.)*
  What is **not** covered, precisely:
  - **Notes only.** `uapp_note_*` is the entire surface. There is no C ABI for documents,
    blocks, files, PDF, OCR, scanner, search-beyond-notes, sync or AI, because none of those
    engines exist. When the `document`+`block` model lands (database-design.md §3.3), its ABI
    is new surface, not a reshaping of this one.
  - **NOT IMPLEMENTED: the `uapp_error_t` cause chain** from architecture.md §7. The boundary
    carries a code plus a static, content-free message — no `domain`, no `detail`, no causes.
  - **NOT IMPLEMENTED: the job protocol** (`uapp_job_submit`/`cancel`, progress and done
    callbacks) and the `uapp_platform_vtable_t` from mobile-architecture.md §6. Nothing in this
    slice is long-running or needs a platform service, so neither would have been exercised.
  - **NOT IMPLEMENTED: FlatBuffers payloads** (`uapp_buf_t`) for structured data, per
    api-design.md §10 rule 4. Search results cross as an opaque handle with typed accessors,
    which is adequate for four scalar fields and would not be for a block tree.
  - **Deferred: shared-library packaging and the ABI freeze.** `libuapp_capi` is a static
    archive; the `.so`/`.framework` build with a closed export set (the `UAPP_CAPI_API`
    visibility annotations are already in place for it) is not done, and neither is the
    ABI-diff CI job over the exported symbol list required by api-design.md §10 rule 5. The
    version is **0.1** precisely because the surface is not frozen yet.
  - **Deferred: the bindings themselves.** No `uapp-kotlin`, no `UAPPKit`, no JNI or Swift
    code, no Gradle or Xcode project. This run built the boundary those will attach to and
    stopped there.
  - **Not proven: concurrent use from multiple threads.** Distinct handles share no mutable
    state and are designed to be independent, and the tests prove independence — but they
    exercise it sequentially on one thread. "Safe to use two handles from two threads at once"
    is a design property here, not a tested one.
- **Deferred: vendoring SQLite.** The build links the platform libsqlite3. Reproducible mobile
  builds need the vendored amalgamation (+ SQLCipher).
- **Deferred: external-content FTS5 and an ICU tokenizer.** `note_fts` stores its own copy of
  the text, and `unicode61` does not segment CJK — both are Phase 9 items already named in
  database-design.md §3.9.
- **Deferred: the real document/block schema.** `note` is a flat title+body table, not the
  `document`+`block` tree of database-design.md §3.3. The 16-byte BLOB id matches the real
  design so growing into it needs no id migration.
- **Deferred: allocation-failure handling in C++ containers.** *(Narrowed — the C ABI boundary
  now exists and honours ADR-0001's contract for its own allocations: handles and owned strings
  use `new (std::nothrow)`, check, and return `UAPP_STATUS_OUT_OF_MEMORY` having changed
  nothing.)* What remains: `std::string`/`std::vector` growth **inside** the C++ core and
  inside the handle structs still terminates on OOM under `-fno-exceptions`. So the boundary
  reports allocation failure it can see and aborts on allocation failure it cannot. Making that
  contract complete means a nothrow-aware container policy through `core/`, which is its own
  piece of work.
- **Deferred: fuzz targets.** §29 and architecture.md §5 require a `LLVMFuzzerTestOneInput`
  entry point per parser. `build_fts5_match_expression` is the natural first target and has
  unit tests but no fuzz harness.

---

## [0.0.0] — 2026-09-29 — Phase 0: architecture and feasibility study

**No production code.** This release is documentation only, per spec §40
(*"Do not write production code until this analysis is complete"*).

### Added

- `docs/product-requirements.md` — PRD: personas, prioritised feature inventory (P0–P3),
  user stories with acceptance criteria, measurable quality attributes, explicit non-goals,
  v1.0 release gates, and 7 open product questions requiring sign-off.
- `docs/architecture.md` — system architecture. Five whole-system alternatives evaluated
  (cross-platform UI framework, Rust core, thin client, microkernel/process isolation,
  the spec's own layering). The spec §4 diagram is **accepted with three revisions**: split the
  platform bridge into a C ABI plus idiomatic bindings; collapse the duplicated persistence
  layers into one port; add an explicit Policy & Audit layer and a diagnostics spine.
- `docs/technical-requirements.md` — module architecture, port contracts, a technology
  selection matrix with rejected alternatives and confidence levels, six known-hard
  sub-problems, and 8 time-boxed Phase 0 spikes (~32 engineer-days).
- `docs/database-design.md` — SQLite local schema across 13 namespaces with sync columns,
  UUIDv7 identifiers, fractional indexing, EAV for user-defined databases (with the rationale
  and a documented escape hatch), plus the PostgreSQL cloud schema and the local↔cloud
  mapping. Capacity assumptions stated so the design is falsifiable.
- `docs/api-design.md` — REST surface refining spec §31 (deviations marked and justified),
  path-major versioning with a 24-month support window, idempotency, job protocol, cursor
  pagination, error envelope, rate limits, auth model, and the separately-versioned core C ABI.
- `docs/security-model.md` — assets, four trust boundaries, a STRIDE threat model with ~35
  enumerated threats and mitigations, key hierarchy with chunked AEAD, authorisation model,
  a correct redaction procedure, and **seven explicitly accepted risks**.
- `docs/ai-architecture.md` — provider abstraction (primitives vs. task services), a
  non-bypassable policy gate with an unforgeable policy token, router, local inference with
  honest limits, an 8-stage RAG pipeline including **citation verification**, agent framework
  with mechanically-enforced §20 prohibitions, model licensing rules, and an eval harness.
- `docs/file-format-support.md` — the full support matrix over all spec §9 formats with the
  required columns, plus a **dependency and licence analysis**. Key findings in §7 below.
- `docs/conversion-architecture.md` — three intermediate representations (UDM, SSM, IMM),
  a three-tier conversion strategy (direct / IR / render), a declarative capability graph,
  the full conversion matrix with fidelity classes, the `Unsupported` node contract, and
  automated post-conversion verification.
- `docs/sync-architecture.md` — per-datatype merge strategies rather than one algorithm,
  version vectors for exact conflict detection, Lamport clocks (**wall clocks never used for
  ordering**), outbox pattern, push/pull protocol, and a mechanical statement of the
  "never silently overwrite" invariant.
- `docs/offline-architecture.md` — the offline capability matrix including what does *not*
  work, the three-state connectivity model, on-demand asset handling, storage eviction with
  unsynced data never evicted, checkpointing instead of background-time assumptions, and the
  required UI states.
- `docs/mobile-architecture.md` — Android (Kotlin + Compose + JNI) and iOS (Swift + ObjC++
  bridge + SwiftUI/UIKit) architectures, JNI lifetime and threading rules, copy-free bitmap
  paths on both platforms, and the platform-service vtable.
- `docs/testing-strategy.md` — test levels weighted by damage class, property-based testing,
  mandatory fuzzing per parser with 18 named targets, the full fixture inventory from spec §28,
  the CI pipeline, and the **format-matrix verification test** that makes the support matrix
  unable to drift from reality.
- `docs/performance-strategy.md` — reference device tiers and ~60 measurable budgets,
  techniques mapped to where they are load-bearing, thermal/power policy, the benchmark
  regression gate, and 8 known performance risks.
- `docs/deployment.md` — monorepo build pipeline for six artefact families, four independent
  version axes, release trains, staged rollout with automatic halt criteria, kill switches
  (the only rollback mobile has), and backend deploy/migration practice.
- `docs/contributing.md` — development workflow, the non-negotiable rules from spec §37/§38,
  code standards, the nine-item dependency checklist, and the Definition of Done.
- `docs/roadmap.md` — the 20 phases from spec §36 expanded with entry criteria, deliverables,
  and phase-specific "done when" lists tied to the spec §41 Definition of Done; plus sequencing
  constraints and the phases most likely to slip.
- `docs/adr/0001-core-language-c-vs-cpp.md` — ADR: **C++17 internals behind a C99 ABI**, with
  four alternatives evaluated and a disciplined C++ subset defined.
- `docs/risk-register.md` — 28 risks with probability, impact, owner role, mitigation, and
  trigger conditions.
- `docs/engineering-complexity-estimate.md` — phase-by-phase effort estimate with complexity
  drivers, confidence levels, and the historical basis for the estimate.
- `THIRD_PARTY_NOTICES` (repo root) — initialised from the licence findings.
- `docs/PHASE0_SUMMARY.md` — decisions made with high confidence, decisions needing human
  sign-off before Phase 1, and spec ambiguities resolved by judgment call.

### Key licence findings (§7 of `file-format-support.md`)

- **PDFium (BSD-3-Clause) + QPDF (Apache-2.0)** selected for PDF. Both are safe for closed
  commercial distribution and static linking.
- **MuPDF (AGPL-3.0), Poppler (GPL-2.0+), Xpdf (GPL)** — **BLOCKED** for closed distribution.
  MuPDF remains available under a paid Artifex commercial licence if the business chooses.
- **LGPL-3.0 is a hard block for the iOS client.** Its anti-tivoization/relinking requirement
  cannot be satisfied under App Store code signing. This blocks **libheif, libde265, libmobi**.
- **HEIC/HEIF** is additionally blocked by active HEVC patent-pool exposure. Decision: use
  platform decoders on mobile only; **HEIC is unsupported on desktop and web**. AVIF
  (royalty-free) is the strategic alternative.
- **RAR is permanently out of scope** — the `unRAR` licence forbids the use we would need.
- **Legacy binary DOC/XLS/PPT** have no viable permissively-licensed C/C++ parser (antiword,
  catdoc, wv are all GPL). Marked server-only via headless LibreOffice (MPL-2.0, separate
  process), and therefore **not available offline**.
- **FFmpeg deferred.** LGPL static-linking friction plus H.264/HEVC/AAC patent exposure.
  v1.0 ships no transcoding; playback uses the platform media stack.
- **PDFium's vendored AGG copy requires explicit verification** at the pinned commit — AGG 2.4
  and earlier are permissive, AGG 2.5 is GPL.
- **FreeType must be elected under the FTL**, not its GPLv2 alternative.
- **Redis ≥ 7.4 (RSALv2/SSPL)** avoided server-side in favour of Valkey (BSD-3).

### Notes

- No source code exists in `core/`, `file-engine/`, `pdf-engine/`, `scanner/`, `ocr/`,
  `media/`, `ai/`, `knowledge/`, `storage/`, `network/`, `security/`, `api/`, `mobile/`,
  `desktop/`, or `web/`. This is intentional and required by spec §40.
- Every capability in every matrix in these documents is a **plan**, not a status. The v1.0
  release gate requires automated proof of each claim before it may stand.
- Licence findings were compiled from upstream licence files and public documentation and
  **must be re-verified against the pinned upstream revision at integration time, and reviewed
  by counsel** before Phase 5 and Phase 7 begin.

[Unreleased]: #unreleased
[0.0.0]: #000--2026-09-29--phase-0-architecture-and-feasibility-study
