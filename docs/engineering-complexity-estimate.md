# Engineering Complexity & Effort Estimate

**Status:** Phase 0 draft — awaiting human sign-off
**Version:** 0.0.0
**Date:** 2026-09-29

Implements spec §40 item 25.

> **What this is:** a planning estimate produced during a feasibility study, before any code
> exists. Its purpose is to make the scale of the project visible and to identify where the
> cost concentrates — not to commit to dates.
>
> **What this is not:** a schedule. Estimates at this stage are routinely wrong by a factor of
> two in either direction. The right response to a number you dislike here is to cut scope, not
> to compress the estimate.

---

## 1. Basis and method

Estimates are bottom-up per phase, expressed as **engineer-weeks (ew)** for a single competent
engineer working on that area, then reconciled against a team model.

**Anchors used for calibration** (public, approximate, from comparable work):
- A production PDF viewer/editor with annotations, forms and encryption: 2–4 engineer-years.
- A block editor with nesting, undo/redo and structured databases: 2–3 engineer-years.
- A production offline-first sync engine with conflict resolution: 1.5–3 engineer-years.
- An OOXML reader with acceptable fidelity: 1.5–3 engineer-years *per format family*.
- Cross-platform native shells over a shared core: 1–1.5 engineer-years per platform.

**What is included:** design, implementation, unit + integration tests, fuzzing setup,
documentation, code review, and the phase's Definition of Done.

**What is not included:** product management, design, QA headcount beyond engineer-run tests,
DevOps beyond the pipeline itself, support, or slack for vacation and attrition.

**Confidence:** H(igh) ±25 % · M(edium) ±50 % · L(ow) ±100 % or worse.

## 2. Phase-by-phase estimate

> **RE-BASELINE (2026-09-29), after `adr/0002` and the SP-1/SP-5/SP-6 findings.** The table
> below is the **original** Phase 0 estimate. It assumed (assumption 4) that E2EE was *not*
> in v1.0, said nothing about a custom CRDT, and was written against a Tier B device floor.
> All three assumptions have been overturned by product-owner sign-off. **§10 restates the
> affected phases and the new total. Read §10 with this table, not instead of it.**

| Phase | Name | Low (ew) | **Likely (ew)** | High (ew) | Conf. | Primary complexity driver |
|---|---|---:|---:|---:|---|---|
| 0 | Architecture & feasibility | 16 | **24** | 32 | H | 8 spikes (~6.5 ew) + documentation + sign-off cycles |
| 1 | C/C++ core foundation | 20 | **28** | 40 | M | Six-preset build matrix; job system; ABI + tooling from day one |
| 2 | SQLite / local storage | 18 | **26** | 38 | M | Migration framework, crash consistency, chunked AEAD blob store |
| 3 | Document / block engine | 32 | **44** | 65 | M | Block model + **user-defined databases with a filter/sort/group evaluator** — the database engine is roughly half this phase |
| 4 | File detection & abstraction | 16 | **22** | 32 | H | Detection precedence, archive safety, metadata namespaces |
| 5 | **PDF engine** | 55 | **78** | 120 | **L** | PDFium+QPDF integration, annotations, forms, encryption, **redaction done correctly**, plus 72 h-clean fuzzing |
| 6 | Scanner | 24 | **34** | 50 | M | CV tuning against real-world degraded input is empirical, not analytical |
| 7 | OCR | 32 | **44** | 65 | M | Three providers behind one port; layout analysis; language-pack lifecycle; table recovery |
| 8 | **Conversion engine** | 70 | **100** | 160 | **L** | Three IRs, ~10 readers, ~8 writers, **and a text layout engine for UDM→PDF**. See §4. |
| 9 | Search | 20 | **28** | 42 | M | ICU tokenizer for CJK; index/content transactional consistency; fuzzy layer |
| 10 | Android app | 55 | **72** | 105 | M | Full UI surface + JNI binding + platform integration |
| 11 | iOS app | 55 | **72** | 105 | M | Same, minus shared design work, plus the ObjC++ bridge and the UIKit canvas |
| 12 | Cloud backend | 45 | **62** | 90 | M | Six services, auth with rotation/reuse detection, RLS, resumable upload, GDPR paths |
| 13 | **Synchronisation** | 55 | **76** | 120 | **L** | Correctness is the deliverable. The simulation harness alone is ~12 ew. |
| 14 | AI | 38 | **52** | 78 | M | Provider abstraction, policy gate, router, model lifecycle, eval harness |
| 15 | RAG | 30 | **42** | 62 | M | Chunking with provenance, hybrid retrieval, **citation verification**, eval sets |
| 16 | Agent framework | 30 | **42** | 62 | M | Permissions, confirmation flows, transcripts, resumability, injection red-teaming |
| 17 | Collaboration | 38 | **54** | 80 | L | Presence transport is new; real-time editing on top of the CRDT foundation |
| 18 | Security hardening | 30 | **42** | 62 | M | External pentest + remediation; **+40 ew if E2EE is in scope** |
| 19 | Performance optimisation | 24 | **32** | 48 | M | Device-matrix profiling and targeted work |
| 20 | Production release | 22 | **30** | 45 | M | Store submissions, staged rollout, on-call, legal artefacts |
| | **TOTAL** | **725** | **1004** | **1501** | | |

