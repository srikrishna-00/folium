# ADR 0002: Phase 0 Sign-off Decisions

Date: 2026-09-29
Status: Accepted

## Context

Phase 0 architecture study (PHASE0_SUMMARY.md) flagged several decisions
that fork the architecture and required product-owner sign-off before
Phase 1 could start. Decisions below are final for v1.0 planning.

## Decisions

### 1. End-to-end encryption (E2EE)
**Decision: E2EE from v1.0.**
Sync, AI/RAG, and agent architecture (Phases 12-15) must be designed
from the start assuming the server never sees plaintext. This is a
harder v1.0 but avoids a costly redesign later. Cloud AI features that
require server-side plaintext access (e.g. server-side RAG indexing)
must be re-architected as client-side/local-first operations, or must
prompt explicit per-document user consent to decrypt before sending to
a cloud AI provider (see spec section 24, Privacy).

### 2. CRDT engine
**Decision (amended 2026-09-29, see Amendment below): custom C++
implementation of the Fugue algorithm, not yrs/Rust, not classic RGA.**
Stays within the spec's C/C++-first mandate (section 2). Accepted
tradeoff: this requires building and correctness-proving a CRDT from
scratch rather than adopting a mature implementation. Mitigation:
budget dedicated spike + fuzz/property testing time in Phase 13 for
CRDT correctness — convergence, idempotence, commutativity, AND
non-interleaving (see Amendment) — before it carries real user data.

### 3. Minimum device targets
**Decision: 2GB RAM / iOS 15+ / Android 8+.**
This raises the bar on Phase 6 (Scanner), Phase 7 (OCR), and Phase 14
(AI): on-device OCR/AI must degrade gracefully (smaller models, lower
resolution processing, or cloud-offload with explicit user consent per
the E2EE/privacy decision above) rather than assuming flagship-class
hardware. Concretely: OCR uses `tessdata_fast` (4-5MB models, not
`best`'s 15-23MB) on this tier; local AI is capped at ~270M-parameter
INT4 models (~125-200MB resident) which is sufficient for extraction/
classification but NOT viable as a local summarizer — summarization on
this device tier requires the cloud-offload+consent path. Performance
strategy (performance-strategy.md) and file/OCR architecture treat this
as a hard constraint, not an optimization pass.

## Amendment (2026-09-29): CRDT algorithm changed from RGA to Fugue

Pre-Phase-1 spike SP-6 (docs/spikes/SP-6-findings.md) found that classic
RGA — as originally named in Decision 2 above — has a published
backward-interleaving anomaly (Kleppmann, PaPoC 2019): concurrent
insertions at the same position can converge to text that all replicas
agree on, but which is corrupted relative to user intent. The
convergence/idempotence/commutativity test plan originally specified as
this decision's mitigation is structurally blind to this failure mode,
since all three properties hold on the corrupted result.

**Amended decision: implement Fugue (Weidner & Kleppmann, 2023)
instead.** Fugue provides maximal non-interleaving and its correctness
argument is largely inheritable from existing RGA-family proofs (an
Isabelle/HOL strong-eventual-consistency proof exists for RGA; only
Fugue's positioning optimizations are novel and need their own
verification). Estimated cost: ~+1 engineer-week over the original RGA
estimate, revised further upward by SP-6's broader complexity findings
(see engineering-complexity-estimate.md: CRDT line item 6-8 -> 15-17
engineer-weeks).

Phase 13's test plan must add a fourth property beyond convergence/
idempotence/commutativity: **non-interleaving**, verified via reference-
model differential testing against a known-correct sequence CRDT
implementation, not just self-consistency checks.

## Consequences

- ai-architecture.md, sync-architecture.md, and security-model.md need
  a follow-up pass reflecting the E2EE-from-v1.0 decision (cloud AI
  consent flow, sync engine operating on encrypted CRDT payloads).
- roadmap.md / engineering-complexity-estimate.md should be revisited:
  E2EE-from-v1.0 and low-end device support both increase Phase 12-15
  and Phase 6/7/14 effort versus the original estimate.
- Custom CRDT decision means Phase 13 needs an explicit correctness
  spike before it is considered de-risked.
