# Technical Requirements — Module Architecture & Technology Selection Matrix

**Status:** Phase 0 draft — awaiting human sign-off
**Version:** 0.0.0
**Date:** 2026-09-29

Companion to `architecture.md`. That document argues the *shape*; this one fixes the
*modules*, their *contracts*, and the *technology choices* with reasons and rejected
alternatives.

---

## 1. Module map

Directory names match the existing monorepo skeleton (spec §7).

### 1.1 `core/`

| Module | Responsibility | Depends on | Does **not** know about |
|---|---|---|---|
| `core/api` | Use-case facade. One operation per user intent. Transaction boundaries live here. | policy, all engines | UI, platform, SQL |
| `core/document` | Page + block tree, block types, transactional edits, undo/redo stack | StoragePort | rendering, PDF, files on disk |
| `core/blocks` | Block schema registry, validation, migration between block schema versions | — | storage |
| `core/database` | User-defined databases: property schema, rows, views, filter/sort/group AST + evaluator | StoragePort | UI widgets |
| `core/search` | Index maintenance, query planning, hybrid ranking (BM25 ∪ vector → RRF) | StoragePort | AI providers |
| `core/sync` | Version vectors, change log, mutation queue, conflict detection/resolution | StoragePort, NetworkPort | HTTP specifics |
| `core/versioning` | Snapshots, diffs, restore | StoragePort | sync transport |
| `core/permissions` | Subject/resource/action model, evaluation | StoragePort | network |

### 1.2 `file-engine/`

| Module | Responsibility | Notes |
|---|---|---|
| `detection` | Magic bytes → container probe → internal metadata → declared MIME → extension | **Precedence is fixed in that order.** Extension is the *last* signal and never overrides a confident magic match (spec §8). |
| `parsers` | Per-format readers producing a validated model | Untrusted-input boundary. Arena-allocated, byte- and time-capped. |
| `renderers` | Model → pixels / layout | Separate from parsers so a parser bug cannot become a render-time RCE path |
| `conversion` | Orchestrates parser → IR → writer (see `conversion-architecture.md`) | |
| `compression` | ZIP/GZ/TAR/7Z read; deflate/zstd for internal use | Bomb limits enforced here |
| `metadata` | EXIF/XMP/ID3/PDF Info read, write, strip | Privacy-relevant: strip-on-share is a feature |

### 1.3 `pdf-engine/`

Split by *who owns the bytes*, because two libraries are involved:

| Module | Backend | Responsibility |
|---|---|---|
| `reader` | PDFium | Open, decrypt, page tree, text extraction with coordinates |
| `renderer` | PDFium | Page → bitmap at scale, tile rendering, progressive |
| `editor` | QPDF | Page-level structural ops: merge, split, extract, reorder, rotate, delete |
| `annotation` | own + PDFium | Annotation model, serialisation to PDF annotation dicts |
| `forms` | PDFium (`FPDF_FORMFILL`) | AcroForm render + fill. XFA: NOT IMPLEMENTED |
| `signature` | own + libsodium/platform | Visual signature (P0); PKCS#7 (P2, TBD) |
| `security` | QPDF | Standard security handler: RC4-128, AESV2, AESV3; permissions |

**Why two libraries.** PDFium is a renderer and a parser; it is poor at lossless structural
rewriting and object-level surgery. QPDF is excellent at structural rewriting, linearisation
and encryption, and cannot render at all. Each is permissively licensed. Using one for what
the other is good at is where PDF projects usually go wrong.

**Redaction ownership.** Redaction is *not* an annotation. It lives in `editor` and must
rewrite content streams. Implementation sketch in `file-format-support.md` §8.

### 1.4 `scanner/`, `ocr/`, `media/`

