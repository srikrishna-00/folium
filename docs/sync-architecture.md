# Sync Architecture

**Status:** Phase 0 draft — awaiting human sign-off
**Version:** 0.0.0
**Date:** 2026-09-29

Implements spec §22. Requirements taken as binding: device identity, object IDs, version
numbers, change logs, sync queues, conflict detection, conflict resolution, retry, offline
mutations, encryption, eventual consistency, and — the one that shapes everything —
**"Never overwrite changes silently."**

---

## 1. The problem, precisely

N devices, each fully functional offline, mutate a shared object graph. On reconnect the
system must converge to a state that (a) is the same on every device, (b) loses no user
intent, and (c) never silently discards a change.

Those three cannot all be had automatically for all data. Convergence without loss requires
either a merge function that is mathematically guaranteed to commute, or a human. So the
design question is not "which algorithm" but **"which data gets which treatment."**

## 2. Per-datatype strategy

This is the central decision of the document. One algorithm for everything is the mistake.

| Data | Strategy | Why |
|---|---|---|
| Scalar object fields (title, icon, page-setup, property config) | **LWW register** ordered by `(lamport, device_id)` | Concurrent edits to the same scalar are genuinely ambiguous. LWW is correct *provided the loser is preserved in version history and surfaced*, which is what makes it non-silent. |
| Block/page tree structure (parent, order) | **Tree CRDT**: fractional-index ordering + LWW parent with a cycle-breaking rule | Ordering must commute. Fractional indices make an insert a single-row op and avoid the renumbering storm (`database-design.md` §3.3). |
| Rich text inside a block | **Sequence CRDT (RGA-style)** | Two people editing the same paragraph is the one case where character-level merge is both expected and tractable. |
| Sets (tags, multi-select, members) | **OR-Set** (add-wins, with tombstones) | Add-wins matches intent: concurrent add+remove usually means the adder had newer information. |
| Counters | Not used | Avoided deliberately — no user-facing counter needs CRDT semantics; derive instead. |
| Database rows/cells | **Per-cell LWW** | A row is not a document. Cell-level granularity means two people editing different columns never conflict. |
| Deletes | **Tombstone + LWW**, delete-wins over concurrent edit, **but the edit is preserved** | A concurrent edit to a deleted object is surfaced as a restorable conflict, never dropped. |
| File blobs | **Immutable, content-addressed** | A "conflict" on a file is impossible: a new version is a new blob. The *pointer* conflicts, which is an LWW scalar. |
| Derived data (FTS, chunks, vectors, caches) | **Not synced** | Recomputed locally. Syncing derived data is how indexes get out of step. |

**Why not "just CRDTs everywhere":** a full CRDT over the whole object graph carries permanent
metadata overhead (tombstones never fully go away), makes permission changes and access
revocation hard to reason about, and gives no better outcome for scalar fields than LWW. We
pay CRDT costs only where CRDTs earn them.

## 3. Identity and clocks

- **Device identity.** Ed25519 keypair generated on first run; private key in
  Keychain/Keystore, non-exportable. `device_id` = UUIDv4. The device registers its public key
  with the server. Every pushed mutation is signed.
- **Object IDs.** UUIDv7, client-generated (`database-design.md` §2). Creation never waits for
  the server.
- **Lamport clock per device.** `lamport = max(local, max(observed)) + 1` on every mutation.
  Gives causal ordering.
- **Version vector per object:** `{device_id → counter}`, CBOR-encoded. This is what makes
  conflict *detection* exact rather than heuristic.

**Wall-clock timestamps are never used for ordering, resolution, or causality.** They exist
only for display. Device clocks are wrong — by minutes routinely, by years occasionally — and
every system that has used them for ordering has lost data doing it.

### 3.1 Conflict detection with version vectors

For an incoming remote version R against local L:

```
if VV(R) ⊒ VV(L)   →  R dominates: fast-forward, apply
if VV(L) ⊒ VV(R)   →  L dominates: ignore R (already have it)
otherwise          →  CONCURRENT: neither dominates → run the merge strategy for the datatype
```

