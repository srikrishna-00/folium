// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the Table view: the NOT-IMPLEMENTED view kinds, the
// operator legality table, view-config validation, and the SEMANTIC
// REFERENCE evaluator (database::evaluate_table_view) that the SQL compiler
// in storage/sqlite has to agree with.
//
// These cases are pure: no store, no file, no SQLite. If one of them fails,
// the MEANING of a view changed, not its persistence.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <variant>
#include <vector>

#include "database_fixtures.h"
#include "test_framework.h"
#include "uapp/database/database.h"
#include "uapp/database/property.h"
#include "uapp/database/view.h"
#include "uapp/status.h"

namespace {

using uapp::Status;
using uapp::database::Database;
using uapp::database::DateValue;
using uapp::database::Filter;
using uapp::database::FilterOperator;
using uapp::database::NumberValue;
using uapp::database::OptionId;
using uapp::database::PropertyId;
using uapp::database::PropertyType;
using uapp::database::PropertyValue;
using uapp::database::Row;
using uapp::database::SelectValue;
using uapp::database::Sort;
using uapp::database::TextValue;
using uapp::database::View;
using uapp::database::ViewConfig;
using uapp::database::ViewId;
using uapp::database::ViewKind;
using uapp::test::describe;
using uapp::test::filter_config;
using uapp::test::fixture_id;
using uapp::test::sort_config;
using uapp::test::titles_of;

/// The five-row corpus every evaluation case below runs against. It is built
/// so that EVERY property type has: distinct values, a tie, and at least one
/// row that leaves the property UNSET -- because "unset" is the case that
/// distinguishes a correct filter from a plausible one.
///
///  id  Title    Priority  Done   Status       Tags                  Due
///  100 alpha    3         false  In progress  urgent, docs          5
///  101 beta     1         true   Done         backend               2
///  102 gamma    10        false  To do        (unset)               9
///  103 delta    3         (uns)  (unset)      docs                  (unset)
///  104 epsilon  (unset)   true   Done         urgent, backend, docs 2
std::vector<Row> corpus() {
    std::vector<Row> rows;
    rows.push_back(uapp::test::RowBuilder(100)
                       .title("alpha")
                       .priority(3.0)
                       .done(false)
                       .status(uapp::test::kOptInProgress)
                       .tags({uapp::test::kOptUrgent, uapp::test::kOptDocs})
                       .due(5)
                       .build());
    rows.push_back(uapp::test::RowBuilder(101)
                       .title("beta")
                       .priority(1.0)
                       .done(true)
                       .status(uapp::test::kOptDone)
                       .tags({uapp::test::kOptBackend})
                       .due(2)
                       .build());
    rows.push_back(uapp::test::RowBuilder(102)
                       .title("gamma")
                       .priority(10.0)
                       .done(false)
                       .status(uapp::test::kOptToDo)
                       .due(9)
                       .build());
    rows.push_back(uapp::test::RowBuilder(103)
                       .title("delta")
                       .priority(3.0)
                       .tags({uapp::test::kOptDocs})
                       .build());
    rows.push_back(uapp::test::RowBuilder(104)
                       .title("epsilon")
                       .done(true)
                       .status(uapp::test::kOptDone)
                       .tags({uapp::test::kOptUrgent, uapp::test::kOptBackend,
                              uapp::test::kOptDocs})
                       .due(2)
                       .build());
    return rows;
}

constexpr std::size_t kNoLimit = 1000;

}  // namespace

// --- view kinds ------------------------------------------------------------