| Module | Responsibility |
|---|---|
| `scanner/camera` | Frame intake contract only — the actual camera is a platform service |
| `scanner/edge-detection` | Quad detection (OpenCV: grayscale → blur → Canny → contours → quad scoring) |
| `scanner/perspective` | Homography + warp |
| `scanner/enhancement` | Adaptive threshold, shadow removal (morphological background estimate), sharpen, denoise |
| `scanner/batch` | Multi-page session state, reorder, retake |
| `ocr/text` | Provider-abstracted recognition → `TextBlock[]` |
| `ocr/layout` | Block/line/reading-order analysis |
| `ocr/tables` | Table structure recovery (ruling-line + projection-profile; explicitly approximate) |
| `ocr/handwriting` | Platform providers only. BEST-EFFORT. |
| `media/image` | Decode, encode, resize, colour ops |
| `media/audio`, `media/video` | Probe + thumbnail at v1.0. Transcode is TBD (PQ-5). |

### 1.5 `ai/`, `knowledge/`

See `ai-architecture.md` for contracts. Modules: `llm`, `embeddings`, `rag`, `agents`,
`summarization`, `classification`, `extraction`, `translation`;
`knowledge/knowledge-graph`, `backlinks`, `semantic-search`.

### 1.6 `storage/`, `network/`, `security/`

Adapters behind ports. `storage/sqlite` is the **only** place `sqlite3.h` is included.
`security/key-management` is a thin façade over an injected platform vtable — no key material
is ever generated or stored by portable code alone.

## 2. Port contracts (the testing seam)

Sketched in C for ABI clarity; internals are C++.

```c
/* StoragePort — the single persistence abstraction (spec §32) */
typedef struct uapp_store_s uapp_store_t;
uapp_status_t uapp_store_open (const uapp_store_cfg_t*, uapp_store_t**);
uapp_status_t uapp_store_txn_begin (uapp_store_t*, uapp_txn_t**);
uapp_status_t uapp_store_txn_commit(uapp_txn_t*);
uapp_status_t uapp_store_txn_abort (uapp_txn_t*);
/* typed repositories obtained from a txn; no raw SQL crosses this port */

/* BlobPort — content-addressed byte storage, may be remote-backed */
uapp_status_t uapp_blob_put   (uapp_blob_store_t*, uapp_stream_t* src, uapp_blob_id_t* out);
uapp_status_t uapp_blob_open  (uapp_blob_store_t*, uapp_blob_id_t, uapp_stream_t** out);
/* returns BLOB_NOT_LOCAL if selective sync has not fetched it */

/* AIProviderPort — see ai-architecture.md */
/* OCRProviderPort, NetworkPort, CryptoPort, ClockPort, RandomPort, TelemetryPort */
```

`ClockPort` and `RandomPort` exist so that sync, version vectors, IDs and crypto are
deterministic under test. Without them, sync tests are flaky and unreproducible, which
historically means sync bugs ship.

## 3. Technology Selection Matrix

Legend — **Confidence:** High (decide now) · Medium (needs sign-off) · Low (spike first).
**Licence risk:** ✅ safe for closed commercial distribution · ⚠️ conditional · ❌ blocked.
Full licence analysis and sources: `file-format-support.md` §7.

### 3.1 Core platform

| Concern | Choice | Licence | Alternatives rejected | Rationale | Conf. |
|---|---|---|---|---|---|
| Core language | C++17 internals / C99 ABI | — | C99-only; Rust; C++20 | See `adr/0001-core-language-c-vs-cpp.md` | High |
| Build system | CMake ≥ 3.24 + Ninja | BSD-3 | Bazel, Meson, Gradle-native | Only system with first-class Android NDK *and* Xcode support and universal third-party support. Bazel is better at hermeticity but its iOS/Xcode story costs more than it returns at this team size. | High |
| Dependency mgmt | Vendored `third_party/` + CMake `FetchContent` pinned by commit hash + SHA256 | — | vcpkg, Conan | Mobile cross-compilation triplets are where both package managers hurt most. Vendoring pinned sources gives reproducibility and makes the licence audit mechanical. | High |
| Toolchain | Clang (NDK r26+, Xcode 15+), GCC ≥ 12 for Linux CI | — | MSVC-first | Single primary compiler keeps UB/warning behaviour consistent; MSVC is a secondary CI target only. | High |
| Unit test | GoogleTest + GoogleMock | BSD-3 | Catch2, doctest | Mature Android/iOS runners, good death-test and parameterised support. | High |
| Fuzzing | libFuzzer + OSS-Fuzz-style corpora; AFL++ for deep runs | Apache-2.0 / NCSA | honggfuzz | Already integrated with Clang sanitizers. | High |
| Benchmark | Google Benchmark | Apache-2.0 | — | | High |
| Serialisation (internal/IPC) | FlatBuffers | Apache-2.0 | Protobuf, Cap'n Proto, JSON | Zero-copy read matters for large block trees crossing the JNI boundary. | Medium |
| Serialisation (wire) | JSON (API) + CBOR (sync deltas) | — | Protobuf | JSON for debuggability at the public API; CBOR for compact binary sync. | Medium |