This is exact. A simple integer `rev` cannot distinguish "you are behind" from "you diverged",
which is precisely the distinction that determines whether overwriting is safe.

## 4. Protocol

Server-mediated, client-pull. No peer-to-peer at v1.0.

### 4.1 Change log

The server keeps a per-workspace append-only log with a monotonic `seq`
(`database-design.md` §5.2). A client's pull cursor is a `seq`. That makes pagination cheap,
correct, resumable, and trivially debuggable.

### 4.2 Push

```
POST /v1/sync/push
{ "workspace_id": "...", "device_id": "...",
  "since_cursor": 4412,
  "mutations": [
    { "object_kind": 1, "object_id": "018f...", "op": "update",
      "version_vector": {"dev-a": 12, "dev-b": 7},
      "delta": { ... },                      // CBOR-in-JSON or base64 CBOR
      "lamport": 13,
      "sig": "ed25519:..." }
  ] }
```

Server:
1. Verifies the device is registered and unrevoked, and the signature is valid.
2. Authorises the subject against each object.
3. Validates payload schema and size.
4. For each mutation, compares version vectors against the stored version.
   - dominates → apply, append to the change log, return the new `seq`
   - dominated → return `already_applied`
   - concurrent → **apply the server-side merge for that datatype, and if the datatype is
     human-resolution, store both and return `conflict`**
5. Returns per-mutation results. **Partial success is normal and expected** — one rejected
   mutation must not fail the batch.

```json
{ "results": [ { "object_id":"018f...", "status":"applied", "seq":4419 },
               { "object_id":"018e...", "status":"conflict",
                 "server_version": {...}, "server_vv": {...} } ],
  "cursor": 4419 }
```

### 4.3 Pull

```
POST /v1/sync/pull { "workspace_id":"...", "cursor":4412, "limit":500 }
→ { "changes":[...], "cursor":4519, "has_more":true, "reset_required":false }
```

`reset_required: true` means the client's cursor is older than the log's compaction horizon
(the log is trimmed after 90 days). The client then performs a full resync of that workspace.
Handling this path properly — rather than discovering it in production 90 days after launch —
is a Phase 13 deliverable with its own test.

### 4.4 Ordering guarantees

- Push is strictly ordered per device (the local outbox is a FIFO keyed by `seq`).
- Pull applies changes in `seq` order.
- **Apply is transactional per pull batch**: either the whole batch and the cursor advance
  together, or neither. A crash mid-apply leaves a consistent state, and the batch is refetched.

## 5. Client sync engine

```
┌────────────────────────────────────────────────────────────┐
│ Local mutation                                             │
│   write content + enqueue sync_mutation IN ONE TRANSACTION │  ← outbox pattern
└──────────────┬─────────────────────────────────────────────┘
               │
┌──────────────▼─────────────────────────────────────────────┐
│ Outbox (sync_mutation, durable, FIFO)                      │
│   coalescing: N updates to the same object between pushes  │
│   collapse into one delta (but a delete never coalesces    │
│   away a preceding create — the create must be pushed so   │
│   history exists)                                          │
└──────────────┬─────────────────────────────────────────────┘
               │  when online, batched (≤ 500 ops or ≤ 1 MB)
┌──────────────▼─────────────────────────────────────────────┐
│ Pusher → retry with jittered exponential backoff           │
│   1s, 2s, 4s … 5 min cap. Permanent failures (4xx) move    │
│   the mutation to a dead-letter table and RAISE A VISIBLE  │
│   ERROR. Nothing is dropped quietly.                       │
└──────────────┬─────────────────────────────────────────────┘
┌──────────────▼─────────────────────────────────────────────┐
│ Puller → apply → conflict detection → resolution           │
└──────────────┬─────────────────────────────────────────────┘
┌──────────────▼─────────────────────────────────────────────┐
│ Blob sync (separate, bandwidth-aware, resumable)           │
│   metadata syncs eagerly; bytes sync per the selective-    │
│   sync policy                                              │
└────────────────────────────────────────────────────────────┘
```

