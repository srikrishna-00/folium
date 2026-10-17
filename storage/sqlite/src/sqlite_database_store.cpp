// SPDX-License-Identifier: Apache-2.0
//
// The SQLite adapter for the property-typed database system (spec section 15).
// With sqlite_note_store.cpp, sqlite_block_store.cpp and
// sqlite_search_index.cpp this is one of only a handful of translation units
// in the project that include <sqlite3.h>. See PHASE0_SUMMARY.md H-3.
//
// ===========================================================================
// SHAPE: EAV, AND WHY THAT IS RIGHT HERE
// ===========================================================================
// database-design.md section 3.4 already made this call and the argument is
// not repeated so much as confirmed: the alternative -- a real SQL table per
// user-defined database, with `ALTER TABLE` on every property change -- means
// DDL executed on user data, on N devices, that must converge under sync, and
// two concurrent `ALTER TABLE`s are not a solvable merge. EAV makes a schema
// change an ordinary row change.
//
// THREE DELIBERATE DEVIATIONS FROM SECTION 3.4, each because this run has
// something that document did not have when it was written:
//
// 1. `db_schema` IS NOT KEYED ON `document_id`. Section 3.4 made a database a
//    kind of document. This build anchors a database TO a page instead, with
//    its own id -- see docs/adr/0004-database-anchored-to-page.md for the
//    full argument and the migration back toward section 3.4 when `page.kind`
//    and child pages land. The table is called `db_collection` rather than
//    `db_schema` precisely so a future `db_schema(document_id)` can exist
//    alongside it during that migration instead of colliding with it.
//
// 2. MULTI-SELECT IS A MEMBERSHIP TABLE, NOT A CBOR BLOB IN `v_blob`.
//    Section 3.4 puts multi-valued properties in `v_blob` as CBOR. That makes
//    the Multi-select `contains` filter -- the ONLY filter the type has --
//    impossible to answer with an index: every candidate row's blob has to be
//    read and decoded in the application. `db_value_option` is one row per
//    (row, property, option), so `contains` is an index seek on
//    (property_id, option_id) and `does not contain` is its negation. The
//    cost is one extra table and one extra statement per row write, which is
//    the right trade for making the type's only operator sargable.
//
// 3. NO `db_view.spec` CBOR BLOB. Section 3.4 stores a view's filter AST and
//    sort spec as one CBOR blob. Here they are `db_view_filter` and
//    `db_view_sort` rows. Two reasons, and neither is taste: a hand-rolled
//    CBOR codec reading a file off the network needs its own fuzz target
//    before it can be trusted (the same argument sqlite_block_store.cpp makes
//    for keeping block content in typed columns), and real rows let the
//    foreign key on `property_id` make "a saved view referencing a deleted
//    property" impossible instead of merely unlikely. A CBOR blob becomes the
//    right answer at exactly the moment the flat AND becomes a nested AST --
//    `kDatabaseSchemaVersion` exists for that migration.
//
// ===========================================================================
// HOW A TABLE VIEW IS ANSWERED
// ===========================================================================
// `database::evaluate_table_view` in core/database is the SEMANTIC
// DEFINITION of a Table view. This file does NOT reimplement those semantics
// in C++; it COMPILES the same ViewConfig into ONE SQL statement, and
// tests/database/database_store_test.cpp asserts differentially that the two
// return the identical row sequence over the identical data. If they ever
// disagree, the SQL is wrong by definition.
//
// The compiled statement references each filtered/sorted property ONCE, as a
// correlated subquery over `db_value` (or `db_value_option`), rather than
// joining `db_value` M times for an M-property schema. Section 3.4's own
// mitigation note says exactly this: "one `db_value` join per *referenced*
// property (typically 1-3, not M)". Filters use `r.id IN (SELECT v.row_id
// ... WHERE v.property_id = ? AND <col> <op> ?)` so SQLite can drive from the
// partial index on (property_id, <col>) instead of scanning every row of the
// database; sorts use a correlated scalar subquery, which is a primary-key
// lookup per row on a WITHOUT ROWID table.
//
// `NOT IN` is used for the negative operators and that is safe here for a
// specific reason: `db_value.row_id` is NOT NULL, so the subquery cannot
// yield a NULL and `NOT IN` cannot collapse to UNKNOWN. (That collapse is the
// classic `NOT IN` trap, and it would silently return zero rows.)

#include "uapp/storage/sqlite/sqlite_database_store.h"

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "uapp/database/database.h"
#include "uapp/database/database_store.h"
#include "uapp/database/property.h"
#include "uapp/database/view.h"
#include "uapp/object_id.h"
#include "uapp/status.h"
#include "document_sql.h"
#include "sqlite_raii.h"

namespace uapp::storage::sqlite {
namespace {

using database::Database;
using database::DatabaseId;
using database::Filter;
using database::FilterOperator;
using database::OptionId;
using database::PropertyDefinition;
using database::PropertyId;
using database::PropertyType;
using database::PropertyValue;
using database::Row;
using database::RowId;
using database::SelectOption;
using database::Sort;
using database::View;
using database::ViewConfig;
using database::ViewId;
using database::ViewKind;

// ---------------------------------------------------------------------------
// Schema
//
// `UNIQUE(database_id, sort_index)` on `db_property` is safe here even though
// sqlite_block_store.cpp deliberately refuses the analogous constraint on
// `block`. The difference is real: block positions are SHIFTED by a
// row-at-a-time UPDATE that transiently collides, while a property's column
// order is written once at creation and never renumbered (schema evolution is
// NOT IMPLEMENTED -- see core/database/database.h). The constraint therefore
// costs nothing and catches a corrupt file.
//
// The partial indexes on `db_value` are what make a Table view filterable
// without a scan. They are PARTIAL (`WHERE <col> IS NOT NULL`) because a
// db_value row populates exactly one of the four value columns, so a full
// index on each would be three-quarters NULL entries per index -- four times
// the index bytes for the same lookups.
//
// The `CHECK(... = 1)` on `db_value` enforces "exactly one value column is
// populated" in the file itself, so a hand-edited database cannot present a
// row that is simultaneously a number and a date. `db_view_filter` carries NO
// such CHECK, and that is not an oversight: `is empty` / `is not empty` take
// no operand, so all four of its value columns are legitimately NULL.
// ---------------------------------------------------------------------------
constexpr char kSchemaSql[] =
    "CREATE TABLE IF NOT EXISTS db_collection ("
    "  id             BLOB    NOT NULL PRIMARY KEY CHECK(length(id) = 16),"
    "  page_id        BLOB    NOT NULL REFERENCES page(id) ON DELETE CASCADE,"
    "  name           TEXT    NOT NULL,"
    "  created_at_ms  INTEGER NOT NULL CHECK(created_at_ms  >= 0),"
    "  modified_at_ms INTEGER NOT NULL CHECK(modified_at_ms >= 0)"
    ");"
    "CREATE INDEX IF NOT EXISTS ix_db_collection_page ON db_collection(page_id, id);"

    "CREATE TABLE IF NOT EXISTS db_property ("
    "  id          BLOB    NOT NULL PRIMARY KEY CHECK(length(id) = 16),"
    "  database_id BLOB    NOT NULL REFERENCES db_collection(id) ON DELETE CASCADE,"
    "  name        TEXT    NOT NULL,"
    "  type        INTEGER NOT NULL CHECK(type >= 0),"
    "  sort_index  INTEGER NOT NULL CHECK(sort_index >= 0),"
    "  UNIQUE(database_id, sort_index)"
    ");"

    "CREATE TABLE IF NOT EXISTS db_select_option ("
    "  id          BLOB    NOT NULL PRIMARY KEY CHECK(length(id) = 16),"
    "  property_id BLOB    NOT NULL REFERENCES db_property(id) ON DELETE CASCADE,"
    "  name        TEXT    NOT NULL,"
    "  sort_index  INTEGER NOT NULL CHECK(sort_index >= 0),"
    "  UNIQUE(property_id, sort_index)"
    ");"

    "CREATE TABLE IF NOT EXISTS db_row ("
    "  id             BLOB    NOT NULL PRIMARY KEY CHECK(length(id) = 16),"
    "  database_id    BLOB    NOT NULL REFERENCES db_collection(id) ON DELETE CASCADE,"
    "  created_at_ms  INTEGER NOT NULL CHECK(created_at_ms  >= 0),"
    "  modified_at_ms INTEGER NOT NULL CHECK(modified_at_ms >= 0)"
    ");"
    "CREATE INDEX IF NOT EXISTS ix_db_row_database ON db_row(database_id, id);"

    "CREATE TABLE IF NOT EXISTS db_value ("
    "  row_id      BLOB NOT NULL REFERENCES db_row(id) ON DELETE CASCADE,"
    "  property_id BLOB NOT NULL REFERENCES db_property(id) ON DELETE CASCADE,"
    "  v_text      TEXT,"
    "  v_num       REAL,"
    "  v_int       INTEGER,"
    "  v_option    BLOB CHECK(v_option IS NULL OR length(v_option) = 16),"
    "  PRIMARY KEY (row_id, property_id),"
    "  CHECK((v_text IS NOT NULL) + (v_num IS NOT NULL) +"
    "        (v_int IS NOT NULL) + (v_option IS NOT NULL) = 1)"
    ") WITHOUT ROWID;"
    "CREATE INDEX IF NOT EXISTS ix_db_value_text "
    "  ON db_value(property_id, v_text)   WHERE v_text   IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS ix_db_value_num "
    "  ON db_value(property_id, v_num)    WHERE v_num    IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS ix_db_value_int "
    "  ON db_value(property_id, v_int)    WHERE v_int    IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS ix_db_value_option "
    "  ON db_value(property_id, v_option) WHERE v_option IS NOT NULL;"

