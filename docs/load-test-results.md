# Load / Scale Test Results

**Status:** first real measurement run — numbers below were produced, not estimated
**Date:** 2026-10-20
**Harness:** `tools/loadtest/` (`uapp-loadtest-storage`, `uapp-loadtest-crypto`, `uapp-loadtest-ocr`)
**Commit under test:** `c6f76dc` (24 commits; `ocr/engine` is the most recent module)

---

## 0. What this is, and what it is not

There is no server, no network API and no running application in this repository. It is a
native library layer with CLI tools and test suites. "Load testing" here therefore means
**stress and scale testing the real engines**: feed each one increasing data volumes and
operation counts, measure what actually happens, and find out where it stops being smooth.

Every number below came out of a real process doing real work against a real on-disk SQLite
database, a real libsodium and a real Tesseract. Nothing is extrapolated and nothing is
simulated. Where a module could not be meaningfully load tested as currently scoped, §9 says
so and says why instead of inventing a figure.

### How each point was measured

| Metric | How |
|---|---|
| Wall clock | `std::chrono::steady_clock`, total per phase and divided per operation |
| Peak RSS | `getrusage(RUSAGE_SELF).ru_maxrss` — process high-water mark (bytes on macOS, KiB on Linux; the harness normalises) |
| Current RSS | `task_info(MACH_TASK_BASIC_INFO)` / `/proc/self/statm` — needed for leak detection, because a *peak* cannot show a rising floor |
| Correctness | Checked at every scale point: row/block/entry counts, search hit counts and kinds, AEAD round trips, OCR ground truth |

Three separate executables, one per module group, because **peak RSS is a per-process
high-water mark**: measuring a 19-megapixel OCR run in the same process that just built a
100 000-note database would report the maximum of the two and attribute it to neither. Each
Argon2id profile also gets its own process, for the same reason.

### Reference hardware, and why that matters more than usual here

All numbers are from the **development host**: Apple Silicon, 16 GiB RAM, NVMe, macOS 27,
AppleClang 21, `-O3` Release, SQLite 3.54.0 (FTS5), libsodium 1.0.22, Tesseract 5.5.3.

`docs/performance-strategy.md` §1 writes its budgets against **Tier B** (a 3-year-old mid-tier
phone) and makes **Tier D** (2–3 GB RAM, entry SoC, eMMC) a *gating* tier per `adr/0002`
decision 3. This machine is neither. It is several times faster than Tier B on CPU and roughly
an order of magnitude faster on storage. **So a latency number here that is already close to a
budget is a number that fails on the devices the budget is written for**, and that is how the
verdicts below are read. Memory figures travel better than latency figures: RSS is mostly a
function of the data structures, not the clock speed, so the peak-RSS comparisons against the
Tier D 220 MB ceiling are meaningful as measured.

---

## 1. Summary — the findings that matter

| # | Finding | Severity | Status |
|---|---|---|---|
| L-1 | **A 32-byte key costs 32 KiB of RSS.** `sodium_malloc` guard pages at a 16 KiB page size give a ~1000× amplification. 10 000 live keys = 314 MiB; 100 000 = 2.27 GiB. The allocator never refuses — it just grows until the OS kills the process. | **High** | Needs a human decision (§7.3) |
| L-2 | **`DesktopModerate` Argon2id reserves 258 MiB and `ArchivalSensitive` 1 028 MiB**, measured. Both are above the Tier D 220 MB peak-RSS ceiling; `ArchivalSensitive` is half of a 2 GB device. | **High** | Documented, needs a policy gate (§7.4) |
| L-3 | **`fts_query.cpp` prefix-matches the last term unconditionally, which turns an otherwise selective query into an O(corpus) scan.** Measured: the same selective token alone stays flat at ~70 µs from N=10 to N=100 000; the same token followed by one common word goes 130 µs → 3 459 µs. 48× at 100 k. | **Medium-High** | Root cause identified (§3.1) |
| L-4 | **Reordering or moving one block is O(blocks in the page).** At 50 000 blocks a single reorder is 86.6 ms and a single cross-parent move is **240.9 ms**, on this machine. Cause: a dense contiguous `sort_index` renumbered by two `UPDATE ... WHERE sort_index >= ?` statements per move. | **Medium-High** | Root cause identified (§4.1) |
| L-5 | **Every bounded insert path re-counts the collection on each insert**, making bulk ingest quadratic. `insert_block` runs `COUNT(*)` over the page to enforce `kMaxBlocksPerPage`; `insert_row` runs `COUNT(*)` over the database to enforce `kMaxRowsPerDatabase`. 50 000 block appends take 56 s where linear cost would be ~6 s. | **Medium** | Root cause identified (§4.2, §5) |
| L-6 | **OCR latency is driven by glyph count, not pixel count, and is superlinear in glyphs.** At a fixed 9.6 Mpx canvas: 11 chars = 93 ms, 1 100 chars = 810 ms, 4 400 chars = 7 278 ms. A dense A4-equivalent page costs **7.3 s against a 2.5 s budget** — on hardware far faster than Tier B. Risk **PR-1 is confirmed.** | **Medium-High** | Confirmed with numbers (§8.2) |
| L-7 | **FTS5 storage amplification is ~8×.** 100 000 notes holding ~27 MB of text occupy 219.8 MB. Cause: the body is stored three times (`note`, content-stored `note_fts`, content-stored `search_fts`) with `prefix='2 3'` adding two extra index sets to each FTS table. | **Medium** | Already-known duplication, now quantified (§2) |
| L-8 | **No leaks found in `ocr/engine`.** 300 back-to-back recognitions on one handle grew RSS by 0.109 MiB total (0.37 KiB/call) and the floor was flat from the 25 % mark onward. Open/recognise/close growth plateaus at ~4.8 MiB regardless of 30, 100 or 300 cycles — a bounded one-time cache, not accumulation. | — | **Clean result** |
| L-9 | **Every documented `kMax*` limit fires with the correct `Status` and nothing crashed.** No segfault, no hang, no silent truncation, no wrong answer, at any scale point in any module. | — | **Clean result** |

**Nothing crashed.** Across roughly 1.3 million measured operations there was no crash, no hang, no
assertion failure and no incorrect result. The problems found are all of the "this gets slow, or
this uses more memory than the floor device has" kind, which is the kind that is cheap to fix
before an open-source release and expensive to fix after.

---

## 2. core/notes + storage/sqlite

Fresh database per scale point. One note ≈ 250 bytes of body plus a title. `NoteService` is
driven, not the store directly, so validation and id generation are in the measured path.