**Triggers:** app foreground; local mutation (debounced 2 s); `change_available` hint over the
WebSocket; periodic background refresh (15 min, platform-scheduled); manual pull-to-refresh;
network-reachability transition to online.

**The WebSocket carries hints, never data** (`api-design.md` §7). A dropped socket degrades to
polling. This keeps the failure mode boring.

## 6. CRDT for rich text — **decided: custom C++, not `yrs`**

**`adr/0002` decision 2: a custom C++ sequence CRDT. Rust does not enter the mobile build.**

This closes the option that Phase 0 originally recommended (`yrs`). The rationale is spec
§2's C/C++-first mandate, and the accepted trade is that correctness becomes ours to prove
rather than something we inherit from a battle-tested upstream. That trade is only honest if
the proof obligation is written down concretely, which is what §6.1–§6.3 do.

Consequences of the decision, recorded so they are not rediscovered:

- Rust is out of the mobile toolchain entirely. That also settles **SVG** (`resvg` is
  unavailable → NanoSVG, `file-format-support.md` §7.5) and the **tokenizer** question
  (AIQ-6 → approximate budgeting or a C++ tokenizer, not the Rust `tokenizers` crate).
- Wire compatibility with the Yjs ecosystem is given up. Under E2EE (§10) a future web
  client cannot reuse a server-side Yjs document anyway, so this costs less than it would
  have under the original plan.
- Risk R-15 (Rust build friction) is closed by decision and replaced by **R-29: a merge
  defect that converges** — see §6.1.

### 6.1 Algorithm: Fugue, not classic RGA — **needs confirmation**

Spike SP-6 (`spikes/SP-6-findings.md`) found a specification-level defect in RGA that the
ADR's stated mitigation cannot detect.

**RGA is proved free of *forward* interleaving but exhibits *backward* interleaving**
(Kleppmann et al., PaPoC 2019). Two users concurrently inserting at the same position, where
one builds text right-to-left, get a result interleaved character by character. Every replica
converges — on the same garbage. A convergence test passes. An idempotence test passes. A
commutativity test passes. The user sees `HWeolrllod`.

**Fugue** (Weidner, Kleppmann et al., 2023) defines *maximal non-interleaving* as a
correctness property for replicated lists and achieves it by disambiguating concurrent
inserts on **`leftOrigin` first, then `rightOrigin`** — exactly the hole RGA leaves open. It
is comparable in implementation complexity to RGA. The same work proves that avoiding
interleaving in *every* situation is impossible, so the target is maximal non-interleaving,
and that limit should be stated honestly rather than marketed away.

**Recommendation: amend `adr/0002` decision 2 from "RGA" to "a Fugue-family sequence CRDT".**
Everything the ADR wanted is preserved — custom, C++, no Rust — and the change costs roughly
+1 ew. **This is a change to a signed-off ADR and needs product-owner confirmation.**

> **Correction owed (2026-10-04, `spikes/fugue-correctness-spike.md` §4.3).** The paragraph
> above attributes *maximal* non-interleaving to Fugue. It is **FugueMax's** property (paper
> §5.3, Definition 6). Plain Fugue is forward- and backward-non-interleaving — Table 1 of the
> paper marks it proven on all three interleaving columns — but it is not *maximal*, and the
> spike reproduces the paper's own distinguishing execution (its Figure 7) to show it: Fugue
> returns `AYXBC` where FugueMax returns `AXYBC`, the latter being the unique order maximal
> non-interleaving permits. The paper proves that wherever the two differ, some interleaving is
> unavoidable for *any* algorithm, and recommends plain Fugue in practice. **The prototype
> implements both.** Which one ships is a product-owner decision (see the Open items in
> `adr/0002`); the wording here must change either way.
>
> Measured cost of FugueMax over Fugue: the right origin must travel on the wire with every
> right child (8 more bytes **per character**), it becomes an extra causal dependency that
> widens the out-of-order buffering window, and sibling ordering needs the *list positions* of
> two right origins — an O(n) traversal per delivered right child without a position index.

