# Performance Strategy

**Status:** Phase 0 draft — awaiting human sign-off
**Version:** 0.0.0
**Date:** 2026-09-29

Implements spec §27. Principle: a budget that is not measured on a fixed device is not a
budget. Everything below has a number, a reference device, and an automated benchmark.

---

## 1. Reference devices

Budgets are meaningless without naming the hardware. All numbers below are for **Tier B**
unless stated.

| Tier | Device class | Role |
|---|---|---|
| A | Current flagship (iPhone latest, Pixel latest) | Headroom check |
| **B** | **Mid-tier, ~3 years old: 6–8 GB RAM, mid SoC** | **The device budgets are written against** |
| C | Low-end: 4 GB RAM, entry SoC, eMMC storage | Degradation check; some features disabled |
| **D (new)** | **Floor: 2–3 GB RAM, entry SoC, Android Go-class, eMMC** | **Hard constraint per `adr/0002` decision 3. The app must be usable, not merely installable.** |
| E | Tablet | Layout + large-document check |

Benchmarks run on **reserved physical devices**, not emulators and not shared CI runners.
Emulator numbers are noise; shared-runner numbers are worse.

**Tier C policy:** features are *disabled with an explanation*, never silently slow. Local
generative AI is off on Tier C. Very large document handling degrades to a reduced cache and a
lower render scale, and says so in diagnostics.

## 1.1 Tier D — the low-end floor (`adr/0002` decision 3)

**`adr/0002` decision 3 makes low-end/older device support a hard constraint, not an
optimisation pass.** Tier D is therefore a *gating* tier: a build that fails Tier D budgets
fails, in the same way a build that fails Tier B budgets fails. It is not a "degradation
check" — that is Tier C's job.

### Why the memory ceiling is a hard wall, not a soft one

On a 2–3 GB device the app does not get slow when it exceeds its budget; it gets **killed**.
Android 17 introduced per-app memory limits that make this explicit and staged: an app
reaching its limit has pages forced into **zRAM** (visible UI jank), and an app that keeps
growing is **terminated by the system**. Manufacturers are applying these across
configurations from 4 GB upward, and the low-RAM classes were already governed by LMK.

Android Go's minimum was raised to **2 GB RAM / 16 GB storage** (from Android 13), which sets
the realistic floor for a device we would claim to support. Per-app **Java heap** limits on
such devices are commonly **96–192 MB** (device-dependent; read it, do not assume it) — but
our core is native, so the binding constraint is **total process RSS**, not heap.

### Tier D budgets

Derived from Tier B by the constraint, not by scaling. Where a Tier B budget cannot be met at
all on Tier D, the row says what happens instead.

| Metric | Tier B | **Tier D** | Notes |
|---|---|---|---|
| Memory, idle foreground | ≤ 180 MB RSS | **≤ 110 MB RSS** | |
| Memory, peak during a heavy op | ≤ 450 MB RSS | **≤ 220 MB RSS** | Hard ceiling. Exceeding it is termination, not slowness. |
| Install size (base, no models) | ≤ 120 MB | **≤ 120 MB, and all optional assets downloadable** | Storage is 16–32 GB and mostly full. Nothing large is bundled that is not used on first run. |
| Cold start → interactive | 1.5 s p50 | **3.0 s p50 / 5.0 s p95** | eMMC, not UFS. I/O-bound, not CPU-bound. |
| Decoded page bitmap LRU | 6 pages @ ARGB_8888 (~24 MB) | **2 pages, `RGB_565` where the surface allows (~4 MB)** | Sized at runtime from `ActivityManager.isLowRamDevice()` / `getMemoryClass()`, never hard-coded (`spikes/SP-5-findings.md` F-6). |
| Rendered tile cache | ~60 MB | **≤ 16 MB** | |
| SQLite `mmap_size` | 256 MB | **32 MB** | |
| Conversion arena cap | 256 MB | **96 MB** | Conversions above it fail with a specific, actionable error — never an OOM kill. |
| Peak RSS browsing a 500-page PDF | 250 MB | **≤ 180 MB**, render scale capped at 1× | |
| Peak RSS during OCR | 300 MB | **≤ 150 MB**, one page at a time, never parallel | |

## 1.2 What is feasible on Tier D — OCR and local AI, concretely

This is the section `adr/0002` decision 3 actually forces, because "support low-end devices"
and "run local AI" are in direct tension and the tension has to be resolved with numbers.

**OCR — feasible, with the right model file.**

