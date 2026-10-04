# Development Roadmap

**Status:** Phase 0 draft — awaiting human sign-off
**Version:** 0.0.0
**Date:** 2026-09-29

The 20 phases from spec §36, expanded with entry criteria, deliverables, and an explicit
definition of "done" tied to spec §41.

---

## How to read this

**Every phase inherits the Definition of Done from spec §41:**

```
[ ] Implemented          [ ] Security reviewed
[ ] Compiles             [ ] Performance considered
[ ] Unit tested          [ ] Documentation updated
[ ] Integration tested   [ ] No known critical defects
[ ] Error handling       [ ] Regression tests pass
```

The **"Done when"** list in each phase below is the *phase-specific* addition on top of that
baseline. Both must hold.

**Effort** is a rough band from `engineering-complexity-estimate.md` (engineer-weeks, assuming
a team of 4–6). These are planning estimates for a feasibility study, not commitments.

**A phase is not started until its entry criteria are met.** Spec §5: *"Only then proceed."*

---

## PHASE 0 — Architecture and technology evaluation

**Status:** in progress (this document set)
**Effort:** 4–6 weeks including spikes

**Deliverables**
- The 22 documents in `docs/` (this set)
- The 8 spikes in `technical-requirements.md` §6 (**~38 engineer-days** after re-scoping:
  SP-1 5→8 d, SP-6 5→8 d — `spikes/`)
- Sign-off on the open decisions in `PHASE0_SUMMARY.md` — **done for S-1/S-2/S-3 via
  `adr/0002`**
- `docs/spikes/` findings for SP-1, SP-5, SP-6 plus `SPIKE_SUMMARY.md`

**Done when**
- [ ] All Phase 0 documents reviewed and signed off by a named human owner
- [x] **PQ-2 (E2EE), SP-6 (CRDT engine) and PQ-7 (device floor) answered** — `adr/0002`
- [x] **SP-1 investigated** — `spikes/SP-1-findings.md`. Passes as intended, fails as written;
      approach changed to a hermetic GN build consumed from CMake.
- [x] **SP-5 investigated** — `spikes/SP-5-findings.md`. Passes; produces four ABI rules that
      must land in Phase 1's design.
- [ ] **SP-1 revised criterion 2: PDFium builds and renders at our actual iOS deployment-target
      floor** (not iOS 17). This is the only part of SP-1 still genuinely open.
- [ ] **SP-6 re-scoped and run (8 d):** reference model + four property generators + a Fugue
      prototype + the differential harness. **SP-6 is NOT complete** — it was scoped to choose
      between two options, one of which `adr/0002` removed, and it found a defect in the one
      that was kept.
- [ ] **`adr/0002` decision 2 amended from "RGA" to "a Fugue-family sequence CRDT", or the
      backward-interleaving anomaly explicitly accepted in writing** (`sync-architecture.md`
      §6.1). One or the other — not left unsaid.
- [ ] Every dependency in the "High confidence" set has a verified licence and a recorded verdict
- [ ] The risk register has an owner per high risk, **including new R-29 (converging merge
      defect) and revised R-12**

---

## PHASE 1 — C/C++ core foundation

**Entry:** Phase 0 signed off; SP-1 (revised), SP-5 and SP-6 (re-scoped) done
**Effort:** 6–8 weeks
**Status:** in progress — the bootstrap vertical slice is **done**; the rest of the phase is not.

> **Vertical slice complete (see `changelog.md`, Unreleased).** Spec §40's post-approval task —
> `CREATE NOTE -> STORE LOCALLY -> SEARCH NOTE -> OPEN NOTE -> EDIT NOTE -> SAVE NOTE` — is
> implemented and tested: a root CMake build at C++17 with the ADR-0001 flag posture,
> `core/common` (`uapp::Status`), `core/notes` (domain + `NoteStore` port + `NoteService`),
> `storage/sqlite` (RAII SQLite/FTS5 adapter, content and index in one transaction), a
> `uapp-notes` CLI harness, and 42 unit/integration cases green under Debug, Release and
> ASan+UBSan at `-Werror`.
>
> This is the **seed only**. It deliberately does not touch the Phase 1 deliverables below:
> there is no `libuapp_capi`, no arena allocator, no worker pool, no fuzz target, no
> ABI-diff/layering CI, no Kotlin/Swift call path — and, per ADR-0002 decision 1, **no E2EE**,
> which remains a v1.0 requirement and a blocking gap before any build holds real user data.
> The slice's `note` table is a flat stand-in for the `document`+`block` schema Phase 2 owns.

> **From SP-5 — these are Phase 1 ABI design rules, not later optimisations.** Retrofitting
> them across two platform bindings is expensive, which is why SP-5 blocks this phase:
> pointer+length for bulk data (never per-item string getters); caller-allocated output
> buffers; direct buffers only for opaque native↔native payloads; an explicit documented
> owner and lifetime for every buffer crossing the boundary.

