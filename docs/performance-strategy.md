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
| D | Tablet | Layout + large-document check |

Benchmarks run on **reserved physical devices**, not emulators and not shared CI runners.
Emulator numbers are noise; shared-runner numbers are worse.

**Tier C policy:** features are *disabled with an explanation*, never silently slow. Local
generative AI is off on Tier C. Very large document handling degrades to a reduced cache and a
lower render scale, and says so in diagnostics.

## 2. Budget table

Budgets are **p95 unless marked p50**. A budget is a build gate, not a target.

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
| PR-7 | JNI/bridge overhead on bitmap transfer | Direct-buffer rendering, no copy (SP-5) | Low |
| PR-8 | Startup regression creep as features are added | Startup budget is a blocking gate from Phase 1, not added later | Low |

## 10. Anti-patterns

1. **Optimising before measuring.** Every optimisation lands with a benchmark showing the delta.
2. **Averages.** We gate on p95 and p99. The average hides exactly the users who are suffering.
3. **Emulator benchmarks.** Directionally wrong as often as not.
4. **Unbounded caches.**
5. **Background work that ignores thermal and battery state.** It makes the app the reason the
   phone is hot, which is how an app gets deleted.
6. **Treating memory budgets as soft.** On mobile, exceeding memory is not slowness — it is
   termination.