### 3.2 Storage

| Concern | Choice | Licence | Rejected | Rationale | Conf. |
|---|---|---|---|---|---|
| Local DB | SQLite (WAL) | Public domain ✅ | Realm, LevelDB/RocksDB, ObjectBox | Mandated by spec §32; also correct. Best-tested embedded DB in existence, FTS5 and JSON1 built in, trivially licensed. | High |
| At-rest encryption | SQLCipher (community) | BSD-3 ✅ | SEE (commercial), app-level field encryption | Page-level AES-256; drop-in. Costs ~5–15 % on read. Note: encrypted DB pages defeat OS-level dedupe/compression — accepted. | High |
| Full-text | SQLite FTS5 + custom tokenizer (ICU-backed) | PD / Unicode ✅ | Tantivy (Rust), Lucene ports, Xapian (GPL ❌) | Stays in one file/one transaction with the data. Critical: index and data commit atomically. | High |
| Vector index | `sqlite-vec` | Apache-2.0 / MIT ✅ | FAISS (MIT, heavy), hnswlib (Apache-2.0), usearch (Apache-2.0), sqlite-vss (deprecated) | Same-transaction consistency with the content is worth more than raw ANN throughput at our corpus sizes (10⁴–10⁶ chunks). **Spike required** (see §6) to confirm recall/latency; `usearch` is the fallback. | Medium |
| Unicode / collation | ICU | Unicode-3.0 ✅ | platform ICU only | Needed for consistent case-folding, normalisation and CJK segmentation across platforms. Note: adds ~10 MB; use the trimmed data set. | Medium |
| Cloud DB | PostgreSQL 16 + `pgvector` | PostgreSQL / PostgreSQL ✅ | MySQL, DynamoDB, Mongo | Transactional sync needs real transactions; pgvector removes a whole service. | High |
| Object storage | S3-compatible API | — | Filesystem, GCS-native | Portability across providers; presigned URLs let clients upload directly. | High |
| Server cache/queue | Redis (or Valkey) | BSD-3 / BSD-3 ✅ | — | Note: Redis ≥ 7.4 is RSALv2/SSPL — **use Valkey (BSD-3) or Redis ≤ 7.2.4** to stay clean. ⚠️ | Medium |

### 3.3 Documents and PDF

