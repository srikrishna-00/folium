# System Architecture

**Status:** Phase 0 draft — awaiting human sign-off
**Version:** 0.0.0
**Date:** 2026-09-29

> Spec §4 says: *"Do not finalize this architecture blindly. First analyze alternatives and
> document tradeoffs."* This document does that. The spec's diagram is **accepted with three
> substantive revisions**, argued in §3.

---

## 1. Forces

Everything below falls out of six constraints that are in tension:

| # | Force | Consequence |
|---|---|---|
| FR-1 | Two primary platforms with incompatible native languages (Kotlin/JVM, Swift/ObjC) | Shared logic must live below both, in a language both can call: C. |
| FR-2 | Document/PDF/OCR/image work is CPU- and memory-bound on battery-powered devices | Hot paths cannot be in a GC'd or interpreted language. |
| FR-3 | Offline-first with eventual-consistency sync | The local store is the source of truth; the server is a replica + relay. Sync logic must be identical on every platform, therefore shared. |
| FR-4 | The best parsers for PDF, OCR, images and crypto are C/C++ | The core is where the third-party surface lives. It is also the fuzzing target. |
| FR-5 | UI quality bar is "native-feeling", with camera, files, share sheets, biometrics | UI and platform integration must be native per platform, not shared. |
| FR-6 | Untrusted binary input (spec §29: *"Treat malformed files as security-sensitive inputs"*) | Parsers must be isolatable and individually fuzzable. Memory safety is a first-class design concern, not a code-review concern. |

FR-1..FR-4 push logic **down** into a shared native core. FR-5 pushes presentation **up**
into per-platform code. FR-6 pushes a hard boundary **around** the parsers. The architecture
is the resolution of those three pressures.

## 2. Alternatives considered

We evaluated five whole-system shapes before accepting the spec's layering.

### A2-1 — Full cross-platform UI framework (Flutter or React Native), thin/no native core

*Shape:* Dart or JS owns everything; native only for camera and file pickers.

- **For:** fastest path to two platforms; one UI codebase; large hiring pool.
- **Against:** the heavy work (PDF render, OCR, image pipeline, conversion) still has to be
  native, so you end up with a native core *anyway* plus a second FFI boundary. Every PDF
  page bitmap crosses a platform-channel/JSI boundary. Text editing and IME behaviour in a
  custom-rendered canvas is a known, persistent quality problem for a block editor.
  Background processing and long-running file jobs are awkward.
- **Verdict:** **Rejected for the primary apps.** It optimises the layer we care least about
  (chrome) and taxes the layer we care most about (bytes).

### A2-2 — Rust core with C ABI, instead of C/C++

*Shape:* identical layering, but the core is Rust.

- **For:** memory safety by construction, which is precisely the FR-6 risk; excellent
  cross-compilation; `cargo` dependency hygiene; strong fuzzing story (`cargo-fuzz`).
- **Against:** the mandated dependency set is C/C++ (PDFium, Tesseract, Leptonica, OpenCV,
  SQLite). We would wrap all of them in `unsafe` FFI anyway, so the memory-safety win applies
  to *our* code but not to the ~80 % of attack surface that lives in the vendored parsers.
  Adds a second toolchain to every mobile build (`cargo-ndk`, Rust-iOS targets, dSYM/symbol
  handling). The master spec explicitly mandates C/C++-first (§2).
- **Verdict:** **Rejected as the core language**, but **adopted as a targeted option** for
  two components where the safety win is real and the C ecosystem is weak: the CRDT engine
  (`yrs`) and, if needed, SVG rendering (`resvg`). Flagged in `PHASE0_SUMMARY.md` as needing
  sign-off, because it introduces a Rust toolchain into the mobile build.

### A2-3 — Thin client, server does the heavy lifting

*Shape:* mobile apps are views; conversion, OCR, PDF ops and AI run server-side.

- **For:** one implementation; trivially upgradable; no mobile perf ceiling; legacy DOC/XLS
  becomes easy (run LibreOffice server-side).
