# ADR 0004 — A `Database` is anchored to a `Page`, not a kind of `Page`

- **Status:** Accepted for implementation
- **Date:** 2026-10-17
- **Deciders:** Implementing engineer (proposed); requires approval from the engineering owner
- **Supersedes:** —
- **Related:** `docs/database-design.md` §3.4, `docs/adr/0003-page-supersedes-note.md`, spec §15

---

## Context

Spec §15 (Database System) wants property-typed collections — Text/Number/Boolean/Select/
Multi-select/Date/Person/URL/Email/File/Relation/Rollup/Formula properties, with Table/Board/
Calendar/Timeline/Gallery/List views, filters, sorting and saved views. Spec §14's own
hierarchy line names where this sits:

    Workspace -> Page -> Blocks / Child Pages / Databases / Files
                                                 ^^^^^^^^^

A Database is a **child of a Page**, a sibling of Blocks — not a kind of Page, and not a kind
of Block. `database-design.md` §3.4, written during the Phase 0 architecture study before
`core/blocks` existed as real code, specified something different: `db_schema.document_id`
keyed directly off a `document` table, i.e. Option A below. Implementing the real feature meant
confirming or overriding that sketch against what `core/blocks` actually is today.

### The three shapes considered

**(A) A Database IS a Page — `page.kind = database`.** This is literally what
`database-design.md` §3.4 specified. Rejected for now, not on taste: `blocks::Page` is a
concrete struct whose contract is "a title plus a validated tree of blocks," and every existing
reader of it — `BlockStore::get_page`, `DocumentService`, the Markdown exporter in
`core/conversion`, and the **frozen C ABI** in `core/capi` — would immediately be reading a
page that is not a block container, and would export it as an empty document. Making that safe
means teaching four modules, one of them a published and versioned ABI this change is not
allowed to touch, that some pages are not documents, plus a `page.kind` column and a schema
version bump on the `page` table itself. That is a migration, not a slice.

**(B) A Database is a block payload — `BlockType::Database`.** Rejected: a block's content is
sized into every `Block` (`sizeof(BlockContent)` is bounded by its largest alternative), and a
full schema plus option sets is orders of magnitude larger than the two `std::string`s a text
block carries. It would also make the database's lifetime the block's lifetime, which makes two
different views of one collection on two different pages impossible — and multiple views of one
collection is an explicit spec §15 requirement.

**(C) A Database is its own entity, with its own `DatabaseId`, anchored to a host
`blocks::PageId` via a foreign key, cascade-deleted with its page.** Chosen. This matches spec
§14's hierarchy line exactly — Databases are a child of a Page, not a kind of one — and the
change is purely additive: new tables (`db_collection`, `db_property`, `db_row`, `db_value`,
`db_view`, ...), with zero edits to the `page` table, `block` table, `core/conversion`, or
`core/capi`.

## Decision

**(C): `database::Database` is a standalone entity (its own id, its own table) that references
a host `blocks::PageId`, non-nullable, cascade-deleted when the host page is deleted.** Rows
and views belong to the database, not to the page.

This is a recorded **deviation from `database-design.md` §3.4**, not a rejection of the
Phase 0 sketch's eventual destination — it defers it. Once `page.kind` and child pages exist as
real features, a "full-page database" becomes a page whose sole content is one database
anchored to it (shape A, finally safe to build because the four readers above will already
handle `page.kind`), and `BlockType::Database` becomes an *inline view block* that references
a `DatabaseId` (shape B, as a thin pointer rather than the payload itself). Both are additive on
top of shape C; neither requires undoing it. This ADR's job is to record that migration and its
trigger condition, so the deferral does not quietly become permanent the way ADR-0003 warned a
`Note`/`Page` split would.

**Trigger for revisiting this ADR:** when `page.kind` (or an equivalent child-page mechanism)
lands, re-open this decision rather than letting shape C persist by default.

### What is explicitly NOT implemented in this slice, and why

- **Property types:** Text, Number, Boolean, Select, Multi-select, Date are real. Person, URL,
  Email, File, Relation, Rollup and Formula are declared in `PropertyType` and refused with a
  machine-readable reason (`property_type_not_implemented_reason()`, the same precedent
  `block_type_not_implemented_reason()` set). Relation and Rollup need cross-database
  references; Formula needs an expression evaluator. Both are real future features, not gaps in
  this pass.
- **Views:** Table only. Board/Calendar/Timeline/Gallery/List are declared, refused with
  reasons. Table is implemented first because it is the only kind that is a view of the
  *schema* rather than of one distinguished property (Board needs a Select to group by,
  Calendar/Timeline need a Date, Gallery needs a File/image) — every other kind is Table plus a
  projection, so Table had to exist first.
- **Rows-open-as-pages**, schema evolution (adding/removing/retyping a property after
  creation — retyping is a product decision about data loss under sync, not a coding gap),
  manual row reordering (needs the same fractional-index work `sqlite_block_store.cpp` already
  defers for blocks), and workspace-scoping (a database inherits its host page's implicit
  workspace, same as `page` itself) are all out of scope, each for its own documented reason in
  `core/database/include/uapp/database/*.h`.

## Consequences

**Positive**

- The real, typed property/row/view model from spec §15 exists and is tested, with zero
  changes to `page`, `block`, the Markdown converter, or the already-frozen C ABI.
- `core/database` links only `core/blocks` (for `PageId`) and `core/common` — never
  `storage/sqlite` directly, confirmed via the link graph (`nm -u` on the built static
  library), matching the layering every other core module in this project holds.

**Negative**

- A deviation from `database-design.md` §3.4's original schema sketch, recorded here rather
  than silently diverged from. A future reader of that document must be pointed at this ADR.
- Two more ids to reason about (`DatabaseId`, `RowId`, `PropertyId`), though — as with every
  other id in this codebase — all are globally unique by construction via the standard
  `ObjectId<Tag>` generator, so this is bookkeeping, not a correctness risk.

**Neutral**

- Three new `Status` codes (`DatabasePropertyNotInSchema`, `DatabaseValueTypeMismatch`,
  `DatabaseOptionNotInSchema`, plus the view/limit-related codes) were added and flattened to
  `UAPP_STATUS_UNEXPECTED` in `core/capi`, with no new ABI constant minted — the same
  reachability rule applied to every prior module (blocks, search, file-engine, pdf-engine)
  before it got its own C ABI surface. `core/database` has none yet.
