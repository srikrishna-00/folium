// SPDX-License-Identifier: Apache-2.0
//
// core/database/database_store.h — the persistence PORT for databases, rows
// and saved views.
//
// Same seam, same rules as core/blocks/block_store.h: PHASE0_SUMMARY.md H-3
// ("nothing outside storage/sqlite includes sqlite3.h") and spec section 32
// ("do not tightly couple business logic to one database engine").
// core/database links this abstract interface only; storage/sqlite implements
// it.
//
// Contract every implementation MUST honour:
//  * Every mutating method is ALL-OR-NOTHING. A database whose schema was
//    written but whose option sets were not is a corrupt collection, not a
//    recoverable error, so each of these runs in one transaction.
//  * Every method VALIDATES AGAINST THE SCHEMA, on write and on read. The
//    service layer is the polite front door; this interface is reachable by
//    anyone holding the pointer, and the file behind it is attacker-reachable
//    (spec section 28).
//  * No method throws (ADR-0001: -fno-exceptions).
//  * Out-parameters are written only on Status::Ok.
//  * `query_table_view` MUST return exactly what
//    database::evaluate_table_view would return over the same rows, in the
//    same order. That function is the semantic definition of a Table view;
//    an implementation that compiles the config into a query is an
//    optimisation of it, and tests/database asserts the agreement
//    differentially rather than trusting it.

#ifndef UAPP_CORE_DATABASE_DATABASE_STORE_H
#define UAPP_CORE_DATABASE_DATABASE_STORE_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "uapp/blocks/page.h"
#include "uapp/database/database.h"
#include "uapp/database/view.h"
#include "uapp/status.h"

namespace uapp::database {

class DatabaseStore {
public:
    virtual ~DatabaseStore();
    DatabaseStore(const DatabaseStore&) = delete;
    DatabaseStore& operator=(const DatabaseStore&) = delete;

    // --- databases ---------------------------------------------------------

    /// Create a database AND its whole schema in one call.
    ///
    /// This differs from BlockStore::create_page, which requires an empty
    /// block list, and the difference is deliberate: a page with no blocks is
    /// a usable page, while a schema is not incrementally buildable in this
    /// build (adding or retyping a property afterwards is NOT IMPLEMENTED --
    /// see database.h). Accepting the schema here is therefore the only way
    /// to create a usable database, not a convenience.
    ///
    /// Status::NotFound if the host page does not exist, Status::AlreadyExists
    /// if the id is taken.
    [[nodiscard]] virtual Status create_database(const Database& db) noexcept = 0;

    /// Load a database and its full schema, properties in column order and
    /// options in option order. Status::NotFound if absent.
    [[nodiscard]] virtual Status get_database(const DatabaseId& id,
                                              Database& out) const noexcept = 0;

    /// Rename a database. The only schema mutation this build supports, and
    /// the only one that needs no value migration.
    [[nodiscard]] virtual Status set_database_name(const DatabaseId& id,
                                                   std::string_view name,
                                                   std::int64_t now_ms) noexcept = 0;

    /// Delete a database, its schema, every row, every value and every saved
    /// view. Status::NotFound if absent.
    [[nodiscard]] virtual Status delete_database(const DatabaseId& id) noexcept = 0;

    /// Every database anchored to one page, in creation order.
    [[nodiscard]] virtual Status list_databases_on_page(
        const blocks::PageId& page_id,
        std::vector<DatabaseId>& out) const noexcept = 0;

    [[nodiscard]] virtual Status database_count(std::size_t& out) const noexcept = 0;

    // --- rows --------------------------------------------------------------

    /// Insert a row. `row.values` is validated against the schema in full:
    /// a value of the wrong type, a value violating its type's own rules, or
    /// an option the property does not define is rejected, not coerced.
    ///
    /// Status::NotFound if the database is absent,
    /// Status::DatabaseRowLimitExceeded at kMaxRowsPerDatabase.
    [[nodiscard]] virtual Status insert_row(const Row& row,
                                            std::int64_t now_ms) noexcept = 0;

    [[nodiscard]] virtual Status get_row(const RowId& id, Row& out) const noexcept = 0;

    /// REPLACE a row's values wholesale. A property absent from
    /// `row.values` becomes UNSET -- this is a PUT, not a PATCH, and the
    /// distinction is in the name of the method rather than in a flag,
    /// because a partial update whose semantics are a flag is a partial
    /// update whose semantics get guessed at the call site.
    [[nodiscard]] virtual Status update_row(const Row& row,
                                            std::int64_t now_ms) noexcept = 0;

    [[nodiscard]] virtual Status delete_row(const RowId& id,
                                            std::int64_t now_ms) noexcept = 0;

    [[nodiscard]] virtual Status row_count(const DatabaseId& id,
                                           std::size_t& out) const noexcept = 0;

    // --- saved views -------------------------------------------------------

    /// Create or replace a named view. Upsert rather than create+update,
    /// because a view IS its configuration: there is no partial edit of one
    /// that is not just "store the new config".
    ///
    /// Status::DatabaseViewKindUnsupported for any kind but Table,
    /// Status::DatabaseViewLimitExceeded at kMaxViewsPerDatabase.
    [[nodiscard]] virtual Status save_view(const View& view,
                                           std::int64_t now_ms) noexcept = 0;

    [[nodiscard]] virtual Status get_view(const ViewId& id, View& out) const noexcept = 0;

    /// Every saved view on a database, in creation order.
    [[nodiscard]] virtual Status list_views(const DatabaseId& id,
                                            std::vector<View>& out) const noexcept = 0;

    [[nodiscard]] virtual Status delete_view(const ViewId& id) noexcept = 0;

    // --- querying ----------------------------------------------------------

    /// Run a Table view described by an AD-HOC config (an unsaved filter the
    /// user is typing). Rows come back filtered, sorted and truncated to
    /// `limit`, with their values materialised.
    [[nodiscard]] virtual Status query_table_view(const DatabaseId& id,
                                                  const ViewConfig& config,
                                                  std::size_t limit,
                                                  std::vector<Row>& out) const noexcept = 0;

    /// Run a SAVED view by id. Equivalent to loading the view and calling
    /// query_table_view with its config; it exists as its own method because
    /// that is the hot path (open a tab) and because it is the one that must
    /// not be able to disagree with the stored config.
    [[nodiscard]] virtual Status query_saved_view(const ViewId& id,
                                                  std::size_t limit,
                                                  std::vector<Row>& out) const noexcept = 0;

protected:
    DatabaseStore() noexcept;
};

}  // namespace uapp::database

#endif  // UAPP_CORE_DATABASE_DATABASE_STORE_H
