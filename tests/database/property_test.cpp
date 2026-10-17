// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the typed property system: the type/value agreement, the
// NOT-IMPLEMENTED table, schema validation and the validation of untrusted
// row values against a schema (spec section 38).

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
#include "uapp/status.h"

namespace {

using uapp::Status;
using uapp::database::CheckboxValue;
using uapp::database::Database;
using uapp::database::DateValue;
using uapp::database::MultiSelectValue;
using uapp::database::NumberValue;
using uapp::database::OptionId;
using uapp::database::PropertyDefinition;
using uapp::database::PropertyId;
using uapp::database::PropertyType;
using uapp::database::PropertyValue;
using uapp::database::Row;
using uapp::database::SelectValue;
using uapp::database::TextValue;
using uapp::test::fixture_id;

}  // namespace

// --- the type tag ----------------------------------------------------------

UAPP_TEST("database.property: the variant index is the persisted type tag") {
    // The whole encode/decode story depends on this: db_property.type stores
    // the integer, and the variant alternative at that index must be the
    // payload for it. The static_asserts in property.h enforce the mapping at
    // compile time; this case proves the runtime accessor agrees.
    UAPP_CHECK(uapp::database::property_type_of(PropertyValue(TextValue{"x"})) ==
               PropertyType::Text);
    UAPP_CHECK(uapp::database::property_type_of(PropertyValue(NumberValue{1.0})) ==
               PropertyType::Number);
    UAPP_CHECK(uapp::database::property_type_of(PropertyValue(CheckboxValue{true})) ==
               PropertyType::Checkbox);
    UAPP_CHECK(uapp::database::property_type_of(
                   PropertyValue(SelectValue{fixture_id<OptionId>(1)})) ==
               PropertyType::Select);
    UAPP_CHECK(uapp::database::property_type_of(PropertyValue(DateValue{0})) ==
               PropertyType::Date);

    MultiSelectValue multi;
    multi.options.push_back(fixture_id<OptionId>(1));
    UAPP_CHECK(uapp::database::property_type_of(PropertyValue(multi)) ==
               PropertyType::MultiSelect);
}

UAPP_TEST("database.property: every declared type has a stable name") {
    for (std::int32_t raw = 0; raw < uapp::database::kPropertyTypeCount; ++raw) {
        const auto type = static_cast<PropertyType>(raw);
        const char* name = uapp::database::property_type_name(type);
        UAPP_REQUIRE(name != nullptr);
        UAPP_CHECK_MSG(std::string(name) != "unknown",
                       std::string("type ") + std::to_string(raw) + " has no name");
    }
    // An integer outside the enum must NOT produce a plausible-looking name:
    // a hand-edited database file can contain one.
    UAPP_CHECK(std::string(uapp::database::property_type_name(
                   static_cast<PropertyType>(uapp::database::kPropertyTypeCount))) ==
               "unknown");
}

UAPP_TEST("database.property: the seven unimplemented types carry real reasons") {
    const PropertyType unimplemented[] = {
        PropertyType::Person,   PropertyType::Url,     PropertyType::Email,
        PropertyType::File,     PropertyType::Relation, PropertyType::Rollup,
        PropertyType::Formula,
    };
    for (const PropertyType type : unimplemented) {
        UAPP_CHECK_MSG(!uapp::database::property_type_is_implemented(type),
                       std::string(uapp::database::property_type_name(type)) +
                           " claims to be implemented");
        const char* reason = uapp::database::property_type_not_implemented_reason(type);
        UAPP_REQUIRE_MSG(reason != nullptr,
                         std::string("no reason for ") +
                             uapp::database::property_type_name(type));
        // The reason is the machine-readable form of the spec section 5
        // marker, so it must actually say so rather than be a placeholder.
        const std::string text(reason);
        UAPP_CHECK_MSG(text.find("NOT IMPLEMENTED") == 0,
                       std::string("reason for ") +
                           uapp::database::property_type_name(type) +
                           " does not start with the marker: " + text);
        UAPP_CHECK_MSG(text.size() > 80,
                       std::string("reason for ") +
                           uapp::database::property_type_name(type) +
                           " is too short to be a real reason");
    }

    // ... and the implemented ones have no reason, so a caller can use
    // nullptr as the "is this usable" test.
    const PropertyType implemented[] = {
        PropertyType::Text,   PropertyType::Number,      PropertyType::Checkbox,
        PropertyType::Select, PropertyType::MultiSelect, PropertyType::Date,
    };
    for (const PropertyType type : implemented) {
        UAPP_CHECK(uapp::database::property_type_is_implemented(type));
        UAPP_CHECK(uapp::database::property_type_not_implemented_reason(type) == nullptr);
    }
}

