// SPDX-License-Identifier: Apache-2.0
//
// The Table view: the legality table, the operator semantics and the
// reference evaluator. See view.h for the design argument.

#include "uapp/database/view.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uapp::database {
namespace {

/// The NOT-IMPLEMENTED table for view kinds. In code, not in a comment, so it
/// cannot drift from reality and a caller can show the reason.
const char* unimplemented_view_reason(ViewKind kind) noexcept {
    switch (kind) {
        case ViewKind::Board:
            return "NOT IMPLEMENTED: a Board is a Table GROUPED BY a Select property and "
                   "drawn as columns, so it needs grouping -- which spec section 15 lists "
                   "separately and which this run does not have -- plus drag-to-change-"
                   "group, i.e. a write path that mutates a property value as a side "
                   "effect of a layout gesture.";
        case ViewKind::Calendar:
            return "NOT IMPLEMENTED: a Calendar lays rows out on a month/week grid by a "
                   "Date property, which needs a calendar system and a timezone to decide "
                   "which DAY an instant belongs to. database::DateValue is deliberately a "
                   "bare UTC instant with no date-only/date-time distinction and no tz "
                   "database behind it (see its declaration), so there is nothing to place "
                   "a row on a day with.";
        case ViewKind::Timeline:
            return "NOT IMPLEMENTED: a Timeline needs a START and an END per row, i.e. "
                   "date RANGES, which database::DateValue explicitly does not model. "
                   "Faking the end from a duration property would make the view depend on "
                   "a schema convention rather than on a type.";
        case ViewKind::Gallery:
            return "NOT IMPLEMENTED: a Gallery renders a card per row with a cover image, "
                   "which requires the File property type -- itself blocked on the blob "
                   "store (database-design.md principle 6).";
        case ViewKind::List:
            return "NOT IMPLEMENTED: a List is a Table with every column but one hidden, "
                   "so it is entirely a column-visibility feature -- and column visibility "
                   "is per-viewer UI state needing the view-state store that does not "
                   "exist (same blocker as the Toggle block's collapsed state).";
        case ViewKind::Table:
            return nullptr;
    }
    return "NOT IMPLEMENTED: unrecognised view kind.";
}

// ---------------------------------------------------------------------------
// Operator semantics
// ---------------------------------------------------------------------------

/// Numeric comparison shared by Number and Date. `op` is already known to be
/// one of the six comparison operators.
bool compare_numeric(double lhs, double rhs, FilterOperator op) noexcept {
    switch (op) {
        case FilterOperator::Equals:         return lhs == rhs;
        case FilterOperator::LessThan:       return lhs < rhs;
        case FilterOperator::LessOrEqual:    return lhs <= rhs;
        case FilterOperator::GreaterThan:    return lhs > rhs;
        case FilterOperator::GreaterOrEqual: return lhs >= rhs;
        case FilterOperator::NotEquals:
        case FilterOperator::Contains:
        case FilterOperator::NotContains:
        case FilterOperator::IsEmpty:
        case FilterOperator::IsNotEmpty:
            return false;
    }
    return false;
}

/// The POSITIVE form of an operator: does `value` (which the row definitely
/// has) stand in relation `op` to `operand`?
///
/// NotEquals and NotContains never reach here -- filter_matches defines them
/// as the negation of Equals / Contains, which is what makes them total over
/// the unset case (see view.h).
bool positive_match(const PropertyDefinition& property,
                    FilterOperator op,
                    const PropertyValue& operand,
                    const PropertyValue& value) noexcept {
    switch (property.type) {
        case PropertyType::Text: {
            const auto* v = std::get_if<TextValue>(&value);
            const auto* o = std::get_if<TextValue>(&operand);
            if (v == nullptr || o == nullptr) { return false; }
            if (op == FilterOperator::Equals) { return v->text == o->text; }
            if (op == FilterOperator::Contains) {
                // Byte-exact substring. The empty operand is rejected by
                // validate_property_value, so this is never a vacuous match.
                return v->text.find(o->text) != std::string::npos;
            }
            return false;
        }
        case PropertyType::Number: {
            const auto* v = std::get_if<NumberValue>(&value);
            const auto* o = std::get_if<NumberValue>(&operand);
            if (v == nullptr || o == nullptr) { return false; }
            return compare_numeric(v->number, o->number, op);
        }
        case PropertyType::Checkbox: {
            const auto* v = std::get_if<CheckboxValue>(&value);
            const auto* o = std::get_if<CheckboxValue>(&operand);
            if (v == nullptr || o == nullptr) { return false; }
            // Ordering a Checkbox is meaningless as a FILTER even though it
            // has a well-defined SORT order: "checked > unchecked" is not a
            // question anyone asks, and offering it would mean two spellings
            // of "is checked". The legality table rejects the comparisons.
            return op == FilterOperator::Equals && v->checked == o->checked;
        }
        case PropertyType::Select: {
            const auto* v = std::get_if<SelectValue>(&value);
            const auto* o = std::get_if<SelectValue>(&operand);
            if (v == nullptr || o == nullptr) { return false; }
            return op == FilterOperator::Equals && v->option == o->option;
        }
        case PropertyType::MultiSelect: {
            const auto* v = std::get_if<MultiSelectValue>(&value);
            // The documented exception: the operand of `contains` on a
            // Multi-select is ONE option, not a set. See Filter::operand.
            const auto* o = std::get_if<SelectValue>(&operand);
            if (v == nullptr || o == nullptr) { return false; }
            if (op != FilterOperator::Contains) { return false; }
            for (const OptionId& selected : v->options) {
                if (selected == o->option) { return true; }
            }
            return false;
        }
        case PropertyType::Date: {
            const auto* v = std::get_if<DateValue>(&value);
            const auto* o = std::get_if<DateValue>(&operand);
            if (v == nullptr || o == nullptr) { return false; }
            // int64 milliseconds inside [kMinDateUnixMs, kMaxDateUnixMs] is
            // at most 2.5e14, far inside the 2^53 integers a double
            // represents exactly, so routing through compare_numeric is
            // lossless rather than convenient.
            return compare_numeric(static_cast<double>(v->unix_ms),
                                   static_cast<double>(o->unix_ms), op);
        }
        case PropertyType::Person:
        case PropertyType::Url:
        case PropertyType::Email:
        case PropertyType::File:
        case PropertyType::Relation:
        case PropertyType::Rollup:
        case PropertyType::Formula:
            // No value of these types can exist (there is no variant
            // alternative), and validate_property_definition rejects a
            // schema that declares one, so this is unreachable rather than a
            // silent "no match".
            return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Sort keys
// ---------------------------------------------------------------------------

/// One row's key for one sort property. `present == false` means the row
/// leaves the property unset, which sorts LAST in both directions.
struct SortKey final {
    bool        present = false;
    bool        textual = false;
    std::string text;
    double      number = 0.0;
};

SortKey sort_key_of(const PropertyDefinition& property, const Row& row) {
    SortKey key;
    const PropertyValue* value = find_value(row, property.id);
    if (value == nullptr) { return key; }

    switch (property.type) {
        case PropertyType::Text:
            if (const auto* v = std::get_if<TextValue>(value)) {
                key.present = true;
                key.textual = true;
                key.text    = v->text;
            }
            break;
        case PropertyType::Number:
            if (const auto* v = std::get_if<NumberValue>(value)) {
                key.present = true;
                key.number  = v->number;
            }
            break;
        case PropertyType::Checkbox:
            if (const auto* v = std::get_if<CheckboxValue>(value)) {
                key.present = true;
                key.number  = v->checked ? 1.0 : 0.0;
            }
            break;
        case PropertyType::Select:
            if (const auto* v = std::get_if<SelectValue>(value)) {
                // By the option's position in the SCHEMA's option order, not
                // by its name. Users mean "To do, In progress, Done", which
                // is the order they defined, not the alphabetical accident
                // "Done, In progress, To do".
                key.present = true;
                key.number  = static_cast<double>(option_order(property, v->option));
            }
            break;
        case PropertyType::MultiSelect:
            if (const auto* v = std::get_if<MultiSelectValue>(value)) {
                // By the EARLIEST selected option in schema order. Sorting a
                // set needs a representative and this is the only one that is
                // stable under the user reordering their own selection: the
                // alternative, "the first option the user picked", changes the
                // sort when they re-pick the same tags in another order.
                std::size_t best = property.options.size();
                for (const OptionId& selected : v->options) {
                    const std::size_t at = option_order(property, selected);
                    if (at < best) { best = at; }
                }
                key.present = true;
                key.number  = static_cast<double>(best);
            }
            break;
        case PropertyType::Date:
            if (const auto* v = std::get_if<DateValue>(value)) {
                key.present = true;
                key.number  = static_cast<double>(v->unix_ms);
            }
            break;
        case PropertyType::Person:
        case PropertyType::Url:
        case PropertyType::Email:
        case PropertyType::File:
        case PropertyType::Relation:
        case PropertyType::Rollup:
        case PropertyType::Formula:
            break;
    }
    return key;
}

/// -1 / 0 / +1, with the direction already applied and absent-last enforced
/// REGARDLESS of direction.
int compare_keys(const SortKey& a, const SortKey& b, bool descending) noexcept {
    if (!a.present && !b.present) { return 0; }
    if (!a.present) { return 1; }   // absent sorts last, ascending or not
    if (!b.present) { return -1; }

    int order = 0;
    if (a.textual || b.textual) {
        // std::string::compare is memcmp semantics over unsigned bytes,
        // which is exactly SQLite's BINARY collation -- that agreement is
        // what lets the SQL compiler in storage/sqlite reproduce this
        // ordering. Locale-aware (ICU) collation is NOT IMPLEMENTED and is
        // the same gap database-design.md section 3.9 records for the FTS
        // tokenizer.
        const int raw = a.text.compare(b.text);
        order = (raw < 0) ? -1 : (raw > 0 ? 1 : 0);
    } else {
        order = (a.number < b.number) ? -1 : (a.number > b.number ? 1 : 0);
    }
    return descending ? -order : order;
}

}  // namespace

// --- view kinds ------------------------------------------------------------

const char* view_kind_name(ViewKind kind) noexcept {
    switch (kind) {
        case ViewKind::Table:    return "table";
        case ViewKind::Board:    return "board";
        case ViewKind::Calendar: return "calendar";
        case ViewKind::Timeline: return "timeline";
        case ViewKind::Gallery:  return "gallery";
        case ViewKind::List:     return "list";
    }
    return "unknown";
}

bool view_kind_is_implemented(ViewKind kind) noexcept {
    const auto value = static_cast<std::int32_t>(kind);
    return value >= 0 && value < kFirstUnimplementedViewKind;
}

const char* view_kind_not_implemented_reason(ViewKind kind) noexcept {
    if (view_kind_is_implemented(kind)) { return nullptr; }
    return unimplemented_view_reason(kind);
}

// --- operators -------------------------------------------------------------

const char* filter_operator_name(FilterOperator op) noexcept {
    switch (op) {
        case FilterOperator::Equals:         return "equals";
        case FilterOperator::NotEquals:      return "not_equals";
        case FilterOperator::LessThan:       return "less_than";
        case FilterOperator::LessOrEqual:    return "less_or_equal";
        case FilterOperator::GreaterThan:    return "greater_than";
        case FilterOperator::GreaterOrEqual: return "greater_or_equal";
        case FilterOperator::Contains:       return "contains";
        case FilterOperator::NotContains:    return "not_contains";
        case FilterOperator::IsEmpty:        return "is_empty";
        case FilterOperator::IsNotEmpty:     return "is_not_empty";
    }
    return "unknown";
}

bool filter_operator_takes_operand(FilterOperator op) noexcept {
    return op != FilterOperator::IsEmpty && op != FilterOperator::IsNotEmpty;
}

bool filter_operator_is_legal(PropertyType type, FilterOperator op) noexcept {
    if (!property_type_is_implemented(type)) { return false; }

    // Every implemented type supports the two presence operators. They are
    // the only way to ask about the unset state, which every property has.
    if (op == FilterOperator::IsEmpty || op == FilterOperator::IsNotEmpty) {
        return true;
    }

    switch (type) {
        case PropertyType::Text:
            return op == FilterOperator::Equals || op == FilterOperator::NotEquals ||
                   op == FilterOperator::Contains || op == FilterOperator::NotContains;
        case PropertyType::Number:
        case PropertyType::Date:
            // The six comparisons. For a Date they are shown as
            // on / is not on / before / on or before / after / on or after.
            return op == FilterOperator::Equals || op == FilterOperator::NotEquals ||
                   op == FilterOperator::LessThan || op == FilterOperator::LessOrEqual ||
                   op == FilterOperator::GreaterThan ||
                   op == FilterOperator::GreaterOrEqual;
        case PropertyType::Checkbox:
        case PropertyType::Select:
            // `is` / `is not` only. Ordering a Select by its option order is
            // meaningful for SORTING and not for FILTERING -- "status < Done"
            // depends on a schema ordering the user never asked to be
            // comparable, and would silently change meaning when they
            // reorder their own options.
            return op == FilterOperator::Equals || op == FilterOperator::NotEquals;
        case PropertyType::MultiSelect:
            // Membership only. Equals on a set would mean exact set equality,
            // which is a different question and is NOT IMPLEMENTED
            // (Filter::operand records why).
            return op == FilterOperator::Contains || op == FilterOperator::NotContains;
        case PropertyType::Person:
        case PropertyType::Url:
        case PropertyType::Email:
        case PropertyType::File:
        case PropertyType::Relation:
        case PropertyType::Rollup:
        case PropertyType::Formula:
            return false;
    }
    return false;
}

const char* filter_operator_label(PropertyType type, FilterOperator op) noexcept {
    if (!filter_operator_is_legal(type, op)) { return nullptr; }

    if (op == FilterOperator::IsEmpty)    { return "is empty"; }
    if (op == FilterOperator::IsNotEmpty) { return "is not empty"; }

    switch (type) {
        case PropertyType::Number:
            switch (op) {
                case FilterOperator::Equals:         return "=";
                case FilterOperator::NotEquals:      return "!=";
                case FilterOperator::LessThan:       return "<";
                case FilterOperator::LessOrEqual:    return "<=";
                case FilterOperator::GreaterThan:    return ">";
                case FilterOperator::GreaterOrEqual: return ">=";
                default:                             return nullptr;
            }
        case PropertyType::Date:
            switch (op) {
                case FilterOperator::Equals:         return "on";
                case FilterOperator::NotEquals:      return "is not on";
                case FilterOperator::LessThan:       return "before";
                case FilterOperator::LessOrEqual:    return "on or before";
                case FilterOperator::GreaterThan:    return "after";
                case FilterOperator::GreaterOrEqual: return "on or after";
                default:                             return nullptr;
            }
        case PropertyType::Text:
            switch (op) {
                case FilterOperator::Equals:      return "is";
                case FilterOperator::NotEquals:   return "is not";
                case FilterOperator::Contains:    return "contains";
                case FilterOperator::NotContains: return "does not contain";
                default:                          return nullptr;
            }
        case PropertyType::MultiSelect:
            switch (op) {
                case FilterOperator::Contains:    return "contains";
                case FilterOperator::NotContains: return "does not contain";
                default:                          return nullptr;
            }
        case PropertyType::Checkbox:
        case PropertyType::Select:
            switch (op) {
                case FilterOperator::Equals:    return "is";
                case FilterOperator::NotEquals: return "is not";
                default:                        return nullptr;
            }
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

// --- validation ------------------------------------------------------------

Status validate_view_name(std::string_view name) noexcept {
    return validate_name(name, kMaxViewNameBytes);
}

Status validate_view_config(const Database& db, const ViewConfig& config) noexcept {
    if (const Status s = validate_database_schema(db); !is_ok(s)) { return s; }

    if (config.filters.size() > kMaxFiltersPerView ||
        config.sorts.size() > kMaxSortsPerView) {
        return Status::DatabaseViewSpecTooComplex;
    }

    for (const Filter& filter : config.filters) {
        const PropertyDefinition* property = find_property(db, filter.property_id);
        if (property == nullptr) { return Status::DatabasePropertyNotInSchema; }

        const auto raw_op = static_cast<std::int32_t>(filter.op);
        if (raw_op < 0 || raw_op >= kFilterOperatorCount) {
            return Status::DatabaseFilterOperatorUnsupported;
        }
        if (!filter_operator_is_legal(property->type, filter.op)) {
            return Status::DatabaseFilterOperatorUnsupported;
        }

        const bool wants_operand = filter_operator_takes_operand(filter.op);
        if (wants_operand != filter.operand.has_value()) {
            // An operand on `is empty`, or a missing one on `=`. Not ignored:
            // a silently dropped operand is a filter that does not do what
            // the editor that built it displayed.
            return Status::DatabaseFilterOperatorUnsupported;
        }
        if (!wants_operand) { continue; }

        const PropertyValue& operand = *filter.operand;

        // The documented exception: `contains` on a Multi-select takes ONE
        // option, so its operand is a SelectValue even though the property is
        // a MultiSelect. Everywhere else the operand's type IS the property's.
        const bool membership_probe =
            property->type == PropertyType::MultiSelect &&
            (filter.op == FilterOperator::Contains ||
             filter.op == FilterOperator::NotContains);

        if (membership_probe) {
            const auto* option = std::get_if<SelectValue>(&operand);
            if (option == nullptr) { return Status::DatabaseValueTypeMismatch; }
            if (const Status s = validate_property_value(operand); !is_ok(s)) { return s; }
            if (find_option(*property, option->option) == nullptr) {
                return Status::DatabaseOptionNotInSchema;
            }
            continue;
        }

        // An operand is validated as a stored VALUE would be, so a filter
        // cannot smuggle a NaN, a 1 MiB string or an out-of-range date past
        // the limits every persisted value has to obey.
        if (const Status s = validate_value_for_property(*property, operand); !is_ok(s)) {
            return s;
        }
    }

    for (const Sort& sort : config.sorts) {
        const PropertyDefinition* property = find_property(db, sort.property_id);
        if (property == nullptr) { return Status::DatabasePropertyNotInSchema; }
        // Every implemented property type has a defined sort key (see
        // sort_key_of), so there is no per-type sort legality table to check
        // -- which is deliberate: "you cannot sort by this column" would be a
        // surprising limitation, and the Multi-select case that would justify
        // one is answered with a documented representative instead.
    }

    return Status::Ok;
}

Status validate_view(const Database& db, const View& view) noexcept {
    if (view.id.is_nil()) { return Status::IdMalformed; }
    if (view.database_id != db.id) { return Status::InvalidArgument; }
    if (const Status s = validate_view_name(view.name); !is_ok(s)) { return s; }

    const auto raw_kind = static_cast<std::int32_t>(view.kind);
    if (raw_kind < 0 || raw_kind >= kViewKindCount) {
        return Status::DatabaseViewKindUnsupported;
    }
    if (!view_kind_is_implemented(view.kind)) {
        return Status::DatabaseViewKindUnsupported;
    }

    return validate_view_config(db, view.config);
}

// --- evaluation ------------------------------------------------------------

bool filter_matches(const PropertyDefinition& property,
                    const Filter& filter,
                    const Row& row) noexcept {
    if (property.id != filter.property_id) {
        return false;  // precondition violation; see the declaration
    }

    const PropertyValue* value = find_value(row, filter.property_id);

    switch (filter.op) {
        case FilterOperator::IsEmpty:
            return value == nullptr;
        case FilterOperator::IsNotEmpty:
            return value != nullptr;
        case FilterOperator::NotEquals:
            if (!filter.operand.has_value()) { return false; }
            return value == nullptr ||
                   !positive_match(property, FilterOperator::Equals, *filter.operand,
                                   *value);
        case FilterOperator::NotContains:
            if (!filter.operand.has_value()) { return false; }
            return value == nullptr ||
                   !positive_match(property, FilterOperator::Contains, *filter.operand,
                                   *value);
        case FilterOperator::Equals:
        case FilterOperator::LessThan:
        case FilterOperator::LessOrEqual:
        case FilterOperator::GreaterThan:
        case FilterOperator::GreaterOrEqual:
        case FilterOperator::Contains:
            if (!filter.operand.has_value() || value == nullptr) { return false; }
            return positive_match(property, filter.op, *filter.operand, *value);
    }
    return false;
}

Status evaluate_table_view(const Database& db,
                           const ViewConfig& config,
                           const std::vector<Row>& rows,
                           std::size_t limit,
                           std::vector<Row>& out) {
    if (const Status s = validate_view_config(db, config); !is_ok(s)) { return s; }
    if (rows.size() > kMaxRowsPerDatabase) { return Status::DatabaseRowLimitExceeded; }

    // Resolve every referenced property ONCE. validate_view_config has
    // already proved each lookup succeeds, so a null here would be a bug in
    // that function rather than a caller error -- say so instead of silently
    // skipping the filter.
    std::vector<const PropertyDefinition*> filter_props;
    filter_props.reserve(config.filters.size());
    for (const Filter& filter : config.filters) {
        const PropertyDefinition* property = find_property(db, filter.property_id);
        if (property == nullptr) { return Status::Internal; }
        filter_props.push_back(property);
    }

    std::vector<const PropertyDefinition*> sort_props;
    sort_props.reserve(config.sorts.size());
    for (const Sort& sort : config.sorts) {
        const PropertyDefinition* property = find_property(db, sort.property_id);
        if (property == nullptr) { return Status::Internal; }
        sort_props.push_back(property);
    }

    std::vector<Row> selected;
    for (const Row& row : rows) {
        if (row.database_id != db.id) { return Status::InvalidArgument; }
        // Rows come from a store, i.e. from a file, i.e. from an untrusted
        // input (spec section 28). A view must not be the thing that
        // launders a schema-violating row into the UI.
        if (const Status s = validate_row(db, row); !is_ok(s)) { return s; }

        bool keep = true;
        for (std::size_t i = 0; i < config.filters.size() && keep; ++i) {
            keep = filter_matches(*filter_props[i], config.filters[i], row);
        }
        if (keep) { selected.push_back(row); }
    }

    // Precompute each surviving row's keys so the comparator does no lookups
    // and no allocation: a comparator that builds a std::string is O(n log n)
    // allocations, which dominates everything else here.
    std::vector<std::vector<SortKey>> keys;
    keys.resize(selected.size());
    for (std::size_t r = 0; r < selected.size(); ++r) {
        keys[r].reserve(sort_props.size());
        for (const PropertyDefinition* property : sort_props) {
            keys[r].push_back(sort_key_of(*property, selected[r]));
        }
    }

    // Sort an index permutation rather than the rows, so the keys stay
    // addressable by their original position.
    std::vector<std::size_t> order;
    order.reserve(selected.size());
    for (std::size_t i = 0; i < selected.size(); ++i) { order.push_back(i); }

    const std::vector<Sort>& sorts = config.sorts;
    std::stable_sort(order.begin(), order.end(),
                     [&keys, &sorts, &selected](std::size_t lhs, std::size_t rhs) {
                         for (std::size_t k = 0; k < sorts.size(); ++k) {
                             const int cmp =
                                 compare_keys(keys[lhs][k], keys[rhs][k],
                                              sorts[k].descending);
                             if (cmp != 0) { return cmp < 0; }
                         }
                         // FINAL TIE-BREAK: row id bytes. This is what makes
                         // the order TOTAL and implementation-independent, so
                         // the SQL compiler can reproduce it exactly; it is
                         // also creation order, because ids are UUIDv7
                         // (database-design.md section 2). Without it, two
                         // rows with equal keys come back in whatever order
                         // the storage engine happened to scan.
                         return selected[lhs].id.bytes() < selected[rhs].id.bytes();
                     });

    const std::size_t capped = (limit > kMaxRowsPerDatabase) ? kMaxRowsPerDatabase : limit;
    const std::size_t count  = (order.size() < capped) ? order.size() : capped;

    std::vector<Row> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(std::move(selected[order[i]]));
    }

    out = std::move(result);
    return Status::Ok;
}

}  // namespace uapp::database
