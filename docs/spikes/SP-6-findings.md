# SP-6 Findings — Text CRDT: the custom C++ sequence CRDT

**Status:** Investigated (literature review + design analysis). No code built.
**Date:** 2026-09-29
**Spike definition:** `technical-requirements.md` §6, row SP-6. Box: 5 d.
**Superseded input:** `adr/0002` decision 2 already chose **custom C++, not `yrs`/Rust**.
**Blocks:** Phase 1 entry, Phase 13 entirely. Risks R-15, R-01-adjacent (data loss).

> **Headline: this spike found a specification-level correctness defect in RGA itself, and
> it is not the kind of defect the ADR's stated mitigation can catch.** Read §3, F-1.

---

## 1. The question, restated

The original SP-6 asked *"own C++ RGA vs. `yrs`?"*. `adr/0002` answered it: **custom C++,
no Rust in the mobile build.** That closes the *language* question and leaves the harder
one open, which is now what SP-6 must answer:

> Given that we are writing a sequence CRDT from scratch, **which algorithm**, and **what
> would actually constitute proof that it is correct** before it carries user data?

The ADR states the mitigation as "fuzz/property testing for convergence, idempotence,
commutativity". **Finding F-1 shows that those three properties are necessary and provably
insufficient**, which makes this the most consequential of the three spikes.

## 2. Method

1. Surveyed the peer-reviewed literature on sequence-CRDT *correctness properties* — not
   just convergence — specifically the interleaving-anomaly line of work.
2. Checked whether RGA specifically is affected, and how.
3. Looked for an algorithm with a stronger property that is still implementable in the
   effort band we have.
4. Looked for reusable formal-verification work, to see how much of a correctness argument
   we can inherit rather than originate.
5. Worked out an affordable proof strategy for a hand-written implementation, and the
   failure modes specific to hand-written CRDTs (tombstone GC, causal delivery).
6. Checked the interaction with `adr/0002` decision 1 (E2EE), which changes where merging
   can happen at all.

## 3. Findings

### F-1 — Plain RGA has a published, user-visible correctness anomaly, and convergence testing cannot detect it.

Kleppmann, Gomes et al., *"Interleaving anomalies in collaborative text editors"* (PaPoC
2019), establishes that when two users concurrently insert text at the same position, the
merged result may **interleave the two insertions character by character**, producing
corrupted and unreadable text. The paper notes this affects both CRDTs and Operational
Transformation and had gone unnoticed for decades.

For RGA specifically the result is sharp: **RGA is proved free of *forward* interleaving but
exhibits *backward* interleaving.** Concretely, when text is built up right-to-left at a
shared position — which happens when a user types a word then prepends to it, and on some
paste and IME paths — two concurrent insertions interleave.

**Why this is the finding that matters:** all replicas *converge*. They converge on the
same garbage. So:

- a convergence property test **passes**;
- an idempotence property test **passes**;
- a commutativity property test **passes**;
- a fuzz harness comparing final states across replicas **passes**;
- and the user sees `HWeolrllod`.

ADR 0002's mitigation — "convergence, idempotence, commutativity" — is exactly the test set
that is blind to this class of defect. This is not a criticism of the ADR; it is the
standard three-property set, and it is what most teams would write. It is simply not
sufficient for a *sequence* CRDT.

### F-2 — There is a stronger property, and an algorithm that achieves it, at comparable cost.

Weidner, Kleppmann et al., *"The Art of the Fugue: Minimizing Interleaving in Collaborative
Text Editing"* (arXiv 2305.00583, 2023), defines **maximal non-interleaving** as a
correctness property for replicated lists and gives **Fugue**, an algorithm that achieves
it. The mechanism is a refinement of the RGA/Yjs-family idea rather than a departure from
it: disambiguate concurrent inserts by **`leftOrigin` first**, and where that is still
ambiguous, by **`rightOrigin`** — which is precisely what closes the backward-interleaving
hole RGA leaves open.

The paper also proves a negative: **it is impossible to avoid interleaving in every
situation.** So the honest target is *maximal* non-interleaving, not *zero* — and that
should be stated in-product terms too, not just in tests.

Fugue is of comparable implementation complexity to RGA and is designed to be optimisable
(Loro, a production Rust CRDT library, adopted it for exactly this reason).

**Consequence: if we are writing one from scratch anyway, the marginal cost of writing
Fugue instead of classic RGA is small, and it buys a correctness property RGA cannot have.**
The cost of *not* doing so is a defect class that will be found by users, in their own
documents, long after Phase 13's gates went green.

### F-3 — A substantial part of the correctness argument can be inherited, not originated.

