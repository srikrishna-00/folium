# Testing Strategy

**Status:** Phase 0 draft — awaiting human sign-off
**Version:** 0.0.0
**Date:** 2026-09-29

Implements spec §28 (testing) and §29 (fuzzing). Governing principle from §28:
**"Never assume an input file is valid."**

---

## 1. What this system's bugs actually look like

Test effort should follow where the damage is, not where the code is. For this product the
high-damage failure classes are:

| Class | Why it is dangerous | Test response |
|---|---|---|
| **Memory corruption in a parser** | Untrusted input + native code = RCE. Highest severity. | Continuous fuzzing + sanitizers, mandatory per parser |
| **Sync divergence / data loss** | Silent, delayed, unrecoverable, destroys trust | Deterministic simulation + property tests |
| **Silent conversion corruption** | "Success" that produces a broken file | Golden corpus + automated verification |
| **Crypto/key-management error** | Unrecoverable data or a false privacy promise | Known-answer vectors + an explicit key-lifecycle suite |
| **Capability lies** | The matrix claims support we do not have | A test that walks the matrix and proves every claim |
| **Permission/authorisation gaps** | Cross-tenant leakage | An exhaustive authorisation matrix test |
| **AI ungroundedness** | Fabricated citations presented as sourced | Citation-integrity eval as a hard gate |

Ordinary UI regressions are comparatively cheap. The pyramid below is weighted accordingly.

## 2. Test levels

```
         ╱ Manual exploratory ╲              — each release, scripted charters
        ╱  E2E journeys (~60)   ╲            — real devices, incl. airplane mode
       ╱  UI tests (~300)         ╲          — Compose / XCUITest
      ╱  Integration (~1500)        ╲        — engine + storage + ports, real SQLite
     ╱  Binding tests (~400)          ╲      — every JNI/Swift entry point
    ╱  Unit tests (~8000)               ╲    — core C++, fast, no I/O
   ╱─────────────────────────────────────╲
  ╱  Continuous: fuzzing, sanitizers,     ╲  — always running, not a level
 ╱   property tests, simulation, benchmarks ╲
╱───────────────────────────────────────────╲
```

Counts are order-of-magnitude targets for v1.0, not quotas to be gamed.

## 3. Unit tests (core)

- **Framework:** GoogleTest + GoogleMock.
- **Rules:** no filesystem, no network, no real clock, no real RNG. `ClockPort` and
  `RandomPort` are injected fakes — this is what makes sync and crypto tests deterministic.
- **Coverage gates:** line ≥ 80 % overall; ≥ 90 % for `core/sync`, `security/crypto`,
  `core/permissions`, `file-engine/detection`, and every parser's error paths.
  Coverage is a floor, not a goal — a 100 %-covered parser with no fuzzing is still unsafe.
- **Every error path is tested.** For parsers this means a test per error code, not just the
  happy path. In practice error-path tests outnumber happy-path tests roughly 3:1 in parser code.
- **Death tests** for the invariants that must abort rather than continue (e.g. a handle used
  after free in a debug build).

## 4. Property-based testing

Where example tests are structurally insufficient. Using RapidCheck (BSD-2) or an equivalent.

| Property | Statement |
|---|---|
| Sync convergence | For any interleaving of ops across N devices, all devices reach an identical state |
| Sync no-loss | For any interleaving, every user-intended value is reachable from version history |
| Sync idempotence | Applying the same change log twice is a no-op |
| Fractional index | For any two adjacent keys, a strictly-between key exists and is generatable |
| CRDT commutativity | `apply(a, b) == apply(b, a)` for concurrent ops |
| Conversion round-trip | For T1 pairs, `read(write(ir)) ≈ ir` under the declared equivalence |
| Crypto round-trip | `decrypt(encrypt(m, k), k) == m` for all m, k; decryption with a wrong key or tampered ciphertext always fails |
| Chunked AEAD | Reordering, truncating, or splicing chunks between files always fails authentication |
| Detection stability | Detection of the same bytes is deterministic and independent of the filename |
| Permission monotonicity | Adding a grant never removes access; removing one never adds it |
| Fidelity report completeness | For a document containing K unsupported constructs, the report contains exactly K corresponding entries |

## 5. Integration tests

Real SQLite (in a temp dir), real parsers, fake network and fake platform vtable.