| Option | Model size | Verdict on Tier D |
|---|---|---|
| Platform OCR (ML Kit, Apple Vision) | ML Kit bundled ≈ **4 MB per script**; unbundled ≈ **260 KB per script** + Play Services download | **The default fast path.** Unbundled keeps the APK small; requires Play Services (see S-15/MQ-4 — this is now a Tier D question, not only a market-coverage one). |
| Tesseract + `tessdata_fast` | **≈ 4–5 MB** for the full fast set; `eng` alone is ~2 MB | **The portable fallback. Ship `tessdata_fast`, never `tessdata_best`.** The fast variant uses a smaller network and integerised weights, so it is both smaller and faster. |
| Tesseract + `tessdata_best` | **≈ 15–23 MB** | **Prohibited on Tier D**, and questionable anywhere on mobile. Also note it targets the Tesseract 4 LSTM engine, not 5. |

> **2026-10-20, `ocr/engine` bootstrap (`spikes/SP-10-findings.md`).** The first implementation
> and its tests run against this dev host's full `tessdata` package (Homebrew default), not
> `tessdata_fast` — real recognition was verified correct (a 300x80 test image reads back as
> its known ground truth, confidence > 50/100), but the model-size and Tier D timing numbers
> above are **not yet re-measured against this actual build**. Vendoring and measuring against
> `tessdata_fast` specifically remains owed before this leaves bootstrap status.

**Tier D OCR policy:** one page at a time, never parallel across pages (Tier B parallelises);
downscale to 200 DPI rather than 300 DPI before recognition where the source permits;
per-page latency budget relaxed to **6 s** (Tesseract) / **1.5 s** (platform), with visible
progress and cancellation. OCR remains an explicit, skippable, resumable stage (J-8) — which
is what makes the slower budget tolerable rather than infuriating.

**Local generative AI — mostly infeasible, and that must be said rather than engineered around.**

Against a **≤ 220 MB peak RSS** app budget on a 2–3 GB device, the numbers are unambiguous:

| Model class | Realistic footprint | Verdict on Tier D |
|---|---|---|
| ~270 M params, INT4 | **≈ 125–200 MB** weights | **The only generative class that could fit** — and it consumes the entire app memory budget on its own. Useful for classification and short structured extraction; **not** useful for document summarisation. |
| ~1 B params, INT4 | **≈ 530 MB+** | **Does not fit.** This is the class that is genuinely useful, and Tier D cannot run it. |
| 1–3 B (the §6 plan for Tier B) | 0.8–2 GB | Not remotely. |
| Embeddings, 384-dim int8 | **≈ 30–90 MB** | **Fits, and works well.** |

Practical notes from field reports on low-RAM Android: `mmap` of model weights can fail
outright, forcing `--no-mmap`, which then costs full RSS rather than page-cached pages. Plan
for the worse case.

**Tier D AI policy — this is the honest split:**

1. **Embeddings and classification run locally.** Semantic search fully on-device is
   realistic on Tier D. This is the local capability that actually works, and it is the one
   `ai-architecture.md` §6 already identified as the strong one.
2. **Local generation is OFF on Tier D.** Not slow — off, with a named reason. A 270 M model
   may be offered later for narrow structured tasks; it is not a summariser and must not be
   presented as one.
3. **Generation on Tier D is cloud, through the explicit per-document consent flow**
   (`ai-architecture.md` §13.2, §13.5) — **or it does not happen.** `LOCAL_ONLY` still fails
   closed with `AI_NO_LOCAL_MODEL` (H-29). A device being small is not a reason to weaken a
   policy the user set.
4. **One embedding model across all tiers, chosen to fit Tier D** (`ai-architecture.md` §13.4).
   Different models per tier would put a user's own devices in different vector spaces.

## 1.3 Tier D degradation matrix

Every row is **announced in-product with a reason**, and visible in diagnostics. Silent
degradation is prohibited (§10 anti-patterns).

| Feature | Tier D behaviour |
|---|---|
| Local generative AI | **Disabled.** Reason shown. Cloud-with-consent offered. |
| Local embeddings / semantic search | **Full.** |
| Initial corpus indexing (now per-device under E2EE — `ai-architecture.md` §13.3) | Runs, but **heavily throttled**: pauses on battery-saver, thermal `serious`, and foreground use. Honest, resumable progress. Search is explicitly labelled incomplete until done. |
| PDF render scale | Capped at 1×; progressive tiles; zoom re-renders rather than caching more |
| PDF page cache | 2 pages |
| Conversion | 96 MB arena; large conversions fail with a specific error and a suggestion, never an OOM |
| OCR | Serial, 200 DPI, relaxed budget |
| Scanner capture resolution | Capped (e.g. 8 MP rather than 12 MP) to keep the perspective-correction working set inside the arena |
| Very large documents (> 200 pages, > 50 MB) | Open and read, but editing features that need the whole model in memory are disabled with a reason |

