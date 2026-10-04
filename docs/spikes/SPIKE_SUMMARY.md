# Spike Summary — SP-1, SP-5, SP-6, and the `adr/0002` follow-up pass

**Date:** 2026-09-29
**Status:** Research complete. **No production code written** (still pre-Phase-1, per spec §40).
**Scope:** the three blocking pre-Phase-1 spikes from `PHASE0_SUMMARY.md` §2 (S-4), plus the
architecture-doc follow-up required by `adr/0002`'s three sign-off decisions.

> **Follow-up, 2026-10-04:** SP-6's re-scoped tasks 1–3 have since been **executed with code** —
> see **`fugue-correctness-spike.md`**. Fugue and FugueMax were implemented, a reference model
> and differential harness built, and the four properties run over 5.49 M generated operations.
> SP-6's §5 recommendations R-1 (build Fugue), R-2 (four properties with a dedicated
> backward-insertion generator) and R-3 (naive reference model first) are discharged; **R-4
> (tombstone GC + causal-delivery specification) and re-scoped task 4 (encrypted framing
> sketch) are not.** The spike also found that R-3 as written is insufficient on its own — a
> reference model cannot catch a bug in operation *generation* — which added a fifth oracle to
> `sync-architecture.md` §6.2.

---

## 0. One-paragraph version

Two of the three spikes came back better than feared and one came back worse. **SP-1** (PDFium
mobile build) fails as written and passes as intended — the "no GN/depot_tools" constraint was
a proxy goal and pursuing it literally would have made our licence inventory *less* accurate;
the residual risk narrows to one thing, the iOS deployment-target floor. **SP-5** (bridge cost)
passes comfortably and its real product is four ABI rules that must land in Phase 1. **SP-6**
found that **classic RGA — the algorithm `adr/0002` named — has a published backward-interleaving
anomaly, and that the ADR's own stated mitigation (convergence, idempotence, commutativity) is
structurally blind to it**, because the replicas converge on the identical corrupted text. That
is the finding that should change a decision.

---

## 1. What each spike found

### SP-1 — PDFium mobile build → **reframe; the constraint was the wrong one**

| | |
|---|---|
| Asked | Can we build trimmed PDFium for Android/iOS arm64 **from our own CMake, without GN/depot_tools**? |
| Answer | **Build for mobile: yes, routinely. From our own CMake: no, and we should stop wanting to.** |

- PDFium has **no standalone build and no release tarball**. The only serious CMake port
  (`madebr/pdfium-cmake`) was **archived May 2024** and **never covered Android or iOS**.
- The real coupling is the **dependency graph**, not the build language: Chromium's `build/`
  tree, abseil pinned to Chromium's copy, ICU, FreeType, libjpeg-turbo, lcms, OpenJPEG,
  partition_alloc. `pdf_use_partition_alloc = false` removes the worst of it; the rest remains.
- **Mobile builds with GN are continuously proven** — `bblanchon/pdfium-binaries` ships Android
  (arm/arm64/x64/x86) and iOS (device/simulator/catalyst) weekly since 2017, currently PDFium
  156.0.8066 / chromium/8066, using exactly our intended args: `pdf_enable_v8=false`,
  `pdf_enable_xfa=false`, `is_component_build=false`, `pdf_is_complete_lib=true`,
  `pdf_use_partition_alloc=false`, `pdf_use_skia` unset (**AGG is the shipped rasteriser**).
- **The CMake port would make the §7.8 licence inventory wrong**, because it substitutes system
  packages for PDFium's vendored tree — i.e. it would describe a build we do not ship. GN gives
  a *more* accurate inventory, which inverts the usual "owning the build gives more control"
  intuition.
- **The 12 MB criterion measures the wrong artefact.** `pdf_is_complete_lib` produces one
  unstripped `.a` containing everything; the budget must be restated against the **stripped,
  GC'd contribution to the linked binary**.
- **The live risk is iOS deployment target.** The maintained recipe targets **iOS 17.0**; our
  floor was iOS 16 and `adr/0002` decision 3 pushes it lower still. This is the one thing the
  original spike did not test and the one most likely to fail.

**Recommendation:** build PDFium from **pinned source with upstream's GN/depot_tools inside a
hermetic container in our own CI**, consumed from our CMake as an `IMPORTED` target. Not a
binary vendor — we still build from source we pin. **Box 5 d → 8 d.** New standing cost:
**≈ 0.5 ew/quarter** for PDFium re-rolls, since upstream ships branch heads rather than releases.

### SP-5 — JNI / Swift bridge cost → **pass, and the criteria were the wrong shape**

