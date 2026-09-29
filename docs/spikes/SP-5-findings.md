# SP-5 Findings — JNI / Swift bridge cost

**Status:** Investigated (desk research + API-contract analysis). No code built.
**Date:** 2026-09-29
**Spike definition:** `technical-requirements.md` §6, row SP-5. Box: 3 d.
**Blocks:** Phase 1 entry (ABI shape). Gates Phase 10 ("bitmap rendering is copy-free") and
Phase 11 ("copy-free into a Swift-allocated bitmap context"). Risk PR-7.

---

## 1. The question as originally posed

> Overhead of moving a **4 MB page bitmap** and a **10k-block tree** across the boundary.
> **Pass:** bitmap via shared/direct buffer ≤ 1 ms; tree via FlatBuffers ≤ 10 ms.

## 2. Method

1. Worked out the arithmetic floor for the bitmap budget, to see how much headroom the
   1 ms figure actually has.
2. Audited the available zero-copy mechanisms on each platform against the NDK / Swift-C
   interop contracts, to establish which of them are *structurally* copy-free rather than
   merely fast.
3. Checked published JNI/ByteBuffer benchmark behaviour, including Android/ART-specific
   anomalies that differ from desktop JVM intuition.
4. Checked published FlatBuffers-vs-protobuf deserialisation figures, then reasoned about
   where the 10k-node cost actually lands (it is not where the benchmark measures).
5. Cross-checked the result against `adr/0002` decision 3 (low-end devices), because buffer
   pooling is exactly where a low-RAM floor bites.

## 3. Findings

### F-1 — The 1 ms bitmap budget has almost no headroom **if a copy exists at all.**

A 4 MB `memcpy` at realistic mobile memory bandwidth (single-threaded, order of
5–10 GB/s on mid-tier ARM) costs roughly **0.4–0.8 ms**. So a design that copies once
technically lands inside 1 ms — with essentially zero margin, before accounting for cache
pollution, a second copy anywhere in the chain, or a lower-end SoC.

This reframes the criterion. **"≤ 1 ms" is not a measurement target; it is a tell.** Any
implementation that meets it by copying is one refactor away from missing it. The criterion
should be *structural*: **zero copies, asserted by a counter**, not *temporal*.

### F-2 — Android: the correct path is not a buffer transfer at all.

The NDK's `AndroidBitmap_lockPixels()` returns a pointer to the `android.graphics.Bitmap`'s
own pixel buffer and guarantees the pixels do not move until `AndroidBitmap_unlockPixels()`.
Combined with PDFium's `FPDFBitmap_CreateEx(width, height, format, first_scan, stride)` —
which wraps caller-owned memory rather than allocating — this gives:

```
Kotlin:  Bitmap.createBitmap(w, h, ARGB_8888)        // allocated once, pooled
  → JNI: AndroidBitmap_lockPixels(env, bmp, &pixels)
  → C++: FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRA, pixels, stride)
         FPDF_RenderPageBitmap(...)                   // PDFium renders INTO the Java bitmap
  → JNI: AndroidBitmap_unlockPixels(env, bmp)
```

**Zero copies.** The cost is two JNI calls plus a lock — nanoseconds-to-microseconds, not
milliseconds. The 4 MB never moves. The budget is met by three orders of magnitude, and it
is met *by construction*, which is the property we actually want.

### F-3 — Direct `ByteBuffer` is the correct **second** choice, with an Android-specific trap.

`GetDirectBufferAddress` is O(1) and yields a raw pointer; after that, access cost is
memcpy-equivalent. That makes direct buffers right for **opaque native→native payloads**
(e.g. the serialised block tree, encrypted sync deltas).

But published Android benchmarks show a counter-intuitive result: **reading a
`DirectByteBuffer` from Java/Kotlin can be materially slower than reading a `ByteArray`**,
attributed to ART's JIT failing to inline `ByteBuffer` accessor methods. This is the
opposite of desktop-JVM guidance.

**Rule that falls out:** direct buffers are for handing bytes *to and from native code*,
never as a Kotlin-side hot read path. If Kotlin must iterate the bytes, that is a design
error — the iteration belongs in native code.