### 6.2 Correctness-testing plan — four properties, not three (and a fifth check, added 2026-10-04)

For **any** random interleaving of operations across N simulated replicas:

| # | Property | Assertion |
|---|---|---|
| **P1** | **Convergence** | All replicas that have received the same set of operations hold byte-identical documents, regardless of delivery order. |
| **P2** | **Idempotence** | Applying the same operation (or the same change-log range) twice is a no-op. Duplicate delivery is normal on a retrying mobile client, so this is a liveness requirement, not a nicety. |
| **P3** | **Commutativity** | For concurrent operations `a`, `b`: `apply(apply(s,a),b) == apply(apply(s,b),a)`. |
| **P4** | **Non-interleaving** ← **new** | Two replicas each insert a known contiguous token at the same position, **one of them built backward (right-to-left)**; after merge, each token appears **contiguously**. |

**P4 needs a dedicated generator.** Random operation interleaving will essentially never
produce the shape that triggers backward interleaving, so without a generator that
deliberately constructs it the property is untested no matter how long the fuzzer runs. This
is the single most important line in this section.

Supporting obligations, from SP-6 F-4 — these are where hand-written CRDTs actually break:

- **Reference model first.** A ~200-line, O(n²), deliberately unoptimised implementation
  written to read like the published algorithm. The production implementation (run-length
  blocks, index caches, tombstone GC) is **differentially tested to produce byte-identical
  documents to the reference** for every generated interleaving. This lets us inherit the
  algorithm's published correctness argument and reduces our own obligation to "the fast one
  equals the obvious one" — a far easier thing to establish than "the fast one is correct".
  (An Isabelle/HOL development with a machine-checked SEC proof for RGA already exists —
  Gomes, Kleppmann et al., OOPSLA 2017 — so the algorithm-level argument is not original
  research. Our optimisations are.)

  > **Not sufficient on its own (2026-10-04, `spikes/fugue-correctness-spike.md` §5).**
  > A reference model reads the parent and side recorded **in each operation**, so it catches
  > *integration* bugs and is structurally blind to *generation* bugs. The spike injected a
  > fault that picks the wrong parent — the right origin taken from the visible successor
  > instead of the tombstone-inclusive one, i.e. Algorithm 1 line 24 — and the differential
  > oracle certified the result as correct, because both implementations faithfully reproduced
  > the wrong parent. **P5 below is therefore mandatory, not optional.**

- **P5 — the strong list specification, checked from the history and not from the algorithm.**
  Record, for every insert, the element ids the generating replica could actually see and the
  index it inserted at. The merged order must then be a linear extension of those constraints:
  everything then before the position precedes the new element, everything then from the
  position onwards follows it (Attiya et al., condition (b)). Its first check — that the
  inserted element is present in the merged order at all — is also the **no-loss** property.
  In the spike this was **the only oracle that caught the wrong-parent fault above.**
- **P4 and P5 need a defective control.** "Assert our CRDT does not interleave" passes just as
  happily when the detector is broken, when the scenario does not trigger the defect, or when
  the merge never happened. Keep classic RGA in the test tree and **assert that it FAILS** the
  non-interleaving check — the spike's control merges the backward shopping-list shape to
  `SXaYbZc`, which is both convergent and corrupt. If P4 ever goes quiet, the control is the
  first thing to check.
- **Generators must include a tombstone-heavy profile.** Measured: at an 18 % delete rate the
  spike's suite needed ~1 000 trials to find the wrong-parent fault; at 45 % it needed ~50.
