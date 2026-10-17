// SPDX-License-Identifier: Apache-2.0
//
// core/database/database.h — the Database entity of spec section 15.
//
//     Workspace -> Page -> Blocks / Child Pages / Databases / Files
//                                                 ^^^^^^^^^
//
// A Database is a NAMED COLLECTION of property DEFINITIONS (the schema) plus
// ROWS of property VALUES conforming to that schema.
//
// ===========================================================================
// THE ARCHITECTURAL DECISION: A DATABASE IS ANCHORED TO A PAGE, NOT A PAGE
// ===========================================================================
// This is a real fork and it is argued in full in
// docs/adr/0004-database-anchored-to-page.md. The short form, because the
// decision has to be legible from the type:
//
// Three shapes were available.
//
//   (A) A Database IS-A Page — `page.kind = database`, which is literally
//       what database-design.md section 3.4 specifies (`db_schema.document_id
//       PRIMARY KEY REFERENCES document(id)`). REJECTED FOR NOW, not on
//       taste: `blocks::Page` is a concrete struct whose contract is "a title
//       plus a validated tree of blocks", and every existing reader of it --
//       BlockStore::get_page, DocumentService, the Markdown exporter in
//       core/conversion, and the FROZEN C ABI in core/capi -- would
//       immediately be reading a page that is not a block container and
//       exporting it as an empty document. Making that safe means teaching
//       four modules (one of them a published ABI this change is not allowed
//       to touch) that some pages are not documents, plus a `page.kind`
//       column and a schema-version bump. That is a migration, not a slice.
//
//   (B) A Database IS a block payload — `BlockType::Database`. REJECTED: a
//       block payload is sized into every Block (`sizeof(BlockContent)` is
//       its largest alternative), and a schema plus an option set is orders
//       of magnitude larger than two std::strings. It also makes the
//       database's lifetime the block's lifetime, so two views of one
//       collection on two pages becomes impossible.
//
//   (C) A Database is its OWN entity with its own `DatabaseId`, anchored to a
//       host `blocks::PageId`. CHOSEN. Spec section 14's own hierarchy line
//       -- quoted at the top of this file and at the top of blocks/page.h --
//       lists Databases as a CHILD OF A PAGE, a sibling of blocks, not as a
//       kind of page. The change is then purely additive: new tables, a new
//       `database_schema_version`, and zero edits to `page`, `block`,
//       core/conversion or core/capi.
//
// What (C) costs, stated plainly rather than hidden: it is a DEVIATION from
// database-design.md section 3.4, which keyed the schema on a document id.
// The deviation is deferred, not rejected -- when `page.kind` and child pages
// land, a "full-page database" becomes a page whose sole content is one
// database anchored to it, and `BlockType::Database` becomes an INLINE VIEW
// that references a DatabaseId. Both of those are additive on top of this
// shape. ADR-0004 records the migration and the end date.
//
// What is NOT IMPLEMENTED here, each with its own reason:
//
//   ROWS OPEN AS PAGES     database-design.md section 3.4 gives `db_row` a
//                          nullable `page_id` so each row can be opened as a
//                          document. Deliberately absent: it makes a row's
//                          lifetime entangled with a page's (delete which
//                          first?) and it is the only feature here that would
//                          need to write to the `page` table, which this
//                          module does not own.
//   SCHEMA EVOLUTION       Adding, removing or RETYPING a property after
//                          creation. Retyping is the blocker and it is a
//                          product decision, not a coding one: what does the
//                          Number 42 become when the column becomes a Select,
//                          and what happens to the rows it cannot become?
//                          Under sync it is additionally the concurrent-DDL
//                          problem section 3.4 chose EAV to avoid. A schema
//                          is therefore fixed at creation in this build.
//   MANUAL ROW ORDER       Dragging a row to a position. Needs the fractional
//                          index story sqlite_block_store.cpp already defers
//                          for blocks; until then the default order is by row
//                          id, which for a UUIDv7 id IS creation order
//                          (database-design.md section 2).
//   WORKSPACES             A database inherits its host page's (implicit)
//                          workspace. No workspace_id column, same as `page`.

#ifndef UAPP_CORE_DATABASE_DATABASE_H
#define UAPP_CORE_DATABASE_DATABASE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "uapp/blocks/page.h"
#include "uapp/database/property.h"
#include "uapp/status.h"