    "CREATE TABLE IF NOT EXISTS db_value_option ("
    "  row_id      BLOB    NOT NULL REFERENCES db_row(id) ON DELETE CASCADE,"
    "  property_id BLOB    NOT NULL REFERENCES db_property(id) ON DELETE CASCADE,"
    "  option_id   BLOB    NOT NULL CHECK(length(option_id) = 16)"
    "                      REFERENCES db_select_option(id) ON DELETE CASCADE,"
    "  sort_index  INTEGER NOT NULL CHECK(sort_index >= 0),"
    "  PRIMARY KEY (row_id, property_id, option_id)"
    ") WITHOUT ROWID;"
    "CREATE INDEX IF NOT EXISTS ix_db_value_option_member "
    "  ON db_value_option(property_id, option_id);"

    "CREATE TABLE IF NOT EXISTS db_view ("
    "  id             BLOB    NOT NULL PRIMARY KEY CHECK(length(id) = 16),"
    "  database_id    BLOB    NOT NULL REFERENCES db_collection(id) ON DELETE CASCADE,"
    "  name           TEXT    NOT NULL,"
    "  kind           INTEGER NOT NULL CHECK(kind >= 0),"
    "  created_at_ms  INTEGER NOT NULL CHECK(created_at_ms  >= 0),"
    "  modified_at_ms INTEGER NOT NULL CHECK(modified_at_ms >= 0)"
    ");"
    "CREATE INDEX IF NOT EXISTS ix_db_view_database ON db_view(database_id, id);"

    "CREATE TABLE IF NOT EXISTS db_view_filter ("
    "  view_id     BLOB    NOT NULL REFERENCES db_view(id) ON DELETE CASCADE,"
    "  sort_index  INTEGER NOT NULL CHECK(sort_index >= 0),"
    "  property_id BLOB    NOT NULL REFERENCES db_property(id) ON DELETE CASCADE,"
    "  op          INTEGER NOT NULL CHECK(op >= 0),"
    "  v_text      TEXT,"
    "  v_num       REAL,"
    "  v_int       INTEGER,"
    "  v_option    BLOB CHECK(v_option IS NULL OR length(v_option) = 16),"
    "  PRIMARY KEY (view_id, sort_index)"
    ") WITHOUT ROWID;"

    "CREATE TABLE IF NOT EXISTS db_view_sort ("
    "  view_id     BLOB    NOT NULL REFERENCES db_view(id) ON DELETE CASCADE,"
    "  sort_index  INTEGER NOT NULL CHECK(sort_index >= 0),"
    "  property_id BLOB    NOT NULL REFERENCES db_property(id) ON DELETE CASCADE,"
    "  descending  INTEGER NOT NULL CHECK(descending IN (0, 1)),"
    "  PRIMARY KEY (view_id, sort_index)"
    ") WITHOUT ROWID;";

// --- small helpers ---------------------------------------------------------

/// Map a SQLite result code to our vocabulary for a *write*. Same mapping the
/// other content adapters use, for the same reason: a constraint violation on
/// an insert of a caller-supplied id is "that id is taken".
Status write_status_for(int rc) noexcept {
    switch (rc) {
        case SQLITE_OK:
        case SQLITE_DONE:
        case SQLITE_ROW:
            return Status::Ok;
        case SQLITE_READONLY:
            return Status::StoreReadOnly;
        case SQLITE_CONSTRAINT:
            return Status::AlreadyExists;
        default:
            return Status::StoreWriteFailed;
    }
}

Status bind_id(Statement& stmt, int index, const ObjectIdBytes& bytes) noexcept {
    return stmt.bind_blob(index, bytes.data(), kObjectIdBytes);
}

Status bind_size(Statement& stmt, int index, std::size_t value) noexcept {
    return stmt.bind_int64(index, static_cast<std::int64_t>(value));
}

Status read_count(Statement& stmt, std::size_t& out) noexcept {
    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_ROW) { return Status::StoreReadFailed; }
    const std::int64_t n = stmt.column_int64(0);
    if (n < 0) { return Status::StoreReadFailed; }
    out = static_cast<std::size_t>(n);
    return Status::Ok;
}

Status run_to_completion(Statement& stmt) noexcept {
    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) { return write_status_for(rc); }
    return Status::Ok;
}

/// Run one `DELETE FROM <table> WHERE <id_column> = ?1` with a 16-byte id.
Status delete_by_id(sqlite3* db,
                    std::string_view sql,
                    const ObjectIdBytes& id) noexcept {
    Statement stmt;
    if (const Status s = stmt.prepare(db, sql); !is_ok(s)) { return s; }
    if (const Status s = bind_id(stmt, 1, id); !is_ok(s)) { return s; }
    return run_to_completion(stmt);
}

// --- value <-> columns -----------------------------------------------------

/// Which of the four value columns a property type uses. nullptr means the
/// type does not live in `db_value` at all -- Multi-select, whose values are
/// rows of `db_value_option`.
const char* value_column_for(PropertyType type) noexcept {
    switch (type) {
        case PropertyType::Text:     return "v_text";
        case PropertyType::Number:   return "v_num";
        // Checkbox and Date SHARE v_int, distinguished by the property's own
        // declared type. They cannot be confused, because a value is only
        // ever decoded through the definition that owns it -- and merging
        // them saves an index: `db_value` carries one INTEGER index that
        // serves both, rather than one three-quarters-empty index each.
        case PropertyType::Checkbox: return "v_int";
        case PropertyType::Date:     return "v_int";
        case PropertyType::Select:   return "v_option";
        case PropertyType::MultiSelect: return nullptr;
        case PropertyType::Person:
        case PropertyType::Url:
        case PropertyType::Email:
        case PropertyType::File:
        case PropertyType::Relation:
        case PropertyType::Rollup:
        case PropertyType::Formula:
            return nullptr;
    }
    return nullptr;
}

/// A value projected onto the four columns, ready to bind. For Multi-select
/// `options` is populated and the four scalar slots are all unset.
struct ValueColumns final {
    bool             has_text   = false;
    std::string      text;
    bool             has_num    = false;
    double           num        = 0.0;
    bool             has_int    = false;
    std::int64_t     integer    = 0;
    bool             has_option = false;
    ObjectIdBytes    option{};
    bool             is_multi   = false;
    std::vector<OptionId> options;
};

/// Project a value onto the columns. Total over the variant: an added
/// alternative lands on the final `Status::Internal` rather than being
/// silently dropped.
Status encode_value(const PropertyValue& value, ValueColumns& out) {
    ValueColumns columns;

    if (const auto* text = std::get_if<database::TextValue>(&value)) {
        columns.has_text = true;
        columns.text     = text->text;
    } else if (const auto* number = std::get_if<database::NumberValue>(&value)) {
        columns.has_num = true;
        columns.num     = number->number;
    } else if (const auto* checkbox = std::get_if<database::CheckboxValue>(&value)) {
        columns.has_int = true;
        columns.integer = checkbox->checked ? 1 : 0;
    } else if (const auto* select = std::get_if<database::SelectValue>(&value)) {
        columns.has_option = true;
        columns.option     = select->option.bytes();
    } else if (const auto* multi = std::get_if<database::MultiSelectValue>(&value)) {
        columns.is_multi = true;
        columns.options  = multi->options;
    } else if (const auto* date = std::get_if<database::DateValue>(&value)) {
        columns.has_int = true;
        columns.integer = date->unix_ms;
    } else {
        return Status::Internal;
    }

    out = std::move(columns);
    return Status::Ok;
}

/// Bind the four value columns at `first_index` .. `first_index + 3`
/// (text, num, int, option), sending SQL NULL for every unused one.
Status bind_value_columns(Statement& stmt, int first_index, const ValueColumns& columns) {
    if (columns.has_text) {
        if (const Status s = stmt.bind_text(first_index, columns.text); !is_ok(s)) {
            return s;
        }
    } else if (const Status s = stmt.bind_null(first_index); !is_ok(s)) {
        return s;
    }

    if (columns.has_num) {
        // There is no bind_double in sqlite_raii.h's Statement, and adding
        // one for a single caller would widen a shared header; sqlite3's own
        // bind is used directly here, with the same result-code mapping.
        if (sqlite3_bind_double(stmt.get(), first_index + 1, columns.num) != SQLITE_OK) {
            return Status::StoreWriteFailed;
        }
    } else if (const Status s = stmt.bind_null(first_index + 1); !is_ok(s)) {
        return s;
    }

    if (columns.has_int) {
        if (const Status s = stmt.bind_int64(first_index + 2, columns.integer);
            !is_ok(s)) {
            return s;
        }
    } else if (const Status s = stmt.bind_null(first_index + 2); !is_ok(s)) {
        return s;
    }

    if (columns.has_option) {
        if (const Status s = bind_id(stmt, first_index + 3, columns.option); !is_ok(s)) {
            return s;
        }
    } else if (const Status s = stmt.bind_null(first_index + 3); !is_ok(s)) {
        return s;
    }

    return Status::Ok;
}

/// Rebuild a scalar value from the four columns, given the type the schema
/// declares. Multi-select is NOT handled here (it has no `db_value` row).
///
/// Everything here came out of a file on disk, which is attacker-reachable
/// (a synced or restored .db), so a column that is NULL where the type needs
/// a value, or populated where it does not, is Status::StoreReadFailed rather
/// than a silently defaulted zero -- spec section 28.
Status decode_scalar_value(const PropertyDefinition& property,
                           const Statement& stmt,
                           int text_col,
                           int num_col,
                           int int_col,
                           int option_col,
                           PropertyValue& out) {
    switch (property.type) {
        case PropertyType::Text: {
            if (stmt.column_is_null(text_col)) { return Status::StoreReadFailed; }
            out = database::TextValue{stmt.column_text(text_col)};
            return Status::Ok;
        }
        case PropertyType::Number: {
            if (stmt.column_is_null(num_col)) { return Status::StoreReadFailed; }
            out = database::NumberValue{stmt.column_double(num_col)};
            return Status::Ok;
        }
        case PropertyType::Checkbox: {
            if (stmt.column_is_null(int_col)) { return Status::StoreReadFailed; }
            const std::int64_t raw = stmt.column_int64(int_col);
            if (raw != 0 && raw != 1) { return Status::StoreReadFailed; }
            out = database::CheckboxValue{raw == 1};
            return Status::Ok;
        }
        case PropertyType::Date: {
            if (stmt.column_is_null(int_col)) { return Status::StoreReadFailed; }
            out = database::DateValue{stmt.column_int64(int_col)};
            return Status::Ok;
        }
        case PropertyType::Select: {
            if (stmt.column_is_null(option_col)) { return Status::StoreReadFailed; }
            ObjectIdBytes raw{};
            if (const Status s = stmt.column_fixed_blob(option_col, raw.data(), raw.size());
                !is_ok(s)) {
                return s;
            }
            out = database::SelectValue{OptionId(raw)};
            return Status::Ok;
        }
        case PropertyType::MultiSelect:
        case PropertyType::Person:
        case PropertyType::Url:
        case PropertyType::Email:
        case PropertyType::File:
        case PropertyType::Relation:
        case PropertyType::Rollup:
        case PropertyType::Formula:
            return Status::DatabasePropertyTypeUnsupported;
    }
    return Status::StoreReadFailed;
}