**Likely total ≈ 1004 engineer-weeks ≈ 231 engineer-months ≈ 19.3 engineer-years.**

## 3. Team model

Raw effort does not divide cleanly by headcount. Brooks's observation applies, and this project
has long serial dependencies (0→1→2→3→4→5→8 is a chain).

| Team size | Naive duration | **Realistic** (coordination + serialisation) | Note |
|---|---:|---:|---|
| 3 | 6.4 y | **7.5–8.5 y** | Not viable. |
| 5 | 3.9 y | **4.5–5.0 y** | Viable for a narrowed v1.0 (see §6). |
| 8 | 2.4 y | **3.0–3.5 y** | Reasonable if specialties are covered. |
| 12 | 1.6 y | **2.5–3.0 y** | Coordination overhead and the serial spine start to dominate. |
| 20 | 1.0 y | **2.3–2.8 y** | Diminishing returns are severe. Not recommended. |

**Recommended: 8–10 engineers**, structured as:

| Sub-team | Size | Owns |
|---|---|---|
| Core & storage | 2 | Phases 1, 2, 3, 4, 9 |
| Documents & formats | 2–3 | Phases 5, 8 (the two largest) |
| Imaging & OCR | 1–2 | Phases 6, 7 |
| Mobile | 2–3 | Phases 10, 11 |
| Backend & sync | 2 | Phases 12, 13 |
| AI | 1–2 | Phases 14, 15, 16 |

Plus, **not counted above**: 1 product manager, 1 designer, 1 QA lead, 0.5 DevOps, and access
to legal counsel. Security is a shared responsibility with an external review at Phase 18.

## 4. Where the cost concentrates

```
Phase 8  Conversion     ████████████████████  100 ew   10.0 %
Phase 5  PDF            ███████████████       78 ew    7.8 %
Phase 13 Sync           ███████████████       76 ew    7.6 %
Phase 10 Android        ██████████████        72 ew    7.2 %
Phase 11 iOS            ██████████████        72 ew    7.2 %
Phase 12 Backend        ████████████          62 ew    6.2 %
Phase 17 Collaboration  ██████████            54 ew    5.4 %
Phase 14 AI             ██████████            52 ew    5.2 %
Phase 3  Document       ████████              44 ew    4.4 %
Phase 7  OCR            ████████              44 ew    4.4 %
(remaining 10 phases)                        350 ew   34.9 %
```

**Phases 5, 8 and 13 together are ~25 % of total effort and carry all three Low-confidence
ratings.** They are also the three with the most severe failure modes (security, user-visible
quality, data loss). Any plan that does not concentrate its best people and its schedule
contingency on these three is mis-allocated.

### Why Phase 8 is the largest

It is routinely underestimated because "read DOCX, write PDF" sounds like two libraries. It is
not:

| Component | ew |
|---|---:|
| UDM/SSM/IMM design + test infrastructure | 10 |
| DOCX reader (styles, numbering, tables, sections, images, footnotes) | 20 |
| **UDM→PDF text layout engine** (line breaking, hyphenation, pagination, widows/orphans, table splitting, bidi) | **22** |
| XLSX reader/writer + SSM | 14 |
| HTML/MD/RTF/ODT/EPUB readers + writers | 16 |
| Planner, capability graph, fidelity reporting, verification | 10 |
| Fuzzing, corpus, golden tests | 8 |

The text layout engine alone is a typesetting project. There is no permissively-licensed
embeddable one that fits our IR.

## 5. Complexity ratings by dimension