- **Against:** **directly contradicts the offline-first requirement (§21)**, which is a P0
  product promise for two of four personas. Also inverts the privacy posture: every document
  must leave the device. Also: unbounded server cost per user.
- **Verdict:** **Rejected as the primary model.** **Adopted as a fallback tier** for a small,
  explicitly-labelled set of operations that are infeasible on-device (legacy binary Office
  formats, very large conversions, cloud AI). Anything in that tier is marked
  `REQUIRES NETWORK` in the format matrix.

### A2-4 — Microkernel / plugin-process core (each parser in its own OS process)

*Shape:* the PDF parser, DOCX parser, image decoders each run in separate sandboxed processes,
IPC to the main app.

- **For:** the strongest possible answer to FR-6. A malicious PDF crashes a disposable
  process, not the app. Standard practice in browsers.
- **Against:** **iOS does not permit an app to spawn arbitrary child processes.** The
  technique is unavailable on a primary platform, which kills it as a uniform design. On
  Android it is possible (isolated `Service` process) but then the two platforms diverge on
  the most security-sensitive axis, which is worse than a consistent weaker posture.
- **Verdict:** **Rejected on mobile.** **Adopted server-side** where the conversion workers
  do run in one-shot, resource-capped, network-denied containers. On mobile we substitute
  in-process hardening: see §5.

### A2-5 — Accept the spec's layering as written

- **Verdict:** **Accepted, with revisions.** See §3.

## 3. Revisions to the spec's §4 diagram

The spec's diagram is directionally right. Three things in it are wrong or underspecified
and would cause real problems if built literally.

**Revision 1 — the diagram shows a single "PLATFORM BRIDGE". There are actually two
boundaries, and conflating them is the classic mistake.**

There is (a) the *stable C ABI* exported by the core, and (b) the *per-platform idiomatic
binding* that wraps it. If you let Kotlin and Swift call a C++ API directly, you have
committed to a C++ ABI across a JNI/Swift boundary — fragile, compiler-version-dependent, and
impossible to version. We split it explicitly: `libuapp_capi` (pure C99 headers, opaque
handles, no exceptions, no STL types crossing) and above it `uapp-kotlin` / `UAPPKit` which
are hand-written idiomatic wrappers.

**Revision 2 — the diagram puts "Database Engine" *under* "Document Engine" and "SQLite"
under a separate "Storage Abstraction". That's two layers doing one job and invites the
document engine to reach past the abstraction.**

There is exactly one persistence port. Document, file, search, AI-metadata and sync modules
all go through it. Nothing in the core opens a `sqlite3*` directly except the storage module.
This is what spec §32 ("do not tightly couple business logic to one database engine")
actually requires, and the literal diagram undermines it.

**Revision 3 — the diagram omits the two things most likely to be got wrong: a policy layer
and an error/telemetry spine.**

Spec §20, §23, §24 and §34 all require cross-cutting enforcement (AI data-egress consent,
agent permissions, audit, structured errors, no sensitive data in logs). If these are not a
named layer that every call passes through, they will be reimplemented inconsistently in
twelve places and one of them will leak. We add an explicit **Policy & Audit** layer that
sits between the API facade and the engines, and a **Diagnostics spine** available to all.

## 4. Accepted architecture

