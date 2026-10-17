// SPDX-License-Identifier: Apache-2.0
//
// core/database/view.h — views over a Database (spec section 15).
//
// Spec section 15 lists six view kinds. ONE is implemented: Table. The other
// five are declared, given a permanent numeric value, and reported as
// Status::DatabaseViewKindUnsupported with a machine-readable reason
// (`view_kind_not_implemented_reason`). They are NOT fake-implemented.
//
// Table first because it is the only kind that is a view of the SCHEMA rather
// than of one distinguished property: a Board needs a Select to group by, a
// Calendar and a Timeline need a Date (a Timeline needs two), a Gallery needs
// a File or an image, and a List is a Table with the columns hidden. Every
// one of those is Table plus a projection, so Table is the one that has to be
// right first.
//
// ===========================================================================
// WHAT A VIEW IS, AND WHAT IT IS NOT
// ===========================================================================
// A saved view is a NAMED FILTER+SORT CONFIGURATION, stored SEPARATELY from
// the rows. That separation is the whole point of the type: the same rows are
// read through many views, a view is edited far more often than the rows are,
// and a view must be deletable without touching a single row. Nothing in
// `ViewConfig` is derived from the data, so a view is valid against a schema
// rather than against a row set.
//
// NOT IMPLEMENTED in a view this run, each for its own reason:
//
//   OR / NESTED FILTER GROUPS  The filter list is a flat AND. Section 3.4
//                              specifies a "filter AST" and that is the right
//                              end state, but an AST needs a grouping
//                              compiler, a depth limit of its own (it is
//                              recursive, and recursion over untrusted input
//                              is the kMaxBlockDepth problem again) and a UI
//                              that can express it. A flat AND is what the
//                              overwhelming majority of real views are.
//   GROUPING                   Section 15 lists it. It is only meaningful
//                              when something renders groups -- which is the
//                              Board view -- and a grouped Table is a Board
//                              drawn differently. Deferred with Board.
//   SEARCH WITHIN A VIEW       Section 15 lists it. Text search over property
//                              values belongs in the unified index
//                              (core/search, spec section 16), which already
//                              owns tokenisation, ranking and the CJK
//                              problem. A second, private substring matcher
//                              here would be a second answer to "why did this
//                              not match". The `contains` operator is a
//                              byte-exact substring test and is documented as
//                              that, not as search.
//   COLUMN VISIBILITY / WIDTH  Per-viewer UI state, not document content --
//                              the same argument blocks/block.cpp makes for
//                              the Toggle block's collapsed state. It needs
//                              the view-state store that does not exist.
//   PAGINATION CURSORS         Queries take a `limit`. A resumable cursor
//                              needs a stable total order including the tie
//                              break, which exists here (see
//                              `evaluate_table_view`), but it also needs an
//                              opaque cursor encoding and a staleness policy.

#ifndef UAPP_CORE_DATABASE_VIEW_H
#define UAPP_CORE_DATABASE_VIEW_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "uapp/database/database.h"
#include "uapp/database/property.h"
#include "uapp/status.h"

namespace uapp::database {

struct ViewIdTag;
using ViewId = ObjectId<ViewIdTag>;

/// Maximum bytes in a view name. A view name is a TAB LABEL; it is shown in a
/// horizontal strip above the table, so it has a tighter natural bound than a
/// database name and the same one as a property name.
inline constexpr std::size_t kMaxViewNameBytes = 128;

/// Maximum filters in one view, and maximum sort keys.
///
/// WHY A LIMIT, and why THESE numbers: the adapter compiles a view into ONE
/// SQL statement in which each referenced property contributes a subquery
/// over `db_value` (storage/sqlite/src/sqlite_database_store.cpp). SQLite's
/// own hard ceiling is SQLITE_MAX_COMPOUND_SELECT / the 64-table join limit,
/// and a statement that exceeds it fails at prepare time with an error that
/// says nothing about views. 32 + 8 keeps the worst-case statement at 41
/// correlated references, comfortably inside that ceiling, so the failure a
/// user can reach is a domain error naming the view rather than a SQL
/// compile error. Beyond that, 32 ANDed filters is already past what a filter
/// editor can display and 8 sort keys is past what any ordering a human can
/// reason about needs.
inline constexpr std::size_t kMaxFiltersPerView = 32;
inline constexpr std::size_t kMaxSortsPerView   = 8;

/// Maximum saved views on one database.
///
/// WHY 64: saved views are HUMAN-CURATED TABS, not generated artefacts --
/// every one of them was named by someone. 64 is well past the point where a
/// tab strip stops being navigable, and it bounds the one query in this
/// module that returns whole view configurations at once (`list_views`).
inline constexpr std::size_t kMaxViewsPerDatabase = 64;

// --- view kinds ------------------------------------------------------------

/// Every view kind in spec section 15. Values are PERSISTED in the
/// `db_view.kind` column: append-only, never renumber, never reuse. The
/// numbering matches database-design.md section 3.4's `db_view.kind` comment
/// for Table/Board/Calendar; Gallery/List/Timeline are renumbered to keep
/// this enum contiguous and the deviation is recorded in that document.
enum class ViewKind : std::int32_t {
    // --- implemented -------------------------------------------------------
    Table = 0,