- **Every walk of the CRDT tree must be iterative or explicitly depth-bounded.** Measured: a
  Fugue tree's depth equals the document length *exactly* — sequentially typed text is a spine,
  so a 16 000-character note is a 16 000-deep tree. The published algorithm's recursive
  traversal (its lines 14-20) therefore overflows the 512 KB default stack of a secondary
  thread on iOS and Android at ordinary document sizes. This is a long note, not an adversarial
  input, and it is not covered by any bullet above.
- **Causal delivery.** An insert may arrive before the operation defining its origin. The
  implementation **buffers**; it never assumes. The buffer is **bounded**, and overflow
  escalates visibly rather than pinning memory (a hostile or buggy device otherwise has a
  free memory-exhaustion primitive — cf. T-305).
- **Tombstone GC.** Tombstones may be collected only when **causally stable** — acknowledged
  by every known replica. With devices that may be offline indefinitely, causal stability may
  never be reached. **GC therefore uses the same 90-day horizon as change-log compaction
  (§4.3):** a device absent beyond it is excluded from the stability calculation and takes
  the `reset_required` full-resync path. An operation referencing a collected tombstone is
  **rejected cleanly** — never undefined behaviour — and that rejection path has its own test.

### 6.3 Effort

`sync-architecture.md` previously estimated own-RGA at 6–8 ew including a convergence fuzzer.
That figure covered classic RGA and the three-property set only. With Fugue, the reference
model and differential harness, P4, GC with causal stability, causal-delivery buffering and
the encrypted framing in §10, the line item is **15–17 ew** (`spikes/SP-6-findings.md` F-6),
which is why Phase 13 is re-baselined from 76 ew to ~85 ew.

## 7. Conflict resolution

### 7.1 Automatic (no user involvement)

| Case | Resolution |
|---|---|
| Disjoint fields of the same object | Field-wise merge. Not a conflict. |
| Rich text in the same block | Sequence CRDT merge. |
| Tag/member set add + remove | OR-Set, add-wins. |
| Different cells of the same database row | Cell-wise merge. |
| Concurrent inserts at the same position | Fractional index + `device_id` tie-break — deterministic, both survive. |
| Move creating a cycle (A→B and B→A concurrently) | Deterministic cycle break: the move with the lower `(lamport, device_id)` is reverted to the parent's parent, and a conflict *notice* is recorded. |
| Delete vs. edit | Delete wins for the live tree, **but** the edited version is written to `sync_conflict` and the user is offered restore. |

### 7.2 Human resolution required

Surfaced in a conflict inbox; **both versions are always retained**:

- Same scalar field changed to different values concurrently, where the field is
  user-meaningful (title, a database cell's value, a page-setup change).
- Structural changes the tree rule cannot reconcile without ambiguity.
- A file's blob pointer changed to different blobs concurrently (both files are kept; one is
  renamed `name (conflicted copy from <device>)` — the old, boring, correct answer).

UI contract: show both versions with a diff, let the user pick one, keep both, or merge
manually. **There is no "resolve all automatically" button in v1.0.**

### 7.3 The "never silently overwrite" invariant

Concretely, this means:

1. Every LWW loser is written to `version_snapshot` before being overwritten. Nothing is lost
   that cannot be recovered from history.
2. Every automatic resolution writes a `sync_conflict` row with `resolution` set, so the user
   can review what was decided on their behalf.
3. A push that would overwrite a version the client has not seen is rejected (`409`), not applied.
4. **Tested, not asserted:** a property-based test asserts that for any interleaving of
   operations, the union of all user-intended states is reachable from history.

## 8. Blob sync

Separate pipeline, because bytes behave differently from metadata.

- **Upload:** hash locally → check existence (`POST /v1/files`) → multipart resumable upload →
  complete → server verifies the hash. Dedupe is per-workspace (`security-model.md`, R-06).
- **Download:** on demand, or eagerly per the selective-sync policy.
- **Selective sync policy:** pinned items always; recently-accessed up to a user-set cache
  budget; everything else metadata-only with `BLOB_NOT_LOCAL`. Every feature that touches a
  blob must handle that state and offer to fetch — this is an ambient cost paid everywhere
  (`architecture.md` §12).