| Concern | Choice | Licence | Rejected | Rationale | Conf. |
|---|---|---|---|---|---|
| PDF parse + render | **PDFium** | BSD-3-Clause ✅ | MuPDF (AGPL-3.0 ❌), Poppler (GPL-2.0+ ❌), Xpdf (GPL ❌), PoDoFo (LGPL-2.0+ ⚠️, no renderer) | The only mature, permissively-licensed PDF renderer. Battle-tested in Chrome, so it is also the best-fuzzed. See `file-format-support.md` §7 for its bundled third-party licences. | High |
| PDF structural ops | **QPDF ≥ 11** | Apache-2.0 ✅ | PDFium's own save path, PoDoFo | Lossless object-level rewriting, linearisation, all standard security handlers incl. AES-256. Complements PDFium precisely. | High |
| PDF write from scratch | Own writer over the UDM | — | libharu (ZLIB/MIT, stale), PDFium's page-gen | We already need a layout engine for UDM→PDF; a third-party generator would not know our model. | Medium |
| OOXML container | libzip **or** minizip-ng | BSD-3 / zlib ✅ | libarchive (also fine) | | High |
| XML | libexpat (pull) + pugixml (DOM, small docs) | MIT / MIT ✅ | libxml2 (MIT, larger; more CVE history) | Expat for streaming huge `sheet1.xml`; pugixml for small parts. **XXE/entity expansion disabled unconditionally.** | High |
| XLSX read/write | xlnt | MIT ✅ | libxlsxwriter (BSD-2, write-only), OpenXLSX (BSD-3) | Read + write in one library. Formula *evaluation* is our own — see §5. | Medium |
| DOCX / PPTX | **Own parsers** over expat + libzip | — | LibreOffice/LibreOfficeKit (MPL-2.0 ⚠️, ~400 MB, not embeddable on mobile), Apache POI (Apache-2.0, JVM-only) | No embeddable permissive C/C++ OOXML text-layout library exists. This is a real, large build cost — see `engineering-complexity-estimate.md`. | High (that we must) / Medium (on scope) |
| Legacy DOC/XLS/PPT | **Server-side LibreOffice headless** | MPL-2.0 ⚠️ | antiword/catdoc/wv (all GPL ❌), own binary parser (very high cost) | Running an MPL-2.0 binary as a separate server-side process is clean; embedding it in the client is not. Client-side: **NOT SUPPORTED**. | Medium (gated on PQ-4) |
| Markdown | md4c | MIT ✅ | cmark-gfm (BSD-2+MIT) | CommonMark-compliant, SAX-style, tiny, fast. | High |
| HTML parse | Lexbor | Apache-2.0 ✅ | Gumbo (Apache-2.0, unmaintained) | Spec-compliant HTML5 tree construction, actively maintained. | Medium |
| RTF | Own parser | — | librtf, unrtf (GPL ❌) | RTF is a documented token grammar; a scoped reader is ~2–3k lines. | Medium |
| EPUB | Own (ZIP + XHTML via Lexbor) | — | libgepub (LGPL ⚠️) | EPUB is a container spec, not a parsing problem. | High |
| MOBI / AZW3 | Own minimal reader | — | libmobi (LGPL-3.0 ❌ for iOS) | **DRM'd variants explicitly unsupported.** | Low — P2 |
| Text layout | HarfBuzz + FreeType + a own line-breaker (ICU line-break data) | MIT / FTL ✅ | Pango (LGPL ⚠️), platform text APIs | Needed for deterministic UDM→PDF pagination that matches across platforms. **Choose FreeType under FTL, not the GPLv2 option.** | Medium |

### 3.4 Imaging, scanning, OCR

| Concern | Choice | Licence | Rejected | Rationale | Conf. |
|---|---|---|---|---|---|
| JPEG | libjpeg-turbo | IJG / BSD-3 / zlib ✅ | libjpeg | SIMD; 2–5× faster decode. | High |
| PNG | libpng + zlib | libpng / zlib ✅ | lodepng | | High |
| WebP | libwebp | BSD-3 ✅ | | | High |
| TIFF | libtiff | libtiff (BSD-like) ✅ | | Multi-page TIFF matters for scanning. Historically CVE-heavy → fuzz it hard. | High |
| GIF | giflib | MIT ✅ | | | High |
| SVG | resvg | MPL-2.0 ⚠️ (Rust) **or** NanoSVG (zlib ✅, limited) | librsvg (LGPL ⚠️, Rust) | MPL-2.0 is file-level copyleft and safe for closed distribution, but resvg pulls in Rust. **Decision deferred** — NanoSVG covers basic SVG with zero new toolchain. | Low |
| HEIC/HEIF | **Platform decoders only** (iOS ImageIO, Android `ImageDecoder`) | — | libheif (LGPL-3.0 ❌) + libde265 (LGPL-3.0 ❌) + x265 (GPL-2.0 ❌) | libheif's stack is LGPLv3 (incompatible with App Store relinking) and HEVC carries active patent-pool exposure. Platform decoders are already licensed by the OS vendor. **Desktop/web HEIC: NOT SUPPORTED.** | High |
| AVIF | libavif + dav1d (decode) | BSD-2 / BSD-2 ✅ | | Decode only; no encode at v1.0. | Medium |
| CV / scanning | OpenCV (core, imgproc, photo only) | Apache-2.0 ✅ | own implementations | **Do not link `opencv_contrib`/nonfree** (SURF etc. carry patent baggage). Build a trimmed static lib — full OpenCV would add ~40 MB. | High |
| Barcode / QR | ZXing-C++ | Apache-2.0 ✅ | ZBar (LGPL ⚠️) | | Medium |
| OCR (portable) | **Tesseract 5** + Leptonica | Apache-2.0 / BSD-2 ✅ | PaddleOCR (Apache-2.0, heavy runtime), EasyOCR (Python ❌), Kraken/Calamari (Python) | Only mature C++ OCR with a permissive licence. LSTM engine, 100+ languages, `tessdata_fast` ≈ 2–5 MB/language. | High |
| OCR (fast path) | Apple Vision (iOS/macOS), ML Kit Text Recognition (Android) | Platform / proprietary-but-free ⚠️ | — | Materially better accuracy and speed, free, and already on-device. **But:** ML Kit's unbundled model requires Play Services; Vision is Apple-only. Hence the provider abstraction — Tesseract is the guaranteed floor. | High |
| Handwriting | Platform only | — | Tesseract | Tesseract does not do cursive credibly. Never claim otherwise. | High |