    // --- declared, NOT IMPLEMENTED ----------------------------------------
    Board    = 1,
    Calendar = 2,
    Timeline = 3,
    Gallery  = 4,
    List     = 5,
};

inline constexpr std::int32_t kFirstUnimplementedViewKind = 1;
inline constexpr std::int32_t kViewKindCount              = 6;

/// Stable lowercase identifier ("table", "board", ...). Never null; returns
/// "unknown" for an out-of-range integer cast into the enum.
[[nodiscard]] const char* view_kind_name(ViewKind kind) noexcept;

[[nodiscard]] bool view_kind_is_implemented(ViewKind kind) noexcept;

/// Why a declared-but-absent kind is absent. Returns nullptr for an
/// implemented kind. Machine-readable, for the same reason
/// block_type_not_implemented_reason is: a reason in a comment rots.
[[nodiscard]] const char* view_kind_not_implemented_reason(ViewKind kind) noexcept;

// --- filters ---------------------------------------------------------------

/// The filter operators, as ONE generic set rather than one set per property
/// type.
///
/// WHY ONE SET: spec section 15 asks for filters "appropriate to each
/// property type" -- `>` / `<` / `=` on a Number, `is` / `is not` on a
/// Select, `before` / `after` / `on` on a Date. Those are the same three
/// comparisons with three vocabularies, and minting
/// DateBefore/NumberLessThan/SelectIs as distinct enum values would mean
/// three codes for one condition and three code paths to keep in agreement.
/// Instead: one comparison vocabulary, a LEGALITY TABLE saying which
/// operators apply to which type (`filter_operator_is_legal`), and a
/// PER-TYPE LABEL for display (`filter_operator_label`) so the UI shows
/// "before" on a Date and "<" on a Number. Values are PERSISTED: append-only.
enum class FilterOperator : std::int32_t {
    Equals         = 0,  ///< Select/Text: "is". Date: "on". Checkbox: "is".
    NotEquals      = 1,  ///< Select/Text: "is not". Date: "is not on".
    LessThan       = 2,  ///< Number: "<". Date: "before".
    LessOrEqual    = 3,  ///< Number: "<=". Date: "on or before".
    GreaterThan    = 4,  ///< Number: ">". Date: "after".
    GreaterOrEqual = 5,  ///< Number: ">=". Date: "on or after".
    Contains       = 6,  ///< Text: byte-exact substring. Multi-select: membership.
    NotContains    = 7,
    IsEmpty        = 8,  ///< the row leaves the property UNSET. Takes no operand.
    IsNotEmpty     = 9,  ///< Takes no operand.
};

inline constexpr std::int32_t kFilterOperatorCount = 10;

/// Canonical machine-readable name ("equals", "less_than", ...). Never null.
[[nodiscard]] const char* filter_operator_name(FilterOperator op) noexcept;

/// The label to SHOW for this operator on this property type -- "before" for
/// LessThan on a Date, "<" for LessThan on a Number, "is" for Equals on a
/// Select. Returns nullptr when the pair is not legal, so a caller building a
/// filter editor iterates the operators and skips the nulls.
[[nodiscard]] const char* filter_operator_label(PropertyType type,
                                                FilterOperator op) noexcept;

/// Is this operator applicable to this property type? This is the table that
/// makes "appropriate to each property type" a checked property rather than a
/// convention: `>` on a Checkbox and `contains` on a Number are rejected with
/// Status::DatabaseFilterOperatorUnsupported, not silently evaluated.
[[nodiscard]] bool filter_operator_is_legal(PropertyType type, FilterOperator op) noexcept;

/// True iff the operator compares against an operand. IsEmpty / IsNotEmpty
/// are the only two that do not, and passing one an operand is an error
/// rather than an ignored field -- an ignored field is how a user ends up
/// with a filter that does not do what the editor showed them.
[[nodiscard]] bool filter_operator_takes_operand(FilterOperator op) noexcept;

struct Filter final {
    PropertyId     property_id;
    FilterOperator op = FilterOperator::Equals;

