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
3. The spikes in `technical-requirements.md` §6 complete successfully. A failed SP-1 (PDFium
   mobile build) adds 15–30 ew or forces a commercial MuPDF licence.
4. E2EE is **not** in v1.0. If it is, add ~40 ew and reorder Phases 12–15 — the cost is in the
   redesign, not the crypto.
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