| Phase | Algorithmic | Integration | Correctness risk | Unknowns | Overall |
|---|---|---|---|---|---|
| 1 Core | Low | Med | Med | Low | **Medium** |
| 2 Storage | Low | Med | **High** | Low | **Medium-High** |
| 3 Document | Med | Low | Med | Low | **Medium** |
| 5 PDF | **High** | **High** | **High** | **High** | **Very High** |
| 6 Scanner | Med | Med | Low | Med | **Medium** |
| 7 OCR | Med | **High** | Med | Med | **Medium-High** |
| 8 Conversion | **High** | **High** | Med | **High** | **Very High** |
| 10/11 Mobile | Low | **High** | Med | Med | **Medium-High** |
| 12 Backend | Low | Med | Med | Low | **Medium** |
| 13 Sync | **High** | Med | **Very High** | Med | **Very High** |
| 14/15 AI/RAG | Med | **High** | Med | **High** | **High** |
| 16 Agents | Med | Med | **High** | **High** | **High** |
| 17 Collaboration | **High** | Med | **High** | Med | **High** |

"Unknowns" is High for PDF, conversion, AI and agents because we have not yet built against
real corpora or real devices. The spikes in `technical-requirements.md` §6 exist to convert
some of that into Medium before Phase 1 commits.

## 6. A narrower v1.0 — the recommended alternative

If 4–5 years to v1.0 is not acceptable, the answer is scope, not compression. The
**scan → OCR → searchable PDF → search → grounded AI Q&A** spine is a coherent product on its
own and covers two of four personas.

| Included | Phases | ew |
|---|---|---:|
| Core, storage, document (**without** user-defined databases) | 1, 2, 3(reduced) | 84 |
| File detection | 4 | 22 |
| PDF (**without** forms and digital signatures) | 5(reduced) | 62 |
| Scanner | 6 | 34 |
| OCR | 7 | 44 |
| Conversion (**images + MD/HTML/TXT + DOCX→PDF only**) | 8(reduced) | 45 |
| Search | 9 | 28 |
| Android + iOS | 10, 11 | 144 |
| Backend + sync | 12, 13 | 138 |
| AI + RAG (**no agents**) | 14, 15 | 94 |
| Security, performance, release | 18, 19, 20 | 104 |
| **Total** | | **799 ew ≈ 18.4 engineer-months × … ≈ 15.4 engineer-years** |

At 8 engineers: **~2.4–2.8 years**. Deferred to post-1.0: user-defined databases, XLSX/PPTX,
EPUB/MOBI, agents, collaboration, desktop, web.

**Note how little this saves.** That is the real finding of this estimate: the expensive parts
(PDF, sync, two mobile apps, backend) are load-bearing for *any* version of this product. The
cheap cuts are the ones that sound big in a feature list.

## 7. Assumptions

1. Experienced engineers: ≥ 5 years each, with genuine C++ depth on the core team.
2. Specialty knowledge (PDF internals, CRDTs, mobile native, ML integration) is present or
   hired **before** the relevant phase (risk R-21).
3. ~~The spikes complete successfully.~~ **Partly settled.** SP-1 and SP-5 are investigated
   (`spikes/`). SP-1 passes *as intended* but fails as originally written; the residual risk is
   narrowed to the iOS deployment-target floor. SP-5 passes comfortably. **SP-6 is not
   complete and found a defect in the chosen algorithm — see §10.**
4. ~~E2EE is **not** in v1.0.~~ **Overturned by `adr/0002` decision 1.** E2EE ships at v1.0.
   The old "+~40 ew" figure was a guess made while the answer was still open; §10 replaces it
   with a per-phase breakdown.
4a. ~~The device floor is Tier B.~~ **Overturned by `adr/0002` decision 3.** Tier D (2–3 GB
   RAM) is a gating tier. See §10.
4b. ~~The text CRDT is `yrs`.~~ **Overturned by `adr/0002` decision 2.** A custom C++
   sequence CRDT is built from scratch. See §10.
5. Legacy binary Office formats are server-only. Native parsers would add 40–60 ew.
6. No A/V transcoding. Adding it costs 15–25 ew plus the patent question.
7. Real devices are available for benchmarking from Phase 5 onward.
8. Cloud AI provider contracts (DPAs, zero-retention) are secured without blocking Phase 14.
9. ~20 % of every phase is testing, and that is counted — a phase estimate is not
   implementation-only.

## 8. Estimate confidence and how to improve it

Current overall confidence: **Low-to-Medium**. That is appropriate for Phase 0 and should not
be presented as better than it is.

It improves materially after:
- **The 8 spikes complete** (~6.5 ew) — converts the four Low-confidence unknowns to Medium.
- **Phase 1 and 2 actuals land** — gives a team velocity multiplier to calibrate the rest.
- **SP-8 (DOCX fidelity baseline)** — the single biggest input to the Phase 8 estimate.
- **SP-1 (PDFium mobile build)** — the single biggest input to the Phase 5 estimate.