**Also ruled out on hot paths:** `GetByteArrayElements` (may copy, at the VM's discretion —
"may" is unacceptable in a budget) and `GetPrimitiveArrayCritical` (avoids the copy but
suspends GC; wrapping a multi-hundred-millisecond page render in a critical section would
stall the whole app). Neither may appear on the render path.

### F-4 — iOS: there is no bridge to cost. The cost is elsewhere.

Swift→C is a direct call; there is no marshalling layer. `UnsafeMutablePointer<T>` imports
as `T*` whenever the memory layout matches, and a pointer can be handed to C without
copying. So the equivalent of F-2 on iOS — render into a `CGBitmapContext`'s buffer
(`CGBitmapContextGetData`) or a locked `CVPixelBuffer` base address — is structurally free.

The real iOS boundary cost is **Swift value bridging**: `String`, `Array`, and
`Unmanaged`/ARC transitions. A `const char*` returned per item in a 10k-item loop produces
10k Swift `String` allocations and dominates everything else.

**Rule that falls out, and it is an ABI rule, not an optimisation:** the C ABI exposes
**pointer + length over a whole payload**, never per-item accessors that return strings.
This must be fixed in Phase 1 when `libuapp_capi` is designed — it is expensive to change
after two platform bindings exist.

### F-5 — The 10k-block tree: FlatBuffers is right, but the benchmark measures the wrong half.

Published figures put FlatBuffers deserialisation at ≈711 ns/op against protobuf's
≈1827 ns/op (~2.6×), and for zero-copy formats "deserialise" degenerates to pointer setup.
Those numbers confirm the choice already recorded in `technical-requirements.md` §3.1, but
they measure the cheapest part of our operation.

For 10k blocks, cost lands in two places the benchmark does not touch:

1. **Building** the FlatBuffer natively (10k nodes, strings, vectors). This is real work and
   is where the 10 ms goes.
2. **Per-node accessor calls from Kotlin/Swift.** 10 ms / 10k nodes = **1 µs per block.**
   That is comfortably achievable for build-plus-one-handoff. It is **not** achievable if
   each block costs a JNI or Swift-bridging round-trip.

**Rule that falls out:** never expose 10k nodes as 10k accessor calls. Hand over the
FlatBuffer once; the UI reads only the window it paints. This is the same conclusion
`performance-strategy.md` §3.1 reaches for lazy loading, arrived at from the bridge side —
which is a good sign the two designs agree.

**Secondary confirmation:** FlatBuffers' wire size is 30–50 % larger than protobuf. That is
a real cost *on a network*, and irrelevant *in process*. It confirms the existing split —
FlatBuffers internal/bridge, CBOR on the sync wire, JSON at the public API — rather than
arguing for one format everywhere.

### F-6 — New: `adr/0002` decision 3 turns the bitmap **pool** into the actual risk.

The bridge is free. The memory it hands around is not.

A 4 MB ARGB_8888 page bitmap × the 6-page decoded-bitmap LRU
(`performance-strategy.md` §3.5) is **24 MB of pixel buffers**, before the ~60 MB tile
cache. Against the Tier-B peak budget (450 MB RSS) that is fine. On a 2–3 GB device — now
in scope per ADR 0002 — it is not, and Android 17's per-app memory limits make exceeding it
a *termination*, not a slowdown: the system first pushes pages into zRAM (visible jank) and
then kills the process.

**Rules that fall out:**
- Bitmap pool depth and tile-cache size must be **derived at runtime** from
  `ActivityManager.isLowRamDevice()` / `getMemoryClass()` (and `os_proc_available_memory`
  on iOS), never hard-coded.
- On the low-RAM tier, prefer `RGB_565` (2 MB/page instead of 4 MB) for thumbnails and
  non-zoomed page views, and drop the decoded-bitmap LRU to 2 pages.
- These are not tuning; they are a correctness requirement for staying alive on the floor
  device. See `performance-strategy.md` §1.1.

## 4. Verdict against the original pass criterion

| Criterion | Result |
|---|---|
| Bitmap via shared/direct buffer ≤ 1 ms | **PASS, overwhelmingly** — but the criterion is the wrong shape (F-1) |
| Tree via FlatBuffers ≤ 10 ms | **PASS**, conditional on one crossing per screenful, not per node (F-5) |

**SP-5 is the lowest-risk of the three blocking spikes.** Its value is not "will it be fast
enough" — it will — but the four ABI rules it produces, which must be baked into Phase 1.

