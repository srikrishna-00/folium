# Risk Register

**Status:** Phase 0 draft — awaiting human sign-off
**Version:** 0.0.0
**Date:** 2026-09-29
**Review cadence:** every phase boundary, and on any trigger firing

Implements spec §40 item 24.

---

## Scoring

**Probability:** L (< 20 %) · M (20–50 %) · H (> 50 %)
**Impact:** 1 minor · 2 moderate · 3 serious · 4 severe · 5 project-threatening
**Score** = P(L=1,M=2,H=3) × Impact. **Score ≥ 9 requires a named owner and an active plan.**

Owners are roles, not names, until staffing is assigned.

---

## Top risks (score ≥ 9)

### R-01 — Scope is far larger than a normal v1.0
| | |
|---|---|
| **Category** | Delivery |
| **P × I** | H × 5 = **15** |
| **Owner** | Product lead |

The product combines five categories (workspace, PDF suite, scanner, OCR, AI platform) each of
which is a viable standalone product with a multi-year history. `engineering-complexity-estimate.md`
puts v1.0 at roughly 200–280 engineer-months. A team of 5 delivers that in 3.5–4.5 years.

**Mitigation**
- Treat the P0 set in `product-requirements.md` §5 as the real v1.0 and everything else as
  post-1.0. Enforce it at planning, not at crunch time.
- Ship a narrower product sooner: the scanner→OCR→searchable-PDF→search→AI-Q&A spine is a
  coherent, shippable product on its own, and it is the P1/P2 persona core.
- Re-baseline at every phase boundary with actuals, not with optimism.

**Trigger:** any phase exceeding its estimate by > 40 % ⇒ formal scope review, not a schedule
extension.

---

### R-02 — Parsers share the app address space on mobile
| | |
|---|---|
| **Category** | Security |
| **P × I** | M × 5 = **10** |
| **Owner** | Security lead |

iOS does not allow spawning sandboxed child processes, so the browser-style
one-parser-per-process isolation (`architecture.md` A2-4) is unavailable. A memory-corruption
bug in PDFium, libtiff, Tesseract or our own DOCX parser is a bug in the whole app, with access
to the decrypted database and keys.

**Mitigation**
- Continuous fuzzing of every parser (`testing-strategy.md` §6), 72 h clean as a release gate.
- ASan+UBSan on every PR; MSan and TSan on a schedule.
- Arena allocation with hard byte and time caps; parse failures are errors, not crashes.
- Parse/model split: parsers never touch storage or keys directly.
- Pinned dependencies with same-week patching for Critical parser CVEs.
- Server-side conversion *does* use one-shot, network-denied, resource-capped containers.

**Residual: accepted and documented** (`security-model.md` §14).
**Trigger:** any exploitable finding in a shipped parser ⇒ emergency review of the isolation
posture, including whether to move that format server-side.

---

### R-03 — ~~Without E2EE, a server breach exposes user content~~ → **Key loss makes user data unrecoverable**
| | |
|---|---|
| **Category** | Security / Product / Support |
| **P × I** | ~~M × 5 = 10~~ → **M × 4 = 8** (re-scored) |
| **Owner** | CTO / Security lead |

**Resolved as originally written.** `adr/0002` decision 1 ships E2EE at v1.0: the server holds
no key, so a breach yields ciphertext plus metadata (`security-model.md` §5A). PQ-2 is answered
and the work has moved out of Phase 18 into Phases 2/12/13.

**The risk does not disappear; it inverts.** E2EE trades "a breach exposes content" for "nobody,
including us, can recover a lost key". A single-device user who loses the device and has neither
the recovery phrase nor a platform-keychain copy has **permanently lost their data, and support
cannot help.** At scale, users lose recovery phrases.

**Mitigation**
- Recovery phrase generated client-side with **mandatory re-entry verification**, no deferral
  (open question N-7 in `spikes/SPIKE_SUMMARY.md`).
- Opt-in platform-keychain convenience copy, clearly labelled, never the only copy.
- **Push users toward a second enrolled device** and warn persistently while only one exists —
  multiple devices are the practical recovery mechanism, not the phrase.