**Deliverables**
- CMake build for all six presets; CI green on all
- `libuapp_capi` skeleton: init/shutdown, error model, buffer type, job system, platform vtable
- Core utilities: arena allocator, bounded string ops, checked integer arithmetic, logging with
  enforced redaction, structured error type
- Worker pool and cancellation
- GoogleTest harness, ASan/UBSan/TSan configurations, first fuzz target scaffolding
  - *Revisit:* the bootstrap slice uses a ~120-line in-tree harness instead, because
    GoogleTest requires exceptions and ADR-0001 disables them core-wide. Choosing the
    permanent framework (doctest without exceptions, GoogleTest with a carve-out, or growing
    the in-tree one) is an open Phase 1 decision needing its own ADR — see `changelog.md`.
- ABI-diff tooling and the layering-check script

**Done when**
- [ ] All presets build clean at `-Werror` with the full warning set
- [ ] `libuapp_capi` headers compile standalone as C99
- [ ] The job system passes cancellation, progress and death tests
- [ ] The arena allocator has a fuzz target and enforces its cap
- [ ] ABI-diff and layering checks are blocking in CI
- [ ] Determinism check passes (two machines, identical hashes)
- [ ] A trivial "hello core" call works from Kotlin and from Swift

---

## PHASE 2 — SQLite / local storage

**Entry:** Phase 1 done
**Effort:** 5–7 weeks

**Deliverables**
- `storage/sqlite` adapter, the only place including `sqlite3.h`
- SQLCipher integration and key retrieval via the platform vtable
- Schema v1 (identity, workspace, document, block) with migration framework and checksums
- Transaction API, single-writer serialisation, outbox table
- Blob store: content-addressed, chunked AEAD, two-phase write, orphan sweep
- Storage port with a fully in-memory fake for tests
- **E2EE key hierarchy (`adr/0002` decision 1, +3 ew):** URK → workspace key → document key →
  per-file key, with device key bundles and a rotation-aware key store. **The hierarchy must
  exist from schema v1** — retrofitting a key level is a migration across every encrypted
  byte a user owns.

**Done when**
- [ ] Migration framework tested forward from a fixture DB, with integrity assertions
- [ ] `STORE_SCHEMA_TOO_NEW` correctly refuses a newer DB
- [ ] Crash-consistency test: process killed between every statement pair, no partial state
- [ ] Blob encryption passes chunk reorder/truncate/splice property tests
- [ ] Storage benchmarks meet `performance-strategy.md` §2.2
- [ ] Encrypted and unencrypted paths both tested; key loss is proven unrecoverable

---

## PHASE 3 — Document / block engine

**Entry:** Phase 2 done
**Effort:** 8–10 weeks

**Deliverables**
- Block model, schema registry, `schema_ver` migration
- Transactional edits, fractional indexing, undo/redo
- Page nesting, trash/restore
- Database engine: properties, rows (EAV), views, filter/sort/group AST + evaluator
- Version snapshots and restore
- **The §40 vertical slice: create → store → search → open → edit → save**

**Done when**
- [ ] Every P0 block type round-trips through storage
- [ ] Undo/redo passes property tests (undo∘do = identity for all op sequences)
- [ ] Fractional index property test passes
- [ ] Database view budget met: 10k rows × 20 properties, filtered+sorted, ≤ 100 ms
      (**or** the pivot-table escape hatch is implemented — `database-design.md` §3.4)
- [ ] A 50k-block document opens within budget
- [ ] The vertical slice runs end to end from a CLI harness

---

## PHASE 4 — File detection and universal file abstraction

**Entry:** Phase 3 done
**Effort:** 5–6 weeks

**Deliverables**
- Detection with the fixed precedence: magic → container → internal metadata → declared MIME →
  extension, with a confidence score
- `File` abstraction + capability bitmask
- Format registry and the conversion capability graph (empty of edges at this point)
- Archive browse (ZIP/TAR/GZ), with bomb and zip-slip defences
- Metadata read/strip (EXIF, XMP, ID3)
- Checksum + dedupe on ingest

**Done when**
- [ ] `fuzz_detect` never crashes on arbitrary bytes (definitionally required)
- [ ] Detection is deterministic and filename-independent (property test)
- [ ] Polyglot fixtures are classified with an explicit ambiguity flag
- [ ] Bomb limits enforced and tested; zip-slip fixtures rejected
- [ ] Capability queries are truthful — they reflect registered handlers, not a hard-coded list

---

## PHASE 5 — PDF engine

**Entry:** Phase 4 done; SP-1 complete **including the iOS-deployment-target criterion**;
**PDFium licence inventory checked in** (`file-format-support.md` §7.8)
**Effort:** 14–18 weeks — the single largest engine phase. **78 → 82 ew** (+4, Tier D tiering)

**Deliverables**
- PDFium (render/parse) + QPDF (structure/encryption) integration behind our port
- Viewer: pagination, tiled render, zoom, thumbnails, text extraction and search, text selection
- Page ops: merge, split, extract, reorder, delete, rotate, crop
- Compress; metadata edit; watermark
- Encryption: R2–R6 protect/unlock
- Annotations: highlight, underline, strikethrough, ink, shapes, text box, note
- AcroForm render + fill
- **Redaction** with the verification step
- PDF creation, image→PDF, PDF→image