UAPP_TEST("database.view: Table is the only implemented kind, the other five say why") {
    UAPP_CHECK(uapp::database::view_kind_is_implemented(ViewKind::Table));
    UAPP_CHECK(uapp::database::view_kind_not_implemented_reason(ViewKind::Table) ==
               nullptr);

    const ViewKind unimplemented[] = {ViewKind::Board, ViewKind::Calendar,
                                      ViewKind::Timeline, ViewKind::Gallery,
                                      ViewKind::List};
    for (const ViewKind kind : unimplemented) {
        UAPP_CHECK_MSG(!uapp::database::view_kind_is_implemented(kind),
                       std::string(uapp::database::view_kind_name(kind)) +
                           " claims to be implemented");
        const char* reason = uapp::database::view_kind_not_implemented_reason(kind);
        UAPP_REQUIRE_MSG(reason != nullptr,
                         std::string("no reason for ") +
                             uapp::database::view_kind_name(kind));
        const std::string text(reason);
        UAPP_CHECK_MSG(text.find("NOT IMPLEMENTED") == 0,
                       std::string("reason for ") + uapp::database::view_kind_name(kind) +
                           " lacks the marker");
        UAPP_CHECK(text.size() > 80);
    }

    for (std::int32_t raw = 0; raw < uapp::database::kViewKindCount; ++raw) {
        UAPP_CHECK(std::string(uapp::database::view_kind_name(
                       static_cast<ViewKind>(raw))) != "unknown");
    }
}

UAPP_TEST("database.view: saving a non-Table view is refused, not silently downgraded") {
    const Database db = uapp::test::task_database();
    View view;
    view.id          = fixture_id<ViewId>(200);
    view.database_id = db.id;
    view.name        = "By status";
    view.kind        = ViewKind::Board;
    UAPP_REQUIRE_STATUS(uapp::database::validate_view(db, view),
                        Status::DatabaseViewKindUnsupported);

    view.kind = ViewKind::Table;
    UAPP_REQUIRE_OK(uapp::database::validate_view(db, view));
}

// --- the legality table ----------------------------------------------------

UAPP_TEST("database.view: each property type admits exactly its own operators") {
    struct Expectation final {
        PropertyType type;
        bool equality;     ///< Equals / NotEquals
        bool comparison;   ///< LessThan / LessOrEqual / GreaterThan / GreaterOrEqual
        bool containment;  ///< Contains / NotContains
    };
    // Spec section 15 asks for filters "appropriate to each property type".
    // This table IS that sentence, made checkable.
    const Expectation expectations[] = {
        {PropertyType::Text, true, false, true},
        {PropertyType::Number, true, true, false},
        {PropertyType::Checkbox, true, false, false},
        {PropertyType::Select, true, false, false},
        {PropertyType::MultiSelect, false, false, true},
        {PropertyType::Date, true, true, false},
    };

    for (const Expectation& e : expectations) {
        const char* name = uapp::database::property_type_name(e.type);

        UAPP_CHECK_MSG(uapp::database::filter_operator_is_legal(
                           e.type, FilterOperator::Equals) == e.equality,
                       std::string(name) + ": equals legality is wrong");
        UAPP_CHECK_MSG(uapp::database::filter_operator_is_legal(
                           e.type, FilterOperator::NotEquals) == e.equality,
                       std::string(name) + ": not-equals legality is wrong");

        const FilterOperator comparisons[] = {
            FilterOperator::LessThan, FilterOperator::LessOrEqual,
            FilterOperator::GreaterThan, FilterOperator::GreaterOrEqual};
        for (const FilterOperator op : comparisons) {
            UAPP_CHECK_MSG(
                uapp::database::filter_operator_is_legal(e.type, op) == e.comparison,
                std::string(name) + ": " + uapp::database::filter_operator_name(op) +
                    " legality is wrong");
        }

        UAPP_CHECK_MSG(uapp::database::filter_operator_is_legal(
                           e.type, FilterOperator::Contains) == e.containment,
                       std::string(name) + ": contains legality is wrong");
        UAPP_CHECK_MSG(uapp::database::filter_operator_is_legal(
                           e.type, FilterOperator::NotContains) == e.containment,
                       std::string(name) + ": not-contains legality is wrong");

        // EVERY implemented type admits the two presence operators: they are
        // the only way to ask about the unset state, which every property has.
        UAPP_CHECK_MSG(
            uapp::database::filter_operator_is_legal(e.type, FilterOperator::IsEmpty),
            std::string(name) + ": is-empty must always be legal");
        UAPP_CHECK_MSG(
            uapp::database::filter_operator_is_legal(e.type, FilterOperator::IsNotEmpty),
            std::string(name) + ": is-not-empty must always be legal");
    }

    // No operator is legal on a declared-but-unimplemented type.
    const PropertyType absent[] = {PropertyType::Person, PropertyType::Relation,
                                   PropertyType::Formula};
    for (const PropertyType type : absent) {
        for (std::int32_t raw = 0; raw < uapp::database::kFilterOperatorCount; ++raw) {
            UAPP_CHECK(!uapp::database::filter_operator_is_legal(
                type, static_cast<FilterOperator>(raw)));
        }
    }
}

