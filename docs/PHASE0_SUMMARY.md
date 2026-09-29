# Phase 0 Summary — Architecture & Feasibility Study

**Date:** 2026-09-29
**Status:** Complete pending human sign-off
**Scope:** Documentation only. **No production code was written**, per spec §40.

---

## 0. What exists now

22 documents covering all 25 items of spec §40, plus this summary.

| Doc | Covers spec §40 item |
|---|---|
| `product-requirements.md` | 1 |
| `architecture.md` | 2 |
| `technical-requirements.md` | 3, 4 |
| `adr/0001-core-language-c-vs-cpp.md` | 5 |
| `mobile-architecture.md` | 6, 7 |
| `database-design.md` | 8 |
| `file-format-support.md` | 9 (file engine), 18, 19 |
| `conversion-architecture.md` | 20 |
| `api-design.md` | (spec §31) |
| `ai-architecture.md` | 12, 13, 14 |
| `security-model.md` | 17 |
| `offline-architecture.md` | 15 |
| `sync-architecture.md` | 16 |
| `testing-strategy.md` | 21 |
| `performance-strategy.md` | 22 |
| `roadmap.md` | 23 |
| `risk-register.md` | 24 |
| `engineering-complexity-estimate.md` | 25 |
| `deployment.md`, `contributing.md`, `changelog.md`, `THIRD_PARTY_NOTICES` | supporting |

PDF (item 10), OCR (item 11) and file-engine (item 9) architectures live inside
`technical-requirements.md` §1 and `file-format-support.md` rather than as separate files,
because the monorepo skeleton did not provide those filenames and splitting them would have
separated each engine's design from its licence constraints — which are the binding constraint
for both.

---

## 1. Decisions made with HIGH confidence

These are recommended for adoption without further investigation. Each has a written rationale
and rejected alternatives in the linked document.

### Architecture

| # | Decision | Where |
|---|---|---|
| H-1 | **C++17 internals behind a pure C99 exported ABI.** Rust, pure C, C++20 and mixed-language approaches were all evaluated and rejected with reasons. | `adr/0001` |
| H-2 | **Split the spec's "platform bridge" into two layers**: a versioned C ABI (`libuapp_capi`) plus hand-written idiomatic Kotlin/Swift bindings. Never let the C++ ABI cross JNI or Swift interop. | `architecture.md` §3 |
| H-3 | **One persistence port.** Nothing outside `storage/sqlite` includes `sqlite3.h`. This is what spec §32 actually requires; the spec's own diagram undermines it. | `architecture.md` §3, `database-design.md` §1 |
| H-4 | **Add an explicit non-bypassable Policy & Audit layer** between the API facade and the engines. Spec §20/§23/§24/§34 all require cross-cutting enforcement; without a named layer it gets reimplemented inconsistently and one copy leaks. | `architecture.md` §3, `ai-architecture.md` §4 |
| H-5 | **Reject cross-platform UI frameworks for the primary apps.** They optimise the layer we care least about and tax the layer we care most about. | `architecture.md` A2-1 |
| H-6 | **Platform services are injected downward as a vtable**, never reached for. Keeps `jni.h` and `Foundation/*` out of the core and makes it testable with fakes. | `mobile-architecture.md` §6 |

### Dependencies and licensing

| # | Decision | Where |
|---|---|---|
| H-7 | **PDFium (BSD-3) + QPDF (Apache-2.0) for PDF.** PDFium renders and parses; QPDF does structure and encryption. Neither does the other's job well. | `file-format-support.md` §7.1 |
| H-8 | **MuPDF (AGPL), Poppler (GPL), Xpdf (GPL) are hard blocks** for closed distribution. MuPDF remains purchasable from Artifex if the business chooses. | `file-format-support.md` §7.2 |
| H-9 | **LGPL-3.0 is a hard block for the iOS client** — the relinking right cannot be exercised under App Store code signing. Blocks libheif, libde265, libmobi. | `file-format-support.md` §7.2 |
| H-10 | **HEIC/HEIF via platform decoders only.** Blocked on both licence *and* active HEVC patent exposure. Not supported on desktop or web. AVIF is the strategic alternative. | `file-format-support.md` §7.4 |
| H-11 | **RAR permanently out of scope.** The `unRAR` licence forbids the use we would need; no permissive alternative exists. | `file-format-support.md` §7.7 |
| H-12 | **Tesseract + Leptonica (both permissive) as the portable OCR baseline; platform OCR (Apple Vision, ML Kit) as the fast path** behind one provider port. | `technical-requirements.md` §3.4 |
| H-13 | **SQLite + SQLCipher + FTS5.** Public domain / BSD-3; index and content commit in one transaction. | `technical-requirements.md` §3.2 |
| H-14 | **libsodium for crypto; TLS from the platform stack, not bundled.** Bundling TLS means owning TLS CVE response on two app stores. | `technical-requirements.md` §3.5 |
| H-15 | **FreeType elected under the FTL, not its GPLv2 alternative**; the FTL credit line is required. | `THIRD_PARTY_NOTICES` §4 |
| H-16 | **No LibreOffice in the client.** Server-side, separate-process only — which makes legacy DOC/XLS/PPT network-dependent. | `file-format-support.md` §7.3 |