// --- value validation ------------------------------------------------------

UAPP_TEST("database.property: a value must satisfy its own type's rules") {
    UAPP_REQUIRE_OK(uapp::database::validate_property_value(TextValue{"ok"}));

    // Empty text is NOT a value: it would be a second spelling of "unset".
    UAPP_REQUIRE_STATUS(uapp::database::validate_property_value(TextValue{""}),
                        Status::DatabaseValueMalformed);

    // An embedded NUL truncates at every C boundary.
    UAPP_REQUIRE_STATUS(
        uapp::database::validate_property_value(TextValue{std::string("a\0b", 3)}),
        Status::DatabaseValueMalformed);

    // One byte over the cell cap.
    UAPP_REQUIRE_STATUS(
        uapp::database::validate_property_value(
            TextValue{std::string(uapp::database::kMaxPropertyTextBytes + 1, 'x')}),
        Status::DatabaseValueTooLarge);
    UAPP_REQUIRE_OK(uapp::database::validate_property_value(
        TextValue{std::string(uapp::database::kMaxPropertyTextBytes, 'x')}));
}

UAPP_TEST("database.property: a Number rejects NaN and the infinities") {
    // Not hygiene: NaN compares false against everything, so a NaN sort key
    // makes the ordering depend on the sort algorithm, and SQLite stores a
    // bound NaN as SQL NULL -- silently turning a present value absent.
    const double nan_value = std::numeric_limits<double>::quiet_NaN();
    const double inf_value = std::numeric_limits<double>::infinity();

    UAPP_REQUIRE_STATUS(uapp::database::validate_property_value(NumberValue{nan_value}),
                        Status::DatabaseValueMalformed);
    UAPP_REQUIRE_STATUS(uapp::database::validate_property_value(NumberValue{inf_value}),
                        Status::DatabaseValueMalformed);
    UAPP_REQUIRE_STATUS(uapp::database::validate_property_value(NumberValue{-inf_value}),
                        Status::DatabaseValueMalformed);

    UAPP_REQUIRE_OK(uapp::database::validate_property_value(NumberValue{0.0}));
    UAPP_REQUIRE_OK(uapp::database::validate_property_value(NumberValue{-1.5e300}));
}

UAPP_TEST("database.property: a Date must be inside the representable range") {
    UAPP_REQUIRE_OK(uapp::database::validate_property_value(
        DateValue{uapp::database::kMinDateUnixMs}));
    UAPP_REQUIRE_OK(uapp::database::validate_property_value(
        DateValue{uapp::database::kMaxDateUnixMs}));
    UAPP_REQUIRE_STATUS(uapp::database::validate_property_value(
                            DateValue{uapp::database::kMinDateUnixMs - 1}),
                        Status::DatabaseValueMalformed);
    UAPP_REQUIRE_STATUS(uapp::database::validate_property_value(
                            DateValue{uapp::database::kMaxDateUnixMs + 1}),
                        Status::DatabaseValueMalformed);
}

