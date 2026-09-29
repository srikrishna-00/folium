# Offline-First Architecture

**Status:** Phase 0 draft — awaiting human sign-off
**Version:** 0.0.0
**Date:** 2026-09-29

Implements spec §21. Companion to `sync-architecture.md` (which covers reconciliation); this
document covers what works offline, how, and where the honest limits are.

---

## 1. The definition we are holding ourselves to

"Offline-first" is not "has a cache". The test is:

> **Put the device in airplane mode at first launch after install. Every P0 feature that does
> not inherently require a remote party must work, with no spinner, no error, and no degraded
> placeholder — and must remain correct after force-quit.**

Concretely: create a note, scan a document, OCR it, search it, open a PDF, annotate it, merge
two PDFs, convert an image, ask a question against a local model. All of that offline.

The architectural consequence, stated once: **the network is an enhancement, never a
dependency.** There is no code path in a P0 feature where the absence of a network response
blocks progress. This inverts the default design of most apps and has to be enforced, not
hoped for.

## 2. Offline capability matrix

| Capability | Offline | Notes |
|---|---|---|
| Create/read/update/delete notes, pages, blocks | ✔ Full | |
| Databases: rows, properties, views, filters | ✔ Full | |
| Tasks, calendar (local) | ✔ Full | Device-calendar integration is separate |
| Undo/redo, version history | ✔ Full | History is local |
| Import a file from the device | ✔ Full | |
| File type detection, metadata | ✔ Full | |
| Preview: images, PDF, text, MD, code | ✔ Full | |
| Preview: DOCX/XLSX/EPUB | ✔ Full | Native parsers |
| Preview: legacy DOC/XLS/PPT | **✖** | Server-only (`file-format-support.md` §2) |
| PDF: view, annotate, page ops, compress, protect, redact | ✔ Full | Entirely local |
| Scanner: capture, detect, correct, enhance, batch | ✔ Full | |
| OCR | ✔ Full | **If the language pack is downloaded.** See §5. |
| Conversion: image, MD/HTML/TXT, DOCX→PDF, image→PDF | ✔ Full | |
| Conversion: legacy Office, A/V transcode | **✖** | Server-only / not supported |
| Search: exact, full-text, fuzzy, metadata | ✔ Full | Over locally-present data |
| Search: semantic | ✔ Full | **If the embedding model is downloaded.** |
| AI: summarize, extract, ask, classify | ◐ Conditional | **Local model only.** Otherwise `AI_NO_LOCAL_MODEL`. |
| AI: cloud-model features | **✖** | By definition |
| Open a file whose blob is not on this device | **✖** | `BLOB_NOT_LOCAL` + offer to fetch |
| Sharing, permissions changes, collaboration | ◐ Queued | Queued locally, applied on reconnect. **The UI must say "will apply when online"** rather than implying it took effect. |
| Sign in / sign up / account changes | **✖** | Inherently remote |

**Rows marked ✖ are listed here so they can be designed for, not discovered.** Each needs a
specific, honest UI state — not a generic "no internet" error.

## 3. The local-first data flow

```
User action
   │
   ▼
Validate (local, synchronous)
   │
   ▼
┌──────────────────────────────────────────────┐
│ ONE TRANSACTION:                             │
│   · write content                            │
│   · write FTS rows                           │
│   · invalidate/queue chunk re-embed          │
│   · append version snapshot (if a boundary)  │
│   · enqueue sync_mutation   ← outbox         │
└──────────────────┬───────────────────────────┘
                   │  commit
                   ▼
              UI acknowledges          ← never before commit
                   │
                   ▼
        Sync engine drains the outbox when it can
```

Two rules that fall out of this and are worth stating as invariants:

1. **The UI never acknowledges a change before it is durable.** No optimistic UI over a pending
   write. On mobile, the process can die at any moment.
2. **The content write and the sync-queue write are the same transaction.** Otherwise there is a
   window where the user's change exists but will never be pushed — a silent data-divergence bug
   that surfaces weeks later as "my other device doesn't have it".

## 4. Connectivity model

Three states, not two:

| State | Meaning | Behaviour |
|---|---|---|
| `OFFLINE` | No usable network | Queue everything. No retries. No spinners. |
| `ONLINE_METERED` | Reachable, but cellular / data-saver / roaming | Sync metadata and small deltas. Defer large blobs. Defer model downloads. |
| `ONLINE_UNMETERED` | Wi-Fi or unmetered | Full sync, blob prefetch, model downloads. |

- **Reachability is never trusted as a proxy for usability.** Captive portals, DNS blackholes
  and "connected but no route" are common. The real signal is whether a lightweight probe to
  our own endpoint succeeds. A failed probe demotes to `OFFLINE` regardless of what the OS says.
- Transitions are events the sync engine subscribes to; nothing polls reachability in a loop.
- The state is visible in the UI as a small, non-alarming indicator — with pending-change count
  — rather than as an error banner. Being offline is a normal state, not a failure.

## 5. On-demand assets: the offline feature that most often fails

OCR language packs, the embedding model and the local LLM are all downloaded, not bundled
(bundling them would put the app well over store size limits). That creates a
"offline feature that needs the network first" trap.

**Design:**

1. **Bundle a minimum viable set.** English OCR (`eng.traineddata` fast, ~2 MB) ships in the
   app. So the scanner→OCR journey works offline on day one without any download.
2. **Prompt at a sensible moment, not at first use.** If the user sets a second OCR language or
   enables AI, offer the download then, on Wi-Fi, with the size stated.
3. **Downloads are resumable, verified, and atomic.** Partial files are never loadable; a
   completed download is hash- and signature-verified (T-109) before being moved into place.
4. **Storage-aware.** Check free space before starting; fail early with a clear message rather
   than filling the disk.
