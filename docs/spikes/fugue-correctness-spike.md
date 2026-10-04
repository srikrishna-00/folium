# Fugue Correctness Spike — findings

**Status:** Complete (prototype built, tested, measured). Code is a SPIKE and is not wired
into any product target.
**Date:** 2026-10-04
**Discharges:** `adr/0002` Consequences, "Custom CRDT decision means Phase 13 needs an
explicit correctness spike before it is considered de-risked"; `spikes/SP-6-findings.md`
R-1/R-2/R-3 and the re-scoped SP-6 tasks 1–3.
**Does NOT discharge:** re-scoped SP-6 task 4 (encrypted snapshot/update framing sketch),
R-4 (tombstone GC and causal-delivery specification). Both remain owed.
**Code:** `core/sync/spike/` (library), `tests/sync_spike/` (suite). 19 pre-existing CTest
entries plus 5 new ones; `-DUAPP_BUILD_SYNC_SPIKE=OFF` removes all of it and leaves the
19-test suite byte-for-byte as it was.

> **Headline, in three lines.**
> 1. Fugue works, and the defect SP-6 found in RGA is demonstrably absent — shown by
>    reproducing the defect in a deliberate RGA control and watching the same detector fire
>    on RGA and not on Fugue.
> 2. **`adr/0002` and `sync-architecture.md` §6.1 both overstate the guarantee.** Plain Fugue
>    is *not* maximally non-interleaving; FugueMax is. There is a concrete two-string witness
>    in §4.3 below. One of the two documents' wording, or the algorithm, has to change.
> 3. **The ADR's chosen verification technique has a blind spot, and the spike measured it.**
>    Reference-model differential testing cannot detect a bug in operation *generation*. The
>    one injected fault that the differential oracle missed entirely was caught only by an
>    independent strong-list-specification oracle that the test plan does not currently
>    require. §5 has the numbers.

---

## 1. What was built

Three pieces, in `core/sync/spike/`. None of them links `uapp::core_common`, so the build
graph — not a comment — is what guarantees the spike cannot reach the document model, the
storage adapter or the C ABI.

### 1.1 `fugue.h` / `fugue.cpp` — the algorithm

**Fugue** exactly as Algorithm 1 of Weidner & Kleppmann, *The Art of the Fugue*
(arXiv:2305.00583), page 5. Paper line numbers are quoted in the source at each step so a
reviewer can diff code against paper rather than against intent. The paper's PDF was read for
this; the algorithm was not reconstructed from secondary descriptions, because the two details
that matter most — that the right origin is the successor **in the traversal that includes
tombstones** (line 24), and that the fallback is a *left* child of that right origin (line 28)
— are exactly the details that summaries drop.

**FugueMax** is also implemented: Algorithm 1 plus Definition 6 (§5.3), where right-side
siblings are ordered by the *reverse* order of their right origins, ties broken by id. It is
built because `adr/0002` claims "maximal non-interleaving", and that property is FugueMax's,
not Fugue's. See §4.3.

Three documented departures from the pseudocode:

| | Departure | Why |
|---|---|---|
| 1 | Traversal is **iterative** with an explicit heap stack | Algorithm 1 lines 14–20 are recursion. See §6.1 — this one is a finding, not a style choice. |
| 2 | **Bounded** causal-delivery buffer (`kMaxPendingOperations = 4096`) | `sync-architecture.md` §6.2: buffer, never assume, and bound the buffer. Lets the suite deliver a log in *any* permutation, which the paper's causal-broadcast assumption would not allow. |
| 3 | Explicit limits and malformed-operation rejection | Every operation is, in production, attacker-influenced bytes from another device. |

Not implemented, and not faked: tombstone GC, run-length/waypoint compression, persistence,
encryption, transport. All Phase 13 production work.

### 1.2 `reference_model.h` / `.cpp` — three oracles

`adr/0002` requires non-interleaving to be verified "via reference-model differential testing
against a known-correct sequence CRDT implementation, **not just self-consistency checks**".
Three oracles, because one of them turned out to be the only one that works (§5):