**Done when**
- [ ] All P0 PDF rows in the format matrix pass the matrix verification test (§14 of testing)
- [ ] `fuzz_pdf_parse`, `fuzz_pdf_render`, `fuzz_pdf_content_stream` run 72 h clean
- [ ] Render and open budgets met on Tier B for the complex-PDF fixture set
- [ ] **Redaction verification is a hard gate and passes on every redaction fixture**
- [ ] Encrypted-PDF matrix (R2/R4/R6 × correct/wrong password) fully covered
- [ ] Low-confidence text extraction is detected and reported rather than returned as garbage
- [ ] XFA documents display honestly (no false claim of support)
- [ ] PDFium built without V8 and without XFA — verified in the build manifest
- [ ] **PDFium is built from pinned source in a hermetic container in our CI** and consumed as
      an `IMPORTED` CMake target; the re-roll procedure is scripted and documented
      (`spikes/SP-1-findings.md` §5)
- [ ] **Tier D budgets met** (`performance-strategy.md` §1.1): 1× render cap, 2-page bitmap
      LRU, ≤ 16 MB tile cache, ≤ 180 MB peak RSS on a 500-page PDF
- [ ] Bitmap pool and tile-cache depths are **derived at runtime from device memory class**,
      verified by a test that fakes a low-RAM device

---

## PHASE 6 — Scanner

**Entry:** Phase 5 done
**Effort:** 7–9 weeks. **34 → 37 ew** (+3, Tier D capture tiering per `adr/0002` decision 3)

**Deliverables**
- OpenCV-based quad detection, perspective correction, enhancement (auto/colour/gray/BW,
  brightness, contrast, sharpen, shadow removal)
- Batch session: capture, reorder, retake, delete
- Camera frame contract (luma-plane-only for detection)
- Export: flat PDF, image set (searchable PDF lands in Phase 7)

**Done when**
- [ ] Quad detection ≥ 15 fps on Tier B at 640 px; **capture resolution capped on Tier D so
      perspective correction fits the 96 MB arena, verified on real Tier D hardware**
- [ ] Detection accuracy measured on the 25-fixture scanned corpus, with the failure rate published
- [ ] Manual override available on every automatic step
- [ ] Batch session survives process death (checkpointing)
- [ ] OpenCV linked from core/imgproc/photo only; **`opencv_contrib` absent from the build**,
      verified in the manifest

---

## PHASE 7 — OCR

**Entry:** Phase 6 done; **SP-3 complete and re-run on Tier D hardware, not only Tier B**
**Effort:** 9–11 weeks. **44 → 49 ew** (+5, Tier D per `adr/0002` decision 3)

**Deliverables**
- `OCRProviderPort` with three implementations: Tesseract, Apple Vision, ML Kit
- `TextBlock` output with bbox, confidence, language, reading order
- Layout analysis (blocks, lines, reading order)
- Language pack management: download, verify, version, evict, resume
- Invisible text layer → searchable PDF
- Table structure recovery (marked approximate)

**Done when**
- [ ] English OCR works offline out of the box (bundled `eng` pack)
- [ ] CER measured per language on the 24-fixture multi-language corpus; **results published
      internally so product claims stay honest**
- [ ] Per-page budget met on Tier B (2.5 s Tesseract, 600 ms platform) **and on Tier D (6 s
      Tesseract, 1.5 s platform, serial, 200 DPI)** — `performance-strategy.md` §1.2
- [ ] **`tessdata_fast` ships; `tessdata_best` is absent from the build**, verified in the
      manifest (≈ 4–5 MB vs ≈ 15–23 MB — the difference is most of the Tier D install budget)
- [ ] **CER is measured and published internally per language AND per tier.** A 200 DPI Tier D
      path with different numbers is a different claim, and must not inherit Tier B's.
- [ ] Peak RSS during OCR ≤ 150 MB on Tier D; never parallel across pages there
- [ ] Searchable PDF verified: extracted text matches OCR output and is positioned correctly
- [ ] RTL and CJK fixtures produce correct reading order
- [ ] Handwriting is labelled BEST-EFFORT in-product, never "supported"
- [ ] Missing language packs produce an actionable state, not a failure

---

## PHASE 8 — Conversion engine

**Entry:** Phase 7 done; SP-8 complete
**Effort:** 16–20 weeks — **the most commonly underestimated phase**

**Deliverables**
- UDM, SSM, IMM intermediate representations
- Readers: TXT, MD, HTML, RTF, DOCX, ODT, EPUB, CSV, XLSX, images
- Writers: PDF (with the text layout engine), HTML, MD, TXT, DOCX, XLSX, EPUB, images
- Conversion planner + capability graph edges
- Fidelity reporting with the `Unsupported` node contract
- Post-conversion verification