- Say it before it happens: onboarding, settings, privacy policy.
- Support runbooks written on the basis that there is no back door — **a launch blocker**.
- PIN + enclave escrow (Signal-SVR-class) evaluated for v1.1; **the record shape must exist in
  the v1.0 data model** or it becomes a migration across every user's keys (N-4).

**Residual: accepted.** This is the direct price of E2EE (`security-model.md` §14, item 8).

**Trigger:** any proposal to add a server-side key-recovery path ⇒ that is not E2EE; escalate to
the CTO before it ships.

---

### R-04 — Sync produces silent data loss
| | |
|---|---|
| **Category** | Data integrity |
| **P × I** | M × 5 = **10** |
| **Owner** | Sync tech lead |

Distributed convergence bugs are subtle, delayed, hard to reproduce, and destroy user trust
permanently. A single credible "it ate my notes" story is worth more damage than a year of
feature work.

**Mitigation**
- Per-datatype strategies rather than one algorithm (`sync-architecture.md` §2).
- Version vectors for exact conflict detection; **no wall-clock ordering anywhere**.
- Deterministic simulation with injected clock and RNG; seeded, reproducible failures.
- Property tests for convergence, no-loss and idempotence over 10⁶ operations.
- Every LWW loser written to version history before being overwritten — tested, not asserted.
- 7-day / 5-device soak as a release gate.
- ~~Adopt `yrs` rather than writing our own text CRDT (SP-6).~~ **Overturned by `adr/0002`
  decision 2: we write our own.** Correctness risk stays with us — see **R-29**, and note that
  the four bullets above are *not sufficient* for a sequence CRDT (SP-6 F-1).
- **Add the fourth property: non-interleaving**, with its own backward-insertion generator
  (`sync-architecture.md` §6.2). Convergence, no-loss and idempotence all pass on an
  interleaving bug.
- **And a fifth check, which is not a property of the algorithm but of the history:** the merged
  order must be a linear extension of the constraints the generating replicas actually imposed
  (the strong list specification). `spikes/fugue-correctness-spike.md` §5 shows that the four
  properties *plus* reference-model differential testing still certified an injected
  wrong-parent fault as correct, and that this oracle was the only one that caught it.

**Trigger:** any convergence failure in simulation ⇒ Phase 13 does not exit.

---

### R-05 — Prompt injection via document content
| | |
|---|---|
| **Category** | Security / AI |
| **P × I** | H × 3 = **9** |
| **Owner** | AI lead |

A document can contain instructions aimed at the model. With agents that have tools, that is an
action-taking vector. There is no complete defence.

**Mitigation**
- Retrieved content lives in a structurally distinct `document` role, never concatenated into
  instructions.
- Tool authorisation derives from the **user's** grant, not the model's request.
- Destructive tools always require human confirmation; no auto-approve exists in v1.0.
- No shell, eval, or arbitrary-network tool exists in the registry.
- Per-run, scoped, expiring grants.
- Red-team corpus in CI with a zero-unauthorised-invocation gate.
- Full run transcripts so an anomaly is visible after the fact.

**Residual: accepted** (`security-model.md` §14).

---

### R-06 — Legal exposure from a licence or patent error
| | |
|---|---|
| **Category** | Legal |
| **P × I** | M × 5 = **10** |
| **Owner** | CTO + counsel |

Shipping GPL/AGPL/LGPLv3 code in a closed app, or an HEVC encoder without a patent licence,
can force source disclosure, an app-store takedown, or damages. The failure is often discovered
late, by someone else.

**Mitigation**
- The verdict table in `file-format-support.md` §7.1, with hard blocks stated.
- CI fails any dependency without a `LICENSE`, a pinned hash, and a notices entry (NFR-8).
- SBOM generated per release from the actual link graph.
- **Counsel review before Phase 5 and Phase 7.**
- Re-verify every licence at the pinned upstream revision — AGG 2.4→2.5 (permissive→GPL) and
  OpenCV BSD→Apache are both real precedents for licences changing between versions.
- HEIC/HEVC avoided entirely; AVIF preferred.