// --- the query compiler ----------------------------------------------------

/// One parameter to bind into a compiled statement. A small tagged struct
/// rather than a std::function, so the compiler emits no indirect calls and
/// the binding loop is readable.
struct Binding final {
    enum class Kind { Blob, Text, Double, Int };

    int           index = 0;
    Kind          kind  = Kind::Int;
    ObjectIdBytes blob{};
    std::string   text;
    double        num     = 0.0;
    std::int64_t  integer = 0;
};

Binding blob_binding(int index, const ObjectIdBytes& bytes) {
    Binding b;
    b.index = index;
    b.kind  = Binding::Kind::Blob;
    b.blob  = bytes;
    return b;
}

Status apply_bindings(Statement& stmt, const std::vector<Binding>& bindings) {
    for (const Binding& b : bindings) {
        switch (b.kind) {
            case Binding::Kind::Blob:
                if (const Status s = bind_id(stmt, b.index, b.blob); !is_ok(s)) {
                    return s;
                }
                break;
            case Binding::Kind::Text:
                if (const Status s = stmt.bind_text(b.index, b.text); !is_ok(s)) {
                    return s;
                }
                break;
            case Binding::Kind::Double:
                if (sqlite3_bind_double(stmt.get(), b.index, b.num) != SQLITE_OK) {
                    return Status::StoreWriteFailed;
                }
                break;
            case Binding::Kind::Int:
                if (const Status s = stmt.bind_int64(b.index, b.integer); !is_ok(s)) {
                    return s;
                }
                break;
        }
    }
    return Status::Ok;
}

/// A filter operand, as a binding for the parameter the predicate will use.
/// The operand's column is implied by the property's type, so only the scalar
/// matters here.
Status operand_binding(const PropertyDefinition& property,
                       const Filter& filter,
                       int index,
                       Binding& out) {
    if (!filter.operand.has_value()) { return Status::Internal; }
    const PropertyValue& operand = *filter.operand;

    Binding b;
    b.index = index;

    switch (property.type) {
        case PropertyType::Text: {
            const auto* text = std::get_if<database::TextValue>(&operand);
            if (text == nullptr) { return Status::DatabaseValueTypeMismatch; }
            b.kind = Binding::Kind::Text;
            b.text = text->text;
            break;
        }
        case PropertyType::Number: {
            const auto* number = std::get_if<database::NumberValue>(&operand);
            if (number == nullptr) { return Status::DatabaseValueTypeMismatch; }
            b.kind = Binding::Kind::Double;
            b.num  = number->number;
            break;
        }
        case PropertyType::Checkbox: {
            const auto* checkbox = std::get_if<database::CheckboxValue>(&operand);
            if (checkbox == nullptr) { return Status::DatabaseValueTypeMismatch; }
            b.kind    = Binding::Kind::Int;
            b.integer = checkbox->checked ? 1 : 0;
            break;
        }
        case PropertyType::Date: {
            const auto* date = std::get_if<database::DateValue>(&operand);
            if (date == nullptr) { return Status::DatabaseValueTypeMismatch; }
            b.kind    = Binding::Kind::Int;
            b.integer = date->unix_ms;
            break;
        }
        case PropertyType::Select:
        case PropertyType::MultiSelect: {
            // Both take a SelectValue operand: a Select compares its one
            // option, and a Multi-select tests membership of one option (see
            // database::Filter::operand for why the Multi-select operand is
            // deliberately NOT a set).
            const auto* select = std::get_if<database::SelectValue>(&operand);
            if (select == nullptr) { return Status::DatabaseValueTypeMismatch; }
            b.kind = Binding::Kind::Blob;
            b.blob = select->option.bytes();
            break;
        }
        case PropertyType::Person:
        case PropertyType::Url:
        case PropertyType::Email:
        case PropertyType::File:
        case PropertyType::Relation:
        case PropertyType::Rollup:
        case PropertyType::Formula:
            return Status::DatabasePropertyTypeUnsupported;
    }

    out = std::move(b);
    return Status::Ok;
}

/// The SQL comparison token for a comparison operator, or nullptr.
const char* sql_comparison(FilterOperator op) noexcept {
    switch (op) {
        case FilterOperator::Equals:         return "=";
        case FilterOperator::LessThan:       return "<";
        case FilterOperator::LessOrEqual:    return "<=";
        case FilterOperator::GreaterThan:    return ">";
        case FilterOperator::GreaterOrEqual: return ">=";
        case FilterOperator::NotEquals:
        case FilterOperator::Contains:
        case FilterOperator::NotContains:
        case FilterOperator::IsEmpty:
        case FilterOperator::IsNotEmpty:
            return nullptr;
    }
    return nullptr;
}

/// The row-id-producing subquery for the POSITIVE form of an operator.
/// Negative operators wrap the same text in `NOT IN`, which is exactly what
/// makes them agree with database::filter_matches's definition of them as the
/// negation of the positive form (including over rows with no value).
Status positive_rowid_subquery(const PropertyDefinition& property,
                               FilterOperator op,
                               int property_param,
                               int operand_param,
                               std::string& out) {
    const std::string p = std::to_string(property_param);
    const std::string v = std::to_string(operand_param);

    if (property.type == PropertyType::MultiSelect) {
        if (op != FilterOperator::Contains) {
            return Status::DatabaseFilterOperatorUnsupported;
        }
        out = "SELECT vo.row_id FROM db_value_option vo "
              "WHERE vo.property_id = ?" + p + " AND vo.option_id = ?" + v;
        return Status::Ok;
    }

    const char* column = value_column_for(property.type);
    if (column == nullptr) { return Status::DatabasePropertyTypeUnsupported; }

    if (op == FilterOperator::Contains) {
        if (property.type != PropertyType::Text) {
            return Status::DatabaseFilterOperatorUnsupported;
        }
        // instr(), not LIKE. SQLite's LIKE is CASE-INSENSITIVE for ASCII by
        // default, so compiling `contains` to LIKE would make the SQL answer
        // disagree with database::filter_matches, which is a byte-exact
        // std::string::find. instr() is case-sensitive and needs no escaping
        // of %, _ or the user's backslashes.
        out = std::string("SELECT v.row_id FROM db_value v WHERE v.property_id = ?") + p +
              " AND v.v_text IS NOT NULL AND instr(v.v_text, ?" + v + ") > 0";
        return Status::Ok;
    }

    const char* comparison = sql_comparison(op);
    if (comparison == nullptr) { return Status::DatabaseFilterOperatorUnsupported; }

    out = std::string("SELECT v.row_id FROM db_value v WHERE v.property_id = ?") + p +
          " AND v." + column + " IS NOT NULL AND v." + column + " " + comparison +
          " ?" + v;
    return Status::Ok;
}

/// The presence subquery: does this row have ANY value for this property?
std::string presence_subquery(const PropertyDefinition& property, int property_param) {
    const std::string p = std::to_string(property_param);
    if (property.type == PropertyType::MultiSelect) {
        return "SELECT 1 FROM db_value_option vo "
               "WHERE vo.row_id = r.id AND vo.property_id = ?" + p;
    }
    return "SELECT 1 FROM db_value v WHERE v.row_id = r.id AND v.property_id = ?" + p;
}

/// The correlated scalar subquery that yields one row's sort key for one
/// property, or SQL NULL when the row leaves it unset.
std::string sort_key_subquery(const PropertyDefinition& property, int property_param) {
    const std::string p = std::to_string(property_param);

    if (property.type == PropertyType::MultiSelect) {
        // MIN over the schema's option order, matching
        // database::sort_key_of's "earliest selected option in schema
        // order". MIN of an empty set is NULL, which is the unset case.
        return "(SELECT MIN(o.sort_index) FROM db_value_option vo "
               "JOIN db_select_option o ON o.id = vo.option_id "
               "WHERE vo.row_id = r.id AND vo.property_id = ?" + p + ")";
    }
    if (property.type == PropertyType::Select) {
        // By the OPTION's position in the schema, not by its id bytes and not
        // by its name. See database::sort_key_of.
        return "(SELECT o.sort_index FROM db_value v "
               "JOIN db_select_option o ON o.id = v.v_option "
               "WHERE v.row_id = r.id AND v.property_id = ?" + p + ")";
    }

    const char* column = value_column_for(property.type);
    if (column == nullptr) { return std::string("NULL"); }
    return std::string("(SELECT v.") + column +
           " FROM db_value v WHERE v.row_id = r.id AND v.property_id = ?" + p + ")";
}