UAPP_TEST("database.property: a Multi-select rejects an empty or repeating set") {
    MultiSelectValue empty;
    UAPP_REQUIRE_STATUS(uapp::database::validate_property_value(empty),
                        Status::DatabaseValueMalformed);

    MultiSelectValue repeated;
    repeated.options.push_back(fixture_id<OptionId>(uapp::test::kOptUrgent));
    repeated.options.push_back(fixture_id<OptionId>(uapp::test::kOptUrgent));
    UAPP_REQUIRE_STATUS(uapp::database::validate_property_value(repeated),
                        Status::DatabaseValueMalformed);

    MultiSelectValue nil_option;
    nil_option.options.push_back(fixture_id<OptionId>(0));
    UAPP_REQUIRE_STATUS(uapp::database::validate_property_value(nil_option),
                        Status::DatabaseValueMalformed);

    MultiSelectValue fine;
    fine.options.push_back(fixture_id<OptionId>(uapp::test::kOptUrgent));
    fine.options.push_back(fixture_id<OptionId>(uapp::test::kOptDocs));
    UAPP_REQUIRE_OK(uapp::database::validate_property_value(fine));
}

// --- schema validation -----------------------------------------------------

UAPP_TEST("database.property: the canonical six-type schema is valid") {
    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(uapp::database::validate_database_schema(db));
    UAPP_CHECK(db.properties.size() == 6);
}

UAPP_TEST("database.property: a Select must define options and a Number must not") {
    PropertyDefinition empty_select;
    empty_select.id   = fixture_id<PropertyId>(1);
    empty_select.name = "Status";
    empty_select.type = PropertyType::Select;
    // A Select with no options can hold no legal value, so it is not a usable
    // column -- it is a half-created one.
    UAPP_REQUIRE_STATUS(uapp::database::validate_property_definition(empty_select),
                        Status::DatabaseSchemaMalformed);

    PropertyDefinition number_with_options = uapp::test::number_property();
    number_with_options.options.push_back({fixture_id<OptionId>(99), "nope"});
    // Options on a Number are not harmless: they would be persisted, synced
    // and shown in a property editor as though they meant something.
    UAPP_REQUIRE_STATUS(uapp::database::validate_property_definition(number_with_options),
                        Status::DatabaseSchemaMalformed);
}

UAPP_TEST("database.property: a declared-but-unimplemented type cannot be a column") {
    PropertyDefinition relation;
    relation.id   = fixture_id<PropertyId>(1);
    relation.name = "Project";
    relation.type = PropertyType::Relation;
    UAPP_REQUIRE_STATUS(uapp::database::validate_property_definition(relation),
                        Status::DatabasePropertyTypeUnsupported);

    PropertyDefinition formula;
    formula.id   = fixture_id<PropertyId>(2);
    formula.name = "Total";
    formula.type = PropertyType::Formula;
    UAPP_REQUIRE_STATUS(uapp::database::validate_property_definition(formula),
                        Status::DatabasePropertyTypeUnsupported);
}

UAPP_TEST("database.property: duplicate property ids, names and option ids are rejected") {
    {
        Database db = uapp::test::task_database();
        db.properties.push_back(uapp::test::text_property());  // same id AND name
        UAPP_REQUIRE_STATUS(uapp::database::validate_database_schema(db),
                            Status::DatabasePropertyDuplicated);
    }
    {
        Database db = uapp::test::task_database();
        PropertyDefinition clone = uapp::test::text_property();
        clone.id = fixture_id<PropertyId>(900);  // different id, same name
        db.properties.push_back(clone);
        // Two columns called "Title" cannot be told apart in a filter editor,
        // a header row or an export.
        UAPP_REQUIRE_STATUS(uapp::database::validate_database_schema(db),
                            Status::DatabasePropertyDuplicated);
    }
    {
        Database db = uapp::test::task_database();
        PropertyDefinition second_select;
        second_select.id   = fixture_id<PropertyId>(901);
        second_select.name = "Phase";
        second_select.type = PropertyType::Select;
        // Reusing an OPTION id from the Status property: a stored Select
        // value points at an option id, so the same id under two properties
        // would make that value ambiguous.
        second_select.options.push_back(
            {fixture_id<OptionId>(uapp::test::kOptToDo), "Early"});
        db.properties.push_back(second_select);
        UAPP_REQUIRE_STATUS(uapp::database::validate_database_schema(db),
                            Status::DatabasePropertyDuplicated);
    }
    {
        // Two DIFFERENT properties MAY share an option NAME -- "Done" in both
        // Status and Phase is a normal schema.
        Database db = uapp::test::task_database();
        PropertyDefinition phase;
        phase.id   = fixture_id<PropertyId>(902);
        phase.name = "Phase";
        phase.type = PropertyType::Select;
        phase.options.push_back({fixture_id<OptionId>(910), "Done"});
        db.properties.push_back(phase);
        UAPP_REQUIRE_OK(uapp::database::validate_database_schema(db));
    }
}