**Trigger:** any GPL-family or LGPLv3 code found in a client build ⇒ release blocked.

---

### R-07 — On-device performance does not meet the budgets
| | |
|---|---|
| **Category** | Technical |
| **P × I** | M × 4 = **8** (watch — promote if SP-3/SP-4 disappoint) |
| **Owner** | Performance lead |

OCR, local AI, large PDFs, vector search and a 1 GB SQLite file all competing on a mid-tier
phone. Any one of them can blow the memory ceiling, which on mobile means termination, not
slowness.

**Mitigation**
- Budgets defined now, with automated gates from Phase 1 (`performance-strategy.md`).
- SP-2, SP-3, SP-4 validate the riskiest assumptions before committing.
- Tier C degrades explicitly with features named and disabled, never silently slow.
- Thermal-aware throttling; arena caps; pooled buffers; tiled rendering.

**Trigger:** any P0 budget missed by > 25 % at a phase boundary ⇒ scope or device-floor review.

---

### R-08 — Conversion fidelity disappoints users
| | |
|---|---|
| **Category** | Product |
| **P × I** | H × 3 = **9** |
| **Owner** | Product lead |

Users compare DOCX and PPTX rendering against Word and PowerPoint. Our own OOXML parsers will
lose formatting. If expectations are not set, every conversion is a disappointment.

**Mitigation**
- Fidelity reports surfaced before and after conversion (`conversion-architecture.md` §6).
- `Unsupported` nodes make loss visible rather than silent.
- The published matrix uses ◐ honestly; never ✔ where loss is expected.
- SP-8 measures the real baseline on a 20-document corpus before Phase 8 commits.
- PPTX rendering deliberately deferred to post-1.0 rather than shipped badly.

---

### R-09 — Codec patent exposure (HEVC, H.264, AAC)
| | |
|---|---|
| **Category** | Legal |
| **P × I** | L × 4 = **4** given current decisions; **M × 5 = 10** if PQ-5 says yes to transcode |
| **Owner** | CTO + counsel |

**Mitigation:** ship no transcoding at v1.0; use platform media stacks where the OS vendor holds
the licences; prefer AV1/AVIF/Opus. If transcode is required, do it server-side where exposure
is one assessable enterprise question rather than a per-install one.

---

### R-10 — Key loss makes user data unrecoverable
| | |
|---|---|
| **Category** | Data integrity |
| **P × I** | M × 4 = **8** |
| **Owner** | Security lead |

Encryption at rest with hardware-backed, non-exportable keys means a device wipe, an OS
migration failure, or a biometric reset can orphan local data. Under E2EE, a lost recovery
phrase is permanent.

**Mitigation**
- Keys marked `ThisDeviceOnly` deliberately — but sync means the server holds a recoverable
  copy of content in the non-E2EE model.
- **Local-only mode carries a repeated, prominent warning** that there is no backup.
- Under E2EE: enforced recovery-phrase confirmation at setup, with no silent skip.
- Key-lifecycle test suite; restore-from-new-device tested every release.

---

### R-11 — Native plugins have full process privileges
| | |
|---|---|
| **Category** | Security |
| **P × I** | M × 4 = **8** |
| **Owner** | Security lead |

Spec §30 asks for a plugin system. A native plugin can do anything the app can.

**Mitigation:** plugins are **first-party only** until a safe substrate exists. WASM with an
explicit capability interface is the likely answer. Third-party plugins are not v1.0.

---

## Secondary risks (score 4–8)