### 3.5 Crypto, network, AI

| Concern | Choice | Licence | Rejected | Rationale | Conf. |
|---|---|---|---|---|---|
| Symmetric/AEAD/KDF/signatures | libsodium | ISC ✅ | OpenSSL libcrypto, Botan, monocypher | Misuse-resistant API, tiny, audited, trivial cross-compilation. XChaCha20-Poly1305, Argon2id, Ed25519, X25519 — everything we need. | High |
| TLS | **Platform stack** (NSURLSession / OkHttp+Conscrypt / Schannel / system OpenSSL) | — | Bundled OpenSSL/BoringSSL/mbedTLS | Bundling TLS means owning CVE response for TLS on two app stores. Platform stacks get OS security updates and honour user/enterprise trust config. Pinning is done at the platform layer. | High |
| HTTP client | Platform, injected via NetworkPort | — | libcurl (curl licence ✅) | Follows from the TLS decision; also gets background transfer and cellular policy for free. libcurl is used for desktop/CLI builds only. | High |
| Password hashing (server) | Argon2id (libsodium) | ISC ✅ | bcrypt, scrypt, PBKDF2 | | High |
| Local LLM runtime | llama.cpp / ggml | MIT ✅ | MLC-LLM (Apache-2.0), ExecuTorch (BSD-3), MediaPipe LLM | Widest quantisation support, Metal + Vulkan + NNAPI-ish backends, smallest integration cost, permissive. **Model weights are licensed separately — see `ai-architecture.md` §8.** | Medium |
| On-device embeddings / classifiers | ONNX Runtime | MIT ✅ | TFLite (Apache-2.0), Core ML | One runtime, both platforms, with NNAPI/Core ML execution providers. | Medium |
| Cloud LLM | Provider-agnostic HTTP adapters, ≥2 at v1.0 | — | SDK per provider | Never link a vendor SDK into the core (spec §17). | High |
| Tokenizer | `tokenizers` (Rust, Apache-2.0) or a C++ BPE impl | Apache-2.0 ✅ | — | Needed for accurate context budgeting. **Spike: can we avoid the Rust dep?** | Low |
| CRDT (text) | Own RGA in C++ **or** `yrs` (Rust) | — / MIT ✅ | Automerge (MIT, Rust), OT | See `sync-architecture.md` §6. **Needs sign-off** — `yrs` is proven but introduces Rust into the mobile build. | Medium |

### 3.6 Application layer

