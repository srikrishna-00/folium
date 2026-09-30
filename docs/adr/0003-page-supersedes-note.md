# ADR 0003 — `Page` supersedes `Note`, but they coexist until the C ABI moves

- **Status:** Accepted for implementation — the coexistence window needs an owner and an end date
- **Date:** 2026-09-30
- **Deciders:** Implementing engineer (proposed); requires approval from the engineering owner
- **Supersedes:** —
- **Related:** `docs/architecture.md`, `docs/database-design.md` §3.3, `docs/adr/0001-core-language-c-vs-cpp.md`,
  spec §14

---

## Context

`core/notes` (Phase 1 bootstrap) models a note as a **flat** record: one `id`, one `title`,
one `body` string, two timestamps. Its own header says so explicitly — "the minimum viable
ancestor of the `document` + `block` model in database-design.md §3.3 … the block tree, CBOR
payloads, fractional sort keys and CRDT state are later phases."

`core/blocks` (this change) implements that real model: a `Page` with an ordered, nested tree
of typed `Block`s. Spec §14's `Workspace -> Page -> Blocks` is the product's actual document
abstraction. Everything the product needs later — backlinks, comments, mentions, block-level
sync, version history, block-level permissions — anchors to a **block id**. A flat body string
has nothing to anchor to.

So the two are not peers. `Page` is what `Note` was standing in for. The question this ADR
settles is not *which one wins* — it is **how the loser is retired**.

### Forces

1. **`core/capi` is built on `NoteService`.** Deleting or rewriting `core/notes` today means
   rewriting the C ABI in the same change. The C ABI is separately versioned and is the
   contract the Kotlin and Swift bindings will be written against; churning it while the block
   model is one day old is exactly the wrong order.
2. **The note slice is the working end-to-end proof.** `uapp.notes.cli_demo` drives
   create → store → search → open → edit → save against a real database. It is currently the
   only evidence in the repo that the layering works end to end. Breaking it to make a point
   about layering would be self-defeating.
3. **`Note` is not representable as a `Page` without loss, and the reverse is worse.** A note's
   `body` maps cleanly onto a single paragraph block — that direction is fine. But `Page` has
   no full-text search yet (`core/blocks` has no FTS counterpart to `note_fts`), so making
   `Note` a thin view over `Page` today would *remove* working search.
4. **Two live models is a real cost.** Two id types, two `Clock` ports, two stores, two schemas
   in the same database file. Left alone it becomes permanent, and then every feature is built
   twice.

---

## Decision

**`Page` is the product's document model. `Note` is deprecated in place, not deleted.**

Concretely, for this run:

1. `core/blocks` is the model everything new builds on. No new feature is added to
   `core/notes`.
2. `core/blocks` does **not** link `core/notes` — enforced by the build graph
   (`core/blocks/CMakeLists.txt` names neither `uapp::core_notes` nor `uapp::sqlite3`), and by
   `uapp_blocks_tests` linking only `uapp::storage_sqlite`. The successor must not acquire a
   dependency on the thing it replaces.
3. The generic parts of `core/notes` that `core/blocks` also needs were **promoted, not
   copied**: the 16-byte identifier (`uapp::ObjectId<Tag>`) and the `Clock` / `IdSource` ports
   now live in `core/common`. `notes::NoteId`, `notes::Clock` and `notes::IdGenerator` are
   left untouched so that `core/capi` does not move in this change.
4. `page` and `block` are new tables with their own `block_schema_version`. The `note` table is
   not migrated and not touched.

### The retirement path (the part that needs an owner)

`Note` becomes a **special case of `Page`**: a page whose block list is a single paragraph.
The migration is mechanical and should happen in this order:

1. Give `core/blocks` full-text search over block text (the `text_cache` + FTS5 counterpart to
   `note_fts`). Until this exists, `Page` is strictly less capable than `Note` and the
   migration cannot be justified.
2. Move `core/capi` onto a page/block ABI. This is the real work and the real risk.
3. One-shot migration: each `note` row becomes a `page` row plus one `paragraph` block,
   preserving `id` (both are 16-byte opaque ids, which is precisely why `NoteId` was given the
   real shape in the bootstrap slice — no id migration is required).
4. Delete `core/notes`, `storage/sqlite/src/sqlite_note_store.cpp`, and the `note` table.

**This ADR does not authorise an indefinite coexistence.** If steps 1–2 have not started by
the time a third module needs to choose between the two models, the choice will already have
been made by accident.

---

## Consequences

**Positive**

- The real document model exists and is tested, without destabilising the one working
  end-to-end slice or the published C ABI.
- The duplicated concepts are now exactly three (`NoteId` vs `ObjectId`, `notes::Clock` vs
  `uapp::Clock`, `NoteStore` vs `BlockStore`), all of them named here, all of them retired by
  the same migration.

**Negative**

- Two persistence models in one codebase, and potentially in one database file. A reader of
  the repo has to be told which one is the future; that is what this ADR is for.
- `core/common` now carries two overlapping port sets (`uapp::Clock` and `notes::Clock`). This
  is deliberate temporary duplication and is called out in both headers.

**Neutral**

- `uapp::Status` gained a `5xx` block range and a generic `IdMalformed`. `core/capi` maps all
  of them to `UAPP_STATUS_UNEXPECTED` rather than minting permanent ABI constants for errors
  no ABI call can currently return.