**Re-baseline at every phase boundary using actuals.** An estimate that is never revised
against reality is not an estimate; it is a wish.

## 9. What would make this cheaper

Honest options, with their costs:

| Option | Saves | Costs |
|---|---|---|
| Buy a commercial PDF SDK instead of PDFium+QPDF | 25–40 ew | Licence fees (often per-platform, per-seat or revenue-shared); vendor lock-in; a closed dependency in the most security-sensitive component |
| Purchase an Artifex MuPDF licence | 10–20 ew | Licence fees; but removes the SP-1 risk entirely |
| Use a commercial OCR SDK | 20–30 ew | Per-page or per-device fees; usually no offline mode, which breaks a P0 promise |
| Drop iOS or Android from v1.0 | 72 ew | Halves the market; the spec names both as primary |
| Use Flutter/React Native for the UI | ~40 ew | Rejected in `architecture.md` A2-1 for good reasons; would cost more later in the document canvas |
| Defer conversion beyond images + Markdown | 55 ew | Guts a headline capability |
| Defer sync (single-device v1.0) | 76 ew | Breaks the multi-device promise; sync is much harder to retrofit than to design in |
| Server-side conversion only | 40 ew | Breaks offline-first, which is a P0 promise for two personas |

**The only two that are genuinely attractive** are purchasing a PDF licence (buys down the
highest-risk integration) and the narrowed v1.0 in §6. The rest trade away things the product
cannot do without.

---

## 10. Re-baseline after `adr/0002` (2026-09-29)

Three product-owner decisions landed after the §2 table was written, and each invalidates an
assumption in §7. **None of them is a mistake** — decisions 1 and 3 are the kind of thing that
is far cheaper to decide now than in Phase 18 — but all three cost money, and an estimate that
did not move would be a wish rather than an estimate.

### 10.1 Decision 1 — E2EE from v1.0

The old assumption 4 said "+~40 ew". That was a placeholder. The real cost is spread across
five phases, and — importantly — **part of it is a saving**, because a server that cannot read
content is a smaller server.

| Phase | Δ ew | Why |
|---|---:|---|
| 2 Storage | **+3** | Key hierarchy extended to URK → WSK → document key; device key bundles; rotation-aware key store |
| 12 Cloud backend | **−8** | **Cheaper.** No server-side merge, no server-side index, no server-side thumbnailing, no server-side RAG. The sync service becomes an authenticated ordered relay, which is a much smaller service than a merging one. |
| 12 Cloud backend | **+10** | But: device enrolment/attestation flows, sealed-bundle storage and fan-out, rotation orchestration, and a support process that cannot read data |
| 13 Sync | **+8** | Encrypted op/snapshot framing; snapshot cadence and pruning; all merge and conflict logic moves client-side; the "no plaintext on the server" integration test |
| 14 AI | **+6** | Per-document consent flow, consent records, scope resolution and byte-range preview, revocation semantics, audit integration |
| 15 RAG | **+7** | Per-device corpus indexing as a first-class, resumable, throttled, honest-progress background job; incomplete-index UX; no server-side embedding path to fall back on |
| 18 Security hardening | **−6** | **Cheaper.** E2EE moves *out* of Phase 18 (where the old roadmap put it) and into 2/12/13. Phase 18 reviews it instead of building it. |
| 10/11 Mobile | **+9** | Recovery-phrase onboarding with forced verification; QR/short-code device enrolment UI; consent prompts; lost-device and revocation flows. **This is user-facing crypto UX, which is slow and requires real usability testing — it is not a dialog.** |
| **Net** | **+29** | |

**Lower than the old +40, but distributed differently and earlier** — which is the part that
matters, because it lands on the critical path (12 → 13 → 14 → 15) rather than at the end.

### 10.2 Decision 2 — custom C++ sequence CRDT

| Phase | Δ ew | Why |
|---|---:|---|
| 13 Sync | **+9** | The sequence-CRDT line item goes from 6–8 ew (classic RGA + a convergence fuzzer) to 15–17 ew: Fugue rather than RGA, a naive reference model, a differential harness, the non-interleaving property with its own generator, tombstone GC with causal stability, and bounded causal-delivery buffering. See `spikes/SP-6-findings.md` F-6. |
| 17 Collaboration | **+4** | Real-time editing now sits on a CRDT we wrote, so presence and live editing carry our own correctness risk rather than an upstream library's |
| **Net** | **+13** | |