**Done when**
- [ ] Every non-☁ cell in the conversion matrix has a golden test that passes
- [ ] Fidelity reports are complete: a fixture with K unsupported constructs yields exactly K entries
- [ ] Post-conversion verification is on by default and fails loudly
- [ ] All readers fuzzed; `fuzz_zip`, `fuzz_docx`, `fuzz_xlsx`, `fuzz_xml` run 72 h clean
- [ ] Conversion budgets met; the 256 MB arena cap holds under the large-fixture set
- [ ] **The XLSX "formulas not recalculated" state is surfaced in the UI**, not just in a doc
- [ ] Nothing in the matrix claims a capability without a passing fixture

---

## PHASE 9 — Search

**Entry:** Phase 8 done
**Effort:** 6–8 weeks

**Deliverables**
- FTS5 index maintained in the same transaction as content
- ICU-backed tokenizer with CJK segmentation
- Exact, prefix, boolean, fuzzy (trigram + bounded Levenshtein rescore)
- Metadata/faceted filters
- Search across blocks, files, PDF text, OCR text
- Backlinks and mentions

**Done when**
- [ ] Search budgets met at 100k and 1M blocks
- [ ] Index/content consistency property test passes; the drift checker reports zero drift
- [ ] CJK, RTL and diacritic fixtures return correct results
- [ ] Search works fully offline
- [ ] Deleting content removes it from the index **in the same transaction** (verified — this is
      a privacy bug if it fails)

---

## PHASE 10 — Android application

**Entry:** Phase 9 done
**Effort:** 14–18 weeks. **Phases 10+11 combined: 144 → 157 ew** (+9 E2EE key/consent UX,
+4 Tier D runtime cache sizing and low-RAM paths)

> **The E2EE user-facing crypto UX is the part to not underestimate here.** Recovery-phrase
> onboarding with forced verification, QR + out-of-band short-code device enrolment,
> per-document consent prompts, lost-device and revocation flows — these need real usability
> testing, because a user who skips the short-code check has no protection against T-309, and
> a user who dismisses the recovery phrase has no protection against anything.

**Deliverables**
- `uapp-kotlin` JNI binding with lifetime, threading and error mapping
- **E2EE UX:** recovery-phrase generation with mandatory re-entry verification; device
  enrolment via QR + short-code comparison; per-document AI consent prompts; device list,
  revocation, and the honest "this stops future access only" wording
  (`security-model.md` §5A)
- Compose UI for all P0 flows: workspace, editor, files, PDF viewer/editor, scanner, search,
  settings
- CameraX integration; SAF; WorkManager; Keystore; biometrics; share sheet; deep links
- Design tokens generated into the Compose theme

**Done when**
- [ ] Every binding entry point tested including error and cancellation paths
- [ ] Zero leaked handles at teardown, verified under ASan
- [ ] No JNI call reaches the main thread (assertion holds across the E2E suite)
- [ ] **Bitmap rendering is copy-free** — `AndroidBitmap_lockPixels` + `FPDFBitmap_CreateEx`,
      **verified by a copy counter in the benchmark harness, not by a timing**
      (`spikes/SP-5-findings.md` §5). A timing gate can be passed by a copying implementation.
- [ ] **≤ 1 boundary crossing per screenful of blocks**, verified by a crossing counter
- [ ] No `GetPrimitiveArrayCritical` held across any render or parse; no
      `GetByteArrayElements` on a hot path (lint-enforced)
- [ ] **Bitmap pool and tile-cache depths derived at runtime** from `isLowRamDevice()` /
      `getMemoryClass()`; Tier D journeys pass on real Tier D hardware
- [ ] All P0 journeys pass, including airplane-mode variants
- [ ] Accessibility: TalkBack pass on every P0 flow
- [ ] Startup, memory and frame budgets met on Tier B and Tier C
- [ ] 16 KB page alignment verified; R8 keep rules verified against the release build

---

## PHASE 11 — iOS application

**Entry:** Phase 10 substantially done (parallelisable from ~60 %)
**Effort:** 14–18 weeks

**Deliverables**
- `UAPPCore` (ObjC++) + `UAPPKit` (Swift) bridge
- XCFramework build in CI
- SwiftUI chrome + UIKit document canvas
- AVFoundation camera; document picker; background tasks; Keychain; LocalAuthentication
- Apple Vision registered as the default OCR provider
- Privacy manifest, export compliance

**Done when**
- [ ] Bridge tested at every entry point; zero leaks under Instruments and ASan
- [ ] Page rendering is copy-free into a Swift-allocated bitmap context
- [ ] All P0 journeys pass, including airplane-mode variants
- [ ] VoiceOver pass on every P0 flow
- [ ] Budgets met on Tier B and C
- [ ] **Licence audit confirms zero GPL/AGPL/LGPLv3 code in the shipped binary**
- [ ] TestFlight build accepted with the privacy manifest and export declaration

---

## PHASE 12 — Cloud backend

