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
- The 8 spikes in `technical-requirements.md` §6 (~32 engineer-days)
- Sign-off on the open decisions in `PHASE0_SUMMARY.md`

**Done when**
- [ ] All Phase 0 documents reviewed and signed off by a named human owner
- [ ] SP-1 (PDFium mobile build), SP-5 (bridge cost) and SP-6 (CRDT choice) complete — these
      three constrain the core's shape and block Phase 1
- [ ] PQ-2 (E2EE) and PQ-7 (device floor) answered — both are architecture-level forks
- [ ] Every dependency in the "High confidence" set has a verified licence and a recorded verdict
- [ ] The risk register has an owner per high risk

---

## PHASE 1 — C/C++ core foundation

**Entry:** Phase 0 signed off; SP-1, SP-5, SP-6 done
**Effort:** 6–8 weeks

**Deliverables**
- CMake build for all six presets; CI green on all
- `libuapp_capi` skeleton: init/shutdown, error model, buffer type, job system, platform vtable
- Core utilities: arena allocator, bounded string ops, checked integer arithmetic, logging with
  enforced redaction, structured error type
- Worker pool and cancellation
- GoogleTest harness, ASan/UBSan/TSan configurations, first fuzz target scaffolding
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

**Entry:** Phase 4 done; SP-1 complete; **PDFium licence inventory checked in** (`file-format-support.md` §7.8)
**Effort:** 14–18 weeks — the single largest engine phase

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

---

## PHASE 6 — Scanner

**Entry:** Phase 5 done
**Effort:** 7–9 weeks

**Deliverables**
- OpenCV-based quad detection, perspective correction, enhancement (auto/colour/gray/BW,
  brightness, contrast, sharpen, shadow removal)
- Batch session: capture, reorder, retake, delete
- Camera frame contract (luma-plane-only for detection)
- Export: flat PDF, image set (searchable PDF lands in Phase 7)

**Done when**
- [ ] Quad detection ≥ 15 fps on Tier B at 640 px
- [ ] Detection accuracy measured on the 25-fixture scanned corpus, with the failure rate published
- [ ] Manual override available on every automatic step
- [ ] Batch session survives process death (checkpointing)
- [ ] OpenCV linked from core/imgproc/photo only; **`opencv_contrib` absent from the build**,
      verified in the manifest

---

## PHASE 7 — OCR

**Entry:** Phase 6 done; SP-3 complete
**Effort:** 9–11 weeks

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
- [ ] Per-page budget met on Tier B (2.5 s Tesseract, 600 ms platform)
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
**Effort:** 14–18 weeks

**Deliverables**
- `uapp-kotlin` JNI binding with lifetime, threading and error mapping
- Compose UI for all P0 flows: workspace, editor, files, PDF viewer/editor, scanner, search,
  settings
- CameraX integration; SAF; WorkManager; Keystore; biometrics; share sheet; deep links
- Design tokens generated into the Compose theme

**Done when**
- [ ] Every binding entry point tested including error and cancellation paths
- [ ] Zero leaked handles at teardown, verified under ASan
- [ ] No JNI call reaches the main thread (assertion holds across the E2E suite)
- [ ] Bitmap rendering is copy-free (SP-5 approach implemented)
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

**Entry:** Phases 10 and 11 done
**Effort:** 12–16 weeks

**Deliverables**
- Identity, workspace, blob, sync, AI-gateway and conversion services
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

---

## PHASE 13 — Synchronisation

**Entry:** Phase 12 done; **PQ-2 (E2EE) answered**; SP-6 decision implemented
**Effort:** 14–18 weeks — **the highest-risk phase**

**Deliverables**
- Version vectors, Lamport clocks, device signing
- Outbox, pusher with backoff and dead-lettering, puller with transactional apply
- Per-datatype merge strategies; sequence CRDT for text
- Conflict detection, automatic resolution, and the conflict inbox UI
- Selective sync and blob sync
- Deterministic simulation harness

**Done when**
- [ ] Convergence, no-loss and idempotence property tests pass over 10⁶ simulated operations
- [ ] Partition/heal tests pass
- [ ] **Every LWW loser is provably recoverable from version history** (tested, not asserted)
- [ ] The `reset_required` full-resync path is implemented and tested
- [ ] 7-day / 5-device soak converges with zero loss
- [ ] Old-client ↔ new-server and new-client ↔ old-server both work
- [ ] `fuzz_sync_delta` clean; hostile-device deltas rejected safely
- [ ] Chaos tests pass: kill mid-push, mid-apply, mid-upload; disk full; device revoked mid-sync

---

## PHASE 14 — AI

**Entry:** Phase 13 done; SP-4 complete; **PQ-3 (providers) answered**
**Effort:** 10–13 weeks

**Deliverables**
- `AIProviderPort` + ≥2 cloud adapters + local (llama.cpp, ONNX Runtime)
- **Policy gate** with workspace policy, consent, quota, audit
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

---

## PHASE 15 — RAG

**Entry:** Phase 14 done; SP-2 complete
**Effort:** 8–10 weeks

**Deliverables**
- Structure-aware chunking with full provenance
- Embedding pipeline with `content_hash` incrementality
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
**Effort:** 8–10 weeks

**Deliverables**
- External penetration test and remediation
- Tamper-evident audit log
- Certificate-pin rotation procedure implemented and rehearsed
- Key rotation; secure deletion verification
- PKCS#7 digital signatures (if P-13 is approved)
- E2EE (if PQ-2 says yes) — **note: if yes, this reshapes Phases 12–15 and must move earlier**
- Full threat-model review

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
**Effort:** 6–8 weeks

**Deliverables**
- Profiling across the device matrix; targeted optimisation
- Startup optimisation (baseline profiles, order files)
- Memory reduction; cache tuning
- Battery and thermal tuning
- Production performance telemetry with alerting

**Done when**
- [ ] **Every budget in `performance-strategy.md` §2 is met on Tier B**
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
- 13 (sync) **must** follow 12 (backend) and cannot start before PQ-2 is answered.
- 15 (RAG) **must** follow 14 (AI), which must follow 13 — retrieval needs synced content.
- **If PQ-2 resolves to "E2EE at v1.0", the E2EE work moves from Phase 18 to before Phase 12**,
  because it changes the backend and sync designs fundamentally. Discovering this late would be
  the single most expensive mistake available to this project.

**Phases most likely to slip:** 5 (PDF), 8 (conversion), 13 (sync). Together they are roughly
45 % of total effort. Any schedule that does not budget contingency on these three is wrong.
