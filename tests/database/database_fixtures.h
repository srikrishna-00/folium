// SPDX-License-Identifier: Apache-2.0
//
// tests/database/database_fixtures.h — the canonical schema every
// core/database test is written against.
//
// ONE schema, covering ALL SIX implemented property types, shared by the pure
// in-memory suites and the SQLite suite. The sharing is the point: the
// differential cases in database_store_test.cpp compare the SQL compiler
// against database::evaluate_table_view over the SAME schema and the SAME
// rows, which is only a meaningful comparison if "the same" is one definition
// rather than two that drifted.
//
// Every id is a deterministic small integer in the low bytes, so a failure
// message naming an id is readable and so two runs produce the same bytes.

#ifndef UAPP_TESTS_DATABASE_DATABASE_FIXTURES_H
#define UAPP_TESTS_DATABASE_DATABASE_FIXTURES_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "uapp/database/database.h"
#include "uapp/database/property.h"
#include "uapp/database/view.h"
#include "uapp/object_id.h"

namespace uapp::test {

/// A deterministic id: `n` big-endian in the low eight bytes. Never nil for
/// n > 0, which every validator requires.
template <class IdType>
[[nodiscard]] inline IdType fixture_id(std::uint64_t n) {
    uapp::ObjectIdBytes raw{};
    for (std::size_t i = 0; i < 8; ++i) {
        raw[15 - i] = static_cast<std::uint8_t>((n >> (i * 8)) & 0xFFu);
    }
    return IdType(raw);
}

// --- the canonical ids -----------------------------------------------------
// Option ids are globally unique across properties on purpose: a stored
// Select value points at an option id, so the same id under two properties
// would make that value ambiguous, and validate_database_schema rejects it.

inline constexpr std::uint64_t kFixturePage     = 1;
inline constexpr std::uint64_t kFixtureDatabase = 2;

inline constexpr std::uint64_t kPropTitle    = 10;  // Text
inline constexpr std::uint64_t kPropPriority = 11;  // Number
inline constexpr std::uint64_t kPropDone     = 12;  // Checkbox
inline constexpr std::uint64_t kPropStatus   = 13;  // Select
inline constexpr std::uint64_t kPropTags     = 14;  // MultiSelect
inline constexpr std::uint64_t kPropDue      = 15;  // Date

inline constexpr std::uint64_t kOptToDo       = 20;
inline constexpr std::uint64_t kOptInProgress = 21;
inline constexpr std::uint64_t kOptDone       = 22;

inline constexpr std::uint64_t kOptUrgent  = 30;
inline constexpr std::uint64_t kOptBackend = 31;
inline constexpr std::uint64_t kOptDocs    = 32;

/// A date in 2026, as an instant in UTC. `day` is 1-based within January, so
/// the fixtures can say "the 5th" and mean a value the Date filters order.
[[nodiscard]] inline std::int64_t fixture_date(int day) {
    // 2026-01-01T00:00:00Z == 1767225600000 ms.
    constexpr std::int64_t kJan1 = 1767225600000LL;
    constexpr std::int64_t kDay  = 86400000LL;
    return kJan1 + static_cast<std::int64_t>(day - 1) * kDay;
}

[[nodiscard]] inline database::PropertyDefinition text_property() {
    database::PropertyDefinition p;
    p.id   = fixture_id<database::PropertyId>(kPropTitle);
    p.name = "Title";
    p.type = database::PropertyType::Text;
    return p;
}

[[nodiscard]] inline database::PropertyDefinition number_property() {
    database::PropertyDefinition p;
    p.id   = fixture_id<database::PropertyId>(kPropPriority);
    p.name = "Priority";
    p.type = database::PropertyType::Number;
    return p;
}

[[nodiscard]] inline database::PropertyDefinition checkbox_property() {
    database::PropertyDefinition p;
    p.id   = fixture_id<database::PropertyId>(kPropDone);
    p.name = "Done";
    p.type = database::PropertyType::Checkbox;
    return p;
}

/// Options are in a MEANINGFUL order, not an alphabetical one. Sorting by a
/// Select sorts by THIS order, so a fixture whose options happened to be
/// alphabetical could not tell the two apart.
[[nodiscard]] inline database::PropertyDefinition select_property() {
    database::PropertyDefinition p;
    p.id   = fixture_id<database::PropertyId>(kPropStatus);
    p.name = "Status";
    p.type = database::PropertyType::Select;
    p.options.push_back({fixture_id<database::OptionId>(kOptToDo), "To do"});
    p.options.push_back({fixture_id<database::OptionId>(kOptInProgress), "In progress"});
    p.options.push_back({fixture_id<database::OptionId>(kOptDone), "Done"});
    return p;
}

[[nodiscard]] inline database::PropertyDefinition multi_select_property() {
    database::PropertyDefinition p;
    p.id   = fixture_id<database::PropertyId>(kPropTags);
    p.name = "Tags";
    p.type = database::PropertyType::MultiSelect;
    p.options.push_back({fixture_id<database::OptionId>(kOptUrgent), "urgent"});
    p.options.push_back({fixture_id<database::OptionId>(kOptBackend), "backend"});
    p.options.push_back({fixture_id<database::OptionId>(kOptDocs), "docs"});
    return p;
}

[[nodiscard]] inline database::PropertyDefinition date_property() {
    database::PropertyDefinition p;
    p.id   = fixture_id<database::PropertyId>(kPropDue);
    p.name = "Due";
    p.type = database::PropertyType::Date;
    return p;
}

/// The canonical six-property database.
[[nodiscard]] inline database::Database task_database() {
    database::Database db;
    db.id             = fixture_id<database::DatabaseId>(kFixtureDatabase);
    db.page_id        = fixture_id<blocks::PageId>(kFixturePage);
    db.name           = "Tasks";
    db.created_at_ms  = 1'700'000'000'000;
    db.modified_at_ms = 1'700'000'000'000;
    db.properties.push_back(text_property());
    db.properties.push_back(number_property());
    db.properties.push_back(checkbox_property());
    db.properties.push_back(select_property());
    db.properties.push_back(multi_select_property());
    db.properties.push_back(date_property());
    return db;
}

/// A row builder: start empty, set the properties the case cares about.
/// A property left unset STAYS unset -- which is a distinct state from every
/// value it could hold, and is what the `is empty` operator tests.
class RowBuilder final {
public:
    explicit RowBuilder(std::uint64_t n) {
        row_.id             = fixture_id<database::RowId>(n);
        row_.database_id    = fixture_id<database::DatabaseId>(kFixtureDatabase);
        row_.created_at_ms  = 1'700'000'000'000;
        row_.modified_at_ms = 1'700'000'000'000;
    }