- Full pipelines: ingest → detect → parse → index → search; scan → OCR → PDF → index;
  edit → version → sync-enqueue.
- Transactional correctness: kill the process (simulated) between every pair of statements in a
  multi-step operation and assert no partial state is observable.
- Migration tests: every schema version N−1 fixture DB migrates to N, with post-migration
  integrity assertions.
- Storage-full, permission-denied, and corrupted-file paths are integration tests, not unit tests.

## 6. Fuzzing (spec §29)

**Mandatory. A parser without a fuzz target does not merge.** This is enforced by a CI check
that every file under `*/parsers/` has a corresponding target in `tests/fuzz/`.

### 6.1 Targets

| Target | Input | Priority | Note |
|---|---|---|---|
| `fuzz_pdf_parse` | PDF bytes | **P0** | Largest attack surface |
| `fuzz_pdf_render` | PDF + page index | **P0** | Rendering is a separate code path from parsing |
| `fuzz_pdf_content_stream` | content stream bytes | **P0** | Redaction and text extraction depend on this |
| `fuzz_zip` | ZIP bytes | **P0** | Gateway to all OOXML; bombs and zip-slip |
| `fuzz_docx` | DOCX bytes | **P0** | |
| `fuzz_xlsx` | XLSX bytes | **P0** | |
| `fuzz_xml` | XML bytes | **P0** | XXE, entity expansion, deep nesting |
| `fuzz_tiff` | TIFF bytes | **P0** | Historically the most CVE-prone image format we handle |
| `fuzz_jpeg`, `fuzz_png`, `fuzz_webp`, `fuzz_gif`, `fuzz_bmp` | image bytes | P0 | |
| `fuzz_detect` | arbitrary bytes | **P0** | Must never crash on anything, by definition |
| `fuzz_metadata` | EXIF/XMP/ID3 bytes | P1 | |
| `fuzz_rtf`, `fuzz_html`, `fuzz_markdown`, `fuzz_epub`, `fuzz_svg` | format bytes | P1 | |
| `fuzz_ocr_input` | image bytes → Tesseract | P1 | Third-party surface |
| `fuzz_sync_delta` | CBOR delta | **P0** | Hostile-peer input |
| `fuzz_api_json` | request bodies | P1 | Server-side |
| `fuzz_conversion` | (format, bytes, target) | P1 | Exercises reader+writer together |
| `fuzz_crdt_ops` | operation sequences | P1 | Structure-aware; convergence oracle |

### 6.2 Method

- **libFuzzer + ASan + UBSan** as the default configuration; MSan runs separately (it needs an
  instrumented libc++ and cannot be combined with ASan).
- **Structure-aware fuzzing** (`libprotobuf-mutator` or a custom mutator) for formats where
  random byte flipping rarely gets past the header — OOXML, CRDT ops, sync deltas. Naive
  byte-flip fuzzing of a ZIP-based format spends 99 % of its cycles failing CRC checks.
- **Seed corpora** checked into `tests/fixtures/corpus/<target>/`, minimised with
  `-merge=1`. Seeded from our own fixture files plus public corpora.
- **Cadence:** short runs (60 s per target) on every PR as a smoke check; long runs (≥ 4 h per
  P0 target) nightly; a continuous fleet between releases.
- **Release gate:** ≥ 72 h clean across all P0 targets, zero open medium-or-higher findings.
- **Every crash becomes a regression test.** The reproducer is checked into
  `tests/fixtures/regressions/` with the issue id, and runs in the normal unit suite forever.
- **Timeout and OOM are findings**, not noise. A PDF that takes 30 s to parse is a DoS.

## 7. Sanitizers and static analysis

| Tool | Cadence | Gate |
|---|---|---|
| ASan + UBSan (full suite) | every PR | blocking |
| TSan (concurrency-relevant suites) | nightly | blocking on new findings |
| MSan | weekly | triage |
| Valgrind (Linux CI) | weekly | triage |
| clang-tidy (`bugprone-*`, `cert-*`, `concurrency-*`, `cppcoreguidelines-*`) | every PR | blocking |
| CodeQL / Semgrep | every PR | blocking on high severity |
| `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror` | every build | blocking |
| Include-graph/layering check (`architecture.md` §8) | every PR | blocking |
| ABI-diff against the last release | every PR | blocking on a non-additive change |
| Licence/notices check (NFR-8) | every PR | blocking |