/// Compile a ViewConfig into one row-id query plus its bindings.
///
/// The caller must already have validated the config against the schema
/// (database::validate_view_config), so every lookup here succeeds and every
/// operator/type pair is legal; a failure below therefore means this compiler
/// and that validator disagree, which is a bug in us.
Status compile_row_query(const Database& db,
                         const ViewConfig& config,
                         std::size_t limit,
                         std::string& sql,
                         std::vector<Binding>& bindings) {
    int next_param = 1;

    std::string text = "SELECT r.id, r.created_at_ms, r.modified_at_ms FROM db_row r"
                       " WHERE r.database_id = ?1";
    bindings.push_back(blob_binding(next_param, db.id.bytes()));
    ++next_param;

    for (const Filter& filter : config.filters) {
        const PropertyDefinition* property = database::find_property(db, filter.property_id);
        if (property == nullptr) { return Status::DatabasePropertyNotInSchema; }

        const int property_param = next_param++;
        bindings.push_back(blob_binding(property_param, property->id.bytes()));

        if (filter.op == FilterOperator::IsEmpty) {
            text += " AND NOT EXISTS (" + presence_subquery(*property, property_param) + ")";
            continue;
        }
        if (filter.op == FilterOperator::IsNotEmpty) {
            text += " AND EXISTS (" + presence_subquery(*property, property_param) + ")";
            continue;
        }

        const int operand_param = next_param++;
        Binding operand;
        if (const Status s = operand_binding(*property, filter, operand_param, operand);
            !is_ok(s)) {
            return s;
        }
        bindings.push_back(std::move(operand));

        const bool negated = filter.op == FilterOperator::NotEquals ||
                             filter.op == FilterOperator::NotContains;
        const FilterOperator positive =
            filter.op == FilterOperator::NotEquals     ? FilterOperator::Equals
            : filter.op == FilterOperator::NotContains ? FilterOperator::Contains
                                                       : filter.op;

        std::string subquery;
        if (const Status s = positive_rowid_subquery(*property, positive, property_param,
                                                     operand_param, subquery);
            !is_ok(s)) {
            return s;
        }
        // `NOT IN` is safe: db_value.row_id / db_value_option.row_id are both
        // NOT NULL, so the subquery cannot produce the NULL that makes
        // `NOT IN` collapse to UNKNOWN and silently match nothing.
        text += negated ? " AND r.id NOT IN (" : " AND r.id IN (";
        text += subquery;
        text += ")";
    }

    text += " ORDER BY ";
    for (const Sort& sort : config.sorts) {
        const PropertyDefinition* property = database::find_property(db, sort.property_id);
        if (property == nullptr) { return Status::DatabasePropertyNotInSchema; }

        const int property_param = next_param++;
        // The key subquery appears TWICE in the ORDER BY -- once to push the
        // unset rows last, once to order the rest -- so its parameter is
        // bound once and referenced twice by number. Numbered parameters are
        // what make that possible; with `?` it would need two bindings that
        // could drift apart.
        bindings.push_back(blob_binding(property_param, property->id.bytes()));

        const std::string key = sort_key_subquery(*property, property_param);
        // Absent LAST in both directions. `(key IS NULL)` is 0 for present
        // and 1 for absent, and it is always ASC, so flipping the key's
        // direction does not drag the empty rows to the top -- which is what
        // a plain `ORDER BY key DESC` would do and which is never what the
        // user meant.
        text += "(" + key + " IS NULL) ASC, ";
        text += key;
        text += sort.descending ? " DESC, " : " ASC, ";
    }
    // FINAL TIE-BREAK: row id. Identical to database::evaluate_table_view's,
    // which is what makes the two orderings equal rather than merely
    // similar. It is also creation order, because ids are UUIDv7.
    text += "r.id ASC";

    const int limit_param = next_param++;
    text += " LIMIT ?" + std::to_string(limit_param);
    Binding limit_binding;
    limit_binding.index   = limit_param;
    limit_binding.kind    = Binding::Kind::Int;
    limit_binding.integer = static_cast<std::int64_t>(
        limit > database::kMaxRowsPerDatabase ? database::kMaxRowsPerDatabase : limit);
    bindings.push_back(std::move(limit_binding));

    sql = std::move(text);
    return Status::Ok;
}

// ---------------------------------------------------------------------------

class SqliteDatabaseStore final : public database::DatabaseStore {
public:
    SqliteDatabaseStore(Connection conn, bool read_only) noexcept
        : conn_(std::move(conn)), read_only_(read_only) {}

    [[nodiscard]] Status create_database(const Database& db) noexcept override;
    [[nodiscard]] Status get_database(const DatabaseId& id,
                                      Database& out) const noexcept override;
    [[nodiscard]] Status set_database_name(const DatabaseId& id,
                                           std::string_view name,
                                           std::int64_t now_ms) noexcept override;
    [[nodiscard]] Status delete_database(const DatabaseId& id) noexcept override;
    [[nodiscard]] Status list_databases_on_page(
        const blocks::PageId& page_id,
        std::vector<DatabaseId>& out) const noexcept override;
    [[nodiscard]] Status database_count(std::size_t& out) const noexcept override;

    [[nodiscard]] Status insert_row(const Row& row,
                                    std::int64_t now_ms) noexcept override;
    [[nodiscard]] Status get_row(const RowId& id, Row& out) const noexcept override;
    [[nodiscard]] Status update_row(const Row& row,
                                    std::int64_t now_ms) noexcept override;
    [[nodiscard]] Status delete_row(const RowId& id,
                                    std::int64_t now_ms) noexcept override;
    [[nodiscard]] Status row_count(const DatabaseId& id,
                                   std::size_t& out) const noexcept override;

    [[nodiscard]] Status save_view(const View& view,
                                   std::int64_t now_ms) noexcept override;
    [[nodiscard]] Status get_view(const ViewId& id, View& out) const noexcept override;
    [[nodiscard]] Status list_views(const DatabaseId& id,
                                    std::vector<View>& out) const noexcept override;
    [[nodiscard]] Status delete_view(const ViewId& id) noexcept override;

    [[nodiscard]] Status query_table_view(const DatabaseId& id,
                                          const ViewConfig& config,
                                          std::size_t limit,
                                          std::vector<Row>& out) const noexcept override;
    [[nodiscard]] Status query_saved_view(const ViewId& id,
                                          std::size_t limit,
                                          std::vector<Row>& out) const noexcept override;

private:
    [[nodiscard]] Status require_page(const blocks::PageId& id) const noexcept;
    [[nodiscard]] Status database_id_of_row(const RowId& id, DatabaseId& out) const noexcept;
    [[nodiscard]] Status database_id_of_view(const ViewId& id,
                                             DatabaseId& out) const noexcept;
    /// Write one row's values. MUST be called inside the caller's transaction,
    /// with `db_value` / `db_value_option` already cleared for this row.
    [[nodiscard]] Status write_row_values(const Database& db, const Row& row) noexcept;
    /// Materialise the values of every row in `rows`, in two statements over
    /// the whole set rather than two per row.
    [[nodiscard]] Status load_values_for(const Database& db,
                                         const std::string& row_id_sql,
                                         const std::vector<Binding>& bindings,
                                         std::vector<Row>& rows) const noexcept;
    [[nodiscard]] Status touch_database(const DatabaseId& id,
                                        std::int64_t now_ms) noexcept;

    Connection conn_;
    bool       read_only_ = false;
};

// --- private helpers -------------------------------------------------------