UAPP_TEST("database.view: operator labels are per type, so a Date reads 'before'") {
    // One operator, three vocabularies -- which is the whole reason there is
    // one FilterOperator enum and not three.
    UAPP_CHECK(std::string(uapp::database::filter_operator_label(
                   PropertyType::Date, FilterOperator::LessThan)) == "before");
    UAPP_CHECK(std::string(uapp::database::filter_operator_label(
                   PropertyType::Date, FilterOperator::GreaterThan)) == "after");
    UAPP_CHECK(std::string(uapp::database::filter_operator_label(
                   PropertyType::Date, FilterOperator::Equals)) == "on");
    UAPP_CHECK(std::string(uapp::database::filter_operator_label(
                   PropertyType::Number, FilterOperator::LessThan)) == "<");
    UAPP_CHECK(std::string(uapp::database::filter_operator_label(
                   PropertyType::Number, FilterOperator::Equals)) == "=");
    UAPP_CHECK(std::string(uapp::database::filter_operator_label(
                   PropertyType::Select, FilterOperator::Equals)) == "is");
    UAPP_CHECK(std::string(uapp::database::filter_operator_label(
                   PropertyType::Select, FilterOperator::NotEquals)) == "is not");

    // An illegal pair has NO label, so a filter editor that iterates the
    // operators and skips the nulls shows exactly the legal ones.
    UAPP_CHECK(uapp::database::filter_operator_label(PropertyType::Checkbox,
                                                     FilterOperator::GreaterThan) ==
               nullptr);
    UAPP_CHECK(uapp::database::filter_operator_label(PropertyType::Number,
                                                     FilterOperator::Contains) ==
               nullptr);
}

// --- config validation -----------------------------------------------------

UAPP_TEST("database.view: an illegal operator for the type is rejected") {
    const Database db = uapp::test::task_database();

    const PropertyValue three = NumberValue{3.0};
    UAPP_REQUIRE_OK(uapp::database::validate_view_config(
        db, filter_config(uapp::test::kPropPriority, FilterOperator::GreaterThan, &three)));

    // `>` on a Checkbox: "checked > unchecked" is not a question anyone asks.
    const PropertyValue checked = uapp::database::CheckboxValue{true};
    UAPP_REQUIRE_STATUS(
        uapp::database::validate_view_config(
            db, filter_config(uapp::test::kPropDone, FilterOperator::GreaterThan, &checked)),
        Status::DatabaseFilterOperatorUnsupported);

    // `contains` on a Number.
    UAPP_REQUIRE_STATUS(
        uapp::database::validate_view_config(
            db, filter_config(uapp::test::kPropPriority, FilterOperator::Contains, &three)),
        Status::DatabaseFilterOperatorUnsupported);

    // `=` on a Multi-select: exact set equality is a different question, and
    // is NOT IMPLEMENTED rather than quietly reinterpreted as membership.
    const PropertyValue one_tag =
        SelectValue{fixture_id<OptionId>(uapp::test::kOptUrgent)};
    UAPP_REQUIRE_STATUS(
        uapp::database::validate_view_config(
            db, filter_config(uapp::test::kPropTags, FilterOperator::Equals, &one_tag)),
        Status::DatabaseFilterOperatorUnsupported);
}

