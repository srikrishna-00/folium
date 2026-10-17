// SPDX-License-Identifier: Apache-2.0
//
// storage/sqlite/document_sql.h — the `page` + `block` DDL, as ONE constant.
//
// Internal to this target. Does not leave storage/sqlite (PHASE0_SUMMARY H-3).
//
// ===========================================================================
// WHY THIS FILE EXISTS
// ===========================================================================
// This DDL used to live as a file-local `kSchemaSql` inside
// sqlite_block_store.cpp, which was correct while exactly one adapter created
// those tables. sqlite_database_store.cpp is now a SECOND adapter that needs
// them: a `db_collection` row carries `page_id BLOB NOT NULL REFERENCES
// page(id) ON DELETE CASCADE`, so `page` must exist before a database can be
// anchored to one, and the database store cannot assume that the block store
// opened the file first (on ":memory:" it provably did not -- two opens of
// ":memory:" are two private databases).
//
// The two options were: copy the DDL into the second adapter, or share it.
// Copying is how two `CREATE TABLE IF NOT EXISTS page` statements end up
// differing by one CHECK constraint, after which WHICHEVER ADAPTER OPENS THE
// FILE FIRST SILENTLY WINS and the other's constraints are simply absent.
// That failure is invisible until a row that should have been rejected is
// not. So it is shared, exactly as search_index_sql.h is shared by the note
// store and the block store for the same reason, and that header's
// "apply_schema is called by EVERY store adapter that writes content, and the
// DDL is a single shared string precisely so the two can never disagree about
// the shape" applies verbatim here.
//
// NOTHING ELSE MOVED. The encode/decode pair, the tree assembly, the sibling
// arithmetic and the schema-version bookkeeping all stay in
// sqlite_block_store.cpp, which still OWNS these tables: it is the only file
// that writes to them. This header is the shape, not the behaviour.

#ifndef UAPP_STORAGE_SQLITE_DOCUMENT_SQL_H
#define UAPP_STORAGE_SQLITE_DOCUMENT_SQL_H

namespace uapp::storage::sqlite::document_sql {

// ---------------------------------------------------------------------------
// Schema
//
// Mirrors database-design.md section 3.3 (`document` + `block`) minus what
// this run does not have: workspaces, child pages, soft deletes, sync
// columns, CRDT state. Two deliberate, documented deviations from that
// document:
//
// 1. ORDERING IS AN INTEGER `sort_index`, NOT A FRACTIONAL `sort_key`.
//    The design doc argues -- correctly -- that integer positions turn a
//    1-block insert into an N-row renumber, which is catastrophic for sync
//    because it becomes an N-block conflict. That cost is *only* paid once
//    sync exists (Phase 13). Today there is exactly one writer on one device,
//    an insert renumbers at most the tail of one sibling list inside the same
//    transaction, and dense 0..n-1 positions make every ordering assertion in
//    the test suite exact rather than "compare two opaque strings".
//    Fractional indexing is therefore deferred, NOT rejected: the migration
//    is `ALTER TABLE block ADD COLUMN sort_key TEXT` plus a one-pass backfill
//    in sort_index order, with no change to the block identifiers.
//
// 2. CONTENT IS IN TYPED COLUMNS, NOT A CBOR BLOB.
//    The doc chooses CBOR because nineteen block types with disjoint fields
//    would otherwise mean nineteen tables or a sixty-column sparse table.
//    With the eight types implemented here the whole payload space is four
//    columns (text, checked, heading_level, language), and four real columns
//    beat a hand-rolled codec that would need its own fuzz target before it
//    could be trusted with a file off the network. `schema_ver` is present
//    from day one so the switch to CBOR -- which becomes right as soon as a
//    type like `table` or `image` lands -- is a versioned migration rather
//    than a rewrite. core/blocks/block.h documents that trigger point.
//
// There is deliberately NO `kind` COLUMN on `page`, even though
// database-design.md section 3.3 gives `document` one (0 page, 1 database,
// 2 file-page). A property-typed DATABASE is anchored to a page rather than
// being a kind of page in this build; see
// docs/adr/0004-database-anchored-to-page.md for why, and for the migration
// that adds this column when full-page databases and child pages land.
//
// There is deliberately still no `text_cache` COLUMN on `block`: the text is
// already in `block.text`, and a denormalised copy would be derived data the
// store has to keep consistent with its own source for no reader that needs
// it. database-design.md section 9 lists exactly that drift
// ("`text_cache` drifts from `content`") as a known integrity hazard.
//
// `schema_meta` is repeated here (it is also in search_index_sql.cpp's DDL)
// so that this DDL is self-sufficient: whichever adapter opens the file first
// must be able to create everything it needs.
//
// There is deliberately NO UNIQUE(page_id, parent_id, sort_index) constraint.
// Opening a gap is `UPDATE ... SET sort_index = sort_index + 1 WHERE ...`,
// and SQLite applies that row by row: with a unique index the very first row
// it touches would collide with its neighbour and the statement would fail
// even though the end state is perfectly dense. Density is instead an
// invariant sqlite_block_store.cpp maintains and the tests assert.
// ---------------------------------------------------------------------------
inline constexpr char kSchemaSql[] =
    "CREATE TABLE IF NOT EXISTS schema_meta ("
    "  key   TEXT PRIMARY KEY,"
    "  value INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS page ("
    "  id             BLOB    NOT NULL PRIMARY KEY CHECK(length(id) = 16),"
    "  title          TEXT    NOT NULL,"
    "  created_at_ms  INTEGER NOT NULL CHECK(created_at_ms  >= 0),"
    "  modified_at_ms INTEGER NOT NULL CHECK(modified_at_ms >= 0)"
    ");"
    "CREATE TABLE IF NOT EXISTS block ("
    "  id             BLOB    NOT NULL PRIMARY KEY CHECK(length(id) = 16),"
    "  page_id        BLOB    NOT NULL REFERENCES page(id) ON DELETE CASCADE,"
    "  parent_id      BLOB    REFERENCES block(id) ON DELETE CASCADE"
    "                         CHECK(parent_id IS NULL OR length(parent_id) = 16),"
    "  type           INTEGER NOT NULL CHECK(type >= 0),"
    "  sort_index     INTEGER NOT NULL CHECK(sort_index >= 0),"
    "  schema_ver     INTEGER NOT NULL DEFAULT 1,"
    "  text           TEXT    NOT NULL DEFAULT '',"
    "  checked        INTEGER NOT NULL DEFAULT 0 CHECK(checked IN (0, 1)),"
    "  heading_level  INTEGER NOT NULL DEFAULT 0 CHECK(heading_level BETWEEN 0 AND 3),"
    "  language       TEXT    NOT NULL DEFAULT '',"
    "  created_at_ms  INTEGER NOT NULL CHECK(created_at_ms  >= 0),"
    "  modified_at_ms INTEGER NOT NULL CHECK(modified_at_ms >= 0),"
    "  CHECK(parent_id IS NULL OR parent_id <> id)"
    ");"
    "CREATE INDEX IF NOT EXISTS ix_block_page_parent "
    "  ON block(page_id, parent_id, sort_index);"
    "CREATE INDEX IF NOT EXISTS ix_block_parent ON block(parent_id);";

}  // namespace uapp::storage::sqlite::document_sql

#endif  // UAPP_STORAGE_SQLITE_DOCUMENT_SQL_H