### Data and protocol

| # | Decision | Where |
|---|---|---|
| H-17 | **UUIDv7 object IDs, client-generated.** Offline creation cannot wait for a server key; v7 gives B-tree locality that v4 does not. | `database-design.md` §2 |
| H-18 | **Fractional indexing for sibling order.** Integer positions turn a one-block insert into an N-block sync conflict. This is the single most important schema decision for sync sanity. | `database-design.md` §3.3 |
| H-19 | **Wall-clock timestamps are never used for ordering, causality or conflict resolution.** Lamport clocks and version vectors only. | `sync-architecture.md` §3 |
| H-20 | **Outbox pattern:** the content write and the sync-queue write are one transaction. | `offline-architecture.md` §3 |
| H-21 | **Per-datatype merge strategies, not one algorithm.** CRDTs only where they earn their permanent metadata cost. | `sync-architecture.md` §2 |
| H-22 | **EAV for user-defined database properties** — because the alternative is concurrent `ALTER TABLE` across N offline devices, which is not a solvable merge. Cost is bounded and an escape hatch is documented. | `database-design.md` §3.4 |
| H-23 | **Embeddings are device-local and never synced.** Derivable, model-version-specific, and a partial reconstruction of source text. | `database-design.md` §3.8 |
| H-24 | **Path-major REST versioning** with a 24-month support window, plus **independent versioning** of the core ABI and the sync protocol. | `api-design.md` §2 |
| H-25 | **Idempotency keys required on every mutating endpoint.** Mobile clients retry; without this, retries duplicate documents. | `api-design.md` §3.3 |
| H-26 | **Chunked AEAD (1 MiB) for blobs** with the chunk index bound into both the nonce and the AAD — enabling random access into a 500 MB PDF while preventing reorder, truncation and cross-file splicing. | `security-model.md` §5 |

### AI

| # | Decision | Where |
|---|---|---|
| H-27 | **Provider primitives (`chat`/`embed`/`rerank`) are separate from task services (`summarize`/`extract`/…).** Making all seven of spec §17's methods provider primitives would force every adapter to reimplement the same prompts. | `ai-architecture.md` §3 |
| H-28 | **Unforgeable policy token.** A provider adapter refuses any request without one, making gate bypass a compile-and-runtime error rather than a discipline problem. | `ai-architecture.md` §3.1, §4 |
| H-29 | **LOCAL_ONLY fails closed.** No local model ⇒ `AI_NO_LOCAL_MODEL`, never a silent cloud fallback. This single behaviour is the difference between a privacy promise and a privacy claim. | `ai-architecture.md` §4 |
| H-30 | **Citations are verified against the chunk store before rendering.** Unresolvable markers are stripped and their spans marked unsupported. Spec §18: "Never fabricate citations." | `ai-architecture.md` §7.8 |
| H-31 | **The agent tool registry is closed and compiled in.** No shell tool, no eval tool, no arbitrary-HTTP tool — ever. | `ai-architecture.md` §9.2 |
| H-32 | **Model weights are dependencies**: licence-reviewed, pinned, and signature-verified before load. A model file is executable input. | `ai-architecture.md` §8 |

### Process