UAPP_TEST("database.view: an operand must be present iff the operator takes one") {
    const Database db = uapp::test::task_database();

    // `is empty` with an operand: not ignored, because a silently dropped
    // operand is a filter that does not do what the editor displayed.
    const PropertyValue three = NumberValue{3.0};
    UAPP_REQUIRE_STATUS(
        uapp::database::validate_view_config(
            db, filter_config(uapp::test::kPropPriority, FilterOperator::IsEmpty, &three)),
        Status::DatabaseFilterOperatorUnsupported);

    // `=` with no operand.
    UAPP_REQUIRE_STATUS(
        uapp::database::validate_view_config(
            db, filter_config(uapp::test::kPropPriority, FilterOperator::Equals, nullptr)),
        Status::DatabaseFilterOperatorUnsupported);

    UAPP_REQUIRE_OK(uapp::database::validate_view_config(
        db, filter_config(uapp::test::kPropPriority, FilterOperator::IsEmpty, nullptr)));
}

UAPP_TEST("database.view: an operand is validated as a stored value would be") {
    const Database db = uapp::test::task_database();

    // A filter must not be a way around the limits a persisted value obeys.
    const PropertyValue nan_operand =
        NumberValue{std::numeric_limits<double>::quiet_NaN()};
    UAPP_REQUIRE_STATUS(
        uapp::database::validate_view_config(
            db,
            filter_config(uapp::test::kPropPriority, FilterOperator::Equals, &nan_operand)),
        Status::DatabaseValueMalformed);

    const PropertyValue huge =
        TextValue{std::string(uapp::database::kMaxPropertyTextBytes + 1, 'x')};
    UAPP_REQUIRE_STATUS(
        uapp::database::validate_view_config(
            db, filter_config(uapp::test::kPropTitle, FilterOperator::Contains, &huge)),
        Status::DatabaseValueTooLarge);

    // A wrongly-typed operand.
    const PropertyValue text_for_number = TextValue{"3"};
    UAPP_REQUIRE_STATUS(
        uapp::database::validate_view_config(
            db, filter_config(uapp::test::kPropPriority, FilterOperator::Equals,
                              &text_for_number)),
        Status::DatabaseValueTypeMismatch);

    // An out-of-schema option in a Select filter.
    const PropertyValue ghost_option = SelectValue{fixture_id<OptionId>(777)};
    UAPP_REQUIRE_STATUS(
        uapp::database::validate_view_config(
            db,
            filter_config(uapp::test::kPropStatus, FilterOperator::Equals, &ghost_option)),
        Status::DatabaseOptionNotInSchema);
}

UAPP_TEST("database.view: filters and sorts must name a property the schema defines") {
    const Database db = uapp::test::task_database();
    const PropertyValue value = TextValue{"x"};
    UAPP_REQUIRE_STATUS(
        uapp::database::validate_view_config(
            db, filter_config(555, FilterOperator::Equals, &value)),
        Status::DatabasePropertyNotInSchema);
    UAPP_REQUIRE_STATUS(uapp::database::validate_view_config(db, sort_config(555, false)),
                        Status::DatabasePropertyNotInSchema);
}