**Entry:** Phases 10 and 11 done; **`adr/0002` §5A key-management design items settled**
(recovery-phrase encoding; whether the platform-keychain copy holds the URK or a wrapped URK;
whether PIN+enclave escrow is committed for v1.1 — the last changes the v1.0 data model)
**Effort:** 12–16 weeks. **62 → 64 ew** — a large E2EE *saving* (−8: no server-side merge,
index, thumbnailing or RAG) nearly cancelled by a large E2EE *cost* (+10: device enrolment,
sealed-bundle storage and fan-out, rotation orchestration)

**Deliverables**
- Identity, workspace, blob, sync, AI-gateway and conversion services
- **E2EE device enrolment and key distribution (`security-model.md` §5A):** sealed-bundle
  storage and fan-out, enrolment notification to all devices, revocation-triggers-rotation
  orchestration. **The server stores and forwards opaque ciphertext; it holds no key.**
- PostgreSQL schema with RLS; object storage; change log
- REST `/v1` per `api-design.md`, with OpenAPI as the source of truth
- Auth: JWT + rotating refresh with reuse detection; MFA; device registration/revocation
- Chunked resumable upload; presigned URLs
- Data export and account deletion

**Done when**
- [ ] OpenAPI contract tests pass; handlers validated against the spec in CI
- [ ] The generated authorisation matrix test passes (every endpoint × role × cross-tenant id)
- [ ] Idempotency verified on every mutating endpoint
- [ ] RLS verified as a second net with an application-check bypass test
- [ ] Load tested to the projected first-year peak with headroom
- [ ] Backup **restore** rehearsed successfully, not just backup
- [ ] GDPR export and deletion work end to end, including backups within the published SLA
- [ ] **Revoking a device rotates the workspace key and re-wraps to remaining devices** —
      tested, not assumed. De-listing without re-encryption is a failing test (T-310).
- [ ] **A device offline across a rotation catches up correctly on reconnect**
- [ ] **Support runbooks are written on the basis that support cannot read user data and
      cannot recover a lost key.** This is a launch blocker, not a nice-to-have.

---

## PHASE 13 — Synchronisation

**Entry:** Phase 12 done; ~~re-scoped SP-6 passed~~ **re-scoped SP-6 tasks 1–3 PASSED
2026-10-04** (`spikes/fugue-correctness-spike.md`: reference model + Fugue *and* FugueMax
prototype + differential harness, four properties over 5.49 M generated operations, 5/5
injected faults caught); **re-scoped SP-6 task 4 (encrypted framing sketch) and SP-6 R-4 (GC +
causal-delivery specification) still OWED**; **the algorithm decision in `adr/0002` confirmed
or amended** — the spike found the ADR's *wording* is wrong (it claims plain Fugue is
*maximally* non-interleaving; that is FugueMax's property) so this entry item is **not yet
satisfied** and needs a product-owner decision, not an implementation choice.
**Effort:** 14–18 weeks → **16–20 weeks. 76 → 93 ew** (+8 E2EE framing, +9 custom CRDT).
**Unchanged by the spike.** Roughly 4 ew of the CRDT line item's risk-bearing work now exists,
but as *spike* code against a `char` sequence; promoting it is a rewrite against the real
document model, not a move. What the spike did buy is cheaper: the correctness obligation for
the optimised implementation drops from "prove it is correct" to "prove it is byte-identical to
a model we already have".
**This is still the most dangerous phase in the project**, ahead of 5 and 8: Low confidence,
"Very High" correctness risk, and an encrypted transport that prevents server-side debugging of
exactly the failures that are hardest to reproduce. **What changed is which part is dangerous.**
The algorithm-level trap is now demonstrated closed (**R-29 downgraded to L×5=5**) and the risk
has moved to the optimisations, GC and framing that the spike did not touch (**new R-32,
M×5=10**).

**Deliverables**
- Version vectors, Lamport clocks, device signing
- Outbox, pusher with backoff and dead-lettering, puller with transactional apply
- Per-datatype merge strategies; **custom C++ Fugue-family sequence CRDT for text**
- **Naive reference model + differential harness** (`sync-architecture.md` §6.2)
- **Tombstone GC bounded by the 90-day causal-stability horizon; bounded causal-delivery
  buffering**
- **Encrypted op/snapshot framing; snapshot cadence and server-side pruning**
  (`sync-architecture.md` §10) — **all merging and all conflict resolution are client-side**
- Conflict detection, automatic resolution, and the conflict inbox UI
- Selective sync and blob sync
- Deterministic simulation harness

**Done when**
- [ ] Convergence, no-loss and idempotence property tests pass over 10⁶ simulated operations
- [ ] **Non-interleaving property passes, using its dedicated backward-insertion generator**
      (`sync-architecture.md` §6.2 P4). **Convergence tests alone cannot detect this defect
      class — they pass while the user sees corrupted text.** If only one line of this phase
      is enforced, it is this one.
- [ ] **The optimised CRDT produces byte-identical documents to the naive reference model**
      across every generated interleaving