- **`reference_order()` — the differential model.** Computes the Fugue/FugueMax order by a
  route that shares no code with `fugue.cpp`. Where the implementation maintains sorted
  sibling lists *incrementally* as operations arrive and reads the order off a tree walk, the
  model takes the whole operation set at once, precomputes each element's root path, and
  derives the order from an O(n²) pairwise comparator with no traversal and no incremental
  state. SP-6 R-3 asked for "~200 lines, O(n²), optimised for nothing"; this is that.

  It also **proves its own comparator is a total order** as a side effect: ranks are computed
  by brute force as "how many elements compare less than me", and the ranks must come out as a
  permutation of 0..n−1. If two elements compare equal the ranks collide and the model refuses
  to answer rather than silently picking one of several possible outputs. (That is also why the
  ranks are counted by hand rather than handed to `std::sort`, where an inconsistent comparator
  is undefined behaviour rather than a reported failure.)

- **`satisfies_strong_list_spec()` — the specification oracle.** The only oracle here that is
  independent of Fugue altogether. At generation time the harness records, for every insert,
  the ids the generating replica could actually see and where in that list it inserted. The
  oracle then checks the merged order against condition (b) of the Attiya et al. strong list
  specification: everything then before the position must precede the new element, everything
  then from the position onwards must follow it. Its first check — "the inserted element is
  present in the merged order at all" — is also the **no-loss** property the roadmap lists.

- **`maximal_run_count()` — the non-interleaving detector.** Non-interleaving is a property of
  the merged *text*, not of the tree: a replica's contribution must appear as one contiguous
  run.

### 1.3 `rga.h` / `rga.cpp` — a deliberately defective control

Classic RGA: left origin only, depth-first **pre**-order, same-origin children in descending
Lamport-timestamp order.

This exists because "assert Fugue does not interleave" passes just as happily when the detector
is broken, when the scenario does not actually trigger the defect, or when the merge never
happened. The suite therefore asserts that the **same detector fires on RGA**, and calibrates
the RGA model against the paper's own Appendix A.1.8 (which states the expected output for the
minimal backward example). That turns P4 from a claim into a measurement.

---

## 2. What was actually run

`clang` (Apple, `-std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Wconversion
-Wsign-conversion -Wshadow -Werror`) — the same posture as production, because a spike that
compiled under weaker rules would not answer "is this implementable under our constraints",
which is half the question. Zero warnings.

**25 test cases, 25 pass.** Default run (PR path): **9.7 s**.

Soak run, `UAPP_FUGUE_TRIALS=60000`, **115 s**, 25/25 pass:

| Suite | Trials | Operations generated | of which deletes |
|---|---:|---:|---:|
| Fugue / uniform | 60 000 | 913 656 | 135 764 |
| Fugue / prepend-biased | 60 000 | 914 436 | 136 082 |
| Fugue / tombstone-heavy (45 % deletes) | 60 000 | 914 561 | 314 928 |
| FugueMax / uniform | 60 000 | 915 094 | 136 048 |
| FugueMax / prepend-biased | 60 000 | 915 021 | 135 458 |
| FugueMax / tombstone-heavy | 60 000 | 914 457 | 315 598 |
| **Total** | **360 000** | **5 487 225** | **1 173 878** |

Every one of those histories is then delivered **five times** — three different random
*topological* orders of its causal dependency DAG, plus the same log delivered twice to one
replica — so the soak performs roughly **27 million operation applications**, and each history
is additionally checked against the differential model and the specification oracle. This
clears SP-6's exit criterion (≥ 10⁶ generated operations) by a factor of five, and the
roadmap's Phase 13 "10⁶ simulated operations" line by the same margin — for the *naive*
implementation, which is the important caveat in §7.

Alongside that, in the same run:

- **120 000** scripted two-replica token scenarios for P4, **89 781** of them with at least one
  token built right-to-left;
- **116 344** concurrent operation pairs applied in both orders (P3, isolated);
- **60 000** histories delivered in a plain shuffle that ignores causality altogether, each
  required to end **byte-identical** to a causal delivery of the same log — tombstone
  placement included, not just visible text;
- **60 000** unordered-within-token trials, measured rather than asserted (§4.4).

**ASan + UBSan** (`-DUAPP_ENABLE_SANITIZERS=ON`), `UAPP_FUGUE_TRIALS=8000`, 56 s: 25/25 pass,
731 881 generated operations, **no reports**.

Full project `ctest` in the same build tree: **24/24 pass** (19 pre-existing + 5 new).
`-DUAPP_BUILD_SYNC_SPIKE=OFF`: **19 tests registered**, i.e. the spike is excludable and the
pre-existing suite is untouched.