UAPP_TEST("database.bounds: a view spec past the filter/sort caps is rejected") {
    const Database db = uapp::test::task_database();

    ViewConfig config;
    const PropertyValue value = TextValue{"x"};
    for (std::size_t i = 0; i < uapp::database::kMaxFiltersPerView; ++i) {
        Filter filter;
        filter.property_id = fixture_id<PropertyId>(uapp::test::kPropTitle);
        filter.op          = FilterOperator::Contains;
        filter.operand     = value;
        config.filters.push_back(filter);
    }
    UAPP_REQUIRE_OK(uapp::database::validate_view_config(db, config));

    {
        ViewConfig over = config;
        Filter filter;
        filter.property_id = fixture_id<PropertyId>(uapp::test::kPropTitle);
        filter.op          = FilterOperator::Contains;
        filter.operand     = value;
        over.filters.push_back(filter);
        UAPP_REQUIRE_STATUS(uapp::database::validate_view_config(db, over),
                            Status::DatabaseViewSpecTooComplex);
    }

    ViewConfig sorts;
    for (std::size_t i = 0; i < uapp::database::kMaxSortsPerView; ++i) {
        Sort sort;
        sort.property_id = fixture_id<PropertyId>(uapp::test::kPropPriority);
        sorts.sorts.push_back(sort);
    }
    UAPP_REQUIRE_OK(uapp::database::validate_view_config(db, sorts));
    Sort one_more;
    one_more.property_id = fixture_id<PropertyId>(uapp::test::kPropPriority);
    sorts.sorts.push_back(one_more);
    UAPP_REQUIRE_STATUS(uapp::database::validate_view_config(db, sorts),
                        Status::DatabaseViewSpecTooComplex);
}

// --- filtering, by property type -------------------------------------------

UAPP_TEST("database.view: filtering a Text property by is / is not / contains") {
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    std::vector<Row> out;

    const PropertyValue beta = TextValue{"beta"};
    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropTitle, FilterOperator::Equals, &beta), rows,
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>{"beta"}, describe(out));

    const PropertyValue a = TextValue{"a"};
    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropTitle, FilterOperator::Contains, &a), rows,
        kNoLimit, out));
    // alpha, beta, gamma, delta all contain "a"; epsilon does not. Order is
    // the id tie-break, i.e. creation order.
    UAPP_REQUIRE_MSG(
        titles_of(out) == std::vector<std::string>({"alpha", "beta", "gamma", "delta"}),
        describe(out));

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropTitle, FilterOperator::NotContains, &a), rows,
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>{"epsilon"}, describe(out));

    // `contains` is CASE-SENSITIVE and byte-exact. It is a substring test,
    // not search -- and the SQL compiler must agree, which is why it uses
    // instr() rather than SQLite's case-insensitive LIKE.
    const PropertyValue upper = TextValue{"A"};
    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropTitle, FilterOperator::Contains, &upper), rows,
        kNoLimit, out));
    UAPP_REQUIRE_MSG(out.empty(), describe(out));
}

UAPP_TEST("database.view: filtering a Number property by < <= = >= >") {
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    std::vector<Row> out;

    const PropertyValue three = NumberValue{3.0};

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropPriority, FilterOperator::LessThan, &three),
        rows, kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>{"beta"}, describe(out));

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropPriority, FilterOperator::LessOrEqual, &three),
        rows, kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"alpha", "beta", "delta"}),
                     describe(out));

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropPriority, FilterOperator::Equals, &three), rows,
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"alpha", "delta"}),
                     describe(out));

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropPriority, FilterOperator::GreaterThan, &three),
        rows, kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>{"gamma"}, describe(out));

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db,
        filter_config(uapp::test::kPropPriority, FilterOperator::GreaterOrEqual, &three),
        rows, kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"alpha", "gamma", "delta"}),
                     describe(out));

    // "10" vs "9": the reason this type is a double and not a string. A
    // stringly-typed column would put gamma BELOW beta here.
    const PropertyValue nine = NumberValue{9.0};
    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropPriority, FilterOperator::GreaterThan, &nine),
        rows, kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>{"gamma"}, describe(out));
}