| | |
|---|---|
| Asked | 4 MB bitmap ≤ 1 ms; 10k-block tree ≤ 10 ms |
| Answer | **Both pass. But "≤ 1 ms" is a tell, not a target.** |

- A 4 MB `memcpy` costs roughly **0.4–0.8 ms** on mid-tier ARM. So a design that copies *once*
  technically passes, with no margin. **The criterion must be structural — zero copies,
  asserted by a counter — not temporal.**
- **Android:** `AndroidBitmap_lockPixels` + `FPDFBitmap_CreateEx(…, pixelPtr, stride)` has
  PDFium render **directly into the Java `Bitmap`'s buffer**. Zero copies, by construction.
- **iOS:** there is no marshalling layer; Swift→C is a direct call. The real iOS cost is Swift
  **value bridging** (`String`/`Array`) — which is an **ABI design rule**, not an optimisation.
- **Android/ART gotcha, opposite to desktop-JVM intuition:** reading a `DirectByteBuffer` from
  Kotlin can be *slower* than a `ByteArray` (JIT inlining failure). Direct buffers are for
  native↔native handoff only.
- **10k-block tree:** FlatBuffers is right (~711 ns/op vs protobuf's ~1827 ns/op), but the
  benchmark measures the cheapest part. 10 ms / 10k nodes = **1 µs/block**, achievable for
  build-plus-one-handoff, **impossible if each node costs a boundary crossing**.
- **New, from `adr/0002` decision 3:** the bridge is free; the *memory it hands around* is not.
  A 6-page ARGB_8888 LRU is 24 MB of pixel buffers, which Tier D cannot afford — and under
  Android 17's per-app limits, exceeding the budget is **termination**, not slowness.

**Recommendation:** pass. Promote the criteria into Phase 1's DoD as invariants, with four ABI
rules (pointer+length for bulk data; caller-allocated outputs; direct buffers for opaque
payloads only; explicit owner and lifetime per buffer). **Box 3 d, unchanged.** PR-7 closes;
new PR-9 (cache exceeds the low-RAM ceiling) opens.

### SP-6 — the sequence CRDT → **NOT complete, and it found a real defect**

| | |
|---|---|
| Asked | Own C++ RGA vs. `yrs`? |
| Status | The *language* question is closed by `adr/0002`. **The algorithm question is open, and the ADR named the wrong algorithm.** |

**The finding, stated plainly:**

> **RGA is proved free of *forward* interleaving but exhibits *backward* interleaving**
> (Kleppmann et al., PaPoC 2019). Two users concurrently inserting at the same position, one
> building text right-to-left, get a character-by-character interleaved result.
>
> **All replicas converge — on the same garbage.** A convergence test passes. An idempotence
> test passes. A commutativity test passes. A cross-replica fuzz harness passes. The user sees
> `HWeolrllod`.

`adr/0002` decision 2's mitigation is "fuzz/property testing for convergence, idempotence,
commutativity". That is the standard three-property set, it is what most teams would write, and
**it is exactly the test set that cannot see this defect class.**

Supporting findings:

- **Fugue** (Weidner, Kleppmann et al., 2023) defines **maximal non-interleaving** and achieves
  it by disambiguating on `leftOrigin` then `rightOrigin`. Comparable complexity to RGA; adopted
  by Loro in production. **If we are writing one from scratch anyway, writing Fugue instead of
  RGA costs ≈ +1 ew and removes a published defect class.** The same work proves avoiding
  interleaving *entirely* is impossible — so the honest target is *maximal*, not zero.
- **A machine-checked SEC proof for RGA already exists** (Gomes, Kleppmann, Mulligan &
  Beresford, OOPSLA 2017; Isabelle/HOL, in the Archive of Formal Proofs). "Correctness-proven
  from scratch" therefore overstates the obligation: the *algorithm* argument is inherited; only
  **our optimisations** (run-length blocks, index caches, tombstone GC) are ours to prove.
- **The two real killers of hand-written CRDTs are not in the ADR's list**: tombstone GC
  (requires causal stability, which with indefinitely-offline devices may never be reached —
  resolved by reusing the 90-day change-log compaction horizon) and causal-delivery violations
  (must buffer, boundedly, or a hostile device gets a free memory-exhaustion primitive).
- **E2EE makes the ADR's choice more defensible, not less.** With no server-side merge, `yrs`'s
  Yjs wire compatibility buys nothing a future E2EE web client could use.

**Recommendation:** **amend `adr/0002` decision 2 from "RGA" to "a Fugue-family sequence
CRDT"** — needs product-owner confirmation, since it changes a signed-off ADR. Test **four**
properties, not three, with a **dedicated backward-insertion generator** for the fourth. Build
a naive reference model first and differentially test the optimised implementation against it.
**Box 5 d → 8 d.** CRDT line item 6–8 ew → **15–17 ew**. New **R-29** (a merge defect that
converges), P×I = M×5 = **10**.

---

## 2. What changed in the docs

| Doc | Change |
|---|---|
| `spikes/SP-1-findings.md` | New. Build approach reframed; revised pass criteria; R-12 rewritten. |
| `spikes/SP-5-findings.md` | New. Four ABI rules for Phase 1; criteria made structural; PR-7 → PR-9. |
| `spikes/SP-6-findings.md` | New. RGA interleaving defect; Fugue recommendation; four-property plan; revised effort. |
| `sync-architecture.md` §6 | Rewritten. `yrs` option closed; Fugue-vs-RGA raised; **§6.2 four-property correctness plan** (convergence, idempotence, commutativity, **non-interleaving**) with reference-model differential testing, causal-delivery buffering and 90-day-horizon tombstone GC. |
| `sync-architecture.md` §10 | Rewritten. **How a CRDT merges on a server that cannot read it**: the server stops merging and becomes an authenticated ordered relay; sealed ops with authenticated-but-cleartext AAD; snapshots as a requirement not an optimisation; enumerated list of what the server loses; §4 protocol amendments; the "no plaintext on the server" build-failing test. |
| `ai-architecture.md` §13 (new) | Three encryption boundaries; the **9-step per-document consent flow**; RAG stage-by-stage under E2EE (every stage on-device except generation); per-device re-indexing as a first-class job; one-embedding-model-for-all-tiers; cloud-offload-with-consent as the Tier D degradation path. AIQ-4 and AIQ-6 answered; AIQ-7/8 added. |
| `security-model.md` §5A (new) | Extended key hierarchy (URK → WSK → document key → per-file key); **three device-enrolment mechanisms** with the non-optional out-of-band short-code check; **rotation with multiple devices** including the offline-device case and the lazy-rotation trap; **the recovery problem with a real options table**, Signal SVR and iCloud Keychain escrow as reference points, and the v1.0 position; what E2EE does not protect. New T-309, T-310; T-303 downgraded; accepted risks 8/9/10 added. |
| `performance-strategy.md` §1.1–1.3 (new) | **Tier D (2–3 GB RAM) as a gating tier.** Full budget table; concrete OCR feasibility (`tessdata_fast` ≈ 4–5 MB vs `tessdata_best` ≈ 15–23 MB); concrete local-AI feasibility (~270 M INT4 ≈ 125–200 MB fits and is not a summariser; ~1 B INT4 ≈ 530 MB does not fit); degradation matrix. PR-9/10/11 added. |
| `file-format-support.md` §11 (new) | Tier D engine constraints; formats whose viability changes; **☁ no longer means "server-side for you" but "decrypt-and-send with consent"**, which strengthens the existing "no legacy DOC/XLS/PPT at v1.0" recommendation. §7.8 updated with the SP-1 build outcome. |
| `engineering-complexity-estimate.md` §10 (new) | Per-phase re-baseline: **E2EE +29**, **custom CRDT +13**, **Tier D +32**, spikes +1 → **1004 → ≈ 1079 ew (+7.5 %)**. Narrowed v1.0 799 → ≈ 865–870. Assumptions 3, 4, 4a, 4b overturned. **Phase 13: 76 → 93 ew and now the most dangerous phase in the project.** |
| `roadmap.md` | Phase 0 done-when updated; SP-5's ABI rules made Phase 1 entry material; key hierarchy into Phase 2 schema v1; Tier D gates in 5/6/7/10/11/19; enrolment and rotation into Phase 12; Phase 13 rewritten with the four-property and no-plaintext gates; consent flow into Phase 14; per-device indexing into Phase 15; **E2EE removed from Phase 18** (−6 ew, reviewed not built); sequencing notes rewritten. |

**Two findings worth noticing because they were not asked for:**

1. **E2EE partly pays for itself.** Phase 12 gets ~8 ew *cheaper* (no server-side merge, index,
   thumbnailing or RAG) and Phase 18 gets 6 ew cheaper. The net +29 is much less than the old
   placeholder +40 — but it lands **on the critical path** (12→13→14→15) instead of at the end,
   which is the part that actually matters for schedule.
2. **Decisions 1 and 3 interact badly, and neither one alone predicts it.** E2EE means chunks
   and vectors never sync, so **every device re-indexes the whole corpus locally** — and
   decision 3 says one of those devices has 2 GB of RAM and eMMC storage. That combination
   (new PR-10) is the heaviest background workload in the product and appears in neither ADR
   decision read on its own.

---

## 3. NEW open questions — decisions needed before Phase 1 coding

Ordered by cost-to-get-wrong-late, same convention as `PHASE0_SUMMARY.md` §2.

### Blocking Phase 1

| # | Question | Why it blocks | Recommendation |
|---|---|---|---|
| **N-1** | **Fugue or classic RGA?** `adr/0002` decision 2 names RGA; SP-6 found RGA's backward-interleaving anomaly and a convergence-test blind spot. | The merge algorithm is the hardest thing in the product to change once written, and this is a change to a signed-off ADR. ≈ +1 ew now; a rewrite later. | **Amend the ADR to a Fugue-family CRDT.** If RGA is kept, **write the accepted anomaly down explicitly** — an unrecorded known defect is the worst of both. |
| **N-2** | **What is the actual device floor?** `adr/0002` says "low-end/older" without a number. Every Tier D budget, the OCR resolution path, the local-AI cutoff and the SP-1 iOS criterion all depend on it. | Tier D budgets are build gates. A gate without a number is not a gate. | Propose **Android API 26 / 2 GB RAM; iOS 16**. Validate against target-market device data — and note that SP-1's iOS criterion cannot be run until this is fixed. |
| **N-3** | **Does a Tier D device actually exist in the benchmark rack, and when?** | A gating tier first measured in Phase 19 is not a gating tier. | Procure Tier D hardware **before Phase 5**. |

### Blocking Phase 12 (but cheap now, expensive later)

| # | Question | Why | Recommendation |
|---|---|---|---|
| **N-4** | **Is PIN + enclave escrow (Signal-SVR-style) committed for v1.1?** | If yes, **the escrow record's shape must exist in the v1.0 data model** or v1.1 is a migration across every user's keys. | Decide now even if the answer is "not in v1.0". Reserve the shape. |
| **N-5** | **Does the platform-keychain convenience copy hold the URK or a wrapped URK?** | Determines whether Apple/Google hold something that is, in effect, the key. A privacy-policy statement depends on the answer. | **Wrapped**, with the wrapping key device-bound. |
| **N-6** | **Recovery-phrase encoding and wordlist.** | One-way door — changing it later strands every phrase already issued. | BIP-39 English wordlist, 24 words. Boring on purpose. |
| **N-7** | **Is onboarding allowed to defer recovery-phrase verification?** | Product will want to; skipping it converts accepted risk 8 from rare to routine. | **No deferral.** Accept the funnel cost, and say so to whoever owns the funnel *now*. |

### Blocking Phase 14/15

| # | Question | Why | Recommendation |
|---|---|---|---|
| **N-8** | **Which embedding model runs on Tier D?** (= a harder AIQ-5) | One model across all tiers is required for cross-device consistency, so **the floor device sets a quality ceiling for every device**. | Fold into SP-2, re-run on Tier D hardware. |
| **N-9** | **Cloud-offload-with-consent for OCR on Tier D, or strictly on-device there?** (AIQ-8) | Two P0 promises collide: "works offline" vs. "works on your phone". | Default on-device and slow; offer offload only where local genuinely fails. |
| **N-10** | **Consent revocation semantics** (AIQ-7). | Revocation cannot recall sent data. If the UI implies it can, that is a false privacy claim. | Wording reviewed by whoever signs the privacy policy, not by engineering alone. |

### Operational, needed before launch

| # | Question | Why |
|---|---|---|
| **N-11** | **Who writes the support process for "I lost my device and my phrase"?** | Under E2EE the answer is "your data is gone" and there is no escalation path. Support training, canned responses and the pre-loss warning UX are a **launch blocker**, and they belong to someone outside engineering. |
| **N-12** | **Does a web client exist at v1.0 at all?** (narrowed AIQ-4) | Under E2EE it must hold keys and index in-browser. That is a different product decision now, not a port. |

---

## 4. The single biggest remaining risk

**The custom sequence CRDT (new R-29).**

SP-1's residual is narrow and testable. SP-5's residual is essentially closed. **SP-6's is
neither**: we are writing, from scratch, the component with the highest correctness risk in the
project; the algorithm the ADR named has a published defect; the test set the ADR named cannot
detect that defect; and E2EE means that when it does go wrong in production, **the server cannot
be inspected to diagnose it** — the operations are opaque ciphertext and the merge happened on
a device we cannot see.

Phase 13 is now 93 ew, Low confidence, "Very High" correctness risk, and on the critical path.
**It should be treated as the project's centre of gravity, and it should be staffed first.**

Mitigations are known and affordable (Fugue; the fourth property with its own generator; the
reference-model differential harness; GC and causal-delivery specified before the merge code is
written). **They are only affordable if N-1 is decided before anyone writes merge code.**