Status SqliteDatabaseStore::require_page(const blocks::PageId& id) const noexcept {
    Statement stmt;
    if (const Status s = stmt.prepare(conn_.get(), "SELECT 1 FROM page WHERE id = ?1;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW)  { return Status::Ok; }
    if (rc == SQLITE_DONE) { return Status::NotFound; }
    return Status::StoreReadFailed;
}

Status SqliteDatabaseStore::database_id_of_row(const RowId& id,
                                               DatabaseId& out) const noexcept {
    Statement stmt;
    if (const Status s =
            stmt.prepare(conn_.get(), "SELECT database_id FROM db_row WHERE id = ?1;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) { return Status::NotFound; }
    if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

    ObjectIdBytes raw{};
    if (const Status s = stmt.column_fixed_blob(0, raw.data(), raw.size()); !is_ok(s)) {
        return s;
    }
    out = DatabaseId(raw);
    return Status::Ok;
}

Status SqliteDatabaseStore::database_id_of_view(const ViewId& id,
                                                DatabaseId& out) const noexcept {
    Statement stmt;
    if (const Status s =
            stmt.prepare(conn_.get(), "SELECT database_id FROM db_view WHERE id = ?1;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) { return Status::NotFound; }
    if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

    ObjectIdBytes raw{};
    if (const Status s = stmt.column_fixed_blob(0, raw.data(), raw.size()); !is_ok(s)) {
        return s;
    }
    out = DatabaseId(raw);
    return Status::Ok;
}

Status SqliteDatabaseStore::touch_database(const DatabaseId& id,
                                           std::int64_t now_ms) noexcept {
    if (now_ms < 0) { return Status::InvalidArgument; }
    Statement stmt;
    if (const Status s = stmt.prepare(
            conn_.get(), "UPDATE db_collection SET modified_at_ms = ?2 WHERE id = ?1;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }
    if (const Status s = stmt.bind_int64(2, now_ms);   !is_ok(s)) { return s; }
    return run_to_completion(stmt);
}

Status SqliteDatabaseStore::write_row_values(const Database& db, const Row& row) noexcept {
    for (const auto& entry : row.values) {
        const PropertyDefinition* property = database::find_property(db, entry.first);
        if (property == nullptr) { return Status::DatabasePropertyNotInSchema; }

        ValueColumns columns;
        if (const Status s = encode_value(entry.second, columns); !is_ok(s)) { return s; }

        if (columns.is_multi) {
            // One membership row per selected option. Deviation 2 at the top
            // of this file explains why this is a table and not a blob.
            for (std::size_t i = 0; i < columns.options.size(); ++i) {
                Statement stmt;
                if (const Status s = stmt.prepare(
                        conn_.get(),
                        "INSERT INTO db_value_option(row_id, property_id, option_id, "
                        "                            sort_index) "
                        "VALUES(?1, ?2, ?3, ?4);");
                    !is_ok(s)) {
                    return s;
                }
                if (const Status s = bind_id(stmt, 1, row.id.bytes());     !is_ok(s)) { return s; }
                if (const Status s = bind_id(stmt, 2, property->id.bytes()); !is_ok(s)) { return s; }
                if (const Status s = bind_id(stmt, 3, columns.options[i].bytes());
                    !is_ok(s)) {
                    return s;
                }
                if (const Status s = bind_size(stmt, 4, i); !is_ok(s)) { return s; }
                if (const Status s = run_to_completion(stmt); !is_ok(s)) { return s; }
            }
            continue;
        }

        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "INSERT INTO db_value(row_id, property_id, v_text, v_num, v_int, v_option) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6);");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, row.id.bytes());       !is_ok(s)) { return s; }
        if (const Status s = bind_id(stmt, 2, property->id.bytes()); !is_ok(s)) { return s; }
        if (const Status s = bind_value_columns(stmt, 3, columns);   !is_ok(s)) { return s; }
        if (const Status s = run_to_completion(stmt);                !is_ok(s)) { return s; }
    }
    return Status::Ok;
}

Status SqliteDatabaseStore::load_values_for(const Database& db,
                                            const std::string& row_id_sql,
                                            const std::vector<Binding>& bindings,
                                            std::vector<Row>& rows) const noexcept {
    if (rows.empty()) { return Status::Ok; }

    // Index the rows by id so the two value statements below can be ONE
    // statement each over the whole result set rather than two per row. The
    // row-id query is re-used verbatim as a subquery, with the SAME bindings,
    // so the set it selects provably matches the rows already loaded.
    std::map<ObjectIdBytes, std::size_t> position;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        position[rows[i].id.bytes()] = i;
    }

    {
        const std::string sql =
            "SELECT v.row_id, v.property_id, v.v_text, v.v_num, v.v_int, v.v_option "
            "FROM db_value v WHERE v.row_id IN (SELECT q.id FROM (" + row_id_sql +
            ") AS q);";
        Statement stmt;
        if (const Status s = stmt.prepare(conn_.get(), sql); !is_ok(s)) { return s; }
        if (const Status s = apply_bindings(stmt, bindings); !is_ok(s)) { return s; }

        for (;;) {
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) { break; }
            if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

            ObjectIdBytes row_raw{};
            if (const Status s = stmt.column_fixed_blob(0, row_raw.data(), row_raw.size());
                !is_ok(s)) {
                return s;
            }
            const auto at = position.find(row_raw);
            if (at == position.end()) { continue; }

            ObjectIdBytes property_raw{};
            if (const Status s =
                    stmt.column_fixed_blob(1, property_raw.data(), property_raw.size());
                !is_ok(s)) {
                return s;
            }
            const PropertyId property_id(property_raw);
            const PropertyDefinition* property = database::find_property(db, property_id);
            if (property == nullptr) {
                // A value naming a column the schema does not define. The
                // foreign key makes this unreachable through this adapter, so
                // it means the file was edited by something else.
                return Status::StoreReadFailed;
            }

            PropertyValue value;
            if (const Status s = decode_scalar_value(*property, stmt, 2, 3, 4, 5, value);
                !is_ok(s)) {
                return s;
            }
            rows[at->second].values.emplace(property_id, std::move(value));
        }
    }

    {
        const std::string sql =
            "SELECT vo.row_id, vo.property_id, vo.option_id FROM db_value_option vo "
            "WHERE vo.row_id IN (SELECT q.id FROM (" + row_id_sql + ") AS q) "
            "ORDER BY vo.row_id, vo.property_id, vo.sort_index;";
        Statement stmt;
        if (const Status s = stmt.prepare(conn_.get(), sql); !is_ok(s)) { return s; }
        if (const Status s = apply_bindings(stmt, bindings); !is_ok(s)) { return s; }

        for (;;) {
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) { break; }
            if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

            ObjectIdBytes row_raw{};
            if (const Status s = stmt.column_fixed_blob(0, row_raw.data(), row_raw.size());
                !is_ok(s)) {
                return s;
            }
            const auto at = position.find(row_raw);
            if (at == position.end()) { continue; }

            ObjectIdBytes property_raw{};
            if (const Status s =
                    stmt.column_fixed_blob(1, property_raw.data(), property_raw.size());
                !is_ok(s)) {
                return s;
            }
            ObjectIdBytes option_raw{};
            if (const Status s =
                    stmt.column_fixed_blob(2, option_raw.data(), option_raw.size());
                !is_ok(s)) {
                return s;
            }

            const PropertyId property_id(property_raw);
            const PropertyDefinition* property = database::find_property(db, property_id);
            if (property == nullptr || property->type != PropertyType::MultiSelect) {
                return Status::StoreReadFailed;
            }

            Row& target = rows[at->second];
            const auto existing = target.values.find(property_id);
            if (existing == target.values.end()) {
                database::MultiSelectValue fresh;
                fresh.options.push_back(OptionId(option_raw));
                target.values.emplace(property_id, std::move(fresh));
            } else {
                auto* multi = std::get_if<database::MultiSelectValue>(&existing->second);
                if (multi == nullptr) { return Status::StoreReadFailed; }
                multi->options.push_back(OptionId(option_raw));
            }
        }
    }

    // Spec section 28: a database file is an untrusted input even though it
    // used to be ours. A row that violates the schema is reported, never
    // handed back half-decoded.
    for (const Row& row : rows) {
        if (const Status s = database::validate_row(db, row); !is_ok(s)) {
            return Status::StoreReadFailed;
        }
    }

    return Status::Ok;
}

// --- databases -------------------------------------------------------------

Status SqliteDatabaseStore::create_database(const Database& db) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    // Re-validate at the adapter boundary: the caller validates too, but this
    // interface is reachable by anyone holding the pointer, and the schema
    // CHECKs are the last line rather than the only one.
    if (const Status s = database::validate_database_schema(db); !is_ok(s)) { return s; }

    Transaction txn;
    if (const Status s = txn.begin(conn_.get()); !is_ok(s)) { return s; }

    // The host page must exist. This is the malformed-input path a caller
    // hits by holding on to the id of a page someone else deleted.
    if (const Status s = require_page(db.page_id); !is_ok(s)) { return s; }

    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "INSERT INTO db_collection(id, page_id, name, created_at_ms, "
                "                          modified_at_ms) "
                "VALUES(?1, ?2, ?3, ?4, ?5);");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, db.id.bytes());      !is_ok(s)) { return s; }
        if (const Status s = bind_id(stmt, 2, db.page_id.bytes()); !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_text(3, db.name);           !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(4, db.created_at_ms); !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(5, db.modified_at_ms); !is_ok(s)) { return s; }
        if (const Status s = run_to_completion(stmt);              !is_ok(s)) { return s; }
    }

    for (std::size_t i = 0; i < db.properties.size(); ++i) {
        const PropertyDefinition& property = db.properties[i];
        {
            Statement stmt;
            if (const Status s = stmt.prepare(
                    conn_.get(),
                    "INSERT INTO db_property(id, database_id, name, type, sort_index) "
                    "VALUES(?1, ?2, ?3, ?4, ?5);");
                !is_ok(s)) {
                return s;
            }
            if (const Status s = bind_id(stmt, 1, property.id.bytes()); !is_ok(s)) { return s; }
            if (const Status s = bind_id(stmt, 2, db.id.bytes());       !is_ok(s)) { return s; }
            if (const Status s = stmt.bind_text(3, property.name);      !is_ok(s)) { return s; }
            if (const Status s = stmt.bind_int64(
                    4, static_cast<std::int64_t>(property.type)); !is_ok(s)) { return s; }
            if (const Status s = bind_size(stmt, 5, i);           !is_ok(s)) { return s; }
            if (const Status s = run_to_completion(stmt);         !is_ok(s)) { return s; }
        }

        for (std::size_t j = 0; j < property.options.size(); ++j) {
            const SelectOption& option = property.options[j];
            Statement stmt;
            if (const Status s = stmt.prepare(
                    conn_.get(),
                    "INSERT INTO db_select_option(id, property_id, name, sort_index) "
                    "VALUES(?1, ?2, ?3, ?4);");
                !is_ok(s)) {
                return s;
            }
            if (const Status s = bind_id(stmt, 1, option.id.bytes());   !is_ok(s)) { return s; }
            if (const Status s = bind_id(stmt, 2, property.id.bytes()); !is_ok(s)) { return s; }
            if (const Status s = stmt.bind_text(3, option.name);        !is_ok(s)) { return s; }
            if (const Status s = bind_size(stmt, 4, j);                 !is_ok(s)) { return s; }
            if (const Status s = run_to_completion(stmt);               !is_ok(s)) { return s; }
        }
    }

    return txn.commit();
}