**Offsetting saving: 0 ew, but a real one in kind.** Rust never enters the mobile build, so
`cargo-ndk`, target triples, dSYM handling and a second toolchain in CI are all avoided. That
was the *only* thing the `yrs` path cost, and it was the cheaper risk. We have traded a
recoverable build risk for an unrecoverable correctness risk, knowingly (R-22).

### 10.3 Decision 3 — low-end device support as a hard constraint

| Phase | Δ ew | Why |
|---|---:|---|
| 5 PDF | **+4** | Render-scale and cache tiering, `RGB_565` paths, runtime-derived cache sizing, Tier D fixture runs |
| 6 Scanner | **+3** | Capture-resolution tiering; the CV pipeline must fit a 96 MB arena, which is empirical work |
| 7 OCR | **+5** | `tessdata_fast`-only path, 200 DPI recognition path, serial scheduling, relaxed-budget UX, per-tier accuracy measurement (we must now publish CER *per tier*, not just per language) |
| 8 Conversion | **+4** | 96 MB arena and an explicit too-large-for-this-device failure path everywhere, rather than an OOM |
| 14 AI | **+4** | Device-capability probe driving model offering; the "no local generation here, and here's why" UX; one-embedding-model-for-all-tiers constraint |
| 19 Performance | **+8** | **A second gating device tier roughly doubles the device-matrix profiling work**, and Tier D is the tier where budgets are hardest to hit |
| 10/11 Mobile | **+4** | Runtime cache sizing, low-RAM code paths, Tier D fixture journeys in the E2E suite |
| **Net** | **+32** | |

### 10.4 Combined effect

| | ew |
|---|---:|
| Original §2 likely total | 1004 |
| Decision 1 — E2EE from v1.0 | **+29** |
| Decision 2 — custom C++ sequence CRDT | **+13** |
| Decision 3 — low-end device floor | **+32** |
| Spike re-scoping (SP-1 5→8 d, SP-6 5→8 d; ≈ +1 ew in Phase 0) | **+1** |
| **Revised likely total** | **≈ 1079 ew ≈ 20.8 engineer-years** |

**≈ +7.5 % on the whole programme.** That is a reasonable price for closing the single
largest architectural fork before Phase 1 rather than during Phase 18, and it is far less than
the retrofit would have cost — the original PHASE0_SUMMARY called answering PQ-2 late "the most
expensive available mistake", and it was right.

**The phases that move most:**

| Phase | Was | Now | Δ |
|---|---:|---:|---:|
| 13 Sync | 76 | **93** | +17 |
| 12 Backend | 62 | **64** | +2 (a large saving and a large cost, nearly cancelling) |
| 19 Performance | 32 | **40** | +8 |
| 15 RAG | 42 | **49** | +7 |
| 14 AI | 52 | **62** | +10 |
| 10/11 Mobile | 144 | **157** | +13 |
| 7 OCR | 44 | **49** | +5 |
| 18 Security | 42 | **36** | −6 |

**Phase 13 is now unambiguously the most dangerous phase in the project** — 93 ew, Low
confidence, "Very High" correctness risk, a from-scratch CRDT with a known algorithm-level
trap, and an encrypted transport that prevents server-side debugging of exactly the failures
that are hardest to reproduce. §4's advice to concentrate the best people on Phases 5, 8 and
13 should now read **13 first**.

### 10.5 Effect on the narrowed v1.0 (§6)

The narrowed v1.0 does **not** escape any of this. All three decisions land on phases it
retains — sync, backend, AI, RAG, OCR, performance, both mobile apps. Only Phase 17's +4 falls outside
it, and Phases 5 and 8 are reduced there, so the increase is slightly smaller but not much:
**799 ew → ≈ 865–870 ew**,
and at 8 engineers roughly **2.6–3.0 years** rather than 2.4–2.8. §6's finding holds and is
reinforced: the expensive parts are load-bearing for any version.

### 10.6 Confidence

Overall confidence is **unchanged at Low-to-Medium**, and the reasons have shifted rather than
improved:

- **Improved:** SP-1 and SP-5 are investigated; the PDF build path and the bridge design are
  no longer unknowns. R-12 and PR-7 both drop.
- **Worsened:** SP-6 found a real algorithm defect, the custom CRDT is now on the critical
  path, and Tier D is a gating tier we have never measured anything against. **The next
  material confidence improvement comes from SP-6's revised prototype and from getting real
  Tier D hardware into the benchmark rack — not from more analysis.**
