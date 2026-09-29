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
**Decision: custom C++ RGA (Replicated Growable Array), not yrs/Rust.**
Stays within the spec's C/C++-first mandate (section 2). Accepted
tradeoff: this requires building and correctness-proving a CRDT from
scratch rather than adopting a mature implementation. Mitigation:
budget dedicated spike + fuzz/property testing time in Phase 13 for
CRDT correctness (convergence, idempotence, commutativity) before it
carries real user data.

### 3. Minimum device targets
**Decision: include low-end/older devices**, not modern-only.
This raises the bar on Phase 6 (Scanner), Phase 7 (OCR), and Phase 14
(AI): on-device OCR/AI must degrade gracefully (smaller models, lower
resolution processing, or cloud-offload with explicit user consent per
the E2EE/privacy decision above) rather than assuming flagship-class
hardware. Performance strategy (performance-strategy.md) and file/OCR
architecture should treat low-end capability as a hard constraint, not
an optimization pass.

## Consequences

- ai-architecture.md, sync-architecture.md, and security-model.md need
  a follow-up pass reflecting the E2EE-from-v1.0 decision (cloud AI
  consent flow, sync engine operating on encrypted CRDT payloads).
- roadmap.md / engineering-complexity-estimate.md should be revisited:
  E2EE-from-v1.0 and low-end device support both increase Phase 12-15
  and Phase 6/7/14 effort versus the original estimate.
- Custom CRDT decision means Phase 13 needs an explicit correctness
  spike before it is considered de-risked.