UAPP_TEST("database.view: filtering a Checkbox property by is / is not") {
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    std::vector<Row> out;

    const PropertyValue checked   = uapp::database::CheckboxValue{true};
    const PropertyValue unchecked = uapp::database::CheckboxValue{false};

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropDone, FilterOperator::Equals, &checked), rows,
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"beta", "epsilon"}),
                     describe(out));

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropDone, FilterOperator::Equals, &unchecked), rows,
        kNoLimit, out));
    // delta leaves Done UNSET, so it does NOT match "Done is false". Unset is
    // not false -- which is precisely why the payload is a bool and absence is
    // modelled by the property being missing from the row.
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"alpha", "gamma"}),
                     describe(out));

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropDone, FilterOperator::NotEquals, &checked), rows,
        kNoLimit, out));
    // ... but "Done is not true" DOES include the unset row: the question a
    // user asks with that filter is "show me what is not done".
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"alpha", "gamma", "delta"}),
                     describe(out));
}

UAPP_TEST("database.view: filtering a Select property by is / is not") {
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    std::vector<Row> out;

    const PropertyValue done_option =
        SelectValue{fixture_id<OptionId>(uapp::test::kOptDone)};

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropStatus, FilterOperator::Equals, &done_option),
        rows, kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"beta", "epsilon"}),
                     describe(out));

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropStatus, FilterOperator::NotEquals, &done_option),
        rows, kNoLimit, out));
    // gamma (To do), alpha (In progress) and delta (UNSET) -- the unset row is
    // included, by the same rule as the Checkbox case above.
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"alpha", "gamma", "delta"}),
                     describe(out));
}

UAPP_TEST("database.view: filtering a Multi-select property by contains / does not") {
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    std::vector<Row> out;

    const PropertyValue urgent =
        SelectValue{fixture_id<OptionId>(uapp::test::kOptUrgent)};
    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropTags, FilterOperator::Contains, &urgent), rows,
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"alpha", "epsilon"}),
                     describe(out));

    const PropertyValue docs = SelectValue{fixture_id<OptionId>(uapp::test::kOptDocs)};
    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropTags, FilterOperator::Contains, &docs), rows,
        kNoLimit, out));
    UAPP_REQUIRE_MSG(
        titles_of(out) == std::vector<std::string>({"alpha", "delta", "epsilon"}),
        describe(out));

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropTags, FilterOperator::NotContains, &docs), rows,
        kNoLimit, out));
    // beta (backend only) and gamma (Tags unset).
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"beta", "gamma"}),
                     describe(out));
}

UAPP_TEST("database.view: filtering a Date property by before / after / on") {
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    std::vector<Row> out;

    const PropertyValue fifth = DateValue{uapp::test::fixture_date(5)};

    // "before the 5th"
    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropDue, FilterOperator::LessThan, &fifth), rows,
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"beta", "epsilon"}),
                     describe(out));

    // "after the 5th"
    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropDue, FilterOperator::GreaterThan, &fifth), rows,
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>{"gamma"}, describe(out));

    // "on the 5th" -- exact instant equality, which is what DateValue models
    // and what its declaration documents as the limitation.
    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropDue, FilterOperator::Equals, &fifth), rows,
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>{"alpha"}, describe(out));

    // One millisecond later is NOT "on the 5th", and that is the documented
    // consequence of there being no date-only type yet.
    const PropertyValue fifth_plus_ms = DateValue{uapp::test::fixture_date(5) + 1};
    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropDue, FilterOperator::Equals, &fifth_plus_ms),
        rows, kNoLimit, out));
    UAPP_REQUIRE_MSG(out.empty(), describe(out));
}

UAPP_TEST("database.view: is empty / is not empty address the unset state") {
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    std::vector<Row> out;

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropDue, FilterOperator::IsEmpty, nullptr), rows,
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>{"delta"}, describe(out));

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropTags, FilterOperator::IsEmpty, nullptr), rows,
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>{"gamma"}, describe(out));

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, filter_config(uapp::test::kPropPriority, FilterOperator::IsNotEmpty, nullptr),
        rows, kNoLimit, out));
    UAPP_REQUIRE_MSG(
        titles_of(out) == std::vector<std::string>({"alpha", "beta", "gamma", "delta"}),
        describe(out));
}