| # | Decision | Where |
|---|---|---|
| H-33 | **Every parser has a fuzz target, enforced by CI.** A parser without one does not merge. | `testing-strategy.md` §6 |
| H-34 | **The format-matrix verification test:** a test parses `file-format-support.md` and proves every claimed capability against a fixture. This is what makes "do not claim universal support" enforceable rather than aspirational. | `testing-strategy.md` §14 |
| H-35 | **`THIRD_PARTY_NOTICES` is generated by CI from the link graph**, never hand-maintained. | `THIRD_PARTY_NOTICES` §6 |
| H-36 | **Checkpointing, not background-time guarantees.** Every long job resumes from where it was killed. iOS background execution is at the OS's discretion. | `offline-architecture.md` §7 |
| H-37 | **Kill switches on every risky subsystem.** On mobile, a remote kill switch is the only rollback that exists. | `deployment.md` §9 |

---

## 2. Decisions REQUIRING HUMAN SIGN-OFF before Phase 1

Ordered by how much they cost to get wrong late.

### Blocking — must be answered before Phase 1 or early Phase 2

| # | Decision | Why it blocks | Recommendation |
|---|---|---|---|
| **S-1** | **PQ-2: Is end-to-end encryption a v1.0 promise?** | This is the **single largest fork in the architecture**. E2EE removes server-side search, AI, RAG, thumbnailing and conversion, and moves all merging to the client. Retrofitting it in Phase 18 (where the roadmap currently places it) would require redesigning Phases 12–15. Answering it in Phase 18 is the most expensive available mistake. | **Decide now.** If yes, move E2EE work before Phase 12 and add ~40 ew. If no, document publicly and honestly what the server can see. |
| **S-2** | **PQ-7: Minimum device targets** (Android API level + RAM floor; iOS version) | Sets the entire on-device budget for OCR, local AI and large-document handling. Every performance budget in `performance-strategy.md` is written against an assumed Tier B. | Proposed: Android API 26 / 6 GB floor, iOS 16. **Confirm against target-market device data.** |
| **S-3** | **SP-6: Text CRDT — `yrs` (Rust) or our own C++ RGA?** | Determines whether Rust enters the mobile build. That decision also governs `resvg` (SVG) and `tokenizers`, so it should be made once. | **Recommend `yrs`.** Build-system pain is cheap and recoverable; a text-merge correctness bug is expensive and reputationally damaging. **But it contradicts spec §2's C/C++-first mandate, so it is the user's call.** |
| **S-4** | **Run the three blocking spikes**: SP-1 (PDFium mobile build), SP-5 (bridge cost), SP-6 (CRDT). | All three constrain the core's shape. SP-1 failing would force either a prebuilt-binary vendor or a paid MuPDF licence. | ~13 engineer-days. Do them before Phase 1 commits. |

### High priority — needed before the relevant phase

| # | Decision | Needed by | Recommendation |
|---|---|---|---|
| S-5 | **PQ-1: Monetisation model.** What is behind the paywall? | Phase 14 | Determines whether cloud AI cost is bounded (risk R-17) and whether local AI is the default or the premium path. |
| S-6 | **PQ-3: Which cloud AI providers at v1.0?** | Phase 14 | Each is a named sub-processor requiring a DPA with zero-retention terms. ≥2 required by the abstraction. |
| S-7 | **PQ-4: Is legacy DOC/XLS/PPT required at v1.0?** | Phase 8 | **Recommend no.** Saying yes forces a server-side LibreOffice service and breaks the offline promise for those formats. |
| S-8 | **PQ-5: Is A/V transcoding required at v1.0?** | Phase 8 | **Recommend no.** Saying yes forces the FFmpeg LGPL + codec-patent decision (R-09). Playback via platform stacks is free of both. |
| S-9 | **PQ-6: Team collaboration at v1.0, or single-user multi-device?** | Phase 12 | **Recommend single-user multi-device for v1.0.** Phase 17 is 54 ew. |
| S-10 | **Backend language: Go?** | Phase 12 | Go recommended for operational simplicity. Rust would allow reusing core logic; Kotlin would allow reusing the sync model. |
| S-11 | **Desktop: Compose Multiplatform or Qt?** | Phase 19+ | Compose MP (Apache-2.0) reuses the Kotlin UI and the existing JNI binding. Qt needs a commercial licence for closed static linking. |
| S-12 | **Team size and specialty coverage.** | Now | The estimate assumes genuine C++ depth plus PDF, CRDT, mobile-native and ML specialties. Gaps must be filled *before* the relevant phase (R-21). |
| S-13 | **Legal review of the licence analysis.** | Before Phase 5 | `file-format-support.md` §7 and `THIRD_PARTY_NOTICES` were compiled by an engineer from upstream sources. **They are not legal advice.** |
| S-14 | **Scope: full v1.0 (~1004 ew) or the narrowed v1.0 (~799 ew)?** | Now | See `engineering-complexity-estimate.md` §6. The honest finding is that narrowing saves less than expected — the expensive parts are load-bearing for any version. |
| S-15 | **MQ-4: ML Kit on Android** (better OCR, requires Play Services) **or Tesseract everywhere** (consistent, works on devices without Play Services)? | Phase 7 | Affects the addressable device set in some markets. |
| S-16 | **P-13: PKCS#7 digital signatures at v1.0?** | Phase 18 | **Recommend deferring.** A half-implemented signature validator is worse than none — a false "valid" badge is a security failure. |