- **Bandwidth policy:** Wi-Fi-only by default for large blobs; user-overridable; respects OS
  data-saver settings.
- **Integrity:** the content hash is verified after download. A mismatch discards and retries,
  and after three failures raises a visible error.

## 9. Protocol versioning

The sync protocol versions **independently** of the REST API, because it changes for different
reasons and at a different rate.

```
X-Sync-Protocol: 1
```

- Server supports N and N−1 for at least 12 months.
- Unknown mutation kinds from a newer client are **stored opaquely and forwarded**, not
  rejected — otherwise a newer device's changes would be lost when an older device syncs. This
  "preserve what you don't understand" rule is essential for staged rollouts.
- Unknown fields in a delta are preserved on merge for the same reason.
- A breaking change requires a migration plan with a dual-write window.

## 10. Encryption in sync — **E2EE from v1.0**

**`adr/0002` decision 1: end-to-end encryption from v1.0. The server never sees plaintext.**

This is no longer a fork to be settled before Phase 13; it is the design. Everything in §4
(Protocol) and §7 (Conflict resolution) is re-read in that light below.

Baseline, unchanged: transport is pinned TLS 1.3; blob bytes are encrypted client-side with
per-file keys before upload (`security-model.md` §5); every mutation is signed by the device
key so the server can attribute and reject unregistered devices (T-305).

### 10.1 How a CRDT merges on a server that cannot read it

The apparent paradox — "the server merges CRDT operations but cannot see them" — dissolves
once the server stops merging. **The server is a relay and an orderer, not a merger.** This
is the structural change.

```
┌─ CLIENT (trusted) ─────────────────────────────────────────────────┐
│  plaintext document ── Fugue CRDT ── op                            │
│         op  ──► CBOR encode ──► AEAD seal under the DOCUMENT KEY   │
│                                  XChaCha20-Poly1305-IETF           │
│         └────────────────────────────┐                             │
│  AAD (authenticated, NOT encrypted): │                             │
│    doc_id ‖ snapshot_id ‖ device_id ‖ device_counter ‖ proto_ver   │
│         └──────────► Ed25519 signature over (AAD ‖ ciphertext)     │
└────────────────────────────────┬───────────────────────────────────┘
                                 │  push
┌─ SERVER (untrusted for content) ───────────────────────────────────┐
│  SEES:     doc_id, snapshot_id, device_id, device_counter,         │
│            signature, ciphertext length, arrival time              │
│  DOES:     verify signature · verify device registered/unrevoked   │
│            · authorise device→document · enforce per-device        │
│            monotonic counter (no gaps, no rewrites)                │
│            · assign change-log seq · append · fan out              │
│  CANNOT:   decrypt · merge · reorder within a device · rewrite ·   │
│            resolve a conflict · index · thumbnail · summarise      │
└────────────────────────────────┬───────────────────────────────────┘
                                 │  pull (opaque blobs, in seq order)
┌─ CLIENT (trusted) ─────────────────────────────────────────────────┐
│  verify signature ──► AEAD open ──► CBOR decode ──► CRDT.merge()   │
│  ALL merging, ALL conflict resolution, ALL indexing happen HERE     │
└────────────────────────────────────────────────────────────────────┘
```

**Why this works at all:** a CRDT operation is commutative and idempotent *by construction*.
Merging therefore needs no knowledge of content — only that every replica eventually receives
every operation. Delivery is a transport property, and transport does not require plaintext.
The server's whole job reduces to *durable, ordered, authenticated, exactly-once-ish
delivery of opaque bytes*, which is a much smaller and much more boring job than merging.

This is the same shape as the published `secsync` architecture (encrypted snapshots +
encrypted updates, ordered by unencrypted authenticated metadata), which is useful
corroboration that the design is not novel.

### 10.2 What must stay in cleartext, and why that is a real cost

Everything in the AAD is **authenticated but not confidential**. It cannot be encrypted,
because the server's ordering and authorisation duties depend on it:

