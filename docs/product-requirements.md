# Product Requirements Document

**Status:** Phase 0 draft — awaiting human sign-off
**Version:** 0.0.0
**Date:** 2026-09-29
**Owner:** Architecture (Phase 0)

> Derived from `docs/MASTER_PROMPT.md`. Where the master spec was ambiguous, the judgment
> call is stated inline and repeated in `docs/PHASE0_SUMMARY.md`.

---

## 1. Problem statement

Knowledge work is fragmented across four tool categories that do not share a data model:

1. **Note/knowledge tools** (block editors, wikis) — good at structure, blind to files.
2. **Document/PDF tools** — good at files, blind to knowledge structure.
3. **Scanner/OCR tools** — produce files that immediately leave the system.
4. **AI assistants** — usually sit on top of a *copy* of your content, not the content itself.

The result: a user scans a receipt, it becomes a PDF in a camera roll, is manually
re-uploaded to a PDF tool to compress, exported, attached to a note, and then re-uploaded
again to an AI tool to be asked a question about. Four copies, four trust boundaries,
zero shared index.

**Thesis:** one local-first content store, one file abstraction, one index, one AI
retrieval layer. Everything else is a view on top of it.

## 2. Product definition

A cross-platform (Android/iOS primary; Windows/macOS/Linux/Web secondary) application
providing a unified **personal knowledge + document workspace**:

- Block-based pages, nested pages, structured databases, tasks, calendar
- Universal file ingest, detection, preview, and conversion
- PDF read/edit/annotate/sign/protect pipeline
- Camera-based document scanner producing searchable PDFs via OCR
- Unified search (exact → full-text → fuzzy → semantic)
- Provider-independent AI: summarize, extract, Q&A with citations, agentic workflows
- Offline-first with eventual-consistency sync
- Privacy-first: the user chooses local vs. cloud AI, per operation

## 3. Non-goals (explicit)

Out of scope for v1.0. Must not be implied in UI, marketing, or docs.

| Non-goal | Reason |
|---|---|
| "Universal" file format support | Physically false. We ship a bounded, published matrix (`file-format-support.md`). |
| Pixel-fidelity round-trip DOCX/XLSX/PPTX editing | Requires a full office-suite layout engine — a multi-year project on its own. We do *lossy, documented* conversion. |
| Real-time multi-cursor collaborative editing | Deferred to Phase 17. v1.0 is single-user, multi-device. |
| Video editing / NLE | Media module is preview + (possible) transcode only. |
| Self-hosted server as a shipped product | Backend is our hosted service in v1.0. Self-host is post-1.0. |
| Enterprise SSO / SCIM / DLP | Post-1.0. v1.0 targets prosumer and small team. |
| Training foundation models | We integrate; we do not train. Fine-tuning is out of scope. |
| Handwriting recognition as a guaranteed feature | Best-effort via platform APIs only. Never marketed as supported. |
| Desktop feature parity with mobile at v1.0 | Desktop is reader + light editor at v1.0. |
| Browser PDF editing at native parity | Web is view + light annotate only. |
| DRM'd content (Kindle AZW/KFX, Adobe ADEPT EPUB) | Legal and technical non-starter. |
| RAR extraction | The `unRAR` licence forbids using it to build a competing archiver and is not OSI-approved. |

## 4. Personas

**P1 — "Priya", graduate student (primary).** Scans lecture handouts, annotates PDFs, takes
notes, wants flashcards and quizzes from her own material. Mobile-first, poor campus
connectivity, price-sensitive. *Drives: scanner, OCR, AI study tools, offline.*

**P2 — "Marcus", independent consultant (primary).** Contracts, invoices, receipts. Needs
merge/split/compress/sign/redact, client-separated workspaces, and confidence that a
confidential contract is not shipped to a third-party LLM. *Drives: PDF engine, security,
local AI, audit log.*

**P3 — "Aiko", PM at a 12-person startup (secondary).** Lives in structured databases and
task boards, shares pages, pastes in specs as DOCX/PDF. Desktop + mobile. *Drives:
workspace, databases, sharing, sync, conversion.*