    RowBuilder& title(const char* value) {
        return set(kPropTitle, database::TextValue{std::string(value)});
    }
    RowBuilder& priority(double value) {
        return set(kPropPriority, database::NumberValue{value});
    }
    RowBuilder& done(bool value) {
        return set(kPropDone, database::CheckboxValue{value});
    }
    RowBuilder& status(std::uint64_t option) {
        return set(kPropStatus,
                   database::SelectValue{fixture_id<database::OptionId>(option)});
    }
    RowBuilder& tags(const std::vector<std::uint64_t>& options) {
        database::MultiSelectValue value;
        for (const std::uint64_t option : options) {
            value.options.push_back(fixture_id<database::OptionId>(option));
        }
        return set(kPropTags, value);
    }
    RowBuilder& due(int day) {
        return set(kPropDue, database::DateValue{fixture_date(day)});
    }

    RowBuilder& set(std::uint64_t property, const database::PropertyValue& value) {
        row_.values[fixture_id<database::PropertyId>(property)] = value;
        return *this;
    }

    [[nodiscard]] database::Row build() const { return row_; }

private:
    database::Row row_;
};

/// A one-filter config, for the many cases that test exactly one operator.
[[nodiscard]] inline database::ViewConfig filter_config(
    std::uint64_t property,
    database::FilterOperator op,
    const database::PropertyValue* operand) {
    database::ViewConfig config;
    database::Filter filter;
    filter.property_id = fixture_id<database::PropertyId>(property);
    filter.op          = op;
    if (operand != nullptr) { filter.operand = *operand; }
    config.filters.push_back(std::move(filter));
    return config;
}

[[nodiscard]] inline database::ViewConfig sort_config(std::uint64_t property,
                                                      bool descending) {
    database::ViewConfig config;
    database::Sort sort;
    sort.property_id = fixture_id<database::PropertyId>(property);
    sort.descending  = descending;
    config.sorts.push_back(sort);
    return config;
}

/// The titles of a result set, in order. Makes an ordering assertion one
/// comparison instead of a loop of index arithmetic.
[[nodiscard]] inline std::vector<std::string> titles_of(
    const std::vector<database::Row>& rows) {
    std::vector<std::string> out;
    out.reserve(rows.size());
    for (const database::Row& row : rows) {
        const database::PropertyValue* value =
            database::find_value(row, fixture_id<database::PropertyId>(kPropTitle));
        if (value == nullptr) {
            out.emplace_back("<unset>");
            continue;
        }
        const auto* text = std::get_if<database::TextValue>(value);
        out.emplace_back(text != nullptr ? text->text : "<wrong type>");
    }
    return out;
}

/// A printable form of a result set, for a failure message that shows WHAT
/// came back rather than only that it was wrong.
[[nodiscard]] inline std::string describe(const std::vector<database::Row>& rows) {
    std::string out = "[";
    const std::vector<std::string> names = titles_of(rows);
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) { out += ", "; }
        out += names[i];
    }
    out += "]";
    return out;
}

}  // namespace uapp::test

#endif  // UAPP_TESTS_DATABASE_DATABASE_FIXTURES_H