Gomes, Kleppmann, Mulligan & Beresford, *"Verifying Strong Eventual Consistency in
Distributed Systems"* (OOPSLA 2017), provides a modular Isabelle/HOL framework with an
axiomatic network model and an **abstract convergence theorem**, from which SEC proofs for
concrete algorithms are derived. **RGA is one of the three datatypes already embedded and
machine-checked in it** (alongside a Counter and an OR-Set), and the development is
published in the Archive of Formal Proofs.

This changes the shape of "correctness-proven from scratch" in `adr/0002`:

- The SEC argument for an RGA-family sequence CRDT is **not original research.** It exists,
  machine-checked.
- We do **not** need to do Isabelle work ourselves to benefit. The published algorithm
  specification is a **reference specification we can implement naively and differentially
  test against** — which is far cheaper and catches implementation bugs, which is where our
  actual risk lives.
- What we *cannot* inherit is a proof about **our optimisations**: run-length block
  encoding, index caches, tombstone GC. Those are ours, and they are historically where
  hand-written CRDTs break.

### F-4 — The two failure modes specific to hand-written CRDTs are GC and causal delivery.

Neither is in the ADR's mitigation list, and both are where a from-scratch implementation
actually fails in production:

**(a) Tombstone garbage collection.** Tombstones must never be collected while any
operation that references them may still arrive. The safe criterion is **causal stability**
— collect only what every known replica has acknowledged. With N devices that may be
offline indefinitely, causal stability may **never** be reached, and an unbounded tombstone
set is a permanent, growing metadata cost on a mobile device (the exact cost
`sync-architecture.md` §2 flags as the reason not to use CRDTs everywhere).

The resolution is already half-present in the architecture and needs to be made explicit
and consistent: `sync-architecture.md` §4.3 trims the server change log after 90 days and
forces `reset_required` → full resync. **Tombstone GC must use the same horizon**: a device
absent beyond it is excluded from the causal-stability calculation and resyncs from
scratch. An operation arriving that references a collected tombstone must be **rejected
cleanly, not handled as undefined behaviour** — and that rejection path needs its own test.

**(b) Causal-delivery violations.** An insert arriving before the operation defining its
origin is the single most common crash in hand-written CRDTs. The implementation must
**buffer**, not assume, and the buffer needs a bound and an escalation path (a hostile or
buggy device can otherwise pin memory forever — cf. T-305).

### F-5 — E2EE (ADR 0002 decision 1) changes where merging happens, and validates the shape.

Under E2EE the server cannot merge, so **all merging is client-side.** Two consequences:

- It **removes** one of `yrs`'s advantages retroactively (there is no server-side CRDT
  logic to reuse, and Yjs wire compatibility only matters for a future web editor that
  would also have to be E2EE). The ADR's choice is more defensible under decision 1 than it
  was under the original framing.
- It **adds** a constraint: the client merges without any server-side compaction help. The
  well-trodden shape here is **snapshot + update log**, as in the `secsync` architecture:
  the server stores an opaque encrypted snapshot plus encrypted update blobs referencing
  it, ordered and validated purely by **unencrypted, signed metadata** (document id,
  snapshot id, client public key, per-client monotonic counter). Without periodic
  snapshots, a document with thousands of updates requires downloading and decrypting every
  one of them on first open — which is a startup-budget failure, not just a nicety.

Full design is written up in `sync-architecture.md` §6 and §10.

### F-6 — The existing effort estimate for "own RGA" does not cover any of the above.

`sync-architecture.md` §6 estimated **6–8 weeks including a convergence fuzzer**. That
estimate covers classic RGA plus the three-property test set. It does **not** cover:

| Not in the 6–8 ew estimate | ew |
|---|---:|
| Fugue rather than RGA (rightOrigin disambiguation, its tie-break rules) | +1 |
| Executable reference model + differential harness against the optimised implementation | +2 |
| Non-interleaving property with a dedicated backward-insertion generator | +1 |
| Tombstone GC with causal stability + the 90-day horizon rule + reject-path tests | +2 |
| Causal-delivery buffering, bounded, with hostile-input tests | +1 |
| Encrypted snapshot/update framing (client-side merge, no server compaction) | +2 |
| **Revised total** | **15–17 ew** |

## 4. Verdict against the original pass criterion

| Criterion | Result |
|---|---|
| "A decision memo + working prototype of the winner" | The *language* decision is made (`adr/0002`). **The algorithm decision is not, and this spike says the ADR chose the wrong one.** No prototype exists. |

**SP-6 is not complete and cannot be completed as written**, because it was scoped to
choose between two options one of which is now excluded. It must be re-scoped as below.

## 5. Recommendation

### R-1 — Build **Fugue**, not classic RGA. *(Requires product-owner confirmation.)*