```text
┌──────────────────────────────────────────────────────────────────────┐
│ PRESENTATION  (per platform, not shared)                             │
│  Android: Kotlin + Jetpack Compose   iOS: Swift + SwiftUI/UIKit      │
│  Desktop: Compose Multiplatform      Web: TypeScript + WASM subset   │
└───────────────┬──────────────────────────────────────────────────────┘
                │  idiomatic language bindings (hand-written)
┌───────────────▼──────────────────────────────────────────────────────┐
│ BINDING LAYER                                                         │
│  uapp-kotlin (JNI)   UAPPKit (Swift/ObjC)   uapp-node/wasm           │
│  - ownership + lifetime mapping, error → exception/Result            │
│  - threading: no core call on the UI thread, ever                    │
└───────────────┬──────────────────────────────────────────────────────┘
                │  libuapp_capi  —  the ONLY supported boundary
                │  C99, opaque handles, explicit ownership, versioned
┌───────────────▼──────────────────────────────────────────────────────┐
│ API FACADE  (core/api)                                                │
│  coarse-grained use-case operations, one per user intent              │
└───────────────┬──────────────────────────────────────────────────────┘
┌───────────────▼──────────────────────────────────────────────────────┐
│ POLICY & AUDIT  (security/, cross-cutting, non-bypassable)            │
│  authorization · AI egress consent · agent permissions · audit log    │
│  redaction of sensitive fields before anything reaches diagnostics    │
└───────────────┬──────────────────────────────────────────────────────┘
┌───────────────▼──────────────────────────────────────────────────────┐
│ DOMAIN ENGINES  (C++17 internals, C-ABI-exposed)                      │
│                                                                       │
│  core/document  core/database  core/search  core/sync  core/versioning│
│  file-engine    pdf-engine     scanner      ocr        media          │
│  ai (llm/embed/rag/agents)     knowledge                              │
│                                                                       │
│  Rule: engines depend on PORTS, never on each other's internals.      │
│  Cross-engine calls go through core/api or an explicit port.          │
└───────────────┬──────────────────────────────────────────────────────┘
┌───────────────▼──────────────────────────────────────────────────────┐
│ PORTS  (pure interfaces, the seam for testing and substitution)       │
│  StoragePort · BlobPort · NetworkPort · CryptoPort · AIProviderPort   │
│  OCRProviderPort · ClockPort · RandomPort · TelemetryPort             │
└───────────────┬──────────────────────────────────────────────────────┘
┌───────────────▼──────────────────────────────────────────────────────┐
│ ADAPTERS                                                              │
│  storage/sqlite (SQLCipher, FTS5, sqlite-vec) · storage/filesystem    │
│  storage/object-storage · storage/cache                               │
│  network/http · network/websocket                                     │
│  security/crypto (libsodium) · security/key-management (→ platform)   │
│  ai adapters: local (llama.cpp, ONNX Runtime) · cloud (HTTP)          │
│  ocr adapters: Tesseract · Apple Vision · ML Kit                      │
└───────────────┬──────────────────────────────────────────────────────┘
                │
┌───────────────▼──────────────────────────────────────────────────────┐
│ PLATFORM SERVICES  (injected downward from the host, not linked)      │
│  Keychain/Keystore · camera frames · file picker URIs · background    │
│  scheduler · push · biometrics · network reachability                 │
└──────────────────────────────────────────────────────────────────────┘

        ── cross-cutting, reachable from every layer ──
        DIAGNOSTICS: structured error codes · levelled logging with
        mandatory field redaction · metrics · crash breadcrumbs
```

**Key inversion:** the core does not *reach up* for platform services. The host **injects**
them at init as a vtable of function pointers (`uapp_platform_vtable_t`). This is what makes
the core testable with fakes, and what stops "just call the Keychain from C++" hacks.

## 5. Security-relevant structure (given A2-4 was rejected on mobile)

Because we cannot sandbox parsers in separate processes on iOS, the isolation moves inward:

1. **Parse/render split.** Every format module separates *untrusted parsing* from *trusted
   model construction*. Parsers produce a validated intermediate; nothing downstream touches
   raw file bytes.
2. **Arena allocation per parse job.** A parse runs against a bounded arena with a hard byte
   cap and a wall-clock cap. Exhaustion is an error (`FILE_RESOURCE_LIMIT`), never an OOM kill.
3. **No parser writes to persistent storage.** Parsers are pure: bytes in, model out. Only
   the domain layer persists.
4. **Every parser is a fuzz target.** Each has a `LLVMFuzzerTestOneInput` entry point built in
   CI (`testing-strategy.md` §6). A parser without a fuzz target does not merge.