## 2. Budget table

Budgets are **p95 unless marked p50**. A budget is a build gate, not a target.

> **Tier D overrides.** The tables below are Tier B. Where §1.1 gives a Tier D figure, that
> figure is **also a build gate** (`adr/0002` decision 3), not a best-effort target. The
> regression gate in §7 runs against both tiers, and a memory-budget breach on either fails
> the build — memory has no "slightly over" on a 2 GB device.

### 2.1 Application

| Metric | Budget | Notes |
|---|---|---|
| Cold start → interactive | 1.5 s p50 / 2.5 s p95 | Measured to first meaningful paint of usable content, not to `Activity.onResume` |
| Warm start | 400 ms | |
| Navigation between top-level screens | 150 ms | |
| Frame rendering | ≤ 8 ms p95 at 120 Hz; zero frames > 32 ms during scroll | Jank, not average FPS, is what users feel |
| Memory, idle foreground | ≤ 180 MB RSS | |
| Memory, peak during a heavy op | ≤ 450 MB RSS | Above this, Android LMK and iOS jetsam start killing |
| Install size (base, no models) | ≤ 120 MB | |
| Battery: 1 h active editing | ≤ 6 % on a 4000 mAh device | |
| Battery: background sync per day | ≤ 1 % | |

### 2.2 Storage and search

| Operation | Budget |
|---|---|
| Open DB + migrate check (no pending migration) | 80 ms |
| Load a 500-block document tree | 120 ms |
| Insert a block (commit to durable) | 12 ms |
| FTS query over 100k blocks | 150 ms |
| FTS query over 1M blocks | 400 ms |
| Vector search, 250k chunks, top-10 | 120 ms |
| Hybrid search (FTS + vector + RRF), 100k chunks | 250 ms |
| Incremental index of one edited block | 8 ms |
| Full reindex, 100k blocks | 90 s (background, checkpointed) |
| Database view: 10k rows × 20 properties, filtered + sorted | 100 ms |

### 2.3 PDF

| Operation | Budget |
|---|---|
| Open a 500-page / 100 MB PDF, first page painted | 1.2 s |
| Render an A4 text page at 2× | 60 ms |
| Render a vector-heavy page (map) at 2× | 400 ms (progressive tiles, cancellable) |
| Thumbnail generation, per page | 25 ms |
| Text search across 500 pages | 800 ms (streamed, first hits ≤ 150 ms) |
| Merge 10 × 10-page PDFs | 900 ms |
| Split a 500-page PDF | 1.5 s |
| Compress a 50 MB scanned PDF | 12 s |
| Encrypt/decrypt (AES-256) a 50 MB PDF | 3 s |
| Annotation commit | 100 ms |
| Peak RSS browsing a 500-page PDF | 250 MB |

### 2.4 Scanner and OCR

| Operation | Budget |
|---|---|
| Camera preview + live quad detection | ≥ 15 fps detection, 30 fps preview |
| Quad detection per analysis frame (640 px) | 30 ms |
| Perspective correction + enhancement, 12 MP | 600 ms |
| Tesseract OCR, A4 300 DPI, English | 2.5 s |
| Platform OCR (Vision / ML Kit), same page | 600 ms |
| Searchable-PDF assembly, per page | 200 ms |
| Batch: 20 pages scan → OCR → PDF | 60 s (background, checkpointed) |
| Peak RSS during OCR | 300 MB |

### 2.5 Conversion

| Operation | Budget |
|---|---|
| Image → image, 12 MP | 400 ms |
| Image → PDF, 10 images | 900 ms |
| Markdown → PDF, 20 pages | 1.2 s |
| DOCX → UDM, 40 pages | 1.5 s |
| UDM → PDF, 40 pages | 2.5 s |
| XLSX → SSM, 100k cells | 2 s |
| CSV → SSM, 1M rows | 6 s (streamed) |
| Peak RSS for any single conversion | 256 MB (hard arena cap) |

### 2.6 Sync and AI

| Operation | Budget |
|---|---|
| Sync: push 100 mutations | 800 ms (network-dependent; measured server-side + RTT) |
| Sync: apply 1000 pulled changes | 2 s |
| Sync: convergence after 1000 queued ops | 30 s |
| Blob upload throughput | ≥ 80 % of available bandwidth |
| Embedding, one 400-token chunk (local, ONNX) | 40 ms |
| Embedding, batch of 64 | 1.5 s |
| Local LLM, first token | 1.5 s |
| Local LLM, sustained | ≥ 8 tok/s (Tier B), ≥ 15 tok/s (Tier A) |
| RAG retrieval + context assembly | 400 ms |

