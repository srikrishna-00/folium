// SPDX-License-Identifier: Apache-2.0
#include "uapp/database/property.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>

namespace uapp::database {
namespace {

/// The NOT-IMPLEMENTED table. One row per spec section 15 property type we
/// declare but do not build, with the reason. Keeping this in code rather
/// than in a comment means `property_type_not_implemented_reason` cannot
/// drift from reality, and a caller (CLI, future UI, future C ABI) can show
/// the reason instead of a generic failure.
const char* unimplemented_reason(PropertyType type) noexcept {
    switch (type) {
        case PropertyType::Person:
            return "NOT IMPLEMENTED: a Person value references an identity_account row "
                   "(database-design.md section 3.1), and there is no identity namespace, "
                   "no account table and no membership model in this build. Storing a "
                   "display name instead would be a stringly-typed value that goes stale "
                   "the moment anyone is renamed -- exactly what this property system "
                   "exists to prevent.";
        case PropertyType::Url:
        case PropertyType::Email:
            return "NOT IMPLEMENTED: both are a validated-text type, and the validator is "
                   "the entire feature. A URL property that accepts any string is a Text "
                   "property with a misleading name, and one that accepts javascript: or "
                   "data: URLs is an injection surface the moment anything renders it as a "
                   "link (security-model.md URL allow-listing, spec section 38). Needs the "
                   "same scheme allow-list the Embed/Bookmark block types are waiting on.";
        case PropertyType::File:
            return "NOT IMPLEMENTED: a File value references a blob_ref row by content "
                   "hash; database-design.md principle 6 forbids putting the bytes in the "
                   "database, and the blob store does not exist yet. Same blocker as the "
                   "Image/Video/Audio/File block types.";
        case PropertyType::Relation:
            return "NOT IMPLEMENTED: a Relation points at rows of ANOTHER database, so it "
                   "needs cross-database referential integrity (what happens to the "
                   "pointing row when the target database is deleted), a two-sided "
                   "synthetic back-reference property, and cycle detection across "
                   "databases. None of those is a schema column; all of them are "
                   "behaviour this run does not have.";
        case PropertyType::Rollup:
            return "NOT IMPLEMENTED: a Rollup aggregates a property of the rows a Relation "
                   "points at, so it cannot exist before Relation does. It is also the "
                   "first DERIVED property type, which means it needs an invalidation "
                   "story (recompute when a related row changes) that no property here "
                   "requires -- every implemented type is stored, not computed.";
        case PropertyType::Formula:
            return "NOT IMPLEMENTED: a Formula needs an expression language -- parser, "
                   "type checker, evaluator -- over the other properties, plus a bound on "
                   "evaluation and a dependency graph to detect formula-references-formula "
                   "cycles. A user-authored expression evaluated on load is untrusted-input "
                   "execution (spec section 38) and deserves its own fuzz target before it "
                   "is trusted with a synced file.";
        case PropertyType::Text:
        case PropertyType::Number:
        case PropertyType::Checkbox:
        case PropertyType::Select:
        case PropertyType::MultiSelect:
        case PropertyType::Date:
            return nullptr;
    }
    return "NOT IMPLEMENTED: unrecognised property type.";
}

Status validate_text_value(const TextValue& value) noexcept {
    if (value.text.empty()) {
        // See TextValue's declaration: an empty string would be a second
        // spelling of "unset", and then `is empty` has two answers.
        return Status::DatabaseValueMalformed;
    }
    if (value.text.size() > kMaxPropertyTextBytes) {
        return Status::DatabaseValueTooLarge;
    }
    // An embedded NUL truncates the value the moment it crosses any C
    // boundary (sqlite3_bind_text, printf, the future C ABI). Reject once,
    // here, rather than discover a silently shortened cell later.
    if (value.text.find('\0') != std::string::npos) {
        return Status::DatabaseValueMalformed;
    }
    return Status::Ok;
}

Status validate_multi_select_value(const MultiSelectValue& value) noexcept {
    if (value.options.empty()) {
        return Status::DatabaseValueMalformed;
    }
    if (value.options.size() > kMaxSelectOptionsPerProperty) {
        // A row cannot select more options than the property can define, so
        // the option-set cap is the row cap too; a longer list is either a
        // duplicate-laden request or a corrupt file.
        return Status::DatabaseOptionLimitExceeded;
    }
    for (std::size_t i = 0; i < value.options.size(); ++i) {
        if (value.options[i].is_nil()) { return Status::DatabaseValueMalformed; }
        for (std::size_t j = i + 1; j < value.options.size(); ++j) {
            if (value.options[i] == value.options[j]) {
                // Selecting one option twice is not a legal state: it has no
                // meaning the UI could show and it would make the stored
                // membership rows collide on their primary key.
                return Status::DatabaseValueMalformed;
            }
        }
    }
    // O(n^2) over at most 256 entries, deliberately: a set would allocate on
    // every validation, and 256*256/2 pointer-free comparisons is cheaper
    // than one heap allocation on the write path.
    return Status::Ok;
}

}  // namespace

PropertyType property_type_of(const PropertyValue& value) noexcept {
    // The variant index IS the type tag; the static_asserts in the header
    // guarantee they agree. valueless_by_exception() cannot happen under
    // -fno-exceptions, but index() would return variant_npos if it somehow
    // did, so clamp rather than cast garbage into the enum.
    const std::size_t index = value.index();
    if (index >= std::variant_size_v<PropertyValue>) {
        return PropertyType::Text;
    }
    return static_cast<PropertyType>(static_cast<std::int32_t>(index));
}

const char* property_type_name(PropertyType type) noexcept {
    switch (type) {
        case PropertyType::Text:        return "text";
        case PropertyType::Number:      return "number";
        case PropertyType::Checkbox:    return "checkbox";
        case PropertyType::Select:      return "select";
        case PropertyType::MultiSelect: return "multi_select";
        case PropertyType::Date:        return "date";
        case PropertyType::Person:      return "person";
        case PropertyType::Url:         return "url";
        case PropertyType::Email:       return "email";
        case PropertyType::File:        return "file";
        case PropertyType::Relation:    return "relation";
        case PropertyType::Rollup:      return "rollup";
        case PropertyType::Formula:     return "formula";
    }
    return "unknown";
}

bool property_type_is_implemented(PropertyType type) noexcept {
    const auto value = static_cast<std::int32_t>(type);
    return value >= 0 && value < kFirstUnimplementedPropertyType;
}

const char* property_type_not_implemented_reason(PropertyType type) noexcept {
    if (property_type_is_implemented(type)) { return nullptr; }
    return unimplemented_reason(type);
}

bool property_type_has_options(PropertyType type) noexcept {
    return type == PropertyType::Select || type == PropertyType::MultiSelect;
}

const SelectOption* find_option(const PropertyDefinition& property,
                                const OptionId& id) noexcept {
    for (const SelectOption& option : property.options) {
        if (option.id == id) { return &option; }
    }
    return nullptr;
}

std::size_t option_order(const PropertyDefinition& property, const OptionId& id) noexcept {
    for (std::size_t i = 0; i < property.options.size(); ++i) {
        if (property.options[i].id == id) { return i; }
    }
    return property.options.size();
}

Status validate_name(std::string_view name, std::size_t limit) noexcept {
    if (name.size() > limit) { return Status::DatabaseNameTooLong; }
    if (name.find('\0') != std::string_view::npos) {
        return Status::DatabaseValueMalformed;
    }
    return Status::Ok;
}

Status validate_property_definition(const PropertyDefinition& property) noexcept {
    if (property.id.is_nil()) { return Status::DatabaseSchemaMalformed; }

    if (const Status s = validate_name(property.name, kMaxPropertyNameBytes); !is_ok(s)) {
        return s;
    }

    const auto raw_type = static_cast<std::int32_t>(property.type);
    if (raw_type < 0 || raw_type >= kPropertyTypeCount) {
        return Status::DatabaseSchemaMalformed;
    }
    if (!property_type_is_implemented(property.type)) {
        return Status::DatabasePropertyTypeUnsupported;
    }

    if (property_type_has_options(property.type)) {
        if (property.options.empty()) {
            // A Select with no options can hold no legal value, so it is not
            // a usable column -- it is a half-created one. Rejecting it is
            // what keeps "a Select value references one of its property's
            // defined options" non-vacuous.
            return Status::DatabaseSchemaMalformed;
        }
        if (property.options.size() > kMaxSelectOptionsPerProperty) {
            return Status::DatabaseOptionLimitExceeded;
        }
    } else if (!property.options.empty()) {
        // Options on a Number or a Date are not harmless decoration: they
        // would be persisted, synced and shown in a property editor as though
        // they meant something.
        return Status::DatabaseSchemaMalformed;
    }

    for (std::size_t i = 0; i < property.options.size(); ++i) {
        const SelectOption& option = property.options[i];
        if (option.id.is_nil()) { return Status::DatabaseSchemaMalformed; }
        if (const Status s = validate_name(option.name, kMaxOptionNameBytes); !is_ok(s)) {
            return s;
        }
        for (std::size_t j = i + 1; j < property.options.size(); ++j) {
            if (option.id == property.options[j].id) {
                return Status::DatabasePropertyDuplicated;
            }
            // Duplicate NAMES are rejected too, not only duplicate ids: two
            // options called "Done" are indistinguishable in every UI that
            // shows them, so a user who picks one cannot know which row they
            // just tagged. Empty names are exempt because an unnamed option
            // is a real transient state while the user is typing.
            if (!option.name.empty() && option.name == property.options[j].name) {
                return Status::DatabasePropertyDuplicated;
            }
        }
    }

    return Status::Ok;
}

Status validate_property_value(const PropertyValue& value) noexcept {
    if (const auto* text = std::get_if<TextValue>(&value)) {
        return validate_text_value(*text);
    }
    if (const auto* number = std::get_if<NumberValue>(&value)) {
        // std::isfinite rejects NaN and both infinities. See NumberValue's
        // declaration for why this is a correctness rule and not hygiene.
        return std::isfinite(number->number) ? Status::Ok : Status::DatabaseValueMalformed;
    }
    if (std::get_if<CheckboxValue>(&value) != nullptr) {
        // A bool has no invalid state. Named rather than folded into the
        // fall-through so that a missing case stays visible.
        return Status::Ok;
    }
    if (const auto* select = std::get_if<SelectValue>(&value)) {
        return select->option.is_nil() ? Status::DatabaseValueMalformed : Status::Ok;
    }
    if (const auto* multi = std::get_if<MultiSelectValue>(&value)) {
        return validate_multi_select_value(*multi);
    }
    if (const auto* date = std::get_if<DateValue>(&value)) {
        if (date->unix_ms < kMinDateUnixMs || date->unix_ms > kMaxDateUnixMs) {
            return Status::DatabaseValueMalformed;
        }
        return Status::Ok;
    }
    // Unreachable while the variant and this function agree; an added
    // alternative lands here rather than being silently accepted.
    return Status::Internal;
}

Status validate_value_for_property(const PropertyDefinition& property,
                                   const PropertyValue& value) noexcept {
    if (const Status s = validate_property_definition(property); !is_ok(s)) { return s; }

    if (property_type_of(value) != property.type) {
        return Status::DatabaseValueTypeMismatch;
    }
    if (const Status s = validate_property_value(value); !is_ok(s)) { return s; }

    // Option membership. This is the half of the contract a type tag alone
    // cannot give: a SelectValue is well-typed no matter which 16 bytes it
    // holds, and only the definition knows whether those bytes name an
    // option this column offers.
    if (const auto* select = std::get_if<SelectValue>(&value)) {
        if (find_option(property, select->option) == nullptr) {
            return Status::DatabaseOptionNotInSchema;
        }
    } else if (const auto* multi = std::get_if<MultiSelectValue>(&value)) {
        for (const OptionId& option : multi->options) {
            if (find_option(property, option) == nullptr) {
                return Status::DatabaseOptionNotInSchema;
            }
        }
    }

    return Status::Ok;
}

}  // namespace uapp::database