Status SqliteDatabaseStore::get_database(const DatabaseId& id, Database& out) const noexcept {
    if (id.is_nil()) { return Status::IdMalformed; }

    Database db;
    db.id = id;
    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "SELECT page_id, name, created_at_ms, modified_at_ms "
                "FROM db_collection WHERE id = ?1;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }

        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) { return Status::NotFound; }
        if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

        ObjectIdBytes page_raw{};
        if (const Status s = stmt.column_fixed_blob(0, page_raw.data(), page_raw.size());
            !is_ok(s)) {
            return s;
        }
        db.page_id        = blocks::PageId(page_raw);
        db.name           = stmt.column_text(1);
        db.created_at_ms  = stmt.column_int64(2);
        db.modified_at_ms = stmt.column_int64(3);
    }

    // Properties in COLUMN ORDER, then options in OPTION ORDER. Both orders
    // are part of the schema (the Select sort key is the option's position),
    // so they are read back by sort_index rather than by whatever order the
    // storage engine scans.
    std::map<ObjectIdBytes, std::size_t> property_at;
    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "SELECT id, name, type FROM db_property "
                "WHERE database_id = ?1 ORDER BY sort_index;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }

        for (;;) {
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) { break; }
            if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

            if (db.properties.size() >= database::kMaxPropertiesPerDatabase) {
                return Status::DatabasePropertyLimitExceeded;
            }

            PropertyDefinition property;
            ObjectIdBytes raw{};
            if (const Status s = stmt.column_fixed_blob(0, raw.data(), raw.size());
                !is_ok(s)) {
                return s;
            }
            property.id   = PropertyId(raw);
            property.name = stmt.column_text(1);

            const std::int64_t raw_type = stmt.column_int64(2);
            if (raw_type < 0 || raw_type >= database::kPropertyTypeCount) {
                return Status::StoreReadFailed;
            }
            property.type =
                static_cast<PropertyType>(static_cast<std::int32_t>(raw_type));
            if (!database::property_type_is_implemented(property.type)) {
                // A column written by a newer build that supports more types.
                // Refusing is the honest answer; presenting it as Text would
                // destroy the user's values on the next save.
                return Status::DatabasePropertyTypeUnsupported;
            }

            property_at[raw] = db.properties.size();
            db.properties.push_back(std::move(property));
        }
    }

    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "SELECT o.property_id, o.id, o.name FROM db_select_option o "
                "JOIN db_property p ON p.id = o.property_id "
                "WHERE p.database_id = ?1 ORDER BY p.sort_index, o.sort_index;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }

        for (;;) {
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) { break; }
            if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

            ObjectIdBytes property_raw{};
            if (const Status s =
                    stmt.column_fixed_blob(0, property_raw.data(), property_raw.size());
                !is_ok(s)) {
                return s;
            }
            const auto at = property_at.find(property_raw);
            if (at == property_at.end()) { return Status::StoreReadFailed; }

            SelectOption option;
            ObjectIdBytes option_raw{};
            if (const Status s =
                    stmt.column_fixed_blob(1, option_raw.data(), option_raw.size());
                !is_ok(s)) {
                return s;
            }
            option.id   = OptionId(option_raw);
            option.name = stmt.column_text(2);

            PropertyDefinition& property = db.properties[at->second];
            if (property.options.size() >= database::kMaxSelectOptionsPerProperty) {
                return Status::DatabaseOptionLimitExceeded;
            }
            property.options.push_back(std::move(option));
        }
    }

    if (const Status s = database::validate_database_schema(db); !is_ok(s)) {
        // The file disagrees with the domain's own rules. Report the storage
        // failure rather than the domain code, because the caller's request
        // was fine -- it is the file that is wrong.
        return Status::StoreReadFailed;
    }

    out = std::move(db);
    return Status::Ok;
}

Status SqliteDatabaseStore::set_database_name(const DatabaseId& id,
                                              std::string_view name,
                                              std::int64_t now_ms) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    if (id.is_nil()) { return Status::IdMalformed; }
    if (const Status s = database::validate_database_name(name); !is_ok(s)) { return s; }
    if (now_ms < 0) { return Status::InvalidArgument; }

    Transaction txn;
    if (const Status s = txn.begin(conn_.get()); !is_ok(s)) { return s; }

    Statement stmt;
    if (const Status s = stmt.prepare(
            conn_.get(),
            "UPDATE db_collection SET name = ?2, modified_at_ms = ?3 WHERE id = ?1;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }
    if (const Status s = stmt.bind_text(2, name);      !is_ok(s)) { return s; }
    if (const Status s = stmt.bind_int64(3, now_ms);   !is_ok(s)) { return s; }
    if (const Status s = run_to_completion(stmt);      !is_ok(s)) { return s; }

    if (sqlite3_changes(conn_.get()) == 0) { return Status::NotFound; }
    return txn.commit();
}

Status SqliteDatabaseStore::delete_database(const DatabaseId& id) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    if (id.is_nil()) { return Status::IdMalformed; }

    Transaction txn;
    if (const Status s = txn.begin(conn_.get()); !is_ok(s)) { return s; }

    {
        Statement stmt;
        if (const Status s =
                stmt.prepare(conn_.get(), "SELECT 1 FROM db_collection WHERE id = ?1;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) { return Status::NotFound; }
        if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }
    }

    // Deleted explicitly, child tables first, rather than left to ON DELETE
    // CASCADE. The cascades are declared and enabled, but relying on them
    // would make correctness depend on a per-connection PRAGMA that a future
    // caller could forget to set -- exactly the argument
    // sqlite_block_store.cpp makes for deleting a page's blocks by hand.
    static constexpr const char* kCascade[] = {
        "DELETE FROM db_value WHERE row_id IN (SELECT id FROM db_row WHERE database_id = ?1);",
        "DELETE FROM db_value_option WHERE row_id IN "
        "  (SELECT id FROM db_row WHERE database_id = ?1);",
        "DELETE FROM db_row WHERE database_id = ?1;",
        "DELETE FROM db_view_filter WHERE view_id IN "
        "  (SELECT id FROM db_view WHERE database_id = ?1);",
        "DELETE FROM db_view_sort WHERE view_id IN "
        "  (SELECT id FROM db_view WHERE database_id = ?1);",
        "DELETE FROM db_view WHERE database_id = ?1;",
        "DELETE FROM db_select_option WHERE property_id IN "
        "  (SELECT id FROM db_property WHERE database_id = ?1);",
        "DELETE FROM db_property WHERE database_id = ?1;",
        "DELETE FROM db_collection WHERE id = ?1;",
    };
    for (const char* sql : kCascade) {
        if (const Status s = delete_by_id(conn_.get(), sql, id.bytes()); !is_ok(s)) {
            return s;
        }
    }

    return txn.commit();
}

Status SqliteDatabaseStore::list_databases_on_page(
    const blocks::PageId& page_id, std::vector<DatabaseId>& out) const noexcept {
    if (page_id.is_nil()) { return Status::IdMalformed; }

    Statement stmt;
    if (const Status s = stmt.prepare(
            conn_.get(),
            "SELECT id FROM db_collection WHERE page_id = ?1 ORDER BY id;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, page_id.bytes()); !is_ok(s)) { return s; }

    std::vector<DatabaseId> ids;
    for (;;) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) { break; }
        if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

        ObjectIdBytes raw{};
        if (const Status s = stmt.column_fixed_blob(0, raw.data(), raw.size());
            !is_ok(s)) {
            return s;
        }
        ids.push_back(DatabaseId(raw));
    }

    out = std::move(ids);
    return Status::Ok;
}

Status SqliteDatabaseStore::database_count(std::size_t& out) const noexcept {
    Statement stmt;
    if (const Status s = stmt.prepare(conn_.get(), "SELECT COUNT(*) FROM db_collection;");
        !is_ok(s)) {
        return s;
    }
    return read_count(stmt, out);
}

// --- rows ------------------------------------------------------------------

Status SqliteDatabaseStore::insert_row(const Row& row, std::int64_t now_ms) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    if (row.id.is_nil() || row.database_id.is_nil()) { return Status::IdMalformed; }
    if (now_ms < 0) { return Status::InvalidArgument; }

    Transaction txn;
    if (const Status s = txn.begin(conn_.get()); !is_ok(s)) { return s; }

    Database db;
    if (const Status s = get_database(row.database_id, db); !is_ok(s)) { return s; }

    // THE VALIDATION BOUNDARY. A value of the wrong type, a value violating
    // its own type's rules, or an option this property does not define is
    // rejected here -- before a single column is written, so nothing is
    // half-stored and nothing is coerced.
    if (const Status s = database::validate_row(db, row); !is_ok(s)) { return s; }

    std::size_t existing = 0;
    if (const Status s = row_count(row.database_id, existing); !is_ok(s)) { return s; }
    if (existing >= database::kMaxRowsPerDatabase) {
        return Status::DatabaseRowLimitExceeded;
    }

    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "INSERT INTO db_row(id, database_id, created_at_ms, modified_at_ms) "
                "VALUES(?1, ?2, ?3, ?3);");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, row.id.bytes());          !is_ok(s)) { return s; }
        if (const Status s = bind_id(stmt, 2, row.database_id.bytes()); !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(3, now_ms);                !is_ok(s)) { return s; }
        if (const Status s = run_to_completion(stmt);                   !is_ok(s)) { return s; }
    }

    if (const Status s = write_row_values(db, row);                 !is_ok(s)) { return s; }
    if (const Status s = touch_database(row.database_id, now_ms);    !is_ok(s)) { return s; }
    return txn.commit();
}

Status SqliteDatabaseStore::get_row(const RowId& id, Row& out) const noexcept {
    if (id.is_nil()) { return Status::IdMalformed; }

    DatabaseId database_id;
    if (const Status s = database_id_of_row(id, database_id); !is_ok(s)) { return s; }

    Database db;
    if (const Status s = get_database(database_id, db); !is_ok(s)) { return s; }

    std::vector<Row> rows;
    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "SELECT created_at_ms, modified_at_ms FROM db_row WHERE id = ?1;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }

        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) { return Status::NotFound; }
        if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

        Row row;
        row.id             = id;
        row.database_id    = database_id;
        row.created_at_ms  = stmt.column_int64(0);
        row.modified_at_ms = stmt.column_int64(1);
        rows.push_back(std::move(row));
    }

    // Reuse the bulk value loader with a one-row "query", so a single row and
    // a whole view are decoded by exactly the same code -- the alternative is
    // two decoders that drift.
    std::vector<Binding> bindings;
    bindings.push_back(blob_binding(1, id.bytes()));
    if (const Status s = load_values_for(db, "SELECT id FROM db_row WHERE id = ?1",
                                         bindings, rows);
        !is_ok(s)) {
        return s;
    }

    out = std::move(rows.front());
    return Status::Ok;
}

Status SqliteDatabaseStore::update_row(const Row& row, std::int64_t now_ms) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    if (row.id.is_nil() || row.database_id.is_nil()) { return Status::IdMalformed; }
    if (now_ms < 0) { return Status::InvalidArgument; }

    Transaction txn;
    if (const Status s = txn.begin(conn_.get()); !is_ok(s)) { return s; }

    DatabaseId stored_database;
    if (const Status s = database_id_of_row(row.id, stored_database); !is_ok(s)) {
        return s;
    }
    if (stored_database != row.database_id) {
        // Moving a row between databases is NOT a rename: the target schema
        // is different, so every value would need re-validating and the ones
        // that do not fit would have to be dropped. Rejected rather than
        // silently partially applied.
        return Status::InvalidArgument;
    }

    Database db;
    if (const Status s = get_database(row.database_id, db); !is_ok(s)) { return s; }
    if (const Status s = database::validate_row(db, row);   !is_ok(s)) { return s; }

    // A PUT, not a PATCH (see DatabaseStore::update_row): the old values go
    // first, so a property absent from `row.values` ends up UNSET rather than
    // silently retaining what was there.
    if (const Status s = delete_by_id(conn_.get(), "DELETE FROM db_value WHERE row_id = ?1;",
                                      row.id.bytes());
        !is_ok(s)) {
        return s;
    }
    if (const Status s = delete_by_id(
            conn_.get(), "DELETE FROM db_value_option WHERE row_id = ?1;", row.id.bytes());
        !is_ok(s)) {
        return s;
    }
    if (const Status s = write_row_values(db, row); !is_ok(s)) { return s; }

    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(), "UPDATE db_row SET modified_at_ms = ?2 WHERE id = ?1;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, row.id.bytes()); !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(2, now_ms);       !is_ok(s)) { return s; }
        if (const Status s = run_to_completion(stmt);          !is_ok(s)) { return s; }
    }

    if (const Status s = touch_database(row.database_id, now_ms); !is_ok(s)) { return s; }
    return txn.commit();
}