    /// The operand. Absent exactly when `filter_operator_takes_operand(op)`
    /// is false.
    ///
    /// Its type must be the PROPERTY's type, with ONE documented exception:
    /// for Contains / NotContains on a MULTI-SELECT property the operand is a
    /// `SelectValue` naming ONE option, because "contains" on a set is a
    /// membership test against a single member, not a comparison against
    /// another set. Exact-set-equality on a Multi-select is NOT IMPLEMENTED;
    /// it is a different question ("is this exactly these three tags") that
    /// needs its own operator and has no real demand.
    std::optional<PropertyValue> operand;
};

struct Sort final {
    PropertyId property_id;
    bool       descending = false;
};

/// The saved configuration: a flat AND of filters, plus an ordered list of
/// sort keys. Contains no row data and no derived data.
struct ViewConfig final {
    std::vector<Filter> filters;
    std::vector<Sort>   sorts;
};

/// A named, persisted view.
struct View final {
    ViewId      id;
    DatabaseId  database_id;
    std::string name;
    ViewKind    kind = ViewKind::Table;
    ViewConfig  config;
};

/// Validate a config against a SCHEMA: every filter and sort must reference a
/// property the database defines (Status::DatabasePropertyNotInSchema), every
/// operator must be legal for that property's type
/// (Status::DatabaseFilterOperatorUnsupported), every operand must be present
/// iff the operator takes one and must be of the right type
/// (Status::DatabaseValueTypeMismatch), and the counts must be within
/// kMaxFiltersPerView / kMaxSortsPerView (Status::DatabaseViewSpecTooComplex).
///
/// Operands are also validated as VALUES, so a filter cannot smuggle a NaN or
/// a 1 MiB string past the limits a stored value has to obey.
[[nodiscard]] Status validate_view_config(const Database& db, const ViewConfig& config) noexcept;

/// Everything validate_view_config checks, plus the view's own identity:
/// non-nil ids, the right database_id, a legal name, and an IMPLEMENTED kind
/// (Status::DatabaseViewKindUnsupported).
[[nodiscard]] Status validate_view(const Database& db, const View& view) noexcept;

[[nodiscard]] Status validate_view_name(std::string_view name) noexcept;

// --- evaluation ------------------------------------------------------------

/// Does one row satisfy one filter? The SEMANTIC DEFINITION of every
/// operator, in one place.
///
/// `property` must be the definition `filter.property_id` names; passing a
/// mismatched pair is a precondition violation and yields `false`.
///
/// Three semantics worth stating because they are choices, not consequences:
///
///  * AN UNSET PROPERTY SATISFIES A NEGATIVE OPERATOR. A row with no Status
///    matches "Status is not Done", because the question a user asks with
///    that filter is "show me what is not done" and a row with no status is
///    not done. NotEquals and NotContains are therefore defined as "NOT
///    (the positive form)", which makes them total over the unset case.
///  * AN UNSET PROPERTY SATISFIES NO POSITIVE OPERATOR, including the
///    comparisons. "Price < 10" does not match a row with no price.
///  * `contains` IS BYTE-EXACT AND CASE-SENSITIVE. It is a substring test,
///    not search; see the NOT IMPLEMENTED note at the top of this header.
[[nodiscard]] bool filter_matches(const PropertyDefinition& property,
                                  const Filter& filter,
                                  const Row& row) noexcept;

/// The SEMANTIC REFERENCE implementation of a Table view: filter, then sort,
/// then truncate to `limit`.
///
/// WHY THIS EXISTS WHEN THE ADAPTER COMPILES THE SAME CONFIG INTO SQL: it is
/// the ORACLE. A view's meaning is defined here, in 100 lines of C++ that can
/// be read and tested directly against hand-built rows, and the SQL compiler
/// in storage/sqlite is then a performance optimisation that must AGREE with
/// it -- which the differential cases in tests/database assert over the same
/// data. Without the oracle, "is the ORDER BY right?" has no answer except
/// another reading of the SQL.
///
/// Ordering is TOTAL and implementation-independent: sort keys in order, then
/// row id bytes as the final tie-break. An UNSET value sorts LAST in both
/// directions -- not "largest", which is what a naive NULL ordering gives and
/// which puts the empty rows on top of a descending view, where they are
/// never what the user was looking for.
///
/// `limit` is clamped to kMaxRowsPerDatabase. `rows` is not required to be
/// pre-filtered or sorted, and `out` is written only on Status::Ok.
[[nodiscard]] Status evaluate_table_view(const Database& db,
                                         const ViewConfig& config,
                                         const std::vector<Row>& rows,
                                         std::size_t limit,
                                         std::vector<Row>& out);

}  // namespace uapp::database

#endif  // UAPP_CORE_DATABASE_VIEW_H