`adr/0002` decision 2 names "RGA (Replicated Growable Array)" specifically. On the evidence
in F-1 and F-2, that should be amended to a **Fugue-family sequence CRDT**. This preserves
everything the ADR actually wanted — custom, C++, no Rust, within the spec §2 mandate — and
changes only the merge-ordering rule, at roughly +1 ew, in exchange for eliminating a
published defect class.

**This is flagged as a NEW decision needing sign-off.** It is a change to a signed-off ADR,
and it should be made deliberately rather than absorbed silently into implementation.

### R-2 — Test for **four** properties, not three.

Add **non-interleaving** to convergence, idempotence and commutativity. It needs its own
generator, because random operation interleaving will almost never produce the shape that
triggers it:

> Two replicas each insert a known contiguous token at the same position, **one of them
> built backward (right-to-left)**; merge; assert each token appears contiguously in the
> result.

Without this generator the property is untested regardless of how many fuzz iterations run.

### R-3 — Build a naive reference model **first**, and differentially test against it.

- ~200 lines, O(n²), a plain list of `(id, leftOrigin, rightOrigin, tombstone)`, written to
  read like the published algorithm and optimised for nothing.
- The production implementation (run-length blocks, index cache, tombstone GC) must produce
  **byte-identical documents** to the reference for every generated interleaving.
- This is what makes F-3 usable: we inherit the algorithm's published correctness argument
  and reduce our own obligation to "the fast one equals the obvious one" — a much easier
  thing to test than "the fast one is correct".

### R-4 — Specify tombstone GC and causal delivery before writing the merge code.

Per F-4: causal-stability GC bounded by the **same 90-day horizon as the change-log
compaction** in `sync-architecture.md` §4.3; clean rejection (never UB) for operations
referencing collected tombstones; bounded buffering for out-of-causal-order operations.

### Revised SP-6 scope and box

**Box: 5 d → 8 d.**

| Task | d |
|---|---:|
| Reference model + the four property generators (incl. backward-insertion) | 2 |
| Prototype Fugue core in C++ (insert/delete/merge, no optimisation) | 3 |
| Differential harness: prototype vs. reference over generated interleavings | 2 |
| Encrypted snapshot/update framing sketch (F-5), validated against `sync-architecture.md` §10 | 1 |

**Exit criterion:** the prototype passes all four properties against the reference over
≥ 10⁶ generated operations, *including* the backward-insertion generator, and a written
memo states the GC horizon rule.

### Estimate impact

- **Phase 13 (Synchronisation): 76 ew → ~85 ew.** The sequence CRDT line item moves from
  6–8 ew to 15–17 ew (F-6). Phase 13 was already the highest-risk phase and the only one
  rated "Very High" on correctness risk; this does not change its rating, it prices it.
- **R-15** ("Rust toolchain adds unacceptable mobile build friction") is **closed by
  decision** — `adr/0002` removed Rust from the mobile build. It is replaced by:
- **New risk R-29: "the hand-written sequence CRDT has a merge defect that converges."**
  Probability M × Impact 5 = **10**. Owner: Sync lead. This is the highest-impact technical
  risk in the project after R-01, because the failure mode is silent corruption of user
  text that version history cannot obviously repair. Mitigations: R-1 through R-4 above,
  plus the 7-day 5-device soak already in the Phase 13 gates.

## Sources

- [Kleppmann & Gomes et al., *Interleaving anomalies in collaborative text editors* (PaPoC 2019)](https://martin.kleppmann.com/papers/interleaving-papoc19.pdf)
- [Weidner, Kleppmann et al., *The Art of the Fugue: Minimizing Interleaving in Collaborative Text Editing* (arXiv 2305.00583)](https://arxiv.org/abs/2305.00583)
- [Loro — Introduction to Loro's Rich Text CRDT (RGA backward interleaving; Fugue adoption)](https://loro.dev/blog/loro-richtext)
- [Gomes, Kleppmann, Mulligan & Beresford, *Verifying Strong Eventual Consistency in Distributed Systems* (OOPSLA 2017)](https://www.cl.cam.ac.uk/~arb33/papers/GomesEtAl-VerifyingSEC-OOPSLA2017.pdf)
- [trvedata/crdt-isabelle — the Isabelle/HOL development (RGA, Counter, OR-Set)](https://github.com/trvedata/crdt-isabelle)
- [Kleppmann, *Verifying CRDTs in Isabelle*](https://martin.kleppmann.com/2017/10/25/verifying-crdt-isabelle.html)
- [nikgraf/secsync — architecture for end-to-end encrypted CRDTs](https://github.com/nikgraf/secsync)