| N | create total | create / note | needle (sel. term only) | needle + common term | broad, limit 20 | open+edit+save / note | DB main file | peak RSS | verdict |
|---|---|---|---|---|---|---|---|---|---|
| 10 | 3.2 ms | 321 µs | 76 µs | 130 µs | 144 µs | 397 µs | 4 KiB | 3.5 MiB | ok |
| 100 | 34.1 ms | 341 µs | 39 µs | 68 µs | 148 µs | 247 µs | 311 KiB | 4.3 MiB | ok |
| 1 000 | 215 ms | 215 µs | 44 µs | 104 µs | 761 µs | 180 µs | 2.28 MB | 6.7 MiB | ok |
| 10 000 | 2.32 s | 232 µs | 43 µs | 416 µs | 5.90 ms | 697 µs | 21.97 MB | 13.1 MiB | ok |
| 100 000 | **69.2 s** | **692 µs** | **71 µs** | **3.46 ms** | **66.8 ms** | 556 µs | **219.8 MB** | 14.6 MiB | ok, degraded |

Correctness held at every point: `count()` returned exactly N, the needle query returned
exactly one hit at every scale, and every open → edit → save → re-open round trip compared
equal.

**Where it stayed smooth.** Open / edit / save is flat — 180–700 µs with no trend, because it
is a primary-key lookup and a primary-key update and neither cares how many other notes exist.
A *selective* search is flat too: 76 µs at N=10 and 71 µs at N=100 000, across four orders of
magnitude. That is the FTS5 inverted index doing exactly what an inverted index is for.

**Where it degraded, and why.**

*Insert cost roughly triples between 10 k and 100 k* (232 µs → 692 µs), so ingesting 100 000
notes takes 69 s rather than the ~23 s a linear curve would predict. Two compounding causes.
First, each `create` is its own transaction, so each is a WAL append — the run spends more time
in the kernel than in user space (17.2 s system vs 11.9 s user). Second, and dominant, every
note insert maintains **two** content-stored FTS5 tables (`note_fts` for `NoteStore::search`,
and the shared `search_fts` for the unified index), each with `prefix='2 3'`, so one note is
**six inverted-index updates**, and FTS5's segment merging gets more expensive as the index
grows. This is a bulk-import characteristic, not an interactive one: a user creating a note
pays 0.7 ms, which is comfortably inside the 12 ms "insert a block (commit to durable)" budget.

*A broad ranked query is linear in the corpus* — 148 µs → 66.8 ms over four orders of
magnitude, tracking N almost exactly. This is inherent and not a defect: `ORDER BY
bm25(note_fts)` has to score every matching row before it can pick the top 20, and "planning
roadmap" matches every record by construction. At 100 k it lands at **66.8 ms against the
150 ms "FTS query over 100k blocks" budget** — inside it, but only by 2.2×, on a machine many
times faster than Tier B. **This budget should be treated as at risk until it is measured on
real Tier B/D hardware.**

*Disk amplification is ~8×* (L-7). 219.8 MB of database for roughly 27 MB of text. The body
lives in `note`, again in content-stored `note_fts`, and again in content-stored `search_fts`,
and each FTS table carries a term index plus two prefix indexes. `sqlite_note_store.cpp`'s own
header already records that `note_fts` is redundant and slated for removal once `core/capi`
moves onto the unified API; this measurement puts a number on that follow-up — it is about a
third of the on-disk footprint. The WAL stayed small (190 KB at 100 k), so auto-checkpointing
is working.

---

## 3. core/search — one unified FTS5 index over notes *and* blocks

Cumulative corpus, because a growing corpus is the thing under test. Split 50/50 between notes
and blocks, with blocks spread over pages of 5 000 (see §3.2 for why one giant page would have
measured the wrong thing).

| FTS entries | ingest (cumulative) | needle + common term | broad, limit 20 | broad, limit 1 000 | DB bytes | peak RSS | verdict |
|---|---|---|---|---|---|---|---|
| 10 | 3.8 ms | 132 µs | 173 µs | 198 µs | 0.80 MB | 4.0 MiB | ok |
| 100 | 27.3 ms | 70 µs | 174 µs | 366 µs | 4.12 MB | 4.6 MiB | ok |
| 1 000 | 177 ms | 95 µs | 459 µs | 2.82 ms | 3.50 MB | 7.7 MiB | ok |
| 10 000 | 2.25 s | 383 µs | 4.04 ms | 7.50 ms | 21.70 MB | 22.6 MiB | ok |
| 100 000 | **39.3 s** | **3.33 ms** | **38.6 ms** | **42.0 ms** | **176.7 MB** | **32.4 MiB** | ok |

`mixed_kinds=1` at every scale: a note-exclusive token comes back as `SourceKind::Note` and a
block-exclusive token as `SourceKind::Block`, so the index really is unified at 100 000 entries
and not just at the 20 entries the unit tests use. (The `DB bytes` column is the main file plus
the WAL, which is why 1 000 entries reads smaller than 100 — the WAL had just been
auto-checkpointed. The main file grows monotonically.)

An aside on the verification itself, because it is the kind of thing that makes a load test lie:
the first version of this check queried a token that notes and blocks **share**, and it passed
at 10–1 000 entries and reported `mixed_kinds=0` at 10 000 and 100 000. That was not a missing
kind — bm25 breaks ties by rowid, so at 100 000 entries a shared-token query legitimately
returns the first 1 000 notes and no blocks at all. The probe had to be rebuilt from tokens
exclusive to one kind. A verification that only works at small N is worse than none, because it
turns into a false alarm exactly where the real answers are.

**Verdict: this is the best-behaved storage module under load.** A broad ranked query over
100 000 mixed entries is 38.6 ms, and raising the limit from 20 to 1 000 costs only 3.4 ms more
— the cost is in ranking the candidates, not in returning the rows, which is the right shape.
Peak RSS is 32 MiB while holding a 177 MB database, so the working set is being paged rather
than loaded. 38.6 ms at 100 k entries is inside the 150 ms budget with ~4× headroom, which is
the only storage budget in this document with enough margin to survive a Tier D device
unaided.

### 3.1 L-3: the trailing prefix match is the single largest avoidable search cost

`storage/sqlite/src/fts_query.cpp` appends `*` to the **last** term of every query,
unconditionally:

```
"zq050000 lorem"  ->  "zq050000" AND "lorem"*
"zq050000"        ->  "zq050000"*
```

Its header justifies this, correctly, for search-as-you-type: the user is mid-word, so the last
token should prefix-match. The cost is that a **completed** query pays the same price. The
harness measures both forms against the same corpus, and they separate decisively:

| N | `"zqNNNNNN"*` alone | `"zqNNNNNN" AND "lorem"*` | ratio |
|---|---|---|---|
| 10 | 76 µs | 130 µs | 1.7× |
| 1 000 | 44 µs | 104 µs | 2.4× |
| 10 000 | 43 µs | 416 µs | 9.7× |
| 100 000 | **71 µs** | **3 459 µs** | **48×** |

The selective form is flat. The form with one common trailing word is O(N). The mechanism:
`prefix='2 3'` indexes two- and three-byte prefixes only, so `"lorem"*` (five bytes) is not
served by the prefix index. FTS5 instead walks the term index for every term beginning with
`lorem`, unions their doclists — which is every document in the corpus — and only then
intersects with the selective term's doclist. The selectivity of the *other* term buys nothing,
because the union happens first.

This matters because it is the common case: a user typing `invoice acme` and pressing Enter
gets a corpus-wide prefix scan on `acme`, not a two-term intersection. **Recommended fix:** do
not prefix-match when the raw input ends at a word boundary (a trailing space or punctuation),
or give the API an explicit "the query is complete" flag that the UI sets on Enter and clears
while typing. Either keeps search-as-you-type and removes the scan from committed queries. The
measurement to re-run afterwards is exactly the table above.

---

## 4. core/blocks + storage/sqlite

`kMaxBlocksPerPage` is 50 000, so the ladder's 100 000 point is unreachable **by design**; the
run walks to 50 000 and then probes the limit. `kMaxBlockDepth` (64) is probed separately,
because it is a different ceiling in a different code path.

| blocks on page | append total | append / block | open whole page | reorder (1 op) | cross-parent move (1 op) | DB bytes | peak RSS | verdict |
|---|---|---|---|---|---|---|---|---|
| 10 | 1.2 ms | 123 µs | 20.8 µs | 68 µs | 80 µs | 0.67 MB | 3.1 MiB | ok |
| 100 | 15.4 ms | 154 µs | 40.4 µs | 176 µs | 172 µs | 2.66 MB | 3.8 MiB | ok |
| 1 000 | 175 ms | 175 µs | 233 µs | 2.77 ms | 3.03 ms | 4.54 MB | 6.0 MiB | ok |
| 10 000 | 3.77 s | 377 µs | 2.48 ms | 14.99 ms | 19.24 ms | 16.14 MB | 20.9 MiB | degraded |
| 50 000 (`kMaxBlocksPerPage`) | **56.1 s** | **1 122 µs** | **14.9 ms** | **86.6 ms** | **240.9 ms** | **71.3 MB** | **64.3 MiB** | degraded |
| 50 001 | — | — | — | — | — | — | — | **`PAGE_BLOCK_LIMIT_EXCEEDED` in 1.0 ms** |

Depth, separately:

| depth | result |
|---|---|
| 64 (`kMaxBlockDepth`) | 64 nested inserts in 6.1 ms, 95 µs each — reached cleanly |
| 65 | **`BLOCK_DEPTH_EXCEEDED` in 40 µs** |

Both limits return the documented `Status` promptly and leave the store usable. No crash, no
partial write, no stack growth from the recursive ancestor CTE.

**Where it stayed smooth — and this is the headline good result.** Loading a **50 000-block
document tree takes 14.9 ms**. The budget is 120 ms for a *500*-block tree. The tree load is
a single ordered scan reassembled in memory, and it is 100× better than the budget at 100× the
size. Peak RSS for holding that whole tree is 64 MiB, inside the Tier D ceiling with room
left.

### 4.1 L-4: reorder and move are O(page size)

A single reorder goes 68 µs → 86.6 ms as the page grows from 10 to 50 000 blocks — 1 270×
slower for 5 000× more data, i.e. linear in page size with a small constant. Cross-parent move
is worse on both counts: **240.9 ms per move at 50 000 blocks**, and superlinear between 10 k
and 50 000 (19.24 ms → 240.9 ms, 12.5× for 5× the data).

The cause is visible in `sqlite_block_store.cpp::move_block`, and it is a deliberate design
with an unpriced cost: sibling order is a **dense contiguous `sort_index`**, and position is
maintained by renumbering. Each move runs `shift_siblings` twice — once with `delta = -1` to
close the gap the block left, once with `delta = +1` to open the gap at the destination — and
each is an `UPDATE block SET sort_index = sort_index + ? WHERE page_id = ? AND parent_id IS ?
AND sort_index >= ?`. Moving to the middle of a 50 000-sibling list rewrites ~25 000 rows,
twice. A `sibling_count` `COUNT(*)` over the same list is on top of that, and the cross-parent
path additionally runs the recursive ancestor CTE and a subtree-height probe, which is why move
is superlinear where reorder is merely linear.

86 ms for a reorder and 241 ms for a cross-parent move is a visible stall for one
drag-and-drop *on this machine*; on Tier D it is most of a second. **The structural fix is
fractional (gap) indexing** — give
`sort_index` a sparse key so an insertion between two siblings is one row write instead of
N/2 — which is also what a CRDT-ordered document model wants, and `core/sync/spike` has already
spiked Fugue, whose whole job is exactly this ordering problem. This is a case where the
eventual sync design and the immediate performance problem have the same answer.

### 4.2 L-5: bounded inserts re-count on every insert

Append cost rises 123 µs → 1 122 µs (9×) as the page fills, so 50 000 appends take 56 s where
a linear curve predicts ~6 s. `insert_block` calls `block_count` — `SELECT COUNT(*) FROM block
WHERE page_id = ?1` — on every single insert, to enforce `kMaxBlocksPerPage`. That is O(N) per
insert and therefore **O(N²) to fill a page**. The same pattern is in
`sqlite_database_store.cpp::insert_row` for `kMaxRowsPerDatabase` (§5).

The enforcement itself is right; re-deriving the count from scratch each time is not. A
maintained counter (a column on `page`, or a trigger-kept counter table) makes the check O(1)
and the fill linear. This is the reason the unified-search runner (§3) spreads blocks over
pages of 5 000 rather than piling 50 000 onto one page — otherwise that runner would have been
measuring this quadratic, not the index.

---

## 5. core/database + storage/sqlite — the most "load-test-shaped" module

Six property types (Text, Number, Checkbox, Select, Multi-select, Date) on every row, so each
row is 6–7 values in the EAV tables plus its membership rows. `kMaxRowsPerDatabase` is 100 000,
so this module reaches the top of the ladder exactly and the limit probe is the 100 001st row.