**P4 — "Dan", field operations (secondary).** Offline for hours. Batch-scans forms, needs
everything to reconcile later without loss. *Drives: offline, conflict handling, batch scan.*

## 5. Feature inventory and priority

Priority: **P0** = v1.0 cannot ship without it · **P1** = v1.0 target, cuttable under
schedule pressure · **P2** = post-1.0 · **P3** = speculative.

Status: `PLANNED` (designed, not built) · `NOT IMPLEMENTED` (no design yet) ·
`TBD` (blocked on a decision or feasibility spike) · `BEST-EFFORT` (shipped without a
quality guarantee, labelled as such in-product).

### 5.1 Core content / workspace

| ID | Feature | Pri | Phase | Status |
|---|---|---|---|---|
| W-01 | Create / rename / delete / nest pages | P0 | 3 | PLANNED |
| W-02 | Blocks: paragraph, H1–H3, quote, list, checklist, divider, code | P0 | 3 | PLANNED |
| W-03 | Blocks: callout, toggle, table, equation, bookmark, embed | P1 | 3 | PLANNED |
| W-04 | Media blocks: image, file, audio, video | P0 | 3/6 | PLANNED |
| W-05 | Undo/redo (transactional, per-document) | P0 | 3 | PLANNED |
| W-06 | Version history + restore | P1 | 3/13 | PLANNED |
| W-07 | Backlinks, `@`-mentions, page links | P1 | 9 | PLANNED |
| W-08 | Comments on blocks | P2 | 17 | PLANNED |
| W-09 | Templates | P2 | 3 | PLANNED |
| W-10 | Drag/drop block reordering | P1 | 10/11 | PLANNED |
| W-11 | Trash, restore, hard delete | P0 | 3 | PLANNED |

### 5.2 Structured databases

| ID | Feature | Pri | Phase | Status |
|---|---|---|---|---|
| D-01 | Properties: text, number, bool, select, multi-select, date, URL, email | P0 | 3 | PLANNED |
| D-02 | Properties: person, file, relation | P1 | 3 | PLANNED |
| D-03 | Properties: rollup, formula | P2 | 3 | PLANNED — the formula language is its own sub-project (parser, type system, evaluator, dependency graph, cycle detection) |
| D-04 | Views: table, list | P0 | 3 | PLANNED |
| D-05 | Views: board, calendar, gallery | P1 | 3 | PLANNED |
| D-06 | Views: timeline / Gantt | P2 | — | NOT IMPLEMENTED |
| D-07 | Filter / sort / group / saved views | P0 | 3 | PLANNED |
| D-08 | Tasks (a first-class database preset) | P1 | 3 | PLANNED |
| D-09 | Calendar (preset + device calendar read) | P2 | — | TBD — device calendar write-back is a permissions/privacy question |

### 5.3 File engine

| ID | Feature | Pri | Phase | Status |
|---|---|---|---|---|
| F-01 | Detection: magic bytes → container → internal metadata → declared MIME → extension (in that precedence) | P0 | 4 | PLANNED |
| F-02 | `File` abstraction + capability flags (VIEW/EDIT/CONVERT/OCR/…) | P0 | 4 | PLANNED |
| F-03 | Preview: images, PDF, text, Markdown, code with highlighting | P0 | 4/5 | PLANNED |
| F-04 | Preview: DOCX/XLSX/PPTX read-only | P1 | 8 | PLANNED — layout-approximate by design |
| F-05 | Preview: EPUB, FB2 | P1 | 8 | PLANNED |
| F-06 | Archive browse (ZIP/TAR/GZ/7Z) without full extraction | P1 | 4 | PLANNED |
| F-07 | Checksum + dedupe on ingest | P1 | 4 | PLANNED |
| F-08 | Metadata read / strip (EXIF, XMP, PDF Info) | P1 | 4 | PLANNED |
| F-09 | Runtime-extensible format registry (plugin) | P2 | 30 | PLANNED |

### 5.4 PDF engine