- [ ] **The merged order is a linear extension of the strong list specification**, checked from
      what each replica actually saw when it generated each insert — **not** inferred from the
      reference model. `spikes/fugue-correctness-spike.md` §5 measured that the four properties
      plus reference-model differential testing certify a wrong-parent fault as correct, because
      both implementations faithfully reproduce the wrong parent recorded in the operation. The
      reference model catches *integration* bugs; only this oracle catches *generation* bugs.
      Both are required.
- [ ] **A deliberately defective control algorithm (classic RGA) is in the test tree and is
      asserted to FAIL the non-interleaving check.** It is the only thing that proves P4 can
      still fail at all; if P4 goes quiet, this is the first test to look at.
      (`core/sync/spike/rga.h` is written to be reused as-is.)
- [ ] **Mutation testing of the merge code is part of CI, with a check that the rebuilt binary
      actually changed.** 5/5 injected faults were caught in the spike; a stale-binary rebuild
      silently reported a false result once and nearly got believed
      (`spikes/fugue-correctness-spike.md` §6.4).
- [ ] **Every walk of the CRDT tree is iterative or explicitly depth-bounded.** Measured: Fugue
      tree depth equals document length *exactly* (16 000-deep tree for a 16 000-character
      note), so the published algorithm's recursive traversal overflows the 512 KB default stack
      of a secondary thread on iOS and Android at ordinary document sizes. This is a long note,
      not an adversarial input.
- [ ] Property generators include a **tombstone-heavy** profile. At an 18 % delete rate the
      spike's suite needed ~1 000 trials to find a tombstone-related fault; at 45 % it needed
      ~50.
- [ ] **No plaintext reaches the server — as a test.** A full sync round-trip against a real
      server instance asserts that no known plaintext token appears in its database, object
      store, logs or metrics. Fails the build. Same weight as the Phase 14 egress test.
- [ ] Tamper tests pass: flipped ciphertext byte, swapped `doc_id`, replayed `snapshot_id`,
      reused `device_counter` — all rejected
- [ ] Key-rotation convergence: replicas rotating mid-session, one offline across the
      rotation, still converge
- [ ] Snapshot pruning is safe: a client pulling after the server prunes superseded updates
      reconstructs an identical document
- [ ] An operation referencing a GC'd tombstone is **rejected cleanly**, never UB
- [ ] Out-of-causal-order operations are buffered within a bound; overflow escalates visibly
- [ ] Partition/heal tests pass
- [ ] **Every LWW loser is provably recoverable from version history** (tested, not asserted)
- [ ] The `reset_required` full-resync path is implemented and tested
- [ ] 7-day / 5-device soak converges with zero loss
- [ ] Old-client ↔ new-server and new-client ↔ old-server both work
- [ ] `fuzz_sync_delta` clean; hostile-device deltas rejected safely
- [ ] Chaos tests pass: kill mid-push, mid-apply, mid-upload; disk full; device revoked mid-sync

---

## PHASE 14 — AI

**Entry:** Phase 13 done; **SP-4 complete and re-run on Tier D** (§1.2 says a ~1 B model does
not fit there — SP-4 must confirm or refute against real hardware); **PQ-3 (providers)
answered**
**Effort:** 10–13 weeks. **52 → 62 ew** (+6 E2EE consent flow, +4 Tier D capability gating)

**Deliverables**
- `AIProviderPort` + ≥2 cloud adapters + local (llama.cpp, ONNX Runtime)
- **Policy gate** with workspace policy, consent, quota, audit
- **Per-document decrypt-and-send consent flow** (`ai-architecture.md` §13.2): scope
  resolution to exact byte ranges, a preview of what leaves, consent records written **before**
  decryption, hash-chained into the audit log, and honest revocation semantics
- **Device-capability probe driving what is offered:** local generation is **off on Tier D**
  with a named reason, not slow (`performance-strategy.md` §1.2)
- Router with circuit breaking and explainable selection
- Task services: summarize, rewrite, translate, classify, extract, study artefacts
- Model lifecycle management
- Eval harness

**Done when**
- [ ] **A LOCAL_ONLY workspace provably cannot reach a cloud provider** — tested by an
      integration test that fails the build if any egress occurs
- [ ] No AI call is possible without a valid policy token (architectural invariant, tested)
- [ ] Every egress writes an audit record before the call
- [ ] `AI_NO_LOCAL_MODEL` is returned rather than a cloud fallback
- [ ] Model signature verification blocks unsigned weights
- [ ] Eval suites established with baselines recorded
- [ ] Local vs. cloud quality delta measured and published internally
- [ ] Every shipped model has a reviewed licence row in `THIRD_PARTY_NOTICES`
- [ ] **No plaintext leaves the device without a consent record written first** — tested the
      same way as the LOCAL_ONLY egress test, and failing the build the same way
- [ ] **A LOCAL_ONLY workspace shows no consent prompt at all.** It fails closed with
      `AI_POLICY_BLOCKED`; there is no dialog that can talk it into egress (H-29).
- [ ] The decrypted scratch buffer is zeroed on completion **and** on the crash-recovery
      sweep (T-206)