| rows | insert total | insert / row | page 50, no filter | page 50, Select filter + Number sort | page 50, Text *contains* + Date sort | 1 000 rows, 3 filters + 2 sorts | materialise all rows | DB bytes | peak RSS |
|---|---|---|---|---|---|---|---|---|---|
| 10 | 1.7 ms | 168 µs | 152 µs | 224 µs | 255 µs | 120 µs | 0.13 ms | 0.86 MB | 4.0 MiB |
| 100 | 15.5 ms | 155 µs | 194 µs | 307 µs | 474 µs | 95 µs | 0.27 ms | 1.29 MB | 4.3 MiB |
| 1 000 | 139 ms | 139 µs | 134 µs | 977 µs | 2.20 ms | 2.13 ms | 1.80 ms | 3.16 MB | 6.2 MiB |
| 10 000 | 2.14 s | 214 µs | 151 µs | 8.66 ms | 24.8 ms | 26.9 ms | 19.4 ms | 11.38 MB | 21.1 MiB |
| 100 000 (`kMaxRowsPerDatabase`) | **88.5 s** | **885 µs** | **161 µs** | **113.2 ms** | **301.8 ms** | **309.0 ms** | **227.4 ms** | **85.1 MB** | **86.2 MiB** |
| 100 001 | — | — | — | — | — | — | — | — | **`DATABASE_ROW_LIMIT_EXCEEDED` in 1.6 ms** |

`row_count` returned exactly N at every scale, and `materialise all rows` returned exactly N
rows, so the EAV reassembly is correct at 100 000 rows and 600 000 values, not just at the
handful the unit tests use.

**Where it stayed smooth.**

*An unfiltered, unsorted first page is completely flat* — 152 µs at 10 rows, 161 µs at 100 000.
That is the right shape and it is the one a UI opening a view actually hits first: with no
filter and no sort, `LIMIT 50` stops the scan after 50 rows and the row-id subquery never
materialises the rest.

*Reassembling the full 100 000-row result costs 227 ms and 86 MiB* — 2.3 µs and ~870 bytes per
row for a six-property EAV row. That is good for the shape of the schema; the caveat is that
`limit` is clamped to `kMaxRowsPerDatabase` rather than to something a screen can show, so a
caller *can* ask for all of it, and on Tier D that single call is 86 MiB of the 220 MB budget.

*On-disk cost is modest*: 85.1 MB for 100 000 rows × 6 properties, about 851 bytes per row.
Unlike the FTS5 path (§2), there is no duplication here.

**Where it degraded, and why.**

*Filtered + sorted paging is linear-to-superlinear in matching rows.* 977 µs at 1 000 rows →
8.66 ms at 10 000 → 113.2 ms at 100 000, which is 13.2× for the last 10× of data. The mechanism
is visible in the generated SQL: the filter (`Select = In progress`, matching ~1/3 of rows)
produces a row-id set that grows with N, the sort then needs the Number value for *every* row
in that set — a join back into `db_value` — and SQLite sorts the whole set in a temporary
B-tree before `LIMIT 50` discards 33 283 of the 33 333 rows it just sorted. `LIMIT` cannot
short-circuit a sort. The extra superlinearity over the 10× data growth is the `M log M` of
that sort.

**This is risk PR-3 in `performance-strategy.md` §9, and it is now measured.** The budget is
*"Database view: 10k rows × 20 properties, filtered + sorted — 100 ms"*. At 10 000 rows this
run measured **8.66 ms** — comfortably inside — but with **6** properties, not 20, and on
hardware several times faster than Tier B. At 100 000 rows it is **113.2 ms**, i.e. past the
budget's figure at 10× the budget's row count. Reading the two together: the 10k/20-property
budget is probably met on Tier B and probably not met on Tier D, and the mitigation
`database-design.md` §3.4 already names (materialised view cache, reference-only filter joins)
is needed before a 100 000-row database is a supported configuration rather than a reachable
one.

*`Contains` on a Text property is a full scan and cannot be anything else.* 255 µs → 301.8 ms,
linear in N throughout. `Contains` compiles to a substring test over `db_value.v_text`, and no
B-tree index can serve `%alpha%`. This is honest and documented behaviour, not a defect, but
302 ms for one keystroke in a filter box at 100 000 rows is a UI problem. The existing answer in
the codebase is the FTS5 index — a text search belongs in `core/search`, not in a Table view
filter — and that boundary should be stated in the UI (offer "search" rather than "contains" on
large databases) rather than left for a user to discover.

*Bulk insert is mildly superlinear* (L-5): 214 µs/row at 10 000 → 885 µs/row at 100 000, so
88.5 s to fill a database to its documented maximum. Same cause as §4.2 —
`insert_row` calls `row_count`, a `COUNT(*) ... WHERE database_id = ?`, on every insert to
enforce `kMaxRowsPerDatabase` — plus `get_database` reloading the schema (properties and their
options) on every insert. Both are per-insert O(N) and O(schema) work that a maintained counter
and a cached schema handle would remove. Interactive inserts are fine (0.9 ms at the worst
point, inside the 12 ms budget); CSV import of 100 000 rows taking a minute and a half is the
thing to fix.

### 5.1 Two real validation rules the harness itself tripped over

Worth recording, because both are the product being *right*:

* The first version of the row generator produced a Multi-select value holding the same option
  twice at `i = 7`. `insert_row` rejected it with `DATABASE_VALUE_MALFORMED` on row 7 of 10 and
  the run stopped. `core/database/src/property.cpp` rejects duplicate options deliberately
  ("selecting one option twice is not a legal state").
* The first version of the three-filter view passed a `MultiSelectValue` as the operand of
  `Contains` on a Multi-select property. That is `DATABASE_VALUE_TYPE_MISMATCH`: the documented
  exception is that `contains` takes **one** option, so the operand is a `SelectValue`.

Both failed fast, at the write/validate boundary, with a specific code — which is exactly the
behaviour `spec §33` asks for and the reason the harness bug took two minutes to find rather
than producing quietly wrong numbers.

---

## 6. core/conversion — Page ↔ Markdown

Pure in-memory; no store is involved in either direction. The two directions were measured in
**separate processes** so that peak RSS is attributable to one of them.

### Write: Page → Markdown

| blocks | latency | per block | output bytes | peak RSS | verdict |
|---|---|---|---|---|---|
| 10 | 0.019 ms | 1.89 µs | 2.6 KB | 1.8 MiB | ok |
| 100 | 0.053 ms | 0.53 µs | 25.9 KB | 2.0 MiB | ok |
| 1 000 | 0.430 ms | 0.43 µs | 259 KB | 2.9 MiB | ok |
| 10 000 | 4.20 ms | 0.42 µs | 2.59 MB | 13.4 MiB | ok |
| 50 000 (`kMaxBlocksPerPage`) | **20.1 ms** | **0.40 µs** | **12.96 MB** | **56.0 MiB** | ok |
| 18 000 × 1 KiB blocks (forces the output cap) | 20.9 ms | — | refused | **95.0 MiB** | **`CONVERSION_OUTPUT_TOO_LARGE`** |