| Cleartext | Why the server needs it |
|---|---|
| `doc_id` | routing, fan-out, access control |
| `snapshot_id` | which snapshot an update is rooted at, so stale updates can be dropped |
| `device_id` + public key | signature verification, revocation |
| `device_counter` | gap detection; per-device ordering; replay rejection |
| ciphertext length, timing | unavoidable side channels |

It follows — and must be said plainly in the privacy policy rather than buried — that the
server learns **who edits which documents, when, how often, and roughly how much**. E2EE
protects content, not the social graph of editing. This is already recorded as accepted risk
6 in `security-model.md` §14; it is now a shipped property, not a hypothetical.

**Binding is not optional.** The AAD must be bound into the AEAD, so the server cannot
re-attribute an operation to a different document, device or snapshot without the client
detecting it. Signing the ciphertext alone would leave that attack open.

### 10.3 Snapshots — required, not an optimisation

A document with thousands of updates would require a fresh client to download and decrypt
every one of them before rendering a single character. That is a startup-budget failure
(`performance-strategy.md` §2.1: 1.5 s p50 to interactive).

So: a client periodically seals the **whole CRDT state** under the document key as a
**snapshot**, pushes it, and the server may then discard the updates it supersedes. The
server can do this safely without reading anything, because it enforces the rule
structurally: *a snapshot is persisted only if it references the previous snapshot plus all
of that snapshot's updates; an update is persisted only if the same device's previous update
for that snapshot is already persisted.*

Snapshot cadence is a client policy (e.g. every N updates or M bytes), and it doubles as the
**key-rotation boundary** — rotating the document key means writing a new snapshot under the
new key, after which the old snapshot and its updates can be dropped
(`security-model.md` §5.3).

### 10.4 What the server loses, enumerated

Recorded here because every one of these has been assumed somewhere in the Phase 0 docs and
each is now an explicit client-side obligation:

| Capability | Under E2EE |
|---|---|
| Server-side merge / conflict resolution | **Gone.** §4.2 step 4 collapses — see §10.5. |
| Server-side search index | **Gone.** FTS5 is device-local only. |
| Server-side RAG indexing / embeddings | **Gone.** See `ai-architecture.md` §13. |
| Server-side thumbnailing | **Gone.** Thumbnails are generated on-device and sync as encrypted blobs. |
| Server-side conversion (incl. the LibreOffice path, H-16) | **Gone by default** — available only through the explicit per-document decrypt-and-send consent flow (`ai-architecture.md` §13.2). This makes S-7 (legacy DOC/XLS/PPT at v1.0) materially *less* attractive than it already was. |
| Web client with server-side rendering | **Gone.** A web client must hold keys and do the work in the browser, or not exist at v1.0. |
| Server-side "repair a corrupt document" support tooling | **Gone.** Support cannot look at user data. This is a support-process consequence, not just an engineering one. |

### 10.5 Protocol changes that follow

Amendments to §4, to be applied when Phase 13 is specified in detail:

1. **§4.2 step 4 (server-side merge) is deleted.** The server compares nothing. Its per-
   mutation outcomes reduce to `accepted` / `rejected(reason)` — where reasons are
   signature, authorisation, counter gap, unknown snapshot, or size. There is no
   `conflict` status from the server, because the server cannot detect one.
2. **Conflict detection is purely client-side and purely vector-based.** §3.1's version-vector
   dominance test is unchanged in substance — it never needed plaintext — but it now runs
   only on the client. The `sync_conflict` rows of §7.2 are local rows.
3. **`delta` in the §4.2 push body becomes an opaque base64/CBOR ciphertext**, with
   `version_vector` and `lamport` **moved into the sealed plaintext**, not sent alongside it.
   Leaving a version vector in cleartext would leak the per-device edit graph for no benefit,
   since only clients evaluate it.
4. **§9's "preserve what you don't understand" rule gets easier, not harder.** The server
   already cannot understand anything, so forwarding unknown mutation kinds is automatic.
   The *client* must still preserve unknown fields on merge.
