// SPDX-License-Identifier: Apache-2.0
#include "uapp/database/database.h"

#include <cstddef>
#include <string_view>

namespace uapp::database {

const PropertyDefinition* find_property(const Database& db, const PropertyId& id) noexcept {
    for (const PropertyDefinition& property : db.properties) {
        if (property.id == id) { return &property; }
    }
    // Linear over at most kMaxPropertiesPerDatabase (100) entries. A map keyed
    // by id would be faster asymptotically and slower in practice at this
    // size, and it would lose the column ORDER that Database::properties
    // carries -- which is part of the schema, not an incidental layout.
    return nullptr;
}

const PropertyValue* find_value(const Row& row, const PropertyId& id) noexcept {
    const auto it = row.values.find(id);
    if (it == row.values.end()) { return nullptr; }
    return &it->second;
}

Status validate_database_name(std::string_view name) noexcept {
    return validate_name(name, kMaxDatabaseNameBytes);
}

Status validate_database_metadata(const Database& db) noexcept {
    if (db.id.is_nil()) { return Status::IdMalformed; }
    if (db.page_id.is_nil()) {
        // See Database::page_id: a database with no host page is unreachable.
        return Status::IdMalformed;
    }
    if (const Status s = validate_database_name(db.name); !is_ok(s)) { return s; }
    if (db.created_at_ms < 0 || db.modified_at_ms < 0) {
        return Status::InvalidArgument;
    }
    return Status::Ok;
}

Status validate_database_schema(const Database& db) noexcept {
    if (const Status s = validate_database_metadata(db); !is_ok(s)) { return s; }

    if (db.properties.size() > kMaxPropertiesPerDatabase) {
        return Status::DatabasePropertyLimitExceeded;
    }

    for (std::size_t i = 0; i < db.properties.size(); ++i) {
        const PropertyDefinition& property = db.properties[i];
        if (const Status s = validate_property_definition(property); !is_ok(s)) {
            return s;
        }
        for (std::size_t j = i + 1; j < db.properties.size(); ++j) {
            if (property.id == db.properties[j].id) {
                return Status::DatabasePropertyDuplicated;
            }
            // Duplicate column NAMES are rejected for the same reason
            // duplicate option names are: two columns called "Status" cannot
            // be told apart in a filter editor, a header row or an export, so
            // a user who filters on one cannot know which they filtered.
            // Empty names are exempt -- an unnamed column is a real state.
            if (!property.name.empty() && property.name == db.properties[j].name) {
                return Status::DatabasePropertyDuplicated;
            }
        }
        // The option sets of two DIFFERENT properties are allowed to share
        // names ("Done" in both Status and Phase) and are NOT allowed to
        // share option IDS -- an option id is globally unique because it is
        // what a stored value points at, and the same id under two properties
        // would make a Select value ambiguous. Checked across properties
        // here rather than inside validate_property_definition, which sees
        // one property at a time.
        for (const SelectOption& option : property.options) {
            for (std::size_t j = i + 1; j < db.properties.size(); ++j) {
                if (find_option(db.properties[j], option.id) != nullptr) {
                    return Status::DatabasePropertyDuplicated;
                }
            }
        }
    }

    return Status::Ok;
}

Status validate_row(const Database& db, const Row& row) noexcept {
    if (const Status s = validate_database_schema(db); !is_ok(s)) { return s; }

    if (row.id.is_nil()) { return Status::IdMalformed; }
    if (row.database_id != db.id) {
        // A row carrying another database's id is not a validation detail: it
        // would be stored under this database and read back claiming to
        // belong elsewhere.
        return Status::InvalidArgument;
    }
    if (row.created_at_ms < 0 || row.modified_at_ms < 0) {
        return Status::InvalidArgument;
    }

    // A row cannot carry more values than the schema has columns, and each
    // one must name a column that exists. The map's uniqueness gives the
    // "no property twice" half for free.
    if (row.values.size() > kMaxPropertiesPerDatabase) {
        return Status::DatabasePropertyLimitExceeded;
    }

    for (const auto& entry : row.values) {
        const PropertyDefinition* property = find_property(db, entry.first);
        if (property == nullptr) {
            return Status::DatabasePropertyNotInSchema;
        }
        if (const Status s = validate_value_for_property(*property, entry.second);
            !is_ok(s)) {
            return s;
        }
    }

    return Status::Ok;
}

}  // namespace uapp::database