| ID | Feature | Pri | Phase | Status |
|---|---|---|---|---|
| P-01 | Render, paginate, zoom, thumbnails, text search, text select | P0 | 5 | PLANNED |
| P-02 | Page ops: merge, split, extract, reorder, delete, rotate, crop | P0 | 5 | PLANNED |
| P-03 | Annotations: highlight, underline, strikethrough, ink, shapes, text box, sticky note | P0 | 5 | PLANNED |
| P-04 | Stamps and image annotations | P1 | 5 | PLANNED |
| P-05 | Compress (image downsample/recompress, object streams, font subsetting) | P0 | 5 | PLANNED |
| P-06 | Metadata editing | P1 | 5 | PLANNED |
| P-07 | Watermark (text + image) | P1 | 5 | PLANNED |
| P-08 | Password protect / remove (RC4-128, AES-128, AES-256) | P0 | 5 | PLANNED |
| P-09 | Forms: render + fill AcroForm | P1 | 5 | PLANNED |
| P-10 | Forms: XFA | P3 | — | NOT IMPLEMENTED — XFA is effectively deprecated and the PDFium XFA build is heavyweight |
| P-11 | Redaction (true content removal, not a drawn box) | P0 | 5/18 | PLANNED — security-critical, gets its own review |
| P-12 | Drawn/typed signature (visual, non-cryptographic) | P0 | 5 | PLANNED |
| P-13 | Digital signature: PKCS#7 sign + validate | P2 | 18 | TBD — needs a decision on certificate sourcing and trust store |
| P-14 | PDF creation, image→PDF, OCR→searchable PDF, PDF→image | P0 | 5/7 | PLANNED |
| P-15 | PDF/A output conformance | P2 | — | TBD — **do not claim PDF/A until output is validated by an independent checker** (e.g. veraPDF in CI) |

### 5.5 Scanner

| ID | Feature | Pri | Phase | Status |
|---|---|---|---|---|
| S-01 | Live camera preview with document quad detection | P0 | 6 | PLANNED |
| S-02 | Perspective correction, manual override always available | P0 | 6 | PLANNED |
| S-03 | Enhancement: auto, colour, grayscale, B/W, brightness, contrast, sharpen | P0 | 6 | PLANNED |
| S-04 | Shadow removal | P1 | 6 | PLANNED |
| S-05 | Background removal | P2 | 6 | TBD — needs a segmentation model; size/perf budget unproven |
| S-06 | Batch capture, reorder, retake, delete page | P0 | 6 | PLANNED |
| S-07 | Export: searchable PDF, flat PDF, JPEG/PNG set | P0 | 6/7 | PLANNED |
| S-08 | Auto-shutter on stable quad | P1 | 6 | PLANNED |
| S-09 | ID-card / passport mode (two-sided composition) | P2 | 6 | PLANNED |

### 5.6 OCR

| ID | Feature | Pri | Phase | Status |
|---|---|---|---|---|
| O-01 | Printed-text OCR with per-word bbox + confidence | P0 | 7 | PLANNED |
| O-02 | Language packs, user-selectable, downloaded on demand | P0 | 7 | PLANNED |
| O-03 | Layout analysis: blocks, lines, reading order | P0 | 7 | PLANNED |
| O-04 | Table structure recovery | P1 | 7 | PLANNED — accuracy will be materially below commercial cloud OCR; this must be stated in-product |
| O-05 | Invisible text layer → searchable PDF | P0 | 7 | PLANNED |
| O-06 | Receipts / business cards / forms structured extraction | P1 | 14 | PLANNED — implemented as an LLM extraction pass over OCR output, not a bespoke model |
| O-07 | Handwriting | P2 | 7 | BEST-EFFORT — platform APIs only (Apple Vision, ML Kit). Tesseract does not do this credibly. |
| O-08 | RTL and CJK scripts | P1 | 7 | PLANNED — needs dedicated fixtures (`testing-strategy.md`) |

### 5.7 Conversion