5. **The `X-Sync-Protocol` version stays cleartext**, so a server can reject a protocol it
   cannot order correctly.

### 10.6 Testing additions (extends §12)

- **The server never holds plaintext — as a test, not an assertion.** An integration test
  runs a full sync round-trip against a real server instance and asserts that no known
  plaintext token appears anywhere in the server database, object store, logs or metrics.
  This test fails the build. It is the E2EE equivalent of the LOCAL_ONLY egress test in
  Phase 14 and carries the same weight.
- **Tamper tests:** flip a byte of ciphertext, swap an operation's `doc_id`, replay an
  operation under a different `snapshot_id`, reuse a `device_counter`. Every case must be
  rejected by the client, and where the server can detect it, by the server too.
- **Key-rotation convergence:** replicas that rotate the document key mid-session, with one
  replica offline across the rotation, must still converge (`security-model.md` §5.3).
- **Snapshot/update pruning safety:** a client that pulls after the server has pruned
  superseded updates must reconstruct an identical document.

## 11. Failure modes and their handling

| Failure | Handling |
|---|---|
| Offline | Everything queues. UI shows "N changes pending", not an error. |
| Server 5xx | Backoff and retry. No user-visible error until it persists past ~5 minutes. |
| Server 4xx on a mutation | Dead-letter that mutation, keep syncing the rest, **raise a visible error** with the object named. Never drop silently. |
| Auth expired | Refresh; on refresh failure, pause sync and prompt. Local work continues unaffected. |
| Clock skew | Irrelevant by construction — no ordering uses wall clocks. |
| Cursor too old | `reset_required` → full resync of that workspace. |
| Device revoked | Push rejected with `DEVICE_REVOKED`; the client stops syncing, keeps local data, and tells the user plainly. |
| Storage full mid-apply | Transaction aborts; cursor does not advance; the user is told. Partial apply is impossible by construction. |
| Corrupted local DB | Detected by integrity check at open → offer a rebuild from the server, having first exported anything not yet pushed. |
| Two devices in a push loop | Detected by an unchanged-state counter; backs off and reports. |

## 12. Testing

Sync bugs are the hardest to find and the most damaging. The test strategy is
disproportionate on purpose.

1. **Deterministic simulation.** N virtual devices, a simulated network with configurable
   latency/loss/partition, and injected `ClockPort`/`RandomPort`. Runs are seeded and
   reproducible — a failure can be replayed exactly.
2. **Property tests.** For any random operation interleaving: (a) all devices converge to an
   identical state; (b) no user-intended value is unreachable from history; (c) applying the
   same change log twice is a no-op (idempotence); (d) **concurrent insertions do not
   interleave** (§6.2 P4 — needs its own generator, since random interleaving will not
   produce the triggering shape).
   Plus the differential check of §6.2: the optimised sequence CRDT must produce byte-identical
   documents to the naive reference model.
3. **Partition tests.** Split-brain for 1 h of simulated operations, then heal.
4. **Fuzzed deltas.** Malformed and adversarial deltas from a "hostile device".
5. **Long soak.** 5 real devices, 7 days, scripted concurrent edits — a v1.0 release gate.
6. **Upgrade tests.** Old client ↔ new server and new client ↔ old server, both directions.
7. **Chaos:** kill the app mid-push, mid-apply, mid-upload; fill the disk; revoke a device
   mid-sync.

## 13. Explicit non-goals

- **Real-time collaborative presence** (cursors, live typing). Phase 17. The CRDT foundation
  makes it possible; the transport for it does not exist here.
- **Peer-to-peer / LAN sync.** Server-mediated only.
- **Selective sync at sub-document granularity.** Whole documents, not individual blocks.
- **Cross-account sharing with independent sync state.** Shared content syncs through the
  owning workspace at v1.0.
- **Automatic three-way merge of arbitrary scalar conflicts.** We surface them. Guessing is how
  data gets lost.