- [ ] Agent runs block on the same consent prompt mid-run; there is no "approve all"
- [ ] Revocation UI states plainly that it stops future egress and cannot recall what was sent

---

## PHASE 15 — RAG

**Entry:** Phase 14 done; SP-2 complete
**Effort:** 8–10 weeks. **42 → 49 ew** (+7, per-device indexing under E2EE)

**Deliverables**
- Structure-aware chunking with full provenance
- Embedding pipeline with `content_hash` incrementality
- **Per-device corpus indexing as a first-class background job** (`ai-architecture.md` §13.3):
  chunks and vectors do not sync, so **every new device re-indexes its whole corpus locally**
  — hours of work for a large workspace. Checkpointed, resumable, thermally- and
  battery-aware, prioritised by recent access so the index is *useful* long before it is
  *complete*, with honest progress and an explicit "search is incomplete" state.
- `sqlite-vec` index (+ int8 quantisation above the size threshold)
- Hybrid retrieval with RRF; permission filtering post-retrieval
- Context assembly with budgeting
- **Citation verification**
- Semantic and hybrid search surfaced in the search UI

**Done when**
- [ ] recall@10 ≥ 0.85 on the labelled eval set
- [ ] **Citation integrity ≥ 0.95 after verification, and zero unverified citations reach the
      UI** — the latter is a P0 bug if violated
- [ ] `grounded: false` is rendered distinctly; the model never answers from priors while
      appearing to cite documents
- [ ] Permission filtering verified by a cross-workspace retrieval test
- [ ] Retrieval and embedding budgets met
- [ ] Vector index memory within budget at 400k chunks
- [ ] A model change forces a full re-embed rather than silently mixing embedding spaces
- [ ] **One embedding model across all tiers, chosen to fit Tier D** — two devices of the same
      user must never hold vectors in different spaces (`ai-architecture.md` §13.4)
- [ ] **No chunk, vector or embedding row is ever pushed to the server** — verified by the
      Phase 13 no-plaintext test extended to derived content (A4 is as sensitive as A1)
- [ ] Indexing completes on Tier D within a stated wall-clock target on a reference corpus,
      or the corpus size at which it does not is published internally

---

## PHASE 16 — Agent framework

**Entry:** Phase 15 done
**Effort:** 8–10 weeks

**Deliverables**
- Agent, Tool, Memory, Planner, Executor, Evaluator, PermissionManager, AuditLogger
- Closed, compiled tool registry
- Per-run scoped expiring grants; human confirmation for destructive tools
- Run transcript UI; resumable runs
- Built-in document-ingest agent

**Done when**
- [ ] **All five spec §20 prohibitions are enforced mechanically and tested**, not prompted
- [ ] No shell, eval, or arbitrary-network tool exists in the registry (verified by test)
- [ ] Every tool invocation is audited before execution
- [ ] Injection red-team corpus produces zero unauthorised tool invocations
- [ ] Runs are cancellable and resumable across process death
- [ ] Every run produces a complete, user-readable transcript
- [ ] Budget caps (steps, tokens, time, cost) enforced

---

## PHASE 17 — Collaboration

**Entry:** Phase 16 done; **PQ-6 answered**
**Effort:** 10–14 weeks

**Deliverables**
- Sharing, roles, share links with expiry and password
- Comments and threads
- Presence and awareness channel (new transport — does not exist before this phase)
- Real-time collaborative editing over the existing CRDT foundation
- Notifications

**Done when**
- [ ] Permission changes propagate correctly and are enforced server-side on every path
- [ ] Share links are revocable and rate-limited; the unauthenticated path is pentested
- [ ] Concurrent editing converges with no lost updates under the simulation harness
- [ ] Presence degrades gracefully to offline
- [ ] Comments sync and resolve correctly

---

## PHASE 18 — Security hardening

**Entry:** Phase 17 done
**Effort:** 8–10 weeks. **42 → 36 ew (−6)** — E2EE has **moved out of this phase** into
Phases 2, 12 and 13 per `adr/0002` decision 1. Phase 18 now *reviews* E2EE rather than
building it, which is the whole point of deciding PQ-2 early.

**Deliverables**
- External penetration test and remediation — **scoped to include the E2EE key-exchange,
  rotation and recovery paths, which are the newest and least-reviewed code in the product**
- Tamper-evident audit log
- Certificate-pin rotation procedure implemented and rehearsed
- Key rotation; secure deletion verification
- PKCS#7 digital signatures (if P-13 is approved)
- ~~E2EE (if PQ-2 says yes)~~ — **built in Phases 2/12/13. Reviewed here.**
- Full threat-model review, **including T-309 (enrolment key substitution) and T-310
  (revocation without rotation)**

**Done when**
- [ ] All pentest findings closed or formally accepted with a named owner
- [ ] Crypto known-answer tests and the key-lifecycle suite pass
- [ ] Secure-deletion verification passes, including FTS and vector row removal
- [ ] The secret-in-logs test passes across the full suite
- [ ] The audit hash chain detects tampering
- [ ] SBOM generated and accurate; zero known Critical CVEs in the pinned set