### Read: Markdown → Page

| input bytes | blocks produced | latency | per block | peak RSS | verdict |
|---|---|---|---|---|---|
| 1 KiB | 3 | 0.23 ms | 76 µs | 1.8 MiB | ok |
| 100 KiB | 256 | 1.47 ms | 5.8 µs | 2.2 MiB | ok |
| 1 MiB | 2 622 | 8.71 ms | 3.3 µs | 5.3 MiB | ok |
| 8 MiB | 20 972 | 36.0 ms | 1.7 µs | 30.0 MiB | ok |
| 16 MiB (`kMaxMarkdownInputBytes`) | 41 944 | **47.4 ms** | **1.1 µs** | **61.5 MiB** | ok |
| 16 MiB + 1 | — | 0.0 ms | — | — | **`CONVERSION_INPUT_TOO_LARGE`, rejected before any parsing** |
| round trip, 5 000 blocks | 5 000 → 5 000 | 6.59 ms | 0.66 µs | — | ok, block count preserved |

**Verdict: perfectly smooth, and the best-scaling module in the repository.** Per-block cost
*falls* with size in both directions (1.89 → 0.40 µs writing, 76 → 1.1 µs reading) because the
fixed costs amortise and `std::string`'s geometric growth means fewer reallocations per byte at
larger sizes. Both documented caps fire with the right `Status`. The input cap is checked
**before** parsing, at a cost of 0.0 ms — the ideal shape for a limit: an oversized input costs
nothing.

**The one thing to fix, and it is a memory finding, not a latency one.** `markdown.h` justifies
the 16 MiB caps by saying they sit *"comfortably below the 256 MB mobile arena cap"*. 256 MB is
the **Tier B** arena figure. `performance-strategy.md` §1.1 sets Tier D's conversion arena at
**96 MB**, and measured against that:

* a 16 MiB **read** peaks at **61.5 MiB** — inside 96 MB, but only 1.6× under it;
* a 16 MiB **write** peaks at **95.0 MiB** — *at* the Tier D cap, and it got there while
  producing a **refusal**, because the output cap is enforced as output accumulates rather than
  predicted from the input.

And there is no arena. `grep -ri arena core/conversion/` finds exactly one hit: that comment.
The converter uses plain `std::string` and `std::vector`, so the 96 MB Tier D cap and the 256 MB
Tier B cap are currently **documents, not mechanisms** — nothing enforces them, and the measured
peak for a legal 16 MiB document already sits on the Tier D line. Either the arena has to be
built, or `kMaxMarkdownInputBytes`/`kMaxMarkdownOutputBytes` have to come down to something the
floor device can hold (the data above suggests 4 MiB would peak at ~20–25 MiB, which is safe),
or the caps have to become tier-dependent. This is a human decision, not a silent fix.

---

## 7. security/crypto + security/keys — single-process throughput

No server and no multi-tenancy, so "many users" would be fiction. The real question for a device
is single-process throughput, and the real *limits* turn out to be about memory, not speed.

### 7.1 AEAD (XChaCha20-Poly1305) across message sizes

| message | seal | open | seal MiB/s | open MiB/s | peak RSS | verdict |
|---|---|---|---|---|---|---|
| 1 KiB | 6.3 µs | 14.3 µs | 155 | **68** | 1.9 MiB | ok, allocator-bound |
| 16 KiB | 26.3 µs | 26.7 µs | 595 | 585 | 2.1 MiB | ok |
| 256 KiB | 322 µs | 345 µs | 777 | 724 | 3.1 MiB | ok |
| 1 MiB | 1.29 ms | 1.40 ms | 775 | 717 | 6.1 MiB | ok |
| 8 MiB (`kMaxPlaintextBytes`) | 10.7 ms | 11.2 ms | 750 | 715 | 37.2 MiB | ok |
| 8 MiB + 1 | — | — | — | — | — | **`CRYPTO_MESSAGE_TOO_LARGE` in 0.04 µs** |
| tamper one ciphertext byte | — | 12.3 µs | — | — | — | **`CRYPTO_AUTH_FAILED`** |

Round trips were byte-compared at every size and matched. The cipher plateaus at **~750 MiB/s
sealing and ~715 MiB/s opening** from 16 KiB upward, which is a healthy, flat curve: throughput
does not fall as messages grow, so the 1 MiB AEAD chunking that `security-model.md` §5 uses for
large blobs costs nothing in throughput.

**The one anomaly is small-message decryption**: 68 MiB/s at 1 KiB against 155 MiB/s for
sealing the same size, and against 585 MiB/s at 16 KiB. At 750 MiB/s the cipher work for 1 KiB
is ~1.3 µs, so ~13 µs of the 14.3 µs is **fixed per-call overhead** — and the asymmetry points
straight at it: `open()` writes its plaintext into a `SecureBytes`, which is `sodium_malloc` +
`mlock`, while `seal()` writes into a plain `std::vector`. A guarded allocation and an `mlock`
per decrypt is roughly 10× the cost of decrypting 1 KiB. It is the right security choice; it
just means **decrypting many small records is dominated by the allocator, not the cipher**, and
anything that decrypts per-block (50 000 blocks on a page) wants a reusable `SecureBytes`
buffer rather than one per call.

### 7.2 Operation-count ladders — flat, which is the correct answer

| operations | seal 4 KiB | open 4 KiB | `derive_subkey` | `derive_document_key` | derive + seal one 4 KiB document |
|---|---|---|---|---|---|
| 10 | 6.5 µs | 11.1 µs | 12.2 µs | 10.5 µs | 18.0 µs |
| 100 | 6.1 µs | 10.1 µs | 10.8 µs | 11.2 µs | 17.9 µs |
| 1 000 | 6.0 µs | 10.0 µs | 10.5 µs | 11.0 µs | 17.6 µs |
| 10 000 | 6.1 µs | 10.1 µs | 10.7 µs | 11.1 µs | 17.5 µs |
| 100 000 | **6.1 µs** | **10.3 µs** | **11.0 µs** | **11.2 µs** | **17.9 µs** |
| peak RSS over the whole ladder | 1.95 MiB | 1.95 MiB | 1.92 MiB | 2.22 MiB | 2.22 MiB |

**Completely flat across four orders of magnitude, at ~2 MiB of RSS.** No drift, no leak, no
degradation. In product terms: **one device can derive a per-document key and seal the document
at ~56 000 documents/second**, and run 100 000 of them back to back in 1.8 s while holding
2.2 MiB. The E2EE layer is not going to be the bottleneck for any realistic document count, and
this is the cleanest scale result in this document.