| ID | Risk | P×I | Owner | Mitigation |
|---|---|---|---|---|
| R-12 | ~~PDFium cannot be built for mobile outside GN/depot_tools~~ → **PDFium cannot be built at our iOS deployment-target floor** | ~~M×4=8~~ → **L×4=4** | Core lead | **Re-scoped by SP-1** (`spikes/SP-1-findings.md`). The original premise is answered: it cannot be built outside GN, and it should not be tried — a hand-written CMake port is archived, never covered mobile, and would make the §7.8 licence inventory describe a build we do not ship. **We build from pinned source with upstream GN in a hermetic container in our CI, consumed as an `IMPORTED` CMake target.** Mobile builds are continuously proven upstream; the residual is that the maintained recipe targets iOS 17 while `adr/0002` decision 3 pushes our floor *down*. Gate: revised SP-1 criterion 2. Fallback unchanged (MuPDF commercial licence). |
| R-13 | `sqlite-vec` recall or latency is inadequate at scale | M×3=6 | Search lead | SP-2. Fallbacks: `usearch`, `hnswlib`, or int8 quantisation with a coarse pre-filter. |
| R-14 | Local LLM is unusable on the target device floor | M×3=6 | AI lead | SP-4. Honest fallback: local = embeddings + classification only; generation is cloud with an explicit policy gate. This is an acceptable v1.0. |
| R-15 | ~~Rust toolchain adds unacceptable mobile build friction~~ | **CLOSED** | Build lead | **Closed by decision, not by test.** `adr/0002` decision 2 keeps Rust out of the mobile build entirely. This also settles SVG (NanoSVG, not `resvg` — `file-format-support.md` §7.5) and the tokenizer (AIQ-6). **Note what was traded:** a recoverable, early-detected build risk for an unrecoverable, late-detected correctness risk — see R-29. |
| R-16 | EAV database views miss the 100 ms budget | M×3=6 | Core lead | Reference-only joins + materialised view cache; documented pivot-table escape hatch. |
| R-17 | Cloud AI cost per user is unbounded | M×4=8 | Product lead | Token budgets, quotas, aggressive caching, local-first routing. **Blocked on PQ-1.** |
| R-18 | An AI provider changes terms, pricing, or discontinues a model | H×2=6 | AI lead | The abstraction is the mitigation — ≥2 providers at v1.0, no provider-specific code in the core. |
| R-19 | App Store or Play rejection (privacy, permissions, licences, AI content) | M×3=6 | Mobile lead | Privacy manifest and Data Safety completed early; licence audit; purpose strings that actually explain; a TestFlight/internal-track submission long before the real one. |
| R-20 | OCR accuracy below user expectations for non-Latin scripts | M×3=6 | OCR lead | Measure per language (SP-3) and **publish the real numbers internally**; prefer platform OCR where better; never claim a language we have not measured. |
| R-21 | Team lacks depth in a required specialty (PDF internals, CRDTs, mobile native, ML) | M×4=8 | Eng manager | Identify gaps in Phase 0; hire or contract before the relevant phase, not during it. |
| R-22 | Third-party dependency abandoned or a Critical CVE with no upstream fix | M×3=6 | Per-dependency owner | Named owner per dependency; pinned sources so we can patch locally; CVE monitoring daily. |
| R-23 | SQLite file growth (approaching 1 GB) degrades heavy users | M×3=6 | Storage lead | Version compaction, incremental vacuum, vector quantisation — all must actually run, not just exist. |
| R-24 | Migration failure on a user's device corrupts or orphans data | L×5=5 | Storage lead | Migrations in transactions with checksums; integrity assertions after each; fixture DB per version; refuse-to-open on too-new schema; resumable long migrations. |
| R-25 | CI cost and duration make the pipeline get bypassed | M×2=4 | Build lead | PR path budgeted at ≤ 25 min; expensive jobs nightly; core build cached across app-only changes. |
| R-26 | Accessibility gaps discovered late | M×3=6 | Design lead | Automated scans in CI from Phase 10; manual screen-reader pass every release; a11y is a Definition-of-Done item, not a phase. |
| R-27 | Fixture corpus contains copyrighted or personal data | L×4=4 | QA lead | `PROVENANCE.md` per fixture directory; synthetic data only; review at PR. |
| R-28 | Web client scope creeps toward parity and consumes the schedule | M×3=6 | Product lead | Web is view-mostly by written decision; any parity request goes through scope review. **Under E2EE a web client must hold keys and index in-browser — this is now a product decision, not a port (N-12).** |
| **R-29** | **The CHOSEN ALGORITHM has a merge defect that converges** — replicas agree on corrupted text | ~~M×5=10~~ → **L×5=5** | Sync lead | **Downgraded 2026-10-04 on evidence, by `spikes/fugue-correctness-spike.md`.** Fugue *and* FugueMax were implemented from the paper's Algorithm 1 and Definition 6 as a standalone spike (`core/sync/spike/`), and the four properties hold over **5.49 M generated operations / ~27 M applications**, clean under ASan+UBSan. The RGA defect is reproduced in a deliberate control (`core/sync/spike/rga.h`): the backward shopping-list shape merges to `SXaYbZc` under RGA and `SabcXYZ` under Fugue, with the **same detector asserted on both sides**, so P4 is a measurement rather than a claim. The suite is proven able to fail — **5 of 5 injected faults caught** — and the implementation reproduces the paper's Figure 7 variant-distinguishing result, which nothing in the suite was fitted to. **Not CLOSED:** the spike verified a *naive* implementation; the probability moved to the new **R-32** rather than disappearing. **Two mitigations named in the original row were found insufficient as written:** (a) `adr/0002`'s "reference-model differential testing" is structurally blind to a bug in operation *generation* and certified one injected fault as correct — it must be paired with a **strong-list-specification oracle** derived from the operation history (spike §5); (b) an 18 %-delete generator needed ~1 000 trials to find that same fault versus ~50 at 45 %, so tombstone-heavy generation is not optional. **Compounding factor unchanged: under E2EE the server cannot be inspected to diagnose a production occurrence.** |
| **R-30** | **Per-device corpus re-indexing (mandatory under E2EE) never completes on a low-end device** | **M×4=8** | AI / Search lead | **New, from the interaction of `adr/0002` decisions 1 and 3** — neither predicts it alone. Chunks and vectors never sync, so every device re-indexes locally; one of those devices has 2–3 GB RAM and eMMC. Mitigations: checkpointed, resumable, thermally- and battery-aware indexing; prioritise recently-accessed documents so the index is *useful* long before *complete*; an explicit "search is incomplete" state in the UI rather than silently under-returning. |
| **R-31** | **PDFium roll maintenance lapses and a parser CVE ships** | M×4=8 | Core lead | **New, from SP-1.** Upstream has no releases, only `chromium/NNNN` branch heads, so staying current is an ongoing scripted job, not a one-off integration. Budget **≈ 0.5 ew/quarter** from Phase 5; the re-roll procedure is a documented Phase 5 deliverable; daily CVE monitoring already covers detection. |
| **R-32** | **The OPTIMISED, GC'd production CRDT diverges from the verified naive one** | **M×5=10** | Sync lead | **New, 2026-10-04, from `spikes/fugue-correctness-spike.md` §7.** This is where R-29's probability went; it is not a replacement hazard invented to keep a number alive. The spike verified ~400 lines of unoptimised Fugue. Phase 13 ships waypoint/run-length compression, index caches, tombstone GC with causal stability and encrypted framing — none of which exists or is tested, and SP-6 F-3 is explicit that optimisations are historically where hand-written CRDTs break. Materially more tractable than R-29 was, because the obligation is now "byte-identical to a model we have" rather than "correct": the spike's reference model, specification oracle, P4 scenarios and RGA control are written to be reused against the production implementation unchanged. Mitigations: differential-test the optimised implementation against `reference_order()` on every generated interleaving; keep the specification oracle **alongside** it, never instead of it; keep `uapp.sync_spike.rga_control` green, because it is the only thing proving P4 can still fail; **every tree walk must be iterative or explicitly depth-bounded** — measured Fugue tree depth equals document length *exactly* (a 16 000-character note is a 16 000-deep tree), so the paper's recursive traversal overflows a 512 KB mobile worker-thread stack at ordinary document sizes. GC and encrypted framing remain owed as written specifications **before** merge code (SP-6 R-4; re-scoped SP-6 task 4). |
| **R-33** | **The app ships E2EE without the US export-control filing it now requires** | **M×4=8** | Product lead | **New, 2026-10-17, from `spikes/SP-9-findings.md` §4.2.** Found while vetting libsodium for licensing and discovered to be a *different kind of constraint entirely*: the ISC licence imposes nothing beyond attribution, and yet integrating a bundled crypto library that provides **content confidentiality** makes the app's encryption **non-exempt** for Apple's purposes (`ITSAppUsesNonExemptEncryption = YES`). Apple's exemptions cover encryption provided **by the OS** through Apple's APIs, TLS to one's own backend, and encryption used only for authentication/integrity — our case matches none. The app therefore falls under **EAR Category 5 Part 2**, and License Exception ENC (15 CFR 740.17(b)(1)) requires a **self-classification report to BIS and the NSA encryption request coordinator before export and ANNUALLY thereafter**; France requires a separate declaration. **This follows from `adr/0002` decision 1, not from the choice of libsodium** — any library encrypting user content lands here, and the common belief that Apple's CryptoKit would avoid it is a misreading of the OS-provided exemption (and CryptoKit is ruled out on portability anyway, ADR-0001). **Why it is scored M rather than L:** it is not a technical risk at all, it has no engineering owner, `security-model.md` §12 covers supply chain and is silent on export control, `deployment.md` is silent too, and the recurring annual component is exactly the kind of obligation that is satisfied once and then lapses. Impact 4 rather than 5 because the consequence is a **blocked or pulled submission plus penalty exposure**, not data loss. Mitigations: counsel engagement before the first submission containing `security/crypto`; add the filing to the release checklist as a **gate** (now a row in `security-model.md` §13); make the annual renewal a calendared, owned task rather than a release-time discovery. **Trigger: the first App Store submission containing `security/crypto`.** Note the relationship to **R-06** (legal exposure from a licence or patent error) — this is adjacent but distinct: R-06 is about getting a licence wrong, and this is about a licence being *right* and still not being sufficient to ship. |

