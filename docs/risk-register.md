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

### R-03 — Without E2EE, a server breach exposes user content
| | |
|---|---|
| **Category** | Security / Product |
| **P × I** | M × 5 = **10** |
| **Owner** | CTO / Security lead |

The product will hold contracts, IDs, medical and financial documents. Without end-to-end
encryption, the server can decrypt everything, so a breach is a content breach.

**Mitigation**
- **Answer PQ-2 before Phase 12.** If E2EE is a v1.0 promise, it must be designed into the
  backend and sync from the start, not retrofitted in Phase 18.
- If not E2EE: per-file encryption at rest, strict key separation, least-privilege service
  access, tenant isolation with RLS, and **honest public documentation of what we can see**.

**Trigger:** PQ-2 unanswered at the Phase 11 boundary ⇒ escalate; it becomes a schedule risk as
well as a security one.

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
- Adopt `yrs` rather than writing our own text CRDT (SP-6) to move correctness risk onto
  battle-tested code.

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
| R-12 | PDFium cannot be built for mobile outside Chromium's GN/depot_tools within budget | M×4=8 | Core lead | SP-1 is the gate. Fallback: a prebuilt-binary vendor (adds supply-chain risk) or purchasing a MuPDF commercial licence. |
| R-13 | `sqlite-vec` recall or latency is inadequate at scale | M×3=6 | Search lead | SP-2. Fallbacks: `usearch`, `hnswlib`, or int8 quantisation with a coarse pre-filter. |
| R-14 | Local LLM is unusable on the target device floor | M×3=6 | AI lead | SP-4. Honest fallback: local = embeddings + classification only; generation is cloud with an explicit policy gate. This is an acceptable v1.0. |
| R-15 | Rust toolchain adds unacceptable mobile build friction | M×3=6 | Build lead | SP-6 tests it early. If it fails, write the RGA CRDT in C++ and drop `resvg` for NanoSVG. |
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
| R-28 | Web client scope creeps toward parity and consumes the schedule | M×3=6 | Product lead | Web is view-mostly by written decision; any parity request goes through scope review. |

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