Device key wrapping (X25519 + AEAD) is also flat: 26.5 µs to wrap and 35.3 µs to unwrap, from
10 operations to 10 000, with the unwrapped URK byte-compared equal to the original every time.

### 7.3 L-1: a 32-byte key costs 32 KiB of resident memory

This is the finding that needs a decision. Keys live in `SecureBytes`, i.e. `sodium_malloc`:
guard pages plus `mlock`. The harness holds keys **simultaneously** rather than in a loop,
because allocating and freeing one at a time can never show a capacity limit.

| live `ContentKey`s | RSS before | RSS after | bytes of RSS per 32-byte key | verdict |
|---|---|---|---|---|
| 10 | 1.81 MiB | 2.14 MiB | 34 406 | ok |
| 100 | 1.83 MiB | 4.95 MiB | 32 768 | ok |
| 1 000 | 1.84 MiB | 33.1 MiB | 32 768 | ok |
| 10 000 | 1.86 MiB | **314.5 MiB** | 32 784 | **over the Tier D 220 MB ceiling** |
| 100 000 | 2.03 MiB | **2 266.9 MiB** | 23 749 | **over the whole Tier D device** |

**32 768 bytes of RSS for 32 bytes of key — a 1 024× amplification.** The cause is the page
size: this host uses **16 KiB pages** (`sysctl hw.pagesize` → 16384), and `sodium_malloc`'s
layout costs two resident pages per allocation. On a 4 KiB-page device the constant is smaller
but the shape is identical.

Two consequences, and the second is the dangerous one:

1. **The budget arithmetic changes.** 10 000 live keys is 314 MiB — already past Tier D's
   220 MB *peak* ceiling, with nothing else in the process. A design that caches a
   `DocumentKey` per document in an open workspace is a design that gets the app killed on the
   floor device at four-digit document counts.
2. **There is no error.** `sodium_malloc` never refused, at any point, up to 100 000 live keys
   and 2.27 GiB. It kept succeeding until the process was simply larger than a Tier D device.
   So the failure mode is **not** `CRYPTO_SECURE_ALLOC_FAILED` — it is the Android LMK or iOS
   jetsam, which is precisely the outcome `performance-strategy.md` §10 anti-pattern 6 says
   must never be how a memory budget is discovered.