5. **ASan/UBSan/TSan builds are first-class CI configurations**, not an occasional exercise.
6. **Decompression bombs** are handled at the port: a global expansion-ratio and absolute-size
   limit applies to ZIP/GZ/7Z/PNG/TIFF before any content is materialised.

This is a weaker posture than process isolation and we say so plainly. It is recorded as
risk **R-02**.

## 6. Threading and concurrency model

The single largest source of defects in a C/C++ core behind two GC'd UI layers is lifetime
and thread confusion. The rules are fixed now, not later:

- **The core owns no threads by default.** It exposes operations that are either
  *immediate* (bounded, microseconds — safe to call inline) or *job-based*.
- **Job-based operations** are submitted to a core-owned, fixed-size worker pool sized to
  `min(4, hardware_concurrency)` with a separate single-thread pool for I/O. Jobs are
  cancellable and report progress. Every long operation (PDF render, OCR, conversion, index)
  is a job.
- **One writer.** The SQLite connection is in WAL mode: many readers, exactly one writer,
  serialised through a write queue inside the storage adapter. No caller manages this.
- **Handles are not thread-safe unless documented.** Each opaque handle documents its
  affinity. Most are "use from one thread at a time"; the store handle is internally synchronised.
- **No core call may block the UI thread.** The bindings enforce this: Kotlin wrappers are
  `suspend` functions on `Dispatchers.Default`/`IO`; Swift wrappers are `async` and the C
  callbacks are marshalled back to `@MainActor`.
- **Callbacks from core to host** always occur on a worker thread and must be re-dispatched by
  the binding. The core never assumes a run loop.

## 7. Error model

Per spec §33. One shape everywhere:

```c
typedef struct {
    uapp_status_t code;       /* stable machine-readable enum, never reused */
    uint32_t      domain;     /* FILE | PDF | OCR | SYNC | AI | STORE | SEC ... */
    const char*   message;    /* developer-facing, English, non-localised       */
    const char*   detail;     /* optional; MUST NOT contain document content    */
    uint32_t      cause_count;
    const uapp_error_t* causes; /* error chain, not a flattened string          */
} uapp_error_t;
```

- Codes are a flat, append-only enum (`FILE_CONVERSION_FAILED`, `PDF_ENCRYPTED_UNSUPPORTED_REVISION`,
  `SYNC_CONFLICT_UNRESOLVED`, `AI_POLICY_BLOCKED`, …). Never renumbered, never reused.
- `message`/`detail` are developer strings. User-facing text is produced by the presentation
  layer from the code, so it can be localised and so it never accidentally echoes file content.
- **No function returns `void` on a fallible path and no error is discarded.** Static analysis
  enforces `[[nodiscard]]` on all fallible returns.
- Domain-specific detail travels in a typed payload, not by string-formatting.

## 8. Module dependency rules (enforced in CI)

```
presentation → bindings → capi → api → policy → engines → ports → adapters
```

Enforced mechanically by a CI script over `#include` graphs and CMake link targets:

1. No engine may include another engine's private headers.
2. No engine may include an adapter header. Engines see ports only.
3. `core/` may not include `pdf-engine/`, `ocr/`, `media/` — the dependency is the other way
   or via a port.
4. Nothing below `bindings/` may include a platform SDK header (`jni.h`, `Foundation/*`).
5. `capi` headers are C99 and must compile clean under `-std=c99 -Wall -Wextra -Wpedantic`
   with a C compiler, not just a C++ one. A CI job compiles them standalone.
6. Third-party headers are only included from the one adapter that owns that dependency.
   (If more than one place includes `pdfium/fpdfview.h`, the abstraction has already failed.)

## 9. Data flow examples