---

## Risks explicitly accepted without further mitigation

Recorded so they are decisions, not oversights:

1. **Rooted/jailbroken devices are not defended against.** Out of our control; attempting it
   produces false assurance.
2. **Cloud AI providers are trusted contractually, not technically.** Inherent to using them.
3. **Metadata leaks to the server even under E2EE** — sizes, timing, workspace shape.
4. **Local-only users have no backup.** Mitigated by disclosure, not by engineering.
5. **iOS background time is not guaranteed.** Mitigated by checkpointing, not by assumption.
6. **Handwriting OCR quality is not guaranteed.** Shipped as BEST-EFFORT, labelled in-product.

---

## Review log

| Date | Reviewer | Changes |
|---|---|---|
| 2026-09-29 | Architecture (Phase 0) | Initial register created |
| 2026-10-17 | SP-9 — crypto dependency vetting | **R-33 added (M×4=8)**: shipping E2EE pulls in a US export-control self-classification filing that nothing in the repo owns, found while reviewing libsodium's licence and *not* a licensing problem. No risk downgraded. **R-06's scope clarified by implication** — a clean licence review is not sufficient evidence that a cryptographic dependency is clear to ship. Also raised, as documentation defects rather than risks, and annotated in place in `security-model.md`: §5's Argon2id parameters (`opslimit=4`, `memlimit=256 MiB`) are **unrunnable on `adr/0002` decision 3's own 2 GB device floor** and the implementation uses 64 MiB, a stated 4x reduction in memory-hardness; and §5A.2 names `crypto_box_seal`, which authenticates **nobody**, without saying whether sender anonymity or sender authentication was intended (`security/crypto` implements the authenticated variant and the choice needs a human). |
| 2026-10-04 | Fugue correctness spike | **R-29 downgraded M×5=10 → L×5=5** and re-scoped to the algorithm choice, on the evidence in `spikes/fugue-correctness-spike.md`. **R-32 added (M×5=10)** for the untested optimised/GC'd implementation, which is where R-29's probability moved. **R-04 mitigation extended** with the strong-list-specification oracle, because the four properties plus reference-model differential testing were measured to be insufficient. Two documentation defects raised for product-owner decision — **resolved same day**: `adr/0002` and `sync-architecture.md` §6.1 overstated plain Fugue as *maximally* non-interleaving (that is FugueMax's property); product owner confirmed **plain Fugue** (not FugueMax) as the production choice, matching the paper's own practical recommendation, and both documents were corrected in place. |