UAPP_TEST("database.bounds: schema limits are enforced, one over and one at the cap") {
    {
        Database db = uapp::test::task_database();
        db.properties.clear();
        for (std::size_t i = 0; i < uapp::database::kMaxPropertiesPerDatabase; ++i) {
            PropertyDefinition p;
            p.id   = fixture_id<PropertyId>(1000 + i);
            p.name = "p" + std::to_string(i);
            p.type = PropertyType::Text;
            db.properties.push_back(std::move(p));
        }
        UAPP_REQUIRE_OK(uapp::database::validate_database_schema(db));

        PropertyDefinition one_too_many;
        one_too_many.id   = fixture_id<PropertyId>(9999);
        one_too_many.name = "over";
        one_too_many.type = PropertyType::Text;
        db.properties.push_back(std::move(one_too_many));
        UAPP_REQUIRE_STATUS(uapp::database::validate_database_schema(db),
                            Status::DatabasePropertyLimitExceeded);
    }
    {
        PropertyDefinition select = uapp::test::select_property();
        select.options.clear();
        for (std::size_t i = 0; i < uapp::database::kMaxSelectOptionsPerProperty; ++i) {
            select.options.push_back({fixture_id<OptionId>(2000 + i), "o" + std::to_string(i)});
        }
        UAPP_REQUIRE_OK(uapp::database::validate_property_definition(select));

        select.options.push_back({fixture_id<OptionId>(90000), "over"});
        UAPP_REQUIRE_STATUS(uapp::database::validate_property_definition(select),
                            Status::DatabaseOptionLimitExceeded);
    }
    {
        Database db = uapp::test::task_database();
        db.name = std::string(uapp::database::kMaxDatabaseNameBytes, 'n');
        UAPP_REQUIRE_OK(uapp::database::validate_database_schema(db));
        db.name.push_back('n');
        UAPP_REQUIRE_STATUS(uapp::database::validate_database_schema(db),
                            Status::DatabaseNameTooLong);
    }
    {
        PropertyDefinition p = uapp::test::text_property();
        p.name = std::string(uapp::database::kMaxPropertyNameBytes, 'n');
        UAPP_REQUIRE_OK(uapp::database::validate_property_definition(p));
        p.name.push_back('n');
        UAPP_REQUIRE_STATUS(uapp::database::validate_property_definition(p),
                            Status::DatabaseNameTooLong);
    }
}

UAPP_TEST("database.property: a database with no properties and no page is handled") {
    Database db = uapp::test::task_database();
    db.properties.clear();
    // Zero columns is a legal, common state: it is what every database starts
    // in. Rejecting it would mean the only way to create one is complete.
    UAPP_REQUIRE_OK(uapp::database::validate_database_schema(db));

    // A database with no host page is unreachable in the navigation model.
    db.page_id = fixture_id<uapp::blocks::PageId>(0);
    UAPP_REQUIRE_STATUS(uapp::database::validate_database_schema(db), Status::IdMalformed);
}

// --- the row/schema contract -----------------------------------------------

UAPP_TEST("database.property: a conforming row validates against its schema") {
    const Database db = uapp::test::task_database();
    const Row row = uapp::test::RowBuilder(100)
                        .title("Write the ADR")
                        .priority(3.0)
                        .done(false)
                        .status(uapp::test::kOptInProgress)
                        .tags({uapp::test::kOptUrgent, uapp::test::kOptDocs})
                        .due(5)
                        .build();
    UAPP_REQUIRE_OK(uapp::database::validate_row(db, row));

    // A sparse row is just as valid: unset is a real state.
    const Row sparse = uapp::test::RowBuilder(101).title("Only a title").build();
    UAPP_REQUIRE_OK(uapp::database::validate_row(db, sparse));
    UAPP_CHECK(sparse.values.size() == 1);
}

