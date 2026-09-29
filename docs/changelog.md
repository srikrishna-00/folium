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

### Test framework decision

No third-party test framework. Catch2 and GoogleTest both need exceptions, which ADR-0001
disables core-wide — adopting either would mean testing a configuration we do not ship.
`FetchContent` would also make `cmake` require the network, a poor default for an offline-first
product, and §35 requires a real license/redistribution review plus a `THIRD_PARTY_NOTICES`
entry before any dependency lands. `tests/support/test_framework.h` is ~120 lines providing
named cases, a registry, fatal/non-fatal assertions, a name filter and a pass/fail count.
Recorded as a bootstrap decision to revisit (with an ADR) when the suite needs fixtures or
CI-consumable output.

### Known gaps — deliberately out of scope for this slice, recorded so they are not forgotten

- **NOT IMPLEMENTED: end-to-end encryption.** ADR-0002 decision 1 makes E2EE a **v1.0
  requirement**. This slice stores note titles and bodies as plaintext in an unencrypted SQLite
  file. SQLCipher integration and the key hierarchy in `security-model.md` must land before any
  build carries real user data.
- **NOT IMPLEMENTED: sync, CRDT, multi-device.** Single-device only. The Fugue CRDT is Phase 13
  and needs its own correctness-proof spike first (ADR-0002 amendment); no CRDT work was started.
- **NOT IMPLEMENTED: the C ABI (`libuapp_capi`).** Consumers are C++ today. The `uapp_error_t`
  cause chain from architecture.md §7 is likewise not built; `Status` carries code + message only.
- **Deferred: vendoring SQLite.** The build links the platform libsqlite3. Reproducible mobile
  builds need the vendored amalgamation (+ SQLCipher).
- **Deferred: external-content FTS5 and an ICU tokenizer.** `note_fts` stores its own copy of
  the text, and `unicode61` does not segment CJK — both are Phase 9 items already named in
  database-design.md §3.9.
- **Deferred: the real document/block schema.** `note` is a flat title+body table, not the
  `document`+`block` tree of database-design.md §3.3. The 16-byte BLOB id matches the real
  design so growing into it needs no id migration.
- **Deferred: allocation-failure handling in C++ containers.** Factory functions use
  `new (std::nothrow)` and check, but `std::string`/`std::vector` growth still terminates on
  OOM under `-fno-exceptions`. ADR-0001's explicit allocation-failure contract applies at the C
  ABI boundary, which does not exist yet.
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