Status SqliteDatabaseStore::delete_row(const RowId& id, std::int64_t now_ms) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    if (id.is_nil()) { return Status::IdMalformed; }
    if (now_ms < 0)  { return Status::InvalidArgument; }

    Transaction txn;
    if (const Status s = txn.begin(conn_.get()); !is_ok(s)) { return s; }

    DatabaseId database_id;
    if (const Status s = database_id_of_row(id, database_id); !is_ok(s)) { return s; }

    if (const Status s = delete_by_id(conn_.get(), "DELETE FROM db_value WHERE row_id = ?1;",
                                      id.bytes());
        !is_ok(s)) {
        return s;
    }
    if (const Status s = delete_by_id(
            conn_.get(), "DELETE FROM db_value_option WHERE row_id = ?1;", id.bytes());
        !is_ok(s)) {
        return s;
    }
    if (const Status s =
            delete_by_id(conn_.get(), "DELETE FROM db_row WHERE id = ?1;", id.bytes());
        !is_ok(s)) {
        return s;
    }

    if (const Status s = touch_database(database_id, now_ms); !is_ok(s)) { return s; }
    return txn.commit();
}

Status SqliteDatabaseStore::row_count(const DatabaseId& id, std::size_t& out) const noexcept {
    if (id.is_nil()) { return Status::IdMalformed; }
    Statement stmt;
    if (const Status s = stmt.prepare(
            conn_.get(), "SELECT COUNT(*) FROM db_row WHERE database_id = ?1;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }
    return read_count(stmt, out);
}

// --- saved views -----------------------------------------------------------

Status SqliteDatabaseStore::save_view(const View& view, std::int64_t now_ms) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    if (view.id.is_nil() || view.database_id.is_nil()) { return Status::IdMalformed; }
    if (now_ms < 0) { return Status::InvalidArgument; }

    Transaction txn;
    if (const Status s = txn.begin(conn_.get()); !is_ok(s)) { return s; }

    Database db;
    if (const Status s = get_database(view.database_id, db); !is_ok(s)) { return s; }
    // Rejects a non-Table kind (Status::DatabaseViewKindUnsupported), a filter
    // on a property the schema does not define, an operator the property's
    // type does not support, and a malformed or wrongly-typed operand.
    if (const Status s = database::validate_view(db, view); !is_ok(s)) { return s; }

    bool existed = false;
    {
        Statement stmt;
        if (const Status s =
                stmt.prepare(conn_.get(), "SELECT 1 FROM db_view WHERE id = ?1;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, view.id.bytes()); !is_ok(s)) { return s; }
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_ROW)       { existed = true; }
        else if (rc != SQLITE_DONE) { return Status::StoreReadFailed; }
    }

    if (!existed) {
        std::size_t views = 0;
        {
            Statement stmt;
            if (const Status s = stmt.prepare(
                    conn_.get(), "SELECT COUNT(*) FROM db_view WHERE database_id = ?1;");
                !is_ok(s)) {
                return s;
            }
            if (const Status s = bind_id(stmt, 1, view.database_id.bytes()); !is_ok(s)) {
                return s;
            }
            if (const Status s = read_count(stmt, views); !is_ok(s)) { return s; }
        }
        if (views >= database::kMaxViewsPerDatabase) {
            return Status::DatabaseViewLimitExceeded;
        }
    }

    // Replace the configuration wholesale. A view IS its configuration, so an
    // in-place diff of filter rows would be more code for the same result.
    if (const Status s = delete_by_id(
            conn_.get(), "DELETE FROM db_view_filter WHERE view_id = ?1;", view.id.bytes());
        !is_ok(s)) {
        return s;
    }
    if (const Status s = delete_by_id(
            conn_.get(), "DELETE FROM db_view_sort WHERE view_id = ?1;", view.id.bytes());
        !is_ok(s)) {
        return s;
    }

    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "INSERT INTO db_view(id, database_id, name, kind, created_at_ms, "
                "                    modified_at_ms) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?5) "
                "ON CONFLICT(id) DO UPDATE SET name = excluded.name, "
                "    kind = excluded.kind, modified_at_ms = excluded.modified_at_ms;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, view.id.bytes());          !is_ok(s)) { return s; }
        if (const Status s = bind_id(stmt, 2, view.database_id.bytes()); !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_text(3, view.name);               !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(
                4, static_cast<std::int64_t>(view.kind)); !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(5, now_ms);  !is_ok(s)) { return s; }
        if (const Status s = run_to_completion(stmt);     !is_ok(s)) { return s; }
    }

    for (std::size_t i = 0; i < view.config.filters.size(); ++i) {
        const Filter& filter = view.config.filters[i];

        ValueColumns columns;
        if (filter.operand.has_value()) {
            if (const Status s = encode_value(*filter.operand, columns); !is_ok(s)) {
                return s;
            }
            if (columns.is_multi) {
                // A Multi-select operand is one option (a SelectValue), never
                // a set -- validate_view would already have rejected this, so
                // reaching here means the validator and this encoder disagree.
                return Status::Internal;
            }
        }

        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "INSERT INTO db_view_filter(view_id, sort_index, property_id, op, "
                "                           v_text, v_num, v_int, v_option) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, view.id.bytes());            !is_ok(s)) { return s; }
        if (const Status s = bind_size(stmt, 2, i);                        !is_ok(s)) { return s; }
        if (const Status s = bind_id(stmt, 3, filter.property_id.bytes()); !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(
                4, static_cast<std::int64_t>(filter.op));    !is_ok(s)) { return s; }
        if (const Status s = bind_value_columns(stmt, 5, columns); !is_ok(s)) { return s; }
        if (const Status s = run_to_completion(stmt);              !is_ok(s)) { return s; }
    }

    for (std::size_t i = 0; i < view.config.sorts.size(); ++i) {
        const Sort& sort = view.config.sorts[i];
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "INSERT INTO db_view_sort(view_id, sort_index, property_id, descending) "
                "VALUES(?1, ?2, ?3, ?4);");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, view.id.bytes());          !is_ok(s)) { return s; }
        if (const Status s = bind_size(stmt, 2, i);                      !is_ok(s)) { return s; }
        if (const Status s = bind_id(stmt, 3, sort.property_id.bytes()); !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(4, sort.descending ? 1 : 0); !is_ok(s)) { return s; }
        if (const Status s = run_to_completion(stmt);                     !is_ok(s)) { return s; }
    }

    return txn.commit();
}

Status SqliteDatabaseStore::get_view(const ViewId& id, View& out) const noexcept {
    if (id.is_nil()) { return Status::IdMalformed; }

    DatabaseId database_id;
    if (const Status s = database_id_of_view(id, database_id); !is_ok(s)) { return s; }

    Database db;
    if (const Status s = get_database(database_id, db); !is_ok(s)) { return s; }

    View view;
    view.id          = id;
    view.database_id = database_id;
    {
        Statement stmt;
        if (const Status s = stmt.prepare(conn_.get(),
                                          "SELECT name, kind FROM db_view WHERE id = ?1;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }

        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) { return Status::NotFound; }
        if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

        view.name = stmt.column_text(0);

        const std::int64_t raw_kind = stmt.column_int64(1);
        if (raw_kind < 0 || raw_kind >= database::kViewKindCount) {
            return Status::StoreReadFailed;
        }
        view.kind = static_cast<ViewKind>(static_cast<std::int32_t>(raw_kind));
        if (!database::view_kind_is_implemented(view.kind)) {
            // A view saved by a newer build. Refusing is honest; rendering a
            // Board as a Table would show the user a view they did not save.
            return Status::DatabaseViewKindUnsupported;
        }
    }

    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "SELECT property_id, op, v_text, v_num, v_int, v_option "
                "FROM db_view_filter WHERE view_id = ?1 ORDER BY sort_index;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }

        for (;;) {
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) { break; }
            if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

            if (view.config.filters.size() >= database::kMaxFiltersPerView) {
                return Status::DatabaseViewSpecTooComplex;
            }

            Filter filter;
            ObjectIdBytes raw{};
            if (const Status s = stmt.column_fixed_blob(0, raw.data(), raw.size());
                !is_ok(s)) {
                return s;
            }
            filter.property_id = PropertyId(raw);

            const std::int64_t raw_op = stmt.column_int64(1);
            if (raw_op < 0 || raw_op >= database::kFilterOperatorCount) {
                return Status::StoreReadFailed;
            }
            filter.op = static_cast<FilterOperator>(static_cast<std::int32_t>(raw_op));

            const PropertyDefinition* property =
                database::find_property(db, filter.property_id);
            if (property == nullptr) { return Status::StoreReadFailed; }

            if (database::filter_operator_takes_operand(filter.op)) {
                // A Multi-select `contains` operand was stored as ONE option,
                // so it decodes as a SelectValue even though the property is
                // a MultiSelect. Everywhere else the operand's type is the
                // property's own.
                const bool membership_probe =
                    property->type == PropertyType::MultiSelect &&
                    (filter.op == FilterOperator::Contains ||
                     filter.op == FilterOperator::NotContains);

                PropertyDefinition operand_shape = *property;
                if (membership_probe) { operand_shape.type = PropertyType::Select; }

                PropertyValue operand;
                if (const Status s =
                        decode_scalar_value(operand_shape, stmt, 2, 3, 4, 5, operand);
                    !is_ok(s)) {
                    return s;
                }
                filter.operand = std::move(operand);
            }

            view.config.filters.push_back(std::move(filter));
        }
    }

    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "SELECT property_id, descending FROM db_view_sort "
                "WHERE view_id = ?1 ORDER BY sort_index;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }

        for (;;) {
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) { break; }
            if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

            if (view.config.sorts.size() >= database::kMaxSortsPerView) {
                return Status::DatabaseViewSpecTooComplex;
            }

            Sort sort;
            ObjectIdBytes raw{};
            if (const Status s = stmt.column_fixed_blob(0, raw.data(), raw.size());
                !is_ok(s)) {
                return s;
            }
            sort.property_id = PropertyId(raw);

            const std::int64_t raw_desc = stmt.column_int64(1);
            if (raw_desc != 0 && raw_desc != 1) { return Status::StoreReadFailed; }
            sort.descending = (raw_desc == 1);

            view.config.sorts.push_back(sort);
        }
    }

    // Validate the reloaded view against the live schema: a saved view is as
    // attacker-reachable as a row is (spec section 28).
    if (const Status s = database::validate_view(db, view); !is_ok(s)) {
        return Status::StoreReadFailed;
    }

    out = std::move(view);
    return Status::Ok;
}