UAPP_TEST("database.property: a type-mismatched value is rejected, not coerced") {
    const Database db = uapp::test::task_database();

    // A string into a Number property. This is THE case the whole typed
    // design exists for: "10" must not become 10, and it must not sort
    // before "9" either.
    Row wrong = uapp::test::RowBuilder(100).title("x").build();
    wrong.values[fixture_id<PropertyId>(uapp::test::kPropPriority)] = TextValue{"10"};
    UAPP_REQUIRE_STATUS(uapp::database::validate_row(db, wrong),
                        Status::DatabaseValueTypeMismatch);

    // A Number into a Date property: both are scalars, both would even share
    // the v_int column family, and it is still a different type.
    Row date_as_number = uapp::test::RowBuilder(101).build();
    date_as_number.values[fixture_id<PropertyId>(uapp::test::kPropDue)] =
        NumberValue{1767225600000.0};
    UAPP_REQUIRE_STATUS(uapp::database::validate_row(db, date_as_number),
                        Status::DatabaseValueTypeMismatch);

    // A single Select where the column is a Multi-select.
    Row select_into_multi = uapp::test::RowBuilder(102).build();
    select_into_multi.values[fixture_id<PropertyId>(uapp::test::kPropTags)] =
        SelectValue{fixture_id<OptionId>(uapp::test::kOptUrgent)};
    UAPP_REQUIRE_STATUS(uapp::database::validate_row(db, select_into_multi),
                        Status::DatabaseValueTypeMismatch);
}

UAPP_TEST("database.property: an out-of-schema option is rejected for Select and Multi") {
    const Database db = uapp::test::task_database();

    // A well-formed SelectValue holding 16 bytes that name no option of this
    // property. A type tag alone cannot catch this -- only the definition
    // knows which options the column offers.
    Row bad_select = uapp::test::RowBuilder(100).build();
    bad_select.values[fixture_id<PropertyId>(uapp::test::kPropStatus)] =
        SelectValue{fixture_id<OptionId>(777)};
    UAPP_REQUIRE_STATUS(uapp::database::validate_row(db, bad_select),
                        Status::DatabaseOptionNotInSchema);

    // An option that belongs to ANOTHER property of the same database is
    // still out of schema for this one.
    Row foreign_option = uapp::test::RowBuilder(101).build();
    foreign_option.values[fixture_id<PropertyId>(uapp::test::kPropStatus)] =
        SelectValue{fixture_id<OptionId>(uapp::test::kOptUrgent)};
    UAPP_REQUIRE_STATUS(uapp::database::validate_row(db, foreign_option),
                        Status::DatabaseOptionNotInSchema);

    // One bad member poisons the whole Multi-select value.
    MultiSelectValue mixed;
    mixed.options.push_back(fixture_id<OptionId>(uapp::test::kOptUrgent));
    mixed.options.push_back(fixture_id<OptionId>(778));
    Row bad_multi = uapp::test::RowBuilder(102).build();
    bad_multi.values[fixture_id<PropertyId>(uapp::test::kPropTags)] = mixed;
    UAPP_REQUIRE_STATUS(uapp::database::validate_row(db, bad_multi),
                        Status::DatabaseOptionNotInSchema);
}

UAPP_TEST("database.property: a row cannot invent a column or claim another database") {
    const Database db = uapp::test::task_database();

    Row invented = uapp::test::RowBuilder(100).build();
    invented.values[fixture_id<PropertyId>(555)] = TextValue{"ghost"};
    UAPP_REQUIRE_STATUS(uapp::database::validate_row(db, invented),
                        Status::DatabasePropertyNotInSchema);

    Row foreign = uapp::test::RowBuilder(101).title("x").build();
    foreign.database_id = fixture_id<uapp::database::DatabaseId>(4242);
    UAPP_REQUIRE_STATUS(uapp::database::validate_row(db, foreign), Status::InvalidArgument);

    Row nil = uapp::test::RowBuilder(0).title("x").build();
    UAPP_REQUIRE_STATUS(uapp::database::validate_row(db, nil), Status::IdMalformed);
}