## 8. Fixtures (spec §28)

Fixtures are a deliverable in their own right, owned and versioned. Stored in
`tests/fixtures/`, with files > 1 MB in Git LFS.

### 8.1 Required categories

| Category | Contents | Min count |
|---|---|---|
| **Small valid** | One minimal valid file per supported format | 1 per format |
| **Large** | 500-page PDF; 100 MB PDF; 50 MB DOCX; 1 M-row CSV; 500k-cell XLSX; 20 000 × 20 000 TIFF | 10 |
| **Corrupted** | Truncated at 10/50/90 %; flipped bytes in the header, in the middle, in the trailer; wrong length fields; bad CRCs | ≥ 5 per P0 format |
| **Malformed but "plausible"** | Valid magic, invalid structure; conflicting internal lengths; cyclic references; deeply nested structures (1000 levels) | ≥ 5 per P0 format |
| **Adversarial** | ZIP bombs; XXE/billion-laughs; zip-slip paths; PDFs with JS/OpenAction/Launch; polyglots (PDF+ZIP, PDF+HTML); PDFs with cyclic object references; a font designed to stress FreeType | ≥ 20 |
| **Password-protected** | PDF R2/R4/R6; AES-encrypted ZIP; ZipCrypto ZIP; password-protected DOCX/XLSX | 8 |
| **Unicode** | Emoji + ZWJ sequences; combining marks; RTL (Arabic, Hebrew); bidi override characters; CJK; Devanagari; surrogate pairs; NFC vs NFD pairs; zero-width characters in filenames | 15 |
| **Multi-language OCR** | Printed pages in eng/spa/fra/deu/hin/ara/zho/jpn, at 150/300/600 DPI, clean and degraded | 24 |
| **Scanned documents** | Skewed; shadowed; creased; low contrast; handwritten; receipts (thermal); business cards; forms; multi-column; tables with and without ruling lines | 25 |
| **Real-world DOCX/XLSX/PPTX** | Produced by several different generators, since each emits materially different OOXML | 30 |
| **Complex PDFs** | Scanned; vector-heavy (a map); CJK with embedded subset fonts; AcroForms; XFA; tagged/accessible; PDF/A; linearised; incrementally-updated; no `ToUnicode` CMap | 20 |
| **Database fixtures** | A DB at every schema version, for migration tests | 1 per version |
| **Sync scenarios** | Recorded operation logs for known conflict patterns | 20 |
| **Golden outputs** | Expected IR and expected output for each conversion pair | 1 per matrix cell |

### 8.2 Provenance rules

- **No copyrighted third-party documents.** Fixtures are generated by us, taken from
  permissively-licensed public corpora, or synthesised. Each fixture directory carries a
  `PROVENANCE.md` naming the source and licence.
- **No real personal data**, ever, including in "realistic" scanned samples. Synthetic names,
  addresses, and numbers only.
- Adversarial fixtures are clearly quarantined and never opened by tooling that scans the repo.

## 9. Performance testing

Google Benchmark micro-benchmarks plus device-level macro-benchmarks. Budgets and the
regression gate are defined in `performance-strategy.md`; the testing obligations are:

- Every budget in that document has an automated benchmark.
- Benchmarks run nightly on fixed, reserved physical devices (not emulators, not shared CI
  runners — the variance makes results meaningless).
- A > 10 % regression against the rolling 7-day median on a P0 benchmark fails the build.
- Memory-ceiling tests assert peak RSS, not just "it completed".

## 10. Security testing

Detailed in `security-model.md` §13. The obligations that land on this document:

- **Authorisation matrix test:** generated, covering every endpoint × every role × an object
  belonging to another tenant. This is generated from the OpenAPI spec so a new endpoint cannot
  be added without being covered.
- **Crypto known-answer tests** against RFC/NIST vectors for every primitive.
- **Key-lifecycle suite:** generate, wrap, unwrap, rotate, destroy; assert that a destroyed key
  makes its ciphertext permanently unrecoverable.
- **Redaction verification** is a hard gate: for every redaction fixture, extract text and
  metadata from the output and assert the redacted strings are absent (`security-model.md` §9).
- **Secret-in-logs test:** run the full suite with a logging sink that fails the test if it ever
  sees a marker string planted in fixture content.

## 11. AI evaluation