| ID | Feature | Pri | Phase | Status |
|---|---|---|---|---|
| C-01 | Image ↔ image (JPG/PNG/WEBP/TIFF/BMP/GIF) | P0 | 8 | PLANNED |
| C-02 | Image → PDF, PDF → image | P0 | 5/8 | PLANNED |
| C-03 | MD/HTML/TXT/RTF ↔ UDM → PDF/HTML/MD/TXT | P0 | 8 | PLANNED |
| C-04 | DOCX → UDM → PDF/HTML/MD/TXT | P0 | 8 | PLANNED — lossy; fidelity contract published |
| C-05 | UDM → DOCX (write) | P1 | 8 | PLANNED |
| C-06 | XLSX/CSV/ODS ↔ Spreadsheet Model → PDF | P1 | 8 | PLANNED |
| C-07 | PPTX → PDF / images | P2 | 8 | TBD — presentation layout fidelity is the hardest of the three OOXML families |
| C-08 | EPUB ↔ UDM | P1 | 8 | PLANNED |
| C-09 | Legacy binary DOC / XLS / PPT | P2 | 12 | SERVER-ONLY — no viable permissively-licensed C/C++ parser exists (`file-format-support.md` §7) |
| C-10 | Audio / video transcode | P2 | — | TBD — blocked on the FFmpeg licensing + codec patent decision (PQ-5) |
| C-11 | Per-conversion fidelity report surfaced to the user | P0 | 8 | PLANNED |

### 5.8 Search

| ID | Feature | Pri | Phase | Status |
|---|---|---|---|---|
| Q-01 | Exact, prefix, boolean full-text (SQLite FTS5) | P0 | 9 | PLANNED |
| Q-02 | Fuzzy / typo-tolerant | P1 | 9 | PLANNED |
| Q-03 | Metadata / faceted filters (type, date, tag, workspace) | P0 | 9 | PLANNED |
| Q-04 | Semantic (vector) search over local embeddings | P1 | 15 | PLANNED |
| Q-05 | Hybrid ranking (BM25 + vector, reciprocal rank fusion) | P1 | 15 | PLANNED |
| Q-06 | Search inside PDFs, DOCX, OCR'd images | P0 | 9 | PLANNED |
| Q-07 | Full offline search over locally-present data | P0 | 9 | PLANNED |
| Q-08 | Knowledge-graph visualisation | P2 | — | NOT IMPLEMENTED |

### 5.9 AI

| ID | Feature | Pri | Phase | Status |
|---|---|---|---|---|
| A-01 | Provider abstraction; ≥2 cloud providers + ≥1 local runtime at v1.0 | P0 | 14 | PLANNED |
| A-02 | Per-workspace + per-operation routing policy (local-only / cloud-allowed) | P0 | 14 | PLANNED |
| A-03 | Explicit consent before any document content leaves the device | P0 | 14 | PLANNED |
| A-04 | Summarize document / selection | P0 | 14 | PLANNED |
| A-05 | Rewrite, grammar, translate, explain | P1 | 14 | PLANNED |
| A-06 | RAG Q&A over a document, a page, or the workspace, **with citations** | P0 | 15 | PLANNED |
| A-07 | Structured extraction (schema-driven JSON) | P1 | 14 | PLANNED |
| A-08 | Generate notes / questions / flashcards / quizzes / study plan | P1 | 14 | PLANNED |
| A-09 | AI tagging and classification | P1 | 14 | PLANNED |
| A-10 | Document comparison | P2 | 14 | PLANNED |
| A-11 | On-device LLM (small, quantised) | P1 | 14 | PLANNED — gated on a device-capability check; low-RAM devices get cloud-or-nothing |
| A-12 | Agent framework: tools, planner, executor, permissions, audit | P1 | 16 | PLANNED |
| A-13 | Prebuilt agent: document ingest pipeline | P1 | 16 | PLANNED |
| A-14 | User-authored agents | P2 | 16 | PLANNED |

**Hard AI rules (non-negotiable, spec §18/§20/§24):**
- Every RAG answer carries resolvable citations to real stored chunks. A claim with no
  retrievable source is not rendered as a cited claim.
- No agent deletes user data, shares a document, sends data externally, or executes code
  without an explicit, scoped, revocable grant.
- Content never reaches an external provider without a consent event in the audit log.
- If a local-only policy is set and no local model is available, the operation **fails
  loudly**. It never silently falls back to cloud.