Status SqliteDatabaseStore::list_views(const DatabaseId& id,
                                       std::vector<View>& out) const noexcept {
    if (id.is_nil()) { return Status::IdMalformed; }

    std::vector<ViewId> ids;
    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(), "SELECT id FROM db_view WHERE database_id = ?1 ORDER BY id;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }

        for (;;) {
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) { break; }
            if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

            if (ids.size() >= database::kMaxViewsPerDatabase) {
                return Status::DatabaseViewLimitExceeded;
            }
            ObjectIdBytes raw{};
            if (const Status s = stmt.column_fixed_blob(0, raw.data(), raw.size());
                !is_ok(s)) {
                return s;
            }
            ids.push_back(ViewId(raw));
        }
    }

    std::vector<View> views;
    views.reserve(ids.size());
    for (const ViewId& view_id : ids) {
        View view;
        if (const Status s = get_view(view_id, view); !is_ok(s)) { return s; }
        views.push_back(std::move(view));
    }

    out = std::move(views);
    return Status::Ok;
}

Status SqliteDatabaseStore::delete_view(const ViewId& id) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    if (id.is_nil()) { return Status::IdMalformed; }

    Transaction txn;
    if (const Status s = txn.begin(conn_.get()); !is_ok(s)) { return s; }

    DatabaseId unused;
    if (const Status s = database_id_of_view(id, unused); !is_ok(s)) { return s; }

    if (const Status s = delete_by_id(
            conn_.get(), "DELETE FROM db_view_filter WHERE view_id = ?1;", id.bytes());
        !is_ok(s)) {
        return s;
    }
    if (const Status s = delete_by_id(
            conn_.get(), "DELETE FROM db_view_sort WHERE view_id = ?1;", id.bytes());
        !is_ok(s)) {
        return s;
    }
    if (const Status s =
            delete_by_id(conn_.get(), "DELETE FROM db_view WHERE id = ?1;", id.bytes());
        !is_ok(s)) {
        return s;
    }

    // Deleting a view touches NO row. That is the whole point of storing a
    // view's configuration separately from the data (see view.h).
    return txn.commit();
}

// --- querying --------------------------------------------------------------

Status SqliteDatabaseStore::query_table_view(const DatabaseId& id,
                                             const ViewConfig& config,
                                             std::size_t limit,
                                             std::vector<Row>& out) const noexcept {
    if (id.is_nil()) { return Status::IdMalformed; }

    Database db;
    if (const Status s = get_database(id, db); !is_ok(s)) { return s; }
    // Validate BEFORE compiling. Everything the compiler would otherwise
    // have to re-check -- unknown property, illegal operator for the type,
    // missing or wrongly-typed operand, too many filters -- is reported here
    // with the domain code that names the actual problem.
    if (const Status s = database::validate_view_config(db, config); !is_ok(s)) {
        return s;
    }

    std::string sql;
    std::vector<Binding> bindings;
    if (const Status s = compile_row_query(db, config, limit, sql, bindings); !is_ok(s)) {
        return s;
    }

    std::vector<Row> rows;
    {
        Statement stmt;
        if (const Status s = stmt.prepare(conn_.get(), sql + ";"); !is_ok(s)) { return s; }
        if (const Status s = apply_bindings(stmt, bindings);       !is_ok(s)) { return s; }

        for (;;) {
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) { break; }
            if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

            if (rows.size() >= database::kMaxRowsPerDatabase) {
                return Status::DatabaseRowLimitExceeded;
            }

            Row row;
            ObjectIdBytes raw{};
            if (const Status s = stmt.column_fixed_blob(0, raw.data(), raw.size());
                !is_ok(s)) {
                return s;
            }
            row.id             = RowId(raw);
            row.database_id    = id;
            row.created_at_ms  = stmt.column_int64(1);
            row.modified_at_ms = stmt.column_int64(2);
            rows.push_back(std::move(row));
        }
    }

    if (const Status s = load_values_for(db, sql, bindings, rows); !is_ok(s)) { return s; }

    out = std::move(rows);
    return Status::Ok;
}

Status SqliteDatabaseStore::query_saved_view(const ViewId& id,
                                             std::size_t limit,
                                             std::vector<Row>& out) const noexcept {
    View view;
    if (const Status s = get_view(id, view); !is_ok(s)) { return s; }
    return query_table_view(view.database_id, view.config, limit, out);
}

// --- open ------------------------------------------------------------------

/// Run a semicolon-separated batch of DDL/PRAGMA statements.
Status exec_batch(sqlite3* db, const char* sql, Status failure) noexcept {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err != nullptr) {
        // Allocated by sqlite3_malloc; ours to free. The text is deliberately
        // not propagated: it can contain statement fragments, and spec
        // section 34 forbids user content in logs.
        sqlite3_free(err);
    }
    return rc == SQLITE_OK ? Status::Ok : failure;
}

Status apply_schema(sqlite3* db) noexcept {
    // The `page` table FIRST, from the ONE shared constant, because
    // `db_collection.page_id` references it. `IF NOT EXISTS` makes this a
    // no-op when a block store opened the file first. See document_sql.h.
    if (const Status s = exec_batch(db, document_sql::kSchemaSql, Status::StoreSchemaFailed);
        !is_ok(s)) {
        return s;
    }
    if (const Status s = exec_batch(db, kSchemaSql, Status::StoreSchemaFailed); !is_ok(s)) {
        return s;
    }

    Statement stmt;
    if (const Status s = stmt.prepare(
            db,
            "INSERT INTO schema_meta(key, value) VALUES('database_schema_version', ?1) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
        !is_ok(s)) {
        return Status::StoreSchemaFailed;
    }
    if (const Status s = stmt.bind_int64(1, kDatabaseSchemaVersion); !is_ok(s)) {
        return Status::StoreSchemaFailed;
    }
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return Status::StoreSchemaFailed;
    }
    return Status::Ok;
}

/// Refuse a database written by a newer build: opening it read-write would
/// silently drop whatever columns we do not know about.
Status check_schema_version(sqlite3* db) noexcept {
    Statement stmt;
    if (const Status s = stmt.prepare(
            db, "SELECT value FROM schema_meta WHERE key = 'database_schema_version';");
        !is_ok(s)) {
        return Status::Ok;  // table absent => fresh database, not an error
    }
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) { return Status::Ok; }
    if (rc != SQLITE_ROW)  { return Status::StoreSchemaFailed; }

    if (stmt.column_int64(0) > kDatabaseSchemaVersion) { return Status::StoreSchemaFailed; }
    return Status::Ok;
}

}  // namespace

Status open_sqlite_database_store(const SqliteDatabaseStoreOptions& options,
                                  std::unique_ptr<database::DatabaseStore>& out) {
    if (options.path.empty()) {
        return Status::InvalidArgument;
    }
    if (options.path.find('\0') != std::string::npos) {
        // sqlite3_open_v2 takes a NUL-terminated string; an embedded NUL
        // would silently open a different, shorter path than requested.
        return Status::InvalidArgument;
    }
    if (options.busy_timeout_ms < 0) {
        return Status::InvalidArgument;
    }

    const int flags = options.read_only
                          ? SQLITE_OPEN_READONLY
                          : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);

    sqlite3* raw_db = nullptr;
    const int rc = sqlite3_open_v2(options.path.c_str(), &raw_db, flags, nullptr);
    // open_v2 can hand back a handle even on failure; wrapping it
    // unconditionally is what guarantees it gets closed.
    Connection conn(raw_db);
    if (rc != SQLITE_OK || !conn) {
        return Status::StoreOpenFailed;
    }

    if (sqlite3_busy_timeout(conn.get(), options.busy_timeout_ms) != SQLITE_OK) {
        return Status::StoreOpenFailed;
    }

    if (options.read_only) {
        if (const Status s = check_schema_version(conn.get()); !is_ok(s)) { return s; }
    } else {
        if (const Status s = exec_batch(conn.get(),
                                        "PRAGMA journal_mode = WAL;"
                                        "PRAGMA foreign_keys = ON;"
                                        "PRAGMA synchronous  = NORMAL;",
                                        Status::StoreOpenFailed);
            !is_ok(s)) {
            return s;
        }
        if (const Status s = check_schema_version(conn.get()); !is_ok(s)) { return s; }
        if (const Status s = apply_schema(conn.get());         !is_ok(s)) { return s; }
    }

    auto* store = new (std::nothrow) SqliteDatabaseStore(std::move(conn), options.read_only);
    if (store == nullptr) {
        return Status::StoreOpenFailed;  // allocation failure is checked, not assumed away
    }
    out.reset(store);
    return Status::Ok;
}

}  // namespace uapp::storage::sqlite