## 3. Techniques, mapped to where they are load-bearing

Spec §27 lists lazy loading, streaming, memory mapping, worker threads, background processing,
caching, incremental indexing. Each is applied where it earns its complexity — not everywhere.

### 3.1 Lazy loading
- Documents load the visible block window plus a margin, not the whole tree. A 50k-block
  document must open as fast as a 50-block one.
- Heavy subsystems (OCR, AI, conversion) initialise on first use, not at startup.
- Database views materialise the visible page of rows.

### 3.2 Streaming
- CSV, TXT, XML/OOXML sheet parts, TIFF pages, and PDF pages are processed incrementally.
- Conversions write output as they go; nothing buffers a whole output file.
- Search results stream to the UI so the first hits appear before the query finishes.
- Blob upload/download is chunked and resumable.

### 3.3 Memory mapping
- SQLite `mmap_size = 256 MB` (reduced on Tier C).
- Large read-only blobs (PDF bytes) are mapped rather than read, so the OS page cache does the
  work and pages can be evicted under pressure instead of being killed for.
- **Not used for encrypted blobs** — decryption defeats mapping. Those are read in 1 MiB AEAD
  chunks (`security-model.md` §5), which is why that chunk size exists.

### 3.4 Concurrency
- Core worker pool: `min(4, hardware_concurrency)` compute threads + a separate single I/O
  thread. Oversubscribing on mobile causes thermal throttling and worse throughput.
- One SQLite writer, serialised in the storage adapter.
- Page rendering is parallel across tiles; off-screen tiles are cancelled the moment they leave
  the prefetch window.
- **No work on the UI thread, enforced by assertion** (`mobile-architecture.md` §3.2).

### 3.5 Caching
| Cache | Policy |
|---|---|
| Rendered page tiles | LRU, memory-bounded (~60 MB), keyed by (doc, page, scale, rotation) |
| Decoded page bitmaps | LRU, ~6 pages |
| Thumbnails | Disk, LRU, generated lazily |
| Parsed document models | Weak reference + LRU, 3 documents |
| Search results | Query-hash keyed, 30 s TTL, invalidated by any write to the scope |
| Embeddings | Permanent, keyed by `(content_hash, model_id)` |
| Text extraction | Persistent in `file_text`, invalidated by `source_rev` |

Every cache has an explicit eviction policy and a memory bound. An unbounded cache is a leak
with a friendly name.

### 3.6 Incremental indexing
- Editing one block reindexes that block, never the document.
- Chunk `content_hash` means an unchanged chunk is never re-embedded — this is the difference
  between re-indexing a 200-page PDF in 200 ms and in 4 minutes.
- Bulk indexing runs in background jobs with checkpoints, yields to foreground work, and pauses
  on low battery or high thermal state.

### 3.7 Allocation discipline
- Pooled buffers for page bitmaps and image tiles; reused across pages.
- Arena allocation per parse/conversion job, with a hard cap — this is both a performance and a
  security mechanism (T-102).
- **Zero allocation on the scroll/render path after warm-up** (NFR-5), verified by an allocation
  counter in the benchmark harness.
- Small-buffer optimisation and `reserve()` in hot loops; no incidental `std::string`
  construction in parser inner loops.

## 4. Thermal and power

Sustained CPU work on a phone is self-defeating — the SoC throttles and total throughput drops.

- Long jobs poll thermal state (`ProcessInfo.thermalState`,
  `PowerManager.getCurrentThermalStatus`) and **voluntarily reduce concurrency** at `serious`,
  and pause at `critical`, resuming when it clears. Yielding early beats being throttled later.
- Low-power / battery-saver mode: defer indexing, embedding, and prefetch entirely.
- Sync is coalesced and opportunistic. There are no polling timers anywhere in the app.
- Local LLM inference is only offered when thermal headroom allows, and reports honestly when
  it is unavailable for that reason.

## 5. Startup optimisation

Ordered work, measured at each step (see `offline-architecture.md` §8 for the offline
constraint):

1. Open the DB (SQLCipher KDF is the main cost — `kdf_iter` is tuned so that unlock is ≤ 120 ms
   on Tier B while remaining strong; this trade is deliberate and documented).
2. Migration check: a version comparison, not a scan.
3. Render the last screen from local data.
4. Everything else — connectivity probe, sync, index catch-up, subsystem init — is deferred and
   asynchronous.

Baseline profiles (Android) and order-file / `DYLD` prewarm optimisations (iOS) are applied once
the code is stable, not early.