---

## 3. Spec ambiguities resolved by judgment call

Each is a place where the master spec was underspecified or internally in tension. The call and
its reasoning are recorded so they can be overturned deliberately rather than discovered
accidentally.

### J-1 — "C/C++-first" did not choose between C and C++

**Call:** C++17 internals, C99 exported ABI.
**Reasoning:** The exported boundary must be C regardless (JNI and Swift interop demand it), so
the real question is what lies behind it. Our dependency set is mixed, and the three most
important dependencies (PDFium, Tesseract, OpenCV) are C++. A pure-C core would need C++ shims
around all of them — we would be writing C++ anyway, just at the boundary and badly. The
domain is heavily resource-owning, and RAII eliminates by construction the defect class that
the threat model says is our highest technical risk. Full argument in `adr/0001`.

### J-2 — Spec §4's diagram, if built literally, contains three problems

**Call:** Accept it with three revisions (`architecture.md` §3) — split the bridge into ABI +
bindings; collapse the duplicated persistence layers into one port; add a Policy & Audit layer
and a diagnostics spine.
**Reasoning:** The spec explicitly says *"Do not finalize this architecture blindly."* The
single "PLATFORM BRIDGE" box conflates two boundaries that must be separate. "Database Engine"
under "Document Engine" plus a separate "Storage Abstraction" is two layers doing one job and
invites reaching past the abstraction — contradicting §32. And §20/§23/§24/§34 all require
cross-cutting enforcement that the diagram gives no home to.

### J-3 — "Universal file engine" vs. "do not claim universal support"

**Call:** "Universal" describes the *abstraction* (one `File` type, one capability model, one
registry), not the *coverage*. Coverage is a bounded, published, test-enforced matrix.
**Reasoning:** The spec asserts both in §8 and §9. The only coherent reading is that the
architecture is universal and the support is finite. We went further and made the matrix
machine-verified (H-34), so it cannot drift into an overclaim.

### J-4 — Spec §17 lists seven AI methods as if all were provider primitives

**Call:** Three primitives (`chat`, `embed`, `rerank`) plus task services built on top.
**Reasoning:** No provider natively implements `summarize`. Making it a primitive would mean
every adapter reimplements the same prompt, guaranteeing inconsistency across providers and
making "swap providers freely" false in practice. Adding a provider now requires five
functions, not thirteen prompts.

### J-5 — Spec §31 sketches `POST /files/upload` and a single `POST /sync`

**Call:** Replaced with a three-step presigned upload and split `/sync/push` + `/sync/pull`.
**Reasoning:** Single-request multipart upload caps file size at the gateway limit, cannot
resume, and burns API-tier bandwidth — unacceptable for a product whose users upload 100 MB
PDFs on cellular. A single `/sync` endpoint makes cursor semantics ambiguous and prevents
pulling without pushing, which is exactly what a read-only or recovering client needs. All
deviations from §31 are marked **[Δ]** with reasons in `api-design.md` §4.

### J-6 — Spec §22 requires both "eventual consistency" and "never overwrite silently"

**Call:** Per-datatype strategies, with "never silently" made mechanical rather than aspirational.
**Reasoning:** Automatic convergence and zero loss cannot both hold for all data without a
human. So: CRDTs where merging genuinely commutes (text, sets, ordering), LWW where it does not
— **but every LWW loser is written to version history before being overwritten, and every
automatic resolution records a reviewable conflict row.** "Silently" is the operative word;
resolution is fine, silence is not.