| Concern | Choice | Licence | Rejected | Rationale | Conf. |
|---|---|---|---|---|---|
| Android UI | Kotlin + Jetpack Compose | Apache-2.0 ✅ | Views/XML, Flutter | | High |
| Android min | API 26 (Android 8.0) | — | API 21 | 16 KB page size, `ImageDecoder`, modern Keystore, better NDK baseline. **Confirm against market data (PQ-7).** | Medium |
| iOS UI | Swift + SwiftUI, UIKit for the document canvas | — | Pure UIKit, Catalyst | SwiftUI for chrome; UIKit `UIScrollView`+`CATiledLayer` for the PDF/document canvas because SwiftUI's scroll/zoom is not sufficient there. | High |
| iOS min | iOS 16 | — | iOS 15 | Swift concurrency maturity, Vision improvements. **Confirm (PQ-7).** | Medium |
| Desktop | Compose Multiplatform | Apache-2.0 ✅ | Qt (GPL/commercial ⚠️), Electron, wxWidgets, native per-OS | Reuses Kotlin UI and the existing JNI binding — one binding, two targets. **Needs sign-off; Qt's commercial licence is the alternative if fidelity matters more than cost.** | Medium |
| Web | TypeScript + React + WASM core subset | — | Full native port | WASM build excludes PDFium-heavy paths initially; web is view-mostly. | Low |
| Backend | Go 1.22+ | BSD-3 ✅ | Rust (axum), Kotlin/JVM, Node | Best operational simplicity per engineer-hour for an I/O-bound sync/API service; static binaries; good S3/Postgres ecosystem. **Needs sign-off** — Rust would let us reuse core logic, Kotlin would let us reuse the sync model. | Medium |
| Conversion workers | Go orchestrator invoking the C++ core + (gated) LibreOffice | — | — | Workers run one-shot, network-denied, CPU/mem/time-capped. | Medium |
| Observability | OpenTelemetry (traces/metrics) + structured JSON logs | Apache-2.0 ✅ | vendor SDKs | Vendor-neutral. Crash reporting is a separate decision (must not upload document content). | Medium |

## 4. Non-functional technical requirements

| ID | Requirement |
|---|---|
| NFR-1 | Core compiles clean at `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` with zero warnings. Warnings are errors in CI. |
| NFR-2 | ASan+UBSan, TSan and MSan CI configurations; the full unit+integration suite runs under ASan+UBSan on every PR. |
| NFR-3 | Every format parser has a libFuzzer target and a seed corpus checked into `tests/fixtures/`. |
| NFR-4 | Core binary (arm64, stripped, without models or language data) ≤ 25 MB. |
| NFR-5 | No allocation on the UI-critical render path after warm-up; page render uses pooled buffers. |
| NFR-6 | Deterministic builds: same source + toolchain → byte-identical artefacts (`-ffile-prefix-map`, `SOURCE_DATE_EPOCH`). |
| NFR-7 | The C ABI is additive-only within a major version; removed/changed symbols require a major bump. Enforced by an ABI-diff CI job. |
| NFR-8 | No third-party dependency is added without a `third_party/<name>/LICENSE`, a pinned commit hash, and a row in `THIRD_PARTY_NOTICES` — enforced by CI. |
| NFR-9 | 16 KB page-size compatible on Android (required for newer devices); verified in CI. |
| NFR-10 | All user-visible strings live in resource catalogues; the core emits codes, never prose. |

## 5. Known-hard sub-problems (do not underestimate these)

Called out because each is routinely mis-scoped as "a library will handle it":

1. **Spreadsheet formula evaluation.** xlnt reads formula *strings*. Evaluating them needs a
   parser, ~200 functions, a dependency graph, cycle detection, and Excel-compatible numeric
   and date semantics (including the 1900 leap-year bug). Scoped as its own project.
2. **UDM → PDF pagination.** Line breaking, hyphenation, widow/orphan control, table row
   splitting across pages, float/anchor placement, bidi. This is a typesetting engine.
3. **DOCX layout fidelity.** Reading the XML is the easy 20 %. Sections, tabs, numbering
   restarts, style inheritance, and table cell merge/spacing are the other 80 %.
4. **Redaction correctness.** Requires content-stream tokenisation and re-emission, plus
   handling text split across `TJ` arrays and Type3/embedded-font glyph mapping.
5. **Sync convergence.** Provably hard to test. Requires deterministic simulation (§2's
   `ClockPort`/`RandomPort`) and property-based tests, not example tests.
6. **On-device model lifecycle.** Download, verify, version, evict, and handle the device
   running out of storage mid-download — for OCR language data *and* LLM weights.

## 6. Phase 0 spikes required before Phase 1 commits