UAPP_TEST("database.view: several filters are ANDed") {
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    std::vector<Row> out;

    ViewConfig config;
    {
        Filter not_done;
        not_done.property_id = fixture_id<PropertyId>(uapp::test::kPropDone);
        not_done.op          = FilterOperator::NotEquals;
        not_done.operand     = PropertyValue(uapp::database::CheckboxValue{true});
        config.filters.push_back(not_done);

        Filter tagged;
        tagged.property_id = fixture_id<PropertyId>(uapp::test::kPropTags);
        tagged.op          = FilterOperator::Contains;
        tagged.operand =
            PropertyValue(SelectValue{fixture_id<OptionId>(uapp::test::kOptDocs)});
        config.filters.push_back(tagged);
    }

    UAPP_REQUIRE_OK(
        uapp::database::evaluate_table_view(db, config, rows, kNoLimit, out));
    // not done: alpha, gamma, delta. tagged docs: alpha, delta, epsilon.
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"alpha", "delta"}),
                     describe(out));
}

// --- sorting, by property type ---------------------------------------------

UAPP_TEST("database.view: sorting by a Text property, both directions") {
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    std::vector<Row> out;

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, sort_config(uapp::test::kPropTitle, false), rows, kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"alpha", "beta", "delta",
                                                                 "epsilon", "gamma"}),
                     describe(out));

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, sort_config(uapp::test::kPropTitle, true), rows, kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"gamma", "epsilon",
                                                                 "delta", "beta", "alpha"}),
                     describe(out));
}

UAPP_TEST("database.view: sorting by a Number property puts 10 above 9, not below") {
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    std::vector<Row> out;

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, sort_config(uapp::test::kPropPriority, false), rows, kNoLimit, out));
    // 1, 3, 3, 10, then epsilon (UNSET) LAST. The two 3s are tie-broken by
    // row id, i.e. creation order: alpha before delta.
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"beta", "alpha", "delta",
                                                                 "gamma", "epsilon"}),
                     describe(out));

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, sort_config(uapp::test::kPropPriority, true), rows, kNoLimit, out));
    // Descending: 10, 3, 3, 1 -- and epsilon is STILL last. An absent value
    // sorting "largest" would put the empty row on top of a descending view,
    // which is never what the user was looking for.
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"gamma", "alpha", "delta",
                                                                 "beta", "epsilon"}),
                     describe(out));
}

UAPP_TEST("database.view: sorting by a Checkbox property") {
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    std::vector<Row> out;

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, sort_config(uapp::test::kPropDone, false), rows, kNoLimit, out));
    // false (alpha, gamma), then true (beta, epsilon), then UNSET (delta).
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"alpha", "gamma", "beta",
                                                                 "epsilon", "delta"}),
                     describe(out));
}

UAPP_TEST("database.view: sorting by a Select property follows the SCHEMA option order") {
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    std::vector<Row> out;

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, sort_config(uapp::test::kPropStatus, false), rows, kNoLimit, out));
    // Schema order is To do, In progress, Done -- so gamma, alpha, then the
    // two Done rows, then delta (UNSET). ALPHABETICAL order would have been
    // Done, In progress, To do, which is a completely different answer: that
    // is the whole point of the Select sort key being the option's position.
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"gamma", "alpha", "beta",
                                                                 "epsilon", "delta"}),
                     describe(out));

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, sort_config(uapp::test::kPropStatus, true), rows, kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"beta", "epsilon", "alpha",
                                                                 "gamma", "delta"}),
                     describe(out));
}