## 6. Benchmark harness

- **Micro:** Google Benchmark in `benchmarks/`, one file per subsystem. Run on Linux CI for
  algorithmic regressions (cheap, low variance) and on device for the numbers that matter.
- **Macro:** device benchmarks driving real journeys through the binding layer, on reserved
  hardware.
- **Recorded metrics:** wall time (p50/p95/p99), peak RSS, allocation count, bytes read/written,
  and — on device — energy where the platform exposes it.
- **Storage:** results land in a time-series store; every run is tagged with commit, device, OS
  version and thermal state at start.

## 7. Regression gate

```
if p95(current) > 1.10 × p95(rolling 7-day median)  for a P0 benchmark
    → fail the build, name the benchmark and the delta
if p95(current) > 1.25 × median                      for any benchmark
    → fail the build
if peak RSS > budget                                 for any benchmark
    → fail the build (memory has no "slightly over")
```

Thermal state is recorded; a run that started above `nominal` is discarded and retried rather
than reported, because a throttled run produces a false regression and trains people to ignore
the gate.

## 8. Production observability

Field performance differs from lab performance. We measure, without collecting content
(`security-model.md` §10):

- Cold/warm start time, by device class and OS version
- Frame-drop rate during document scroll
- p50/p95 for: PDF open, page render, search, OCR per page, conversion per MB, sync round-trip
- Peak memory and OOM-kill rate
- Job failure rates by error code
- Battery drain attribution where the platform reports it

Alert thresholds mirror the budgets. A regression that ships is caught within a day, not a
release cycle.

## 9. Known performance risks

| # | Risk | Mitigation | Residual |
|---|---|---|---|
| PR-1 | Tesseract at 2.5 s/page may be optimistic on Tier B/C | Platform OCR as the default fast path; Tesseract as the fallback; parallelise across pages; validate in SP-3 | Medium |
| PR-2 | Local LLM may be unusable on Tier B | SP-4 decides; if it fails, local = embeddings only, stated honestly | Medium |
| PR-3 | EAV database views may not meet 100 ms at 20 properties | Compile filters to reference-only joins; materialised view cache; pivot-table escape hatch (`database-design.md` §3.4) | Medium |
| PR-4 | SQLCipher adds 5–15 % to every read | Measured, accepted; `kdf_iter` tuned for unlock latency | Low |
| PR-5 | Vector index memory at 400k chunks (600 MB f32) | int8 quantisation (→150 MB) mandatory above a threshold; validated in SP-2 | Medium |
| PR-6 | PDFium render on complex vector pages can blow the budget | Tiled, progressive, cancellable rendering; a per-page time cap after which we show what we have | Low |
| PR-7 | ~~JNI/bridge overhead on bitmap transfer~~ | **Closed by SP-5.** `AndroidBitmap_lockPixels` + `FPDFBitmap_CreateEx` is structurally zero-copy; iOS Swift→C has no marshalling layer at all (`spikes/SP-5-findings.md` F-2, F-4). Gate is now a copy-counter assertion, not a timing. | **Closed** |
| PR-8 | Startup regression creep as features are added | Startup budget is a blocking gate from Phase 1, not added later | Low |
| **PR-9** | **Bitmap pool + tile cache exceed the Tier D memory ceiling → process termination, not slowness** | Cache depths derived at runtime from `isLowRamDevice()`/`getMemoryClass()`; `RGB_565` on Tier D; §1.1 budgets are build gates | **Medium** |
| **PR-10** | **Per-device corpus re-indexing (now mandatory under E2EE — `ai-architecture.md` §13.3) is the heaviest background job on Tier D and may never complete** | Checkpointed, resumable, thermally-aware, battery-aware; honest incomplete-index state in the search UI; prioritise recently-opened documents so the index is *useful* long before it is *complete* | **Medium-High.** New, and a direct consequence of `adr/0002` decisions 1 + 3 interacting. |
| **PR-11** | **Tier D cannot run any useful local generative model** (§1.2) | Accepted and designed for: local = embeddings + classification; generation is cloud-with-consent or absent. Stated in-product. | **Accepted, not mitigated.** |

## 10. Anti-patterns

1. **Optimising before measuring.** Every optimisation lands with a benchmark showing the delta.
2. **Averages.** We gate on p95 and p99. The average hides exactly the users who are suffering.
3. **Emulator benchmarks.** Directionally wrong as often as not.
4. **Unbounded caches.**
5. **Background work that ignores thermal and battery state.** It makes the app the reason the
   phone is hot, which is how an app gets deleted.
6. **Treating memory budgets as soft.** On mobile, exceeding memory is not slowness — it is
   termination.