### 5.10 Sync, offline, security

| ID | Feature | Pri | Phase | Status |
|---|---|---|---|---|
| Y-01 | Offline CRUD for all local content | P0 | 2/21 | PLANNED |
| Y-02 | Durable mutation queue surviving process death | P0 | 13 | PLANNED |
| Y-03 | Device identity, registration, revocation | P0 | 13 | PLANNED |
| Y-04 | Eventual-consistency sync; never silently overwrite | P0 | 13 | PLANNED |
| Y-05 | Conflict surfacing UI (both versions retained) | P0 | 13 | PLANNED |
| Y-06 | Large-file chunked, resumable upload | P0 | 12 | PLANNED |
| Y-07 | Selective / partial sync (don't pull every blob to every device) | P0 | 13 | PLANNED |
| Y-08 | Encryption at rest (SQLCipher + blob encryption) | P0 | 2/18 | PLANNED |
| Y-09 | TLS 1.3 + certificate pinning | P0 | 12 | PLANNED |
| Y-10 | Biometric app lock | P0 | 10/11 | PLANNED |
| Y-11 | Keychain / Keystore-backed key management | P0 | 10/11 | PLANNED |
| Y-12 | Local tamper-evident audit log | P1 | 18 | PLANNED |
| Y-13 | End-to-end encrypted workspaces | P2 | 18 | TBD — **conflicts with server-side search, AI, thumbnailing and conversion. Product decision required (PQ-2).** |
| Y-14 | Account + workspace export ("take my data out") | P0 | 12 | PLANNED |
| Y-15 | Sharing + role-based permissions | P1 | 12/17 | PLANNED |

## 6. User stories with acceptance criteria

**US-1 (Priya).** *Offline on a bus, I scan 12 pages and get a searchable PDF in my workspace.*
- In airplane mode, batch-capture 12 pages.
- Each page is quad-detected and perspective-corrected; manual override available per page.
- OCR runs locally; progress is visible and cancellable.
- Output is one PDF with an invisible text layer, filed in the workspace and indexed for
  local search, within 90 s on a mid-tier device (budget in `performance-strategy.md`).
- Zero network calls. Nothing is queued for any AI provider.

**US-2 (Marcus).** *I redact a client name from a contract and can prove it is gone.*
- I select regions; the engine removes the underlying content-stream operators and text in
  those regions — it does not merely draw a rectangle.
- XMP/Info metadata containing the string is scrubbed or flagged.
- Re-opening the saved file and extracting text returns zero occurrences of the string.
- The app states plainly that redaction cannot cover content it could not parse (e.g. text
  baked into a raster image is only removed if that region is rasterised and repainted).

**US-3 (Marcus).** *I ask a question about a confidential contract without it leaving my phone.*
- Workspace policy is `AI: local only`.
- Cloud providers are not merely deprioritised — they are unselectable, and the router
  returns `AI_POLICY_BLOCKED`.
- If no local model is installed, the app says so and offers a download. It does not fall back.

**US-4 (Dan).** *Offline for 6 hours; nothing I did is lost.*
- Every mutation is journalled to durable storage **before** the UI acknowledges it.
- On reconnect the queue drains with retry/backoff; failures are visible, never silent.
- If the same object changed on another device, both versions are retained and I am asked.

**US-5 (Aiko).** *I drop a 40-page DOCX spec into a page and it becomes readable and searchable.*
- First page previews within 3 s.
- Text, headings, lists, tables, inline images preserved in the Universal Document Model.
- A fidelity report states exactly what was dropped (e.g. "2 SmartArt diagrams rendered as
  placeholder images; 1 embedded chart unsupported").
- Full text is searchable immediately.

**US-6 (Priya).** *I ask "what did the lecture say about amortised analysis?" across my whole
workspace and get an answer with page-level citations.*
- Retrieval spans notes, PDFs, and OCR'd scans.
- Each claim links to a chunk; tapping opens the source at that page with the span highlighted.
- If retrieval returns nothing above threshold, the app says it found nothing. It does not
  answer from model priors and present that as a document answer.

## 7. Quality attributes (measurable)

Requirements, not aspirations. Full budgets in `performance-strategy.md`.

| Attribute | Target | Reference device |
|---|---|---|
| Cold start to interactive | ≤ 1.5 s p50, ≤ 2.5 s p95 | mid-tier Android (8 GB), iPhone n−3 |
| Open 100 MB / 500-page PDF, first page painted | ≤ 1.2 s | same |
| PDF page render (A4 text, 2× scale) | ≤ 60 ms | same |
| Local full-text search over 100k blocks | ≤ 150 ms p95 | same |
| OCR, one 300 DPI A4 page, English | ≤ 2.5 s | same |
| Peak RSS browsing a 500-page PDF | ≤ 250 MB | same |
| Sync convergence after reconnect, 1k queued ops | ≤ 30 s | good network |
| Crash-free sessions | ≥ 99.5 % | production |
| Data-loss incidents | 0 | production |

## 8. Accessibility and localisation

- **P0:** screen-reader labelling (TalkBack/VoiceOver) for all P0 flows; dynamic type;
  ≥ 4.5:1 contrast; ≥ 44×44 pt touch targets; full keyboard navigation on desktop and web.
- **P0:** UI strings externalised from day one; no sentence concatenation.
- **P1:** RTL layout mirroring. Ship languages TBD by market research.
- **OCR language coverage is tracked separately from UI language coverage.** They are
  different lists and must never be conflated in the UI.

## 9. Regulatory and policy constraints

- **GDPR/CCPA:** lawful basis, data export (Y-14), deletion within a published SLA, and a
  public sub-processor list. **Every AI provider is a sub-processor and must be disclosed
  individually.**
- **App Store / Play:** camera, photo, notification and background-task justifications;
  Apple privacy labels; Play Data Safety. Any feature transmitting user content must be declared.
- **Licensing:** no GPL/AGPL/LGPLv3 code in the distributed client. See
  `file-format-support.md` §7 and `THIRD_PARTY_NOTICES`.
- **Patents:** HEVC/HEIF encode and H.264/HEVC transcode carry active patent-pool exposure.
  See risk R-09.

## 10. Release gates for v1.0

Ships only when **all** hold:

1. Every P0 row is `IMPLEMENTED` and meets the Definition of Done (spec §41).
2. `file-format-support.md` matches shipped reality, enforced by an automated test that walks
   the matrix and asserts each claimed capability against a fixture.
3. Fuzzing (PDF, DOCX, ZIP, image, metadata) has run ≥ 72 h with zero open medium+ crashes.
4. External security review of crypto, key management, sync auth and redaction completed and
   findings closed.
5. Zero known data-loss defects; a 7-day / 5-device sync soak shows convergence.
6. `THIRD_PARTY_NOTICES` is generated by CI from the actual dependency lockfile, not by hand.

## 11. Open product questions requiring sign-off

| # | Question | Why it blocks |
|---|---|---|
| PQ-1 | Monetisation model — what is behind the paywall (AI credits, cloud storage, PDF ops)? | Determines whether cloud AI cost is bounded, and whether local AI is the default or the premium path. |
| PQ-2 | Is E2EE a v1.0 promise? | E2EE removes server-side search, AI, thumbnailing and conversion. Architecture-level fork, not a feature flag. |
| PQ-3 | Which cloud AI providers ship at v1.0? | Each is a named sub-processor with its own DPA, pricing and privacy posture. |
| PQ-4 | Is legacy binary DOC/XLS/PPT required at v1.0? | If yes, it forces a server-side LibreOffice conversion service and breaks the offline story for those formats. |
| PQ-5 | Is A/V transcode required at v1.0? | If yes, it forces the FFmpeg LGPL + codec-patent decision. If no, we ship preview-only and avoid it entirely. |
| PQ-6 | Team collaboration at v1.0, or single-user multi-device? | Determines whether permissions and comments are v1.0 or Phase 17. |
| PQ-7 | Minimum target devices (Android API level + RAM floor; iOS version) | Sets the on-device AI and OCR performance budgets. |