UAPP_TEST("database.view: sorting by a Multi-select uses its earliest option") {
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    std::vector<Row> out;

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, sort_config(uapp::test::kPropTags, false), rows, kNoLimit, out));
    // Option order is urgent(0), backend(1), docs(2). Keys: alpha has
    // {urgent, docs} -> 0; beta has {backend} -> 1; delta has {docs} -> 2;
    // epsilon has all three -> 0; gamma is UNSET -> last. alpha and epsilon
    // tie at 0 and are tie-broken by id.
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"alpha", "epsilon", "beta",
                                                                 "delta", "gamma"}),
                     describe(out));
}

UAPP_TEST("database.view: sorting by a Date property") {
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    std::vector<Row> out;

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(
        db, sort_config(uapp::test::kPropDue, false), rows, kNoLimit, out));
    // 2nd (beta, epsilon), 5th (alpha), 9th (gamma), then delta UNSET.
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"beta", "epsilon", "alpha",
                                                                 "gamma", "delta"}),
                     describe(out));
}

UAPP_TEST("database.view: a multi-key sort breaks ties with the later keys") {
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    std::vector<Row> out;

    ViewConfig config;
    Sort by_priority;
    by_priority.property_id = fixture_id<PropertyId>(uapp::test::kPropPriority);
    config.sorts.push_back(by_priority);
    Sort by_title_desc;
    by_title_desc.property_id = fixture_id<PropertyId>(uapp::test::kPropTitle);
    by_title_desc.descending  = true;
    config.sorts.push_back(by_title_desc);

    UAPP_REQUIRE_OK(
        uapp::database::evaluate_table_view(db, config, rows, kNoLimit, out));
    // The two priority-3 rows are now ordered by title DESCENDING, so delta
    // comes before alpha -- the opposite of the id tie-break, which proves
    // the second key is actually consulted.
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"beta", "delta", "alpha",
                                                                 "gamma", "epsilon"}),
                     describe(out));
}

UAPP_TEST("database.view: filter and sort combine, and the limit truncates after sorting") {
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    std::vector<Row> out;

    ViewConfig config;
    Filter has_priority;
    has_priority.property_id = fixture_id<PropertyId>(uapp::test::kPropPriority);
    has_priority.op          = FilterOperator::IsNotEmpty;
    config.filters.push_back(has_priority);
    Sort by_priority_desc;
    by_priority_desc.property_id = fixture_id<PropertyId>(uapp::test::kPropPriority);
    by_priority_desc.descending  = true;
    config.sorts.push_back(by_priority_desc);

    UAPP_REQUIRE_OK(uapp::database::evaluate_table_view(db, config, rows, 2, out));
    // The limit must apply AFTER the sort: "the top two by priority" is
    // gamma(10) and then alpha(3), not the first two rows that passed.
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"gamma", "alpha"}),
                     describe(out));
}

UAPP_TEST("database.view: an empty config returns every row in id order") {
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    std::vector<Row> out;

    UAPP_REQUIRE_OK(
        uapp::database::evaluate_table_view(db, ViewConfig{}, rows, kNoLimit, out));
    // No sort keys at all still gives a TOTAL order, because the id tie-break
    // always applies -- and for a UUIDv7 id that is creation order.
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"alpha", "beta", "gamma",
                                                                 "delta", "epsilon"}),
                     describe(out));
}

UAPP_TEST("database.view: a schema-violating row is refused, not shown") {
    const Database db = uapp::test::task_database();
    std::vector<Row> rows = corpus();

    // Rows reach a view from a store, i.e. from a file, i.e. from an
    // untrusted input. A view must not be the thing that launders one.
    rows[2].values[fixture_id<PropertyId>(uapp::test::kPropStatus)] =
        SelectValue{fixture_id<OptionId>(999)};

    std::vector<Row> out;
    UAPP_REQUIRE_STATUS(
        uapp::database::evaluate_table_view(db, ViewConfig{}, rows, kNoLimit, out),
        Status::DatabaseOptionNotInSchema);
}