5. **Evictable with warning.** Under storage pressure, offer to remove unused packs. Never
   evict silently — the user would find a feature has stopped working offline with no explanation.
6. **Capability queries are truthful.** `uapp_ocr_available_languages()` returns what is
   actually on disk. The UI derives from that, so it can never offer a language that is not
   present.

## 6. Storage budget and eviction

An offline-first app on a 64 GB phone shared with photos must be a good citizen.

```
Categories, in eviction priority order (first evicted first):
  1. Render caches (PDF page bitmaps, thumbnails)      — trivially regenerable
  2. Unpinned, zero-refcount blobs, LRU                 — re-downloadable
  3. Old version snapshots beyond the retention policy  — lossy, so requires the policy
  4. Unused OCR/AI models                               — user-confirmed only
  5. Vector index                                       — regenerable but expensive
NEVER evicted:
  · content DB    · the outbox    · pinned blobs    · unsynced local-only data
```

- User-configurable cache budget with a sensible default (2 GB or 10 % of free space).
- **Unsynced data is never evicted, ever, under any pressure.** If the device is genuinely out
  of space, the app refuses new writes with `STORE_NO_SPACE` rather than discarding the user's
  only copy of something.
- Eviction runs opportunistically in the background, never on the critical path of a user action.

## 7. Background work

Long operations must survive app backgrounding, which is where mobile offline apps usually
lose work.

| Platform | Mechanism | Limits |
|---|---|---|
| Android | `WorkManager` for deferrable sync/indexing; a foreground service with a notification for a user-initiated long job (batch OCR) | Doze and App Standby defer background work aggressively; a foreground service is the only reliable path for a long user-initiated job |
| iOS | `BGProcessingTask` (sync, indexing) + `BGAppRefreshTask` (light refresh) + `beginBackgroundTask` for a short grace period | Background execution is scheduled at the OS's discretion, not ours. **A design that assumes iOS background time will arrive is a design that loses data.** |

**Therefore: checkpointing, not background guarantees.** Every long job (OCR of 50 pages,
bulk indexing, large conversion) persists progress after each unit of work. If the process is
killed at page 37, it resumes at page 37 — it does not restart and it does not lose 36 pages.
This is a hard requirement on every job implementation, verified by a chaos test that kills the
process mid-job.

## 8. Startup path

Cold start must not depend on the network at any point.

```
 1. Open the SQLCipher DB (key from Keychain/Keystore; biometric prompt if app-lock is on)
 2. Run pending migrations (with progress UI if >1 s)
 3. Integrity check (fast: schema version + a sampled page check)
 4. Render the last-viewed screen from local data              ← interactive here, ≤1.5 s
 5. ... only now, asynchronously: probe connectivity, start sync
```

Step 4 must be reachable with the radio off. This ordering is verified by an automated cold-start
test run in airplane mode.

## 9. Conflict-free offline creation

Everything a user can create offline has a client-generated UUIDv7 and a fractional sort key.
No creation path requires a server-assigned identifier. This is what makes "create 40 pages on a
plane" converge cleanly rather than producing 40 ordering conflicts on landing
(`sync-architecture.md` §2).

## 10. UI contract for offline states

The failure of most "offline-capable" apps is not the data layer; it is that the UI cannot
express the states the data layer produces. Required states, each with a designed treatment:

| State | Treatment |
|---|---|
| Offline, everything local | Subtle indicator. No error. Full functionality. |
| Offline, N changes pending | Indicator with count; tappable for detail. Reassuring, not alarming. |
| Offline, feature genuinely unavailable | Specific message naming *why* and *what would fix it* — "Legacy .doc files are converted on our servers. This will work when you're back online." Never "Something went wrong." |
| Blob not on this device | Inline affordance: "Download (4.2 MB)". Not an error. |
| Model/language pack missing | Inline affordance with size, on the feature itself. |
| Sync failing persistently | Actionable error with the object named and a retry. |
| Conflict awaiting resolution | Persistent, non-blocking entry in a conflict inbox. |
| Local-only account (never signed in) | **Clear, repeated disclosure that data exists only on this device** and will be lost if the device is. This is an ethical obligation, not a UX nicety. |

## 11. Testing

| Test | Method |
|---|---|
| Airplane-mode journeys | Automated E2E for every P0 journey with the network stack stubbed to fail |
| Cold start offline | Verify TTI budget with no network |
| Process death mid-job | Kill at N checkpoints during OCR/conversion/sync-apply; assert resume, not restart |
| Storage exhaustion | Fill the volume; assert `STORE_NO_SPACE`, no corruption, no loss of unsynced data |
| Flaky network | Proxy injecting loss, latency, captive-portal responses, and mid-request disconnects |
| Long offline period | 7 days of simulated offline mutations, then reconnect; assert convergence and no loss |
| Clock manipulation | Move the device clock backwards and forwards by years; assert nothing reorders or is lost |
| Model eviction | Evict a language pack; assert the UI stops offering it rather than failing at use time |

## 12. Explicit limits

Stated so the product does not over-promise:

1. **Cloud AI does not work offline.** Obvious, but the UI must make the local/cloud split
   visible *before* the user relies on it.
2. **Legacy binary Office formats do not work offline** (PQ-4). If that is unacceptable, the
   answer is to build native parsers, which is a large scope addition.
3. **Blobs not on the device cannot be opened.** Selective sync is a deliberate trade; the
   alternative is downloading everyone's entire library to every phone.
4. **Sharing and permission changes are eventual.** They queue. Until they sync, the other
   party sees nothing.
5. **Local-only mode has no backup.** If the user never signs in, we cannot recover their data.
   This must be said clearly and more than once.
6. **iOS background time is not guaranteed.** A queued sync may not run until the user next
   opens the app. Nothing is lost; it is just not instant.