---

## 3. P1 / P2 / P3 — the three properties that were always planned

All three hold across the soak above. Two deliberate strengthenings are worth recording,
because both were cheap and both are the difference between a convergence test that means
something and one that does not:

- **Convergence is asserted on the tombstone-inclusive element order, not only on the visible
  text.** Two replicas can agree on `text()` while disagreeing about where a tombstone sits —
  and that disagreement becomes visible text the moment a concurrent insert attaches next to
  the tombstone. A text-only convergence check passes on a replica that is already wrong.
- **Commutativity is tested in isolation, not inferred from convergence.** The suite finds a
  pair of operations that are *maximal* in the dependency DAG (nothing depends on either) and
  *concurrent* (neither is in the other's causal history), delivers the entire rest of the log
  — which is causally closed without them — and then applies the pair in both orders from that
  identical state.

---

## 4. P4 — non-interleaving. Concrete results.

### 4.1 The minimal case, straight from the paper's appendix

Replica A inserts `b`, then **prepends** `a` before it. Concurrently replica B inserts `x`.

| Algorithm | Merged result | Interleaved? |
|---|---|---|
| **Classic RGA** | **`axb`** | **Yes** — and this is the exact string the Fugue paper's Appendix A.1.8 predicts, so the control is calibrated against the literature. |
| Fugue | `abx` | No |
| FugueMax | `abx` | No |

All replicas converge in every case. RGA converges on `axb`. That is SP-6 F-1 in one line.

### 4.2 The shopping-list case (the paper's Figure 2), the one users would hit

A shared first element `S`; both users then **prepend** a three-item block after it. User one
builds `abc` right-to-left, user two builds `XYZ` right-to-left.

```
replica 1 sees:  Sabc
replica 2 sees:  SXYZ

merged under classic RGA:   SXaYbZc     <- runs of "abc": 3, runs of "XYZ": 3
merged under Fugue:         SabcXYZ     <- runs of "abc": 1, runs of "XYZ": 1
merged under FugueMax:      SabcXYZ     <- runs of "abc": 1, runs of "XYZ": 1
```

`SXaYbZc` is the project's `HWeolrllod` — every replica agrees on it, every one of P1, P2 and
P3 passes on it, and the user's shopping list is destroyed. The suite asserts **both** halves:
that the detector reports three runs for RGA and one run for Fugue. Without the RGA half the
Fugue half would be unfalsifiable, which is precisely the criticism SP-6 made of the ADR's
original three-property plan.

Also asserted, all passing: the forward case (two users typing words at the same position), and
the **three-replica backward case** (paper A.1.2 / A.1.9 — the case where Yjs 13.6.8
interleaves and Fugue is proven not to).

### 4.3 FINDING: plain Fugue is not maximally non-interleaving, and the ADR says it is

`adr/0002`'s amendment says "Fugue provides maximal non-interleaving". `sync-architecture.md`
§6.1 repeats it. **The paper does not say that.** It says FugueMax satisfies maximal
non-interleaving, that plain Fugue is forward and backward non-interleaving but may interleave
more than necessary in rare multi-concurrent situations, and that it "believe[s] that in
practice, Fugue's simpler algorithm is worth its small amount of additional interleaving".

The spike reproduces the paper's own distinguishing execution (its Figure 7) and gets the
predicted split:

```
Five replicas. r1, r2, r3 concurrently insert A, B, C at index 0 (ids order A < B < C).
r4 learns {A, B} only and inserts Y between them   -> "AYB"
r5 learns {A, C} only and inserts X between them   -> "AXC"
                                                      (Y's right origin is B, X's is C)

implementation  Fugue = "AYXBC"      reference model  Fugue = "AYXBC"
implementation  FugueMax = "AXYBC"   reference model  FugueMax = "AXYBC"
```

`AXYBC` is the *unique* order maximal non-interleaving permits here (paper §5.2). Plain Fugue
returns `AYXBC`, which backward-interleaves one extra pair. Both orders satisfy the strong list
specification — which is exactly why a specification oracle alone cannot choose between them,
and why P4 needs its own assertions.

The paper also proves that wherever the two differ, *some* interleaving is unavoidable for any
algorithm. So this is not a defect; it is a documentation defect. **Two acceptable resolutions,
and the decision is the product owner's:**

- **(a) Keep plain Fugue** (the paper's own recommendation, and the cheaper implementation) and
  **correct the wording** in `adr/0002` and `sync-architecture.md` §6.1 from "maximal
  non-interleaving" to "forward and backward non-interleaving; not maximal". This is the
  recommended option.
- **(b) Adopt FugueMax** and keep the stronger claim. Cost, measured here: each right child
  must additionally carry its right origin on the wire (8 more bytes **per character**), the
  right origin becomes an extra causal dependency that widens the out-of-order buffering
  window, and sibling ordering needs the *list positions* of two right origins — an O(n)
  traversal per delivered right child in a naive implementation, i.e. production would need a
  position index it does not otherwise need.

Leaving the documents as they are is not one of the options: the whole point of SP-6 was that
an overstated correctness claim is how this defect class survives a green test suite.

### 4.4 Measured, not asserted: typing out of order inside a token

The paper's non-interleaving guarantee covers tokens built in one direction. Real editing is
messier — a user fixes a typo in the middle of a word they just typed. 60 000 trials in which
each replica inserts its token's characters at random positions *within* the token's own
extent: **0 interleaved (0.00 %)**.

That is reported as a measurement, not a guarantee, and it has a structural explanation worth
writing down: a single replica's insertions within one contiguous extent always land in one
subtree, because at every step either the left origin has no right children (so the new node
becomes its right child, inside the subtree) or the right origin is a descendant of the left
origin (so the new node becomes its left child, also inside the subtree). The paper's
inevitable-interleaving cases need three or more *interacting* replicas, as in §4.3. The suite
deliberately does **not** assert contiguity for those, because demanding it would be demanding
something no algorithm can deliver.

---

## 5. What the mutation tests found — the most useful result in this document

Everything passed on the first run. That is not evidence; it is a reason for suspicion. So five
faults were injected into `fugue.cpp` one at a time, each rebuilt (with the output binary's
hash verified to have changed — see §6.4) and the whole suite re-run at 500 trials per random
suite:

| | Injected fault | Caught? | By what |
|---|---|---|---|
| M1 | Same-side sibling tie-break reversed (descending id) | **yes**, 7 cases | the differential model; convergence still held |
| M2 | Right-origin fallback removed — i.e. regress Fugue to RGA's left-origin-only placement | **yes**, 14 cases | P4 scenarios, basic editing, differential, spec |
| M3 | Right origin taken from the **visible** successor instead of the tombstone-inclusive one (Algorithm 1 line 24) | **initially NO** | see below |
| M4 | Traversal made pre-order (node before its left children) | **yes**, 13 cases | P4 scenarios, basic editing, differential, spec |
| M5 | FugueMax's right-origin comparison not reversed | **yes**, 4 cases | FugueMax suites and the Figure 7 case |

**M3 is the finding.** It survived the entire suite as originally written. Two separate things
were wrong, and both matter to Phase 13:

**(a) The differential oracle is structurally blind to it, and always will be.** The reference
model reads the `parent` and `side` recorded *in each operation*. M3 is a bug in how an
operation is **generated** — it picks the wrong parent — and the model then faithfully
reproduces the wrong parent. Both implementations agree, and they agree on a corrupted order.

> This is `adr/0002`'s stated mitigation failing on its own terms. The amendment requires
> non-interleaving to be verified "via reference-model differential testing against a
> known-correct sequence CRDT implementation, not just self-consistency checks" — and
> reference-model differential testing *is* a self-consistency check with respect to the
> operation set. It cannot see a generation bug. Only the **strong-list-specification oracle**,
> which is derived from what the generating replicas actually saw and not from the algorithm at
> all, caught M3.

**(b) The generator was under-powered for tombstones.** M3 needs a tombstone in a specific
place. Detection, measured:

| Generator | Delete rate | Trials needed to catch M3 |
|---|---:|---:|
| `Fugue/uniform` (original) | 18 % | ~1 000 |
| `Fugue/tombstone-heavy` (added because of this) | 45 % | ~50 |

A 20× improvement for a one-line configuration change. Two tombstone-heavy suites were added
and the default trial count was raised from 2 500 to 5 000 per suite, which keeps the PR-path
run at 9.7 s and puts M3 comfortably inside the detection window. After those changes **all
five mutations are caught at 500 trials per suite**.

---

## 6. Problems found during development, honestly

Nothing in this section is a defect in the Fugue *algorithm*. They are implementation and
methodology problems, which is what a spike is for.

### 6.1 Algorithm 1's recursive traversal cannot ship. Measured.

The paper writes `traverse()` (lines 14–20) as recursion. A direct transcription
stack-overflows on ordinary documents, because **a Fugue tree is a spine for sequentially typed
text**: every character becomes the right child of the one before it. Measured, `-O2`:

```
n= 1000  depth= 1000   sequential-append total=    8.6 ms
n= 2000  depth= 2000                           =   24.9 ms
n= 4000  depth= 4000                           =   63.2 ms
n= 8000  depth= 8000                           =  273.9 ms
n=16000  depth=16000                           = 1166.6 ms
```

Depth equals the element count **exactly**, at every size, forward typing and backward typing
alike. A 100 000-character note is a 100 000-deep tree — not an adversarial input, just a long
note. At a conservative 48–64 bytes per frame that is 5–6 MB of stack for one tree walk, which
exceeds the 512 KB default stack of a secondary thread on both iOS and Android by an order of
magnitude. (That last step is arithmetic, not a measurement; the measured facts are the depths.)

The spike's traversal, `tree_depth()` and the reference model's path walk are therefore all
iterative with explicit heap stacks, and the 16 000-deep case runs without incident.

**This is a new Phase 13 obligation that `sync-architecture.md` §6 does not currently state:
every walk of the CRDT tree must be iterative or explicitly depth-bounded, and the document
should say so where it already says tombstones must be bounded.** It also makes
`core/blocks`'s `kMaxBlockDepth = 64` guard a useful precedent and a bad fit — the block tree's
depth is a user-authoring choice, the CRDT tree's depth is the document length.

The same table also prices the naive implementation: total cost for sequential typing is
O(n²) (each doubling of n roughly quadruples the time), because every local insert walks the
whole traversal to find its left origin. That is expected for a model "optimised for nothing",
and it is the work the paper's own optimised implementation does with waypoints. It is now
priced rather than assumed.

### 6.2 A real bug, caught by reasoning before the first run

`tree_depth()` was first written as a single pass over the in-order traversal, deriving each
node's depth from its parent's. That is wrong: in an **in-order** traversal a left child is
visited *before* its parent, so the parent's depth is still unset when the child reads it. It
was rewritten as a top-down walk before the first build. Worth recording because it is the
generic trap in this data structure — "iterate the traversal" is the wrong default for anything
that needs parents first, and the production implementation will be full of such passes.

### 6.3 Two test bugs that initially hid a real distinction

The Figure 7 scenario (§4.3) first produced the *same* answer for Fugue and FugueMax, which
would have made the differential oracle look variant-blind. The scenario was wrong, not the
code: with three replicas and ids assigned in generation order, Fugue's id tie-break happened
to coincide with FugueMax's right-origin order. The paper's own analysis says the two differ
only when the id order goes the other way, so the scenario was rebuilt with five replicas and
deliberate id assignment. It then reproduced the paper's predicted split exactly.

A second, smaller one: the scenario builder originally merged all replicas before the test
could assert the two divergent pre-merge states (`AYB` and `AXC`) the paper names, so selective
single-operation delivery was added to the harness.

### 6.4 A methodology failure worth writing down

Partway through the mutation matrix, the suite reported four FugueMax failures and a wrong
Figure 7 result. The cause was not the code: a mutation had been reverted in the source, but
`make` had skipped the rebuild because the restore and the previous compile fell inside the same
filesystem-timestamp second, so a stale binary ran against pristine source. The whole mutation
matrix was re-run with a forced rebuild **and a check that the output binary's hash had actually
changed**, and only those re-run numbers are reported in §5.

This is logged because the failure mode — "the test told me something false and I nearly
believed it" — is the same failure mode the entire spike exists to guard against, and because
any Phase 13 CI job that mutation-tests the CRDT needs the same hash check.

---

## 7. Assessment: is Fugue de-risked enough for Phase 13?

**The algorithm decision: yes.** With evidence, not assertion.

- Fugue is implementable in the project's actual constraints (C++17, no exceptions, no RTTI,
  `-Werror` with the full warning set) in roughly 400 lines for the core.
- The specific defect that caused the RGA → Fugue amendment is reproduced in a control and
  demonstrated absent in Fugue, with the same detector on both sides.
- The implementation reproduces a non-trivial, variant-distinguishing published result
  (Figure 7) that nothing in the test suite was fitted to.
- Four properties hold over 5.49 million generated operations and ~27 million applications,
  clean under ASan and UBSan.
- The test suite is demonstrated able to **fail**: 5/5 injected faults caught.

**The implementation risk: no, and the spike narrowed rather than removed it.** What Phase 13
ships is not what was tested here. The optimised implementation (waypoints, index caches),
tombstone GC with causal stability, the encrypted snapshot/update framing and persistence are
all unwritten, and SP-6 F-3 is explicit that optimisations are historically where hand-written
CRDTs break. What the spike *does* give Phase 13 is the thing that was missing: a verified naive
baseline and a harness with proven teeth, so the optimised implementation's obligation reduces
to "byte-identical to the model", which is a far easier thing to establish than "correct".

**Three things must change before Phase 13 starts, and all three come out of this spike:**

1. **Resolve the maximal-non-interleaving overstatement** (§4.3). Recommended: keep plain
   Fugue, correct `adr/0002` and `sync-architecture.md` §6.1. This is a product-owner decision
   because it changes a signed-off ADR's wording and, under option (b), the wire format.
2. **Add the specification oracle to `sync-architecture.md` §6.2's required test set.** The
   section currently requires a reference model and differential testing. §5 shows that
   combination certifies M3 as correct. The reference model catches *integration* bugs; only an
   oracle derived from the operation history catches *generation* bugs. Both are needed, and the
   document should say which catches what.
3. **State the iterative-traversal obligation** (§6.1), next to the existing tombstone and
   buffering obligations, with the measured depth-equals-length result as its justification.

**Still owed, unchanged by this spike:** the encrypted framing sketch (re-scoped SP-6 task 4)
and the tombstone-GC / causal-stability specification (SP-6 R-4), which `sync-architecture.md`
§6.2 says must be written **before** the merge code. Neither was in this spike's scope and
neither should be assumed done.

**Effort:** the spike itself came in well under the 7 days the re-scoped SP-6 boxed for tasks
1–3, which supports F-6's "+1 ew for Fugue rather than RGA" as realistic. It does **not** reduce
the 15–17 ew CRDT line item or the 93 ew Phase 13 figure: roughly 4 ew of that line item's
risk-bearing work now exists as *spike* code, and promoting it is a rewrite against the real
document model, not a move.

---

## 8. Reusing this harness in Phase 13

The suite is built so the production implementation can be dropped in behind it:

- `reference_order()` is the differential oracle for the optimised implementation.
- `satisfies_strong_list_spec()` needs only an insert's then-visible id list; any
  implementation can supply that.
- `maximal_run_count()` plus the scripted scenarios are P4, ready as written.
- `rga.h` stays as the control. **It must keep failing.** A green run of
  `uapp.sync_spike.rga_control` is the only thing that proves P4 can still fail at all, and it
  is the first test to look at if P4 ever goes suspiciously quiet.
- `UAPP_FUGUE_TRIALS` makes the PR path (9.7 s) and the nightly soak (115 s for 5.49 M
  operations) the same binary, per `testing-strategy.md`'s split between the two.

## Sources

- [Weidner & Kleppmann, *The Art of the Fugue: Minimizing Interleaving in Collaborative Text Editing*, arXiv:2305.00583](https://arxiv.org/abs/2305.00583) — Algorithm 1 (p. 5), Definition 6 (§5.3), Table 1 (§2.3), Figures 1/2/6/7, Appendix A.1.2 / A.1.8 / A.1.9
- [Kleppmann, Gomes et al., *Interleaving anomalies in collaborative text editors* (PaPoC 2019)](https://martin.kleppmann.com/papers/interleaving-papoc19.pdf)
- [Attiya et al., *Specification and complexity of collaborative text editing* (PODC 2016)](https://dl.acm.org/doi/10.1145/2933057.2933090) — the strong list specification the §1.2 oracle checks
- `docs/spikes/SP-6-findings.md`, `docs/adr/0002-phase0-signoff-decisions.md`,
  `docs/sync-architecture.md` §6, `docs/risk-register.md` R-29