### J-7 — Spec §30 asks for a plugin system; native plugins have full privileges

**Call:** First-party plugins only at v1.0. Third-party plugins deferred pending a safe
substrate (WASM with an explicit capability interface).
**Reasoning:** A native plugin can read the decrypted database and the keys. Shipping a
third-party plugin system without sandboxing would undo the entire security model. Recorded as
risk R-11.

### J-8 — Spec §12's scanner pipeline implies OCR is always in the capture path

**Call:** OCR is an optional, explicit stage, defaulting to on for scans but always skippable
and always cancellable.
**Reasoning:** OCR is the slowest step (2.5 s/page). Forcing it into capture would make batch
scanning of 20 pages a 50-second blocking operation. Making it a separate, resumable job lets
the user keep scanning while OCR runs behind them.

### J-9 — Spec §9 lists formats without saying what "support" means per format

**Call:** Five-level symbol vocabulary (✔ / ◐ / ☁ / ✖ / —) with a mandatory limitations column,
and platform columns that may differ.
**Reasoning:** A binary supported/unsupported column would force us to either overclaim (call
lossy DOCX conversion "supported") or underclaim (call it unsupported when it is genuinely
useful). ◐ with an explicit limitations entry is the only honest option, and it satisfies spec
§43's "do not pretend unsupported functionality works."

### J-10 — Spec §32 lists 13 domains to "separate" without saying how

**Call:** Schema namespaces with per-namespace migrations, repositories, and declared
cross-namespace references — not separate database files.
**Reasoning:** Separate files would break atomic cross-domain transactions (content + FTS +
sync outbox must commit together, H-20). Namespaces give the modularity §32 wants without
losing the transactionality the correctness requires.

### J-11 — Spec §36 lists 20 phases but not what ends one

**Call:** Every phase gets explicit entry criteria and a phase-specific "done when" list on top
of the §41 Definition of Done.
**Reasoning:** Spec §5 says *"Only then proceed"* but the §41 checklist is generic. Without a
phase-specific gate, "done" becomes negotiable under schedule pressure, which is precisely how
the §42 preferences (tested functionality over feature count) get inverted.

### J-12 — Spec §11 lists "digital signatures" alongside ordinary PDF features

**Call:** Split into visual signature (P0, non-cryptographic, clearly labelled) and
cryptographic PKCS#7 signing/validation (P2, TBD).
**Reasoning:** These are completely different features with different risk profiles. A drawn
signature image is a UI feature. Displaying "signature valid" requires chain validation,
revocation checking and byte-range verification — and getting it wrong means telling a user a
forged contract is authentic. We will not ship the badge until the validation is real.

### J-13 — Spec §13 lists handwriting OCR under "support"

**Call:** BEST-EFFORT, platform APIs only, labelled as such in-product, never marketed.
**Reasoning:** The spec itself hedges with "where feasible." Tesseract does not do cursive
credibly. Presenting it as a supported feature would violate §43.

### J-14 — The scale implied by the spec is not a normal v1.0

**Call:** Estimated honestly (~1004 ew ≈ 19.3 engineer-years) and offered a narrowed v1.0,
rather than producing a comfortable number.
**Reasoning:** The spec asks for five products' worth of functionality. The most useful thing a
feasibility study can do is make that visible before commitments are made. Recorded as risk
R-01 with score 15 — the highest in the register.

---

## 4. Most important things to get right, in order

1. **Answer PQ-2 (E2EE) now.** Everything downstream of Phase 12 depends on it, and it is the
   most expensive thing to discover late.
2. **Run SP-1, SP-5, SP-6 before Phase 1.** ~13 engineer-days that de-risk the core's shape.
3. **Get counsel to review the licence analysis before Phase 5.** A GPL-family mistake in the
   PDF engine is discovered by someone else, at the worst possible time.
4. **Decide the scope honestly (S-14).** Compressing an estimate does not compress the work.
5. **Treat Phases 5, 8 and 13 as the project.** They are 25 % of effort, all three
   Low-confidence, and carry the three worst failure modes (RCE, user-visible quality collapse,
   silent data loss).

---

## 5. Working tree

Nothing has been committed. `docs/` and `THIRD_PARTY_NOTICES` are new and untracked; all
engine directories remain empty as required. Review and commit at your discretion.