Time-boxed, with a pass/fail criterion. These are the "Low"/"Medium" confidence rows above.

| # | Spike | Question | Pass criterion | Box |
|---|---|---|---|---|
| SP-1 | PDFium mobile build **[REVISED — `spikes/SP-1-findings.md`]** | ~~Build via CMake without GN/depot_tools?~~ **Answered: no, and it should not be attempted.** Revised: can we build from pinned source with upstream GN in a hermetic container, **at our actual iOS deployment-target floor**, and consume it from CMake as an `IMPORTED` target? | (1) reproducible container build for android-arm64/arm + ios-arm64/sim; (2) **builds and renders at our iOS floor, not iOS 17**; (3) **stripped, GC'd contribution to the linked binary** ≤ 12 MB per arch (not `.a` size); (4) fixture corpus renders; (5) `gn desc` licence inventory checked in; (6) scripted re-roll procedure | ~~5 d~~ **8 d** |
| SP-2 | `sqlite-vec` at scale | Recall and latency for 250k × 384-dim chunks on a mid-tier phone | recall@10 ≥ 0.9 vs. exact, p95 ≤ 120 ms, index ≤ 1.5× raw vectors | 3 d |
| SP-3 | Tesseract footprint | Binary size + per-page latency + accuracy vs. platform OCR on our fixture set | ≤ 2.5 s/page A4 300 DPI, CER within 5 pts of platform OCR on printed text | 4 d |
| SP-4 | Local LLM feasibility | Can a 1–3 B quantised model run usefully in our RAM budget on the target floor device? | ≥ 8 tok/s, peak RSS ≤ 2.5 GB, no thermal throttle within 60 s | 4 d |
| SP-5 | JNI/Swift bridge cost **[REVISED — `spikes/SP-5-findings.md`]** | Overhead of moving a 4 MB page bitmap and a 10k-block tree across the boundary | ~~bitmap ≤ 1 ms~~ → **zero copies, asserted by a copy counter** (a 4 MB memcpy is already 0.4–0.8 ms, so a timing gate has no margin); tree ≤ 10 ms **and ≤ 1 crossing per screenful**; no `GetPrimitiveArrayCritical` across a render; no global-ref leaks | 3 d |
| SP-6 | **Sequence CRDT [RE-SCOPED — `spikes/SP-6-findings.md`]** | ~~Own C++ RGA vs. `yrs`?~~ **`adr/0002` closed the language question.** Revised: **which algorithm** (SP-6 found RGA's published backward-interleaving anomaly), and what constitutes proof before it carries user data? | Naive reference model + four property generators (convergence, idempotence, commutativity, **non-interleaving**); Fugue prototype in C++; differential harness proving the optimised impl byte-identical to the reference over ≥ 10⁶ ops; a memo fixing the tombstone-GC horizon rule | ~~5 d~~ **8 d** |
| SP-7 | Compose Multiplatform desktop | Does the document canvas perform acceptably on desktop? | 60 fps scroll on a 500-page PDF at 1440p | 3 d |
| SP-8 | DOCX fidelity baseline | Parse a 20-document real-world corpus; measure what fraction of content survives to UDM | ≥ 95 % of text + ≥ 80 % of structural features, with a measured, published gap list | 5 d |

**Total: ~~~32~~ ≈ 38 engineer-days** after re-scoping SP-1 (5→8 d) and SP-6 (5→8 d).
Phase 1 should not start until SP-1, SP-5 and SP-6 are done, because they constrain the core's
shape.

**Status (2026-09-29):** SP-1 and SP-5 are investigated and written up in `docs/spikes/`;
SP-1's only open item is the iOS deployment-target criterion, and SP-5 passes. **SP-6 is not
complete** — it was scoped to choose between two options, `adr/0002` removed one of them, and
it found a defect in the one that was kept. **SP-3 and SP-4 must additionally be re-run on
Tier D hardware** per `adr/0002` decision 3; their existing pass criteria were written against
Tier B and do not transfer (`performance-strategy.md` §1.2 indicates a ~1 B local model does not
fit on Tier D at all, which SP-4 must confirm or refute against real hardware).