## 5. Recommendation

**Pass. Proceed, with the criteria restated as structural invariants and promoted into
Phase 1's Definition of Done rather than left as a one-off measurement.**

### Revised pass criteria

1. **Bitmap: zero copies, verified by an instrumented copy/allocation counter in the
   benchmark harness** — replacing "≤ 1 ms", which a copying implementation can pass (F-1).
   Android via `AndroidBitmap_lockPixels` + `FPDFBitmap_CreateEx`; iOS via
   `CGBitmapContextGetData` / `CVPixelBuffer`.
2. **Tree: ≤ 10 ms to build and hand over a 10k-node FlatBuffer, AND ≤ 1 boundary crossing
   per screenful of blocks** (asserted by a crossing counter).
3. **No `GetPrimitiveArrayCritical` held across any render or parse**, and no
   `GetByteArrayElements` on a documented hot path — checked by a lint rule, since this is
   the kind of thing that gets reintroduced during a refactor.
4. **No global-ref leaks**, verified at teardown under ASan (already a Phase 10 gate).

### ABI rules for Phase 1 (the actual deliverable of this spike)

- Exported C functions take **pointer + length** for bulk data. No per-item string getters.
- Caller-allocated output buffers wherever the caller can size them; the core writes into
  memory it does not own.
- Direct `ByteBuffer` / `UnsafeMutableRawPointer` for opaque payloads; never iterated from
  the managed side.
- Every buffer handed across the boundary has an explicit, documented lifetime and owner.

### Estimate impact

- **Box: 3 d, unchanged.** Reallocated: 1 d Android lock-pixels path, 0.5 d iOS, 1 d
  FlatBuffers 10k-node build-and-hand-over, 0.5 d writing the copy/crossing counters that
  become the Phase 10/11 gates.
- **Phase 1 (28 ew): unchanged.** The ABI rules are design constraints, not extra work —
  provided they are applied at design time. Retrofitting them across two bindings later
  would be expensive, which is why this spike blocks Phase 1.
- **Phase 10/11: +1–2 ew combined**, new, for runtime-derived cache sizing on the low-RAM
  tier (F-6). This is attributable to `adr/0002` decision 3, not to the bridge.
- **PR-7** ("JNI/bridge overhead on bitmap transfer", residual Low) can be closed on
  implementation of the F-2 path. **Replace it with PR-9: "bitmap pool and tile cache
  exceed the low-RAM tier budget" — residual Medium.**

## Sources

- [Native APIs — Android NDK (`bitmap.h` / `AndroidBitmap_lockPixels`)](https://developer.android.com/ndk/guides/stable_apis)
- [JNI+NDK Arrays, Bitmaps, and Games](http://jnispot.blogspot.com/2012/10/jnindk-arrays-bitmaps-and-games-oh-my.html)
- [evolvedbinary/jni-benchmarks — DataBenchmarks](https://github.com/evolvedbinary/jni-benchmarks/blob/main/DataBenchmarks.md)
- [RocksDB — Java API Performance Improvements (JNI benchmarks)](https://rocksdb.org/blog/2023/11/06/java-jni-benchmarks.html)
- [Efficient Java I/O: byte[], ByteBuffers, and OutputStreams](https://www.evanjones.ca/software/java-bytebuffers.html)
- [kotlinx-io #239 — DirectByteBuffer slower than ByteArray on Android/ART](https://github.com/Kotlin/kotlinx-io/issues/239)
- [How Swift Imports C APIs](https://github.com/apple/swift/blob/main/docs/HowSwiftImportsCAPIs.md)
- [Safely manage pointers in Swift — WWDC20](https://developer.apple.com/videos/play/wwdc2020/10167/)
- [FlatBuffers vs Protobuf (Netguru)](https://www.netguru.com/blog/flatbuffers-vs-protobufs)
- [Zero-Copy Serialization: Protobuf, FlatBuffers, and Cap'n Proto Compared](https://mohashari.github.io/zero-copy-serialization-protobuf-flatbuffers-capnproto/)
- [Preparing your app for broader memory limits — Android Developers Blog (Aug 2026)](https://android-developers.googleblog.com/2026/08/app-broader-memory-limits.html)