**Scan → searchable PDF (fully offline):**
```
camera frame (platform) ─► scanner/edge-detection ─► scanner/perspective
  ─► scanner/enhancement ─► ocr (TextBlock[] with bbox+conf)
  ─► pdf-engine/creation (raster page + invisible text layer)
  ─► file-engine (detect, checksum, metadata) ─► storage (blob + row)
  ─► search/indexer (FTS5) ─► [optional, policy-gated] ai/embeddings
  ─► sync/changelog (mutation enqueued, not sent yet)
```
Note what is *not* in that chain: no network, no AI provider, no server. That is the P0 path.

**Ask a question about the workspace (RAG):**
```
query ─► policy (is cloud AI permitted for this workspace? consent on record?)
  ─► search/hybrid (FTS5 BM25 ∪ vector top-k → RRF fusion)
  ─► rag/context-builder (dedupe, budget, attach chunk provenance)
  ─► ai/provider (local or cloud, per policy)
  ─► rag/citation-binder (every citation must resolve to a stored chunk id;
                          unresolvable citations are stripped, not rendered)
  ─► audit log entry (what left the device, to whom, when)
```

## 10. Deployment topology

```
Mobile / Desktop client ──TLS 1.3 (pinned)──► API Gateway
                                                │
             ┌──────────────┬───────────────────┼──────────────┬──────────────┐
             ▼              ▼                   ▼              ▼              ▼
          Identity       Sync service       Blob service   AI gateway   Conversion
          service        (changelog,        (S3-compatible, (provider     workers
                          version vectors)   presigned URLs) routing,      (one-shot,
                                                             quota)       no network,
                                                                          resource-capped)
             └──────────────┴────────► PostgreSQL (+ pgvector) ◄───────────┘
```

The client is fully functional with the entire right-hand side unreachable. That is the
architectural test of "offline-first": if the app degrades to a spinner when the gateway is
down, the architecture is wrong.

## 11. Technology positions (details in `technical-requirements.md`)

| Layer | Choice | Confidence |
|---|---|---|
| Core language | C++17 internals, C99 exported ABI | High — see `adr/0001` |
| Build | CMake ≥ 3.24 + Ninja, vendored pinned third-party | High |
| Local store | SQLite (public domain) + SQLCipher (BSD-3) + FTS5 + `sqlite-vec` | High |
| PDF | PDFium (BSD-3) render/parse + QPDF (Apache-2.0) structure/encryption | High |
| OCR | Tesseract + Leptonica portable baseline; platform OCR as fast path | High |
| Imaging | libjpeg-turbo, libpng, libwebp, libtiff, giflib, OpenCV core | High |
| Crypto | libsodium (ISC); TLS via platform stack, not bundled | High |
| Android UI | Kotlin + Jetpack Compose | High |
| iOS UI | Swift + SwiftUI with UIKit interop for the document canvas | High |
| Desktop UI | Compose Multiplatform | **Medium — needs sign-off** |
| Web | TypeScript + a WASM subset of the core | Medium — heavily reduced scope |
| Sync algorithm | Version vectors + op log; sequence CRDT for text only | **Medium — needs sign-off** |
| Backend language | Go | **Medium — needs sign-off** |
| Cloud DB | PostgreSQL + pgvector | High |

## 12. What this architecture deliberately does not solve

Stated plainly so nobody is surprised in Phase 12:

- **Real-time collaborative editing.** The sync design is eventual-consistency, pull/push,
  not an OT/CRDT live session. Phase 17 will need a presence and awareness channel that does
  not exist here.
- **Server-side rendering of user documents under E2EE.** If PQ-2 resolves to "E2EE at v1.0",
  the conversion-worker and AI-gateway boxes in §10 become unusable and must be redesigned.
- **Plugin sandboxing.** Spec §30 asks for a plugin system. A plugin that is native code has
  full process privileges. Until we define a safe plugin substrate (WASM is the likely
  answer), plugins are first-party only. Recorded as risk R-11.
- **Cross-device large-blob availability.** Selective sync means a blob may not be on this
  device. Every feature that touches a blob must handle `BLOB_NOT_LOCAL` and offer to fetch.
  This is an ambient complexity cost paid in every module.