---

## PHASE 19 — Performance optimisation

**Entry:** Phase 18 done
**Effort:** 6–8 weeks → **8–10 weeks. 32 → 40 ew** (+8). **A second *gating* device tier
roughly doubles the device-matrix profiling work**, and Tier D is the tier where the budgets
are hardest to hit.

**Deliverables**
- Profiling across the device matrix **including Tier D on reserved physical hardware**;
  targeted optimisation
- Startup optimisation (baseline profiles, order files)
- Memory reduction; cache tuning
- Battery and thermal tuning
- Production performance telemetry with alerting

**Done when**
- [ ] **Every budget in `performance-strategy.md` §2 is met on Tier B**
- [ ] **Every Tier D budget in `performance-strategy.md` §1.1 is met.** Tier D is a gating
      tier per `adr/0002` decision 3, not a degradation check — a Tier D memory breach fails
      the build exactly as a Tier B one does.
- [ ] **The Tier D degradation matrix (§1.3) is implemented and every row is announced
      in-product with a reason.** Silently-slow is a failure; disabled-and-explained is not.
- [ ] Tier C degrades explicitly, with disabled features named in-product
- [ ] Benchmark regression gates are blocking in CI
- [ ] Production telemetry and alerts are live
- [ ] No memory budget is exceeded anywhere in the benchmark suite

---

## PHASE 20 — Production release

**Entry:** Phase 19 done
**Effort:** 6–8 weeks

**Deliverables**
- Store submissions; staged rollout; support and incident process
- Privacy policy, terms, sub-processor list, OSS licence screen
- Documentation and onboarding
- Monitoring, alerting, on-call

**Done when — the v1.0 release gates (`product-requirements.md` §10)**
- [ ] Every P0 feature `IMPLEMENTED` and meeting the Definition of Done
- [ ] **The format-matrix verification test passes** — every claimed capability proven by a fixture
- [ ] ≥ 72 h fuzzing clean across all P0 targets, zero open medium+ findings
- [ ] External security review complete, findings closed
- [ ] Zero known data-loss defects; the 7-day sync soak converges
- [ ] `THIRD_PARTY_NOTICES` generated by CI from the actual lockfile
- [ ] Rollback/hotfix path rehearsed
- [ ] Backup restore rehearsed
- [ ] Accessibility audit passed on all P0 flows
- [ ] Sub-processor list published and accurate

---

## Sequencing notes

**Critical path:** 0 → 1 → 2 → 3 → 4 → 5 → 8 → 9 → 10/11 → 12 → 13 → 14 → 15 → 18 → 19 → 20.

**Parallelisable:**
- Phase 6 (scanner) and 7 (OCR) can run alongside 8 (conversion) with a separate sub-team.
- Phase 11 (iOS) can start at ~60 % of Phase 10 once the binding pattern is established.
- Phase 12 (backend) can start during Phase 10 — it has few dependencies on the mobile work.
- Phase 16 (agents) can overlap Phase 15's tail.

**Hard ordering constraints:**
- 13 (sync) **must** follow 12 (backend).
- 15 (RAG) **must** follow 14 (AI), which must follow 13 — retrieval needs synced content.
- ~~If PQ-2 resolves to "E2EE at v1.0", the E2EE work moves from Phase 18 to before Phase 12.~~
  **PQ-2 is answered (`adr/0002` decision 1) and this has happened.** The key hierarchy lands
  in **Phase 2**, enrolment and rotation in **Phase 12**, encrypted framing and client-side
  merge in **Phase 13**, the consent flow in **Phase 14**, per-device indexing in **Phase 15**.
  Phase 18 reviews rather than builds. This reordering is the single largest structural change
  in this revision of the roadmap, and it was made *before* Phase 1 rather than during Phase
  18 — which was the whole reason PHASE0_SUMMARY ranked PQ-2 first.
- **The E2EE key hierarchy must be in schema v1 (Phase 2).** Adding a key level later is a
  migration across every encrypted byte a user owns.
- **`adr/0002` decision 2 must be confirmed or amended before Phase 1** — the Fugue-vs-RGA
  question (`sync-architecture.md` §6.1). It is cheap now and expensive once the merge code
  exists.
- **Tier D hardware must be in the benchmark rack from Phase 5**, not from Phase 19. A gating
  tier discovered at optimisation time is not a gating tier; it is a crisis.

**Phases most likely to slip:** **13 (sync) first**, then 5 (PDF) and 8 (conversion). Phase 13
has moved to the front of that list: it is now 93 ew, it carries a from-scratch CRDT with a
published algorithm-level trap, and E2EE means a failure in production cannot be diagnosed by
looking at the server. Together the three are roughly 45 % of total effort. Any schedule that
does not budget contingency on them is wrong.

**Revised totals after `adr/0002`:** ≈ **1079 ew** full scope, ≈ **865–870 ew** narrowed.
See `engineering-complexity-estimate.md` §10.