namespace uapp::database {

struct DatabaseIdTag;
struct RowIdTag;

using DatabaseId = ObjectId<DatabaseIdTag>;
using RowId      = ObjectId<RowIdTag>;

/// Maximum bytes in a database name. Matches blocks::kMaxPageTitleBytes
/// exactly, and deliberately: a database's name is shown in the same places a
/// page title is (sidebar, breadcrumb, search result), so two different caps
/// would mean one of those places has to truncate and the other does not.
inline constexpr std::size_t kMaxDatabaseNameBytes = 512;

/// One user-defined database: the schema and its identity. Rows are NOT
/// carried here -- a database may hold kMaxRowsPerDatabase of them, and a
/// type that sometimes holds 100 000 rows and sometimes holds none is a type
/// whose callers cannot tell which they have. Rows are read through
/// DatabaseStore, by view.
struct Database final {
    DatabaseId     id;
    /// The HOST PAGE. Never nil: a database that is not on a page is
    /// unreachable in the product's navigation model, and deleting the page
    /// deletes the database with it (ON DELETE CASCADE in the adapter).
    blocks::PageId page_id;
    std::string    name;
    std::int64_t   created_at_ms  = 0;
    std::int64_t   modified_at_ms = 0;

    /// The schema, in COLUMN ORDER. Order is part of the schema, not a view
    /// setting: it is what a Table view shows left-to-right by default and
    /// what an export writes.
    std::vector<PropertyDefinition> properties;
};

/// Find a property definition by id. Returns nullptr when the database does
/// not define it, which is the Status::DatabasePropertyNotInSchema path.
[[nodiscard]] const PropertyDefinition* find_property(const Database& db,
                                                      const PropertyId& id) noexcept;

/// One row: a set of property VALUES keyed by property id.
///
/// WHY A MAP AND NOT A VECTOR PARALLEL TO `Database::properties`: a row is
/// SPARSE. Most rows leave most properties unset, and "unset" is a real,
/// distinct state from every value a property can hold -- it is what the
/// `is empty` filter operator tests. A positional vector would have to
/// represent it as a sentinel per type, and the first type whose sentinel is
/// a legal value (Checkbox: is `false` unset or unchecked?) breaks it. The
/// map's key ordering is over raw id bytes and carries no domain meaning;
/// column order comes from the schema.
struct Row final {
    RowId        id;
    DatabaseId   database_id;
    std::int64_t created_at_ms  = 0;
    std::int64_t modified_at_ms = 0;

    std::map<PropertyId, PropertyValue> values;
};

/// The value of one property on one row, or nullptr if the row leaves it
/// unset. The nullptr case is NOT an error: see Row's declaration.
[[nodiscard]] const PropertyValue* find_value(const Row& row, const PropertyId& id) noexcept;

/// Validate a database's identity and schema: non-nil ids, a legal name,
/// non-negative timestamps, every property definition legal, no duplicate
/// property id or name, and at most kMaxPropertiesPerDatabase properties.
///
/// A database with ZERO properties is legal. That is not an oversight: "new
/// database, no columns yet" is the state every database starts in, and
/// rejecting it would mean the only way to create one is to create it
/// complete.
[[nodiscard]] Status validate_database_schema(const Database& db) noexcept;

/// Validate a row AGAINST a schema. This is the spec section 38 boundary for
/// this module and it is deliberately total:
///
///   * the row's database_id must be the database's id,
///   * every property id the row carries must be one the schema defines
///     (Status::DatabasePropertyNotInSchema -- a row cannot invent a column),
///   * every value must be of its property's declared type
///     (Status::DatabaseValueTypeMismatch),
///   * every Select / Multi-select option must be one the property defines
///     (Status::DatabaseOptionNotInSchema),
///   * every value must satisfy its own type's rules (size, finiteness,
///     date range, non-emptiness).
///
/// Called on write AND on read, because a database file is attacker-reachable
/// -- a synced or restored .db is an untrusted input even though it used to
/// be ours (spec section 28).
[[nodiscard]] Status validate_row(const Database& db, const Row& row) noexcept;

/// Validate database METADATA only (id, page id, name, timestamps), without
/// walking the schema. Used where the schema has already been checked.
[[nodiscard]] Status validate_database_metadata(const Database& db) noexcept;

/// Shared database-name validation. An empty name is allowed -- "Untitled" is
/// a real state, exactly as blocks::validate_page_title allows an empty page
/// title -- so only the upper bound and an embedded NUL are errors.
[[nodiscard]] Status validate_database_name(std::string_view name) noexcept;

}  // namespace uapp::database

#endif  // UAPP_CORE_DATABASE_DATABASE_H