Covered in `ai-architecture.md` §11. Treated as tests, with gates:
retrieval recall@10 ≥ 0.85; citation integrity ≥ 0.95 post-verification; **zero unverified
citations reaching the UI (P0 bug)**; zero unauthorised tool invocations on the injection
red-team corpus.

## 12. Mobile and E2E

- **Binding tests** on both platforms exercising every entry point including error and
  cancellation paths, run under ASan, asserting zero leaked handles at teardown.
- **E2E journeys** (~60) covering every P0 user story, each with an airplane-mode variant where
  applicable.
- **Chaos on device:** kill the app mid-OCR, mid-conversion, mid-sync-apply, mid-upload; fill
  the disk; revoke the device; change the clock. Assert resume, not restart, and no loss.
- **Device matrix:** low-end, mid, flagship, and one tablet per platform, on real hardware.
- **Accessibility:** automated scans (Accessibility Scanner / XCUITest audits) plus a manual
  screen-reader pass on every P0 flow each release.

## 13. CI pipeline

```
PR  ──► format check · lint · layering check · licence check
    ──► build (Linux, macOS, Android arm64, iOS arm64)
    ──► unit + integration (ASan+UBSan)
    ──► property tests (fixed seed set)
    ──► binding tests (Android emulator, iOS simulator)
    ──► fuzz smoke (60 s per P0 target)
    ──► ABI diff · coverage gate · benchmark smoke
    ──► CodeQL
                                        [~25 min target for the PR path]

Nightly ──► full fuzz (4 h per P0 target) · TSan · device benchmarks
        ──► E2E on the physical device matrix · AI evals
        ──► dependency CVE scan · long sync simulation (10⁶ ops)

Weekly  ──► MSan · Valgrind · 7-day sync soak (rolling) · full E2E matrix

Release ──► everything, plus: 72 h fuzz clean · external pentest findings closed
        ──► the format-matrix verification test (§14)
```

**The PR path must stay under ~25 minutes.** A slow PR pipeline gets bypassed, and a bypassed
pipeline provides no safety.

## 14. The format-matrix verification test

A specific requirement from `product-requirements.md` §10.2, called out because it is unusual
and important:

A test reads `docs/file-format-support.md`, parses the matrix, and for **every cell claiming a
capability** locates a fixture and asserts the capability actually works. A ✔ with no passing
assertion fails the build.

This is the mechanism that makes "do not claim universal support" (spec §9) and "do not pretend
unsupported functionality works" (spec §43) enforceable rather than aspirational. Documentation
that can drift from reality will drift; this test removes the possibility.

## 15. Test data management

- Fixtures in Git LFS; a manifest with SHA-256 per file; CI verifies integrity.
- Fuzz corpora stored separately (object storage) and synced by CI, with the minimised seed set
  in-repo.
- Golden outputs regenerated only by an explicit, reviewed command — never silently updated by a
  `--update-snapshots` flag that reviewers rubber-stamp.

## 16. Definition of Done for a test suite

A phase's tests are complete when:

- [ ] Every public function of the phase's modules has unit tests covering success and every
      error code
- [ ] Every parser added in the phase has a fuzz target and a seed corpus
- [ ] Integration tests cover the phase's end-to-end pipeline
- [ ] Property tests exist for any invariant that can be stated as one
- [ ] Benchmarks exist for anything with a budget in `performance-strategy.md`
- [ ] Security tests cover the phase's threat-model entries
- [ ] Fixtures for the phase's formats exist in all required categories
- [ ] The suite runs clean under ASan+UBSan
- [ ] Coverage gates met
- [ ] No test is skipped or disabled without a linked issue and an owner

## 17. Anti-patterns we are explicitly banning

Written down because each of these has sunk a native codebase before:

1. **Deleting or disabling a test to make a build pass** (spec §37). Quarantine with an issue
   and an owner, or fix it.
2. **Asserting on error *messages*** instead of codes. Messages are not API.
3. **Tests that depend on wall-clock time or real randomness.** Inject both.
4. **Snapshot tests over PDF bytes.** Object ordering and timestamps make them useless; compare
   structurally.
5. **Mocking the thing under test.** Parser tests use real parsers and real bytes.
6. **Coverage as the goal.** A fuzz target finds more than 200 lines of coverage-chasing tests.
7. **"Flaky, just rerun it."** A flaky test in sync or concurrency is usually a real bug
   reporting itself. Investigate, do not retry.