**Needs a human decision.** The options are a bounded key cache with an explicit eviction policy
(which `performance-strategy.md` §3.5 already requires of every other cache — "an unbounded
cache is a leak with a friendly name" — and the key cache is currently not in that table), or
packing many keys into one guarded allocation, or simply documenting a hard cap on live keys and
asserting it. What should *not* happen is shipping with the amplification undocumented, because
32 bytes → 32 KiB is not what any reader of `security-model.md` §5A.1 would assume.

### 7.4 L-2: the Argon2id profiles, measured, one process each

| profile | opslimit | memlimit | latency (3 derivations) | per derivation | **measured peak RSS** | Tier D (≤ 220 MB peak) |
|---|---|---|---|---|---|---|
| `MobileInteractive` | 3 | 64 MiB | 194.8 ms | **64.9 ms** | **66.0 MiB** | fits — but is 30 % of the whole budget |
| `DesktopModerate` | 3 | 256 MiB | 836.6 ms | **278.9 ms** | **258.4 MiB** | **breaches it on its own** |
| `ArchivalSensitive` | 4 | 1 024 MiB | 4 668.5 ms | **1 556.2 ms** | **1 027.9 MiB** | **half of a 2 GB device** |

The reservations are real, not nominal: measured peak RSS tracks `memlimit` within 2 MiB in
every case. `crypto.cpp`'s choice of `MobileInteractive` as *"the only mobile-legal profile"* is
confirmed correct by measurement — and the other two are confirmed to be exactly as unusable on
Tier D as that comment implies.

Two things follow. First, **65 ms for a `MobileInteractive` derivation is on an Apple Silicon
workstation.** `performance-strategy.md` §5 budgets DB unlock at ≤ 120 ms on Tier B. An entry
SoC with slower memory will plausibly be 5–10× slower here, because Argon2id is deliberately
memory-bandwidth-bound — so **400–700 ms is the realistic Tier D unlock cost**, over the budget,
and that needs measuring on real hardware before it is promised. Second, nothing in the code
prevents a caller from passing `ArchivalSensitive` on a phone, where it would reserve 1 GiB.
**A profile selection gate — refuse the non-mobile profiles unless the platform says it has the
headroom — is worth more than any micro-optimisation in this module.**

---

## 8. ocr/engine

OCR has no corpus and no row count, so the ladder here is **image size** (toward
`kMaxImagePixels` = 20 Mpx) and **repeated use** (the leak question), not operation count. The
image is `tests/ocr/ocr_fixture.h` — a 300×80 Gray8 render of "HELLO WORLD" with ground truth
verified against the real `tesseract(1)` CLI — tiled to reach 19 megapixels, so every pixel fed
to the recogniser is text a human has confirmed is readable and correctness stays checkable at
every scale.

Model: this host's Homebrew `eng.traineddata`, **4.1 MB** — the standard `tessdata` set, *not*
`tessdata_fast`. Vendoring and re-measuring against `tessdata_fast` is still owed by
`docs/spikes/SP-10-findings.md`, and these numbers do not discharge it.

### 8.1 Latency vs image size

| pixels | dimensions | tiles | latency | Mpx/s | blocks returned | mean confidence | peak RSS | reads ground truth |
|---|---|---|---|---|---|---|---|---|
| 24 000 | 300×80 | 1 | **7.5 ms** | 3.21 | 1 | 96.0 | 35.0 MiB | yes |
| 216 000 | 900×240 | 9 | 42.0 ms | 5.14 | 1 | 80.0 | 42.5 MiB | yes |
| 1 176 000 | 2100×560 | 49 | 250 ms | 4.70 | 7 | 96.0 | 47.0 MiB | yes |
| 4 704 000 | 4200×1120 | 196 | 1.86 s | 2.53 | 14 | 96.0 | 67.8 MiB | yes |
| 9 600 000 | 6000×1600 | 400 | **7.30 s** | 1.32 | 20 | 96.0 | 107.3 MiB | yes |
| 18 816 000 | 8400×2240 | 784 | **33.18 s** | **0.57** | 28 | 96.0 | **153.3 MiB** | yes |
| 20 184 000 | 8700×2320 | — | — | — | — | — | — | **`OCR_IMAGE_SIZE_INVALID` in 0.08 µs, before allocation** |

Throughput falls **9×** across the ladder, from 5.14 Mpx/s to 0.57 Mpx/s. Latency grows roughly
as *pixels*^2.2 in the upper half. The over-limit case is rejected in 83 nanoseconds, before any
buffer is touched — the ideal shape.

### 8.2 L-6: it is the glyphs, not the pixels — and the 2.5 s budget does not hold

The size ladder alone cannot say *which* input grew, because tiling adds pixels and glyphs
together. So the harness holds the canvas at a fixed **9.6 Mpx** (≈ A4 at 300 DPI, which is the
size the 2.5 s budget is written against) and varies only how much of it carries text:

| text tiles | characters | latency | latency ÷ previous | characters ÷ previous |
|---|---|---|---|---|
| 1 | 11 | **93.4 ms** | — | — |
| 25 | 275 | 200.1 ms | 2.14× | 25× |
| 100 | 1 100 | 810.1 ms | 4.05× | 4× |
| 400 | 4 400 | **7 277.5 ms** | **8.98×** | 4× |

This is decisive. **Pixel count buys a fixed ~93 ms** (the page-layout pass over 9.6 Mpx —
about 10 ms per megapixel, and essentially all of the cost on a nearly blank page). Everything
above that tracks the **character count**, and does so *superlinearly*: the last step quadruples
the text and multiplies latency by nine. The LSTM line recogniser plus Tesseract's adaptive
second pass is where the time goes, not the image decode or the layout analysis.

So the earlier size ladder's apparent `pixels^2.2` was an artefact of tiling: the real
relationship is **latency ≈ 93 ms of layout per 9.6 Mpx + a superlinear function of glyph
count.** That is a much more useful model, because it says the thing to cap is text density per
recognition call, not resolution.

**And it confirms risk PR-1.** `performance-strategy.md` §2.4 budgets *"Tesseract OCR, A4
300 DPI, English — 2.5 s"*, with Tier D relaxed to 6 s. 4 400 characters on a 9.6 Mpx canvas is
a dense but entirely ordinary A4 text page, and it measured **7.28 s on an Apple Silicon
workstation**. Even allowing that this fixture is denser than average prose (36 pt glyphs edge
to edge), halving the density only gets to ~0.8–1.5 s *on this machine*, and Tier B is several
times slower. PR-1 says *"Tesseract at 2.5 s/page may be optimistic on Tier B/C"*; this
measurement says it is optimistic **on a desktop**, and that the stated mitigations (platform
OCR as the default fast path, Tesseract as fallback, `tessdata_fast`, 200 DPI downscale on
Tier D) are not optional niceties but the load-bearing part of the plan. The risk's residual
should move from Medium to **High** until `tessdata_fast` numbers exist.

Peak RSS of **153.3 MiB** at 18.8 Mpx also sits just over Tier D's *"Peak RSS during OCR —
≤ 150 MB"*, with an 18.8 MB Gray8 input and nothing else in the process. Tier D's "one page at
a time, 200 DPI" policy is therefore load-bearing too, not a tuning preference.

### 8.3 L-8: no leak, and the open/close growth is a bounded cache

This is the result the module can be proud of.

**Same handle, 300 recognitions back to back** (216 kpx each, 5 warm-up calls excluded):

| sample point | current RSS |
|---|---|
| after warm-up (baseline) | 38.28 MiB |
| 25 % through (75 calls) | 38.39 MiB |
| 50 % through (150 calls) | 38.39 MiB |
| after 300 calls | 38.39 MiB |

Total growth **0.109 MiB over 300 calls — 0.37 KiB per call** — and flat from the 25 % mark
onward, which is the signature of a one-time warm allocation rather than accumulation. All 300
results read the ground truth. **`Engine::recognise` does not leak.**

**Open / recognise / close cycles** — a handle leak looks nothing like a buffer leak, so it is
measured separately, at three cycle counts to tell a leak from a plateau:

| cycles | total RSS growth | growth per cycle |
|---|---|---|
| 30 | 4.70 MiB | 160.5 KiB |
| 100 | 4.66 MiB | 47.7 KiB |
| 300 | 4.95 MiB | 16.9 KiB |

Total growth is **constant at ~4.7–5.0 MiB regardless of 30, 100 or 300 cycles**, so per-cycle
"growth" falls as 1/N. That is a bounded one-time cost — Tesseract/Leptonica static tables and
allocator arenas warming up — not a per-cycle leak. A leak would have shown ~48 MiB at 300
cycles. `Engine::close()` and the destructor release what they own.

Per-cycle cost is ~37–40 ms for open + recognise + close of a 300×80 image, of which the
recognition is ~7.5 ms, so **engine construction costs ~30 ms**. Worth knowing: a batch should
open one `Engine` and reuse it across pages, which the §8.3 repeat numbers show is completely
safe.

---

## 9. Where this methodology does not apply, and why

Four of the repository's modules are deliberately **not** in the tables above. Inventing numbers
for them would be worse than their absence.

* **`pdf-engine/reader`** — not measured in this run because this build was configured with
  `-DUAPP_ENABLE_PDF=OFF` (no vendored PDFium binary present on this host). It *is* load-test
  shaped — page count, page complexity, render scale, the 500-page/100 MB budgets in
  `performance-strategy.md` §2.3 — and it is the most obvious gap to fill next. A
  `pdf_loadtest.cpp` beside the three existing harnesses, gated on `UAPP_PDF_ENABLED`, is the
  right shape. **This is an absence of measurement, not a judgement that it cannot be
  measured.**
* **`file-engine/detection`** — detection is bounded by design: it reads a header-sized prefix,
  not the file. Its cost is O(signature table), not O(file size), so a "scale curve" over file
  size would be a flat line and a scale curve over *operation count* would restate a single
  constant. It already has a libFuzzer harness (`fuzz_detect`), which is the right stress tool
  for a parser boundary. If anything is worth measuring it is throughput over a directory of
  many files, which is a filesystem benchmark rather than an engine one.
* **`ai/provider` + `ai/local`** — `LocalHeuristicProvider` implements four capabilities with
  honest refusals for the rest, and there is **no model, no inference and no network client**
  behind it. Load testing it would measure the dispatch layer and the `std::string` handling of
  a heuristic, and would produce an impressive throughput number that says nothing about what
  the module will cost once a real provider is attached. The budgets that matter here
  (`performance-strategy.md` §2.6: embedding latency, local LLM tokens/second) cannot be
  measured until there is something to measure. **Deliberately not faked.**
* **`core/sync/spike` (Fugue)** — explicitly research code, never linked by a product target.
  It has its own correctness spike with its own `kMaxElements` (2²⁰) and
  `kMaxPendingOperations` bounds, and `docs/spikes/fugue-correctness-spike.md` is where its
  numbers belong. Treating a spike's throughput as a product figure would be the wrong kind of
  precision.
* **`core/capi`** — the C ABI is a thin translation layer over the engines already measured
  here. Load testing it would re-measure §2–§6 plus a flattening copy. The one question that is
  genuinely its own — per-call marshalling overhead — is a micro-benchmark for `benchmarks/`
  under Google Benchmark, and `performance-strategy.md` §9's PR-7 notes the bitmap-transfer
  case is already closed by SP-5 with a copy-counter assertion rather than a timing.

---

## 10. Peak memory against the ADR-0002 2 GB floor

`adr/0002` decision 3 makes the 2–3 GB Tier D device a **hard constraint**, and
`performance-strategy.md` §1.1 turns that into two numbers that are build gates: **≤ 110 MB RSS
idle** and **≤ 220 MB RSS peak during a heavy operation**, with sub-budgets of ≤ 150 MB during
OCR and a ≤ 96 MB conversion arena. Measured peaks, each from its own process:

| Workload (worst scale point reached) | measured peak RSS | vs Tier D 220 MB peak | note |
|---|---|---|---|
| 100 000 notes created + searched | **14.6 MiB** | 6.6 % | holding a 220 MB database |
| 100 000-entry unified search index | **32.4 MiB** | 15 % | holding a 177 MB database |
| 50 000-block page, whole tree loaded | **64.3 MiB** | 29 % | |
| 100 000-row database, all rows materialised | **86.2 MiB** | 39 % | one API call can ask for this |
| 16 MiB Markdown → Page | **61.5 MiB** | 28 % | vs the 96 MB conversion arena: 64 % |
| Page → 16 MiB Markdown (refused at the cap) | **95.0 MiB** | 43 % | vs the 96 MB arena: **99 %** |
| AEAD at the 8 MiB message cap | **37.2 MiB** | 17 % | |
| 100 000 seal / open / derive operations | **2.2 MiB** | 1 % | flat across the whole ladder |
| OCR, 18.8 Mpx image | **153.3 MiB** | 70 % | vs the 150 MB OCR sub-budget: **102 %** |
| OCR, 300 repeated recognitions | **38.4 MiB** | 17 % | |
| Argon2id `MobileInteractive` | **66.0 MiB** | 30 % | |
| **Argon2id `DesktopModerate`** | **258.4 MiB** | **117 %** | **breach** |
| **Argon2id `ArchivalSensitive`** | **1 027.9 MiB** | **467 %** | **breach — half the device** |
| **10 000 live `ContentKey`s** | **314.5 MiB** | **143 %** | **breach** |
| **100 000 live `ContentKey`s** | **2 266.9 MiB** | **1 030 %** | **breach — larger than the device** |

**Reading this honestly.** Every *data* engine is comfortably inside the floor device's budget,
and several are remarkably frugal: 100 000 notes over a 220 MB database in 14.6 MiB of RSS is
SQLite doing its job and the code not fighting it. The storage layer is not where the Tier D
risk is.

**All five breaches are in the security layer**, and none of them is a bug in the sense of
"wrong behaviour" — each is a documented parameter whose measured cost had not been measured:
two Argon2id profiles that are correctly marked non-mobile but are not *prevented* on mobile,
and a guarded-allocator amplification nobody had put a number on. Two of them (§7.3) fail by
OOM-kill rather than by `Status`, which is the one failure mode `performance-strategy.md` §10
singles out as unacceptable. Both are cheap to gate and expensive to discover in the field.

Three sub-budget observations that are not breaches but are *at the line*, and on hardware
faster than the target: OCR at 18.8 Mpx is at 102 % of the 150 MB OCR sub-budget; a 16 MiB
Markdown write is at 99 % of the 96 MB conversion arena (which does not exist as a mechanism);
and `MobileInteractive` Argon2id takes 30 % of the entire peak budget for one unlock.

---

## 11. Reproducing this

```sh
cmake -S . -B build_loadtest -DCMAKE_BUILD_TYPE=Release \
      -DUAPP_BUILD_TESTS=OFF -DUAPP_BUILD_TOOLS=ON
cmake --build build_loadtest -j8

mkdir -p /tmp/uapp-loadtest

# storage engines — one module at a time, so a long run can be resumed
./build_loadtest/bin/uapp-loadtest-storage --dir /tmp/uapp-loadtest notes
./build_loadtest/bin/uapp-loadtest-storage --dir /tmp/uapp-loadtest blocks
./build_loadtest/bin/uapp-loadtest-storage --dir /tmp/uapp-loadtest search
./build_loadtest/bin/uapp-loadtest-storage --dir /tmp/uapp-loadtest database
# the two conversion directions separately: peak RSS is per-process
./build_loadtest/bin/uapp-loadtest-storage --dir /tmp/uapp-loadtest conversion_read
./build_loadtest/bin/uapp-loadtest-storage --dir /tmp/uapp-loadtest conversion_write

# crypto — one subcommand per process, for the same reason
for m in aead docs subkey hierarchy live-keys \
         kdf-mobile kdf-moderate kdf-sensitive; do
    ./build_loadtest/bin/uapp-loadtest-crypto "$m"
done

# OCR
./build_loadtest/bin/uapp-loadtest-ocr sizes
./build_loadtest/bin/uapp-loadtest-ocr density
./build_loadtest/bin/uapp-loadtest-ocr --calls 300 repeat
./build_loadtest/bin/uapp-loadtest-ocr --cycles 300 open-close
```

Each binary writes one CSV row per measured point to **stdout**, flushed as it goes, and
commentary to **stderr** — so `> results.csv` collects machine-readable results and an
interrupted run still keeps every point it reached. `--max-db-scale N` caps the database ladder
(the 100 000-row point takes ~90 s to ingest). Approximate total runtime on this host: notes
74 s, blocks 86 s, search 44 s, database 96 s, conversion < 1 s, crypto 10 s plus 6 s for the
three KDF profiles, OCR 43 s for `sizes` plus 8 s for `density` plus 13 s for `repeat` plus 11 s
for 300 open/close cycles.

### What to add next, in priority order

1. **`pdf_loadtest.cpp`** — the one real module with no coverage here (§9).
2. **Run this on a real Tier B and Tier D device.** Nearly every "inside budget" verdict above
   is inside budget *on a workstation*, and §0 explains why that is not the same claim.
3. **Re-run OCR against `tessdata_fast`**, which is what `performance-strategy.md` §1.2 says
   must ship, and which is the open item in `docs/spikes/SP-10-findings.md`.
4. **Wire the P0 points into `performance-strategy.md` §7's regression gate** once there is a
   baseline series to compare against. The CSV output is already shaped for it.

