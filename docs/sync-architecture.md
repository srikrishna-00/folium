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

## 6. CRDT choice for rich text — **open decision**

Two options, and this needs sign-off (spike SP-6):

| | **Own RGA in C++** | **`yrs` (Y-CRDT, Rust, MIT)** |
|---|---|---|
| Correctness | Ours to prove. RGA is well-understood but the implementation details (tombstone GC, concurrent-insert tie-breaking, split/merge of runs) are where bugs live. | Battle-tested in production editors; extensively fuzzed upstream. |
| Build | No new toolchain. | Adds Rust to the Android NDK and iOS builds (`cargo-ndk`, target triples, symbol stripping, dSYM handling). |
| Size | Small, ~3–5k lines. | ~1–2 MB static. |
| Interop | Ours alone. | Wire-compatible with the Yjs ecosystem — relevant if the web client wants a live editor later. |
| Effort | ~6–8 weeks including a convergence fuzzer. | ~2 weeks integration + FFI wrapper. |
| Risk | A convergence bug is a data-corruption bug, discovered late, hard to reproduce. | Toolchain/build risk, discovered early, easy to reproduce. |

**Recommendation: `yrs`**, on the grounds that build-system pain is cheap and recoverable
whereas a text-merge correctness bug is expensive and reputationally damaging. This also
front-loads the Rust decision so SVG (`resvg`) and tokenizers can reuse the toolchain.

**Counter-argument to weigh:** it contradicts the spec's C/C++-first principle (§2) and adds a
language to a codebase already spanning C++, Kotlin, Swift, TypeScript and Go. Reasonable
people differ. **Flagged for human decision.**

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

## 10. Encryption in sync

- Transport: TLS 1.3, pinned.
- Deltas at rest on the server: encrypted at the storage layer.
- Blob bytes: encrypted client-side with per-file keys before upload
  (`security-model.md` §5).
- Mutations are **signed** by the device key so the server can attribute and reject
  unregistered devices (T-305).
- **If E2EE is adopted (PQ-2):** deltas are encrypted with the workspace key before push. The
  server then cannot merge — which means all merging must move to the client, conflict
  detection must be purely vector-based, and server-side search/AI become impossible. **This is
  the single biggest fork in the sync design and it must be settled before Phase 13.**

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
   same change log twice is a no-op (idempotence).
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
