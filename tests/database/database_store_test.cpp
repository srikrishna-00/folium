// SPDX-License-Identifier: Apache-2.0
//
// Integration tests for the database system against the real SQLite adapter:
// the schema round-trip, typed values surviving storage, the Table view
// compiled into SQL, saved views, persistence across a close/reopen, and the
// guard rails.
//
// EVERY CASE NEEDS A REAL FILE, not ":memory:". A database is anchored to a
// `page` row (ADR-0004), so the host page has to exist -- and it is created
// through the BLOCK store, which is the module that owns the `page` table.
// Two ":memory:" opens are two PRIVATE databases, so the block store's page
// would be invisible to the database store. The scratch files live in a
// TempDir that removes itself.
//
// The DIFFERENTIAL cases are the most important ones here. `query_table_view`
// compiles a ViewConfig into SQL; `database::evaluate_table_view` defines what
// that config MEANS. The cases named "database.differential" run both over
// the same rows and require the identical sequence, so the SQL is checked
// against a specification rather than against someone's reading of it.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "common_fixtures.h"
#include "database_fixtures.h"
#include "test_framework.h"
#include "uapp/blocks/block_store.h"
#include "uapp/blocks/page.h"
#include "uapp/database/database.h"
#include "uapp/database/database_store.h"
#include "uapp/database/property.h"
#include "uapp/database/view.h"
#include "uapp/status.h"
#include "uapp/storage/sqlite/sqlite_block_store.h"
#include "uapp/storage/sqlite/sqlite_database_store.h"

namespace {

using uapp::Status;
using uapp::database::CheckboxValue;
using uapp::database::Database;
using uapp::database::DatabaseId;
using uapp::database::DatabaseStore;
using uapp::database::DateValue;
using uapp::database::Filter;
using uapp::database::FilterOperator;
using uapp::database::MultiSelectValue;
using uapp::database::NumberValue;
using uapp::database::OptionId;
using uapp::database::PropertyDefinition;
using uapp::database::PropertyId;
using uapp::database::PropertyType;
using uapp::database::PropertyValue;
using uapp::database::Row;
using uapp::database::RowId;
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

constexpr std::int64_t kNow     = 1'700'000'000'000;
constexpr std::size_t  kNoLimit = 1000;

/// Everything a case needs, constructed in one place. Non-throwing: the
/// stores are left null when an open fails and each case REQUIREs them.
struct Harness final {
    uapp::test::TempDir                 dir;
    std::unique_ptr<uapp::blocks::BlockStore>   blocks;
    std::unique_ptr<DatabaseStore>              dbs;

    [[nodiscard]] std::string path() const { return dir.file("database.db"); }

    /// Open both stores on one file and create the host page the fixtures'
    /// database is anchored to.
    [[nodiscard]] Status open_with_page() {
        if (const Status s = open_stores(); !is_ok(s)) { return s; }

        uapp::blocks::Page page;
        page.id             = fixture_id<uapp::blocks::PageId>(uapp::test::kFixturePage);
        page.title          = "Project";
        page.created_at_ms  = kNow;
        page.modified_at_ms = kNow;
        return blocks->create_page(page);
    }

    [[nodiscard]] Status open_stores() {
        uapp::storage::sqlite::SqliteBlockStoreOptions block_options;
        block_options.path = path();
        if (const Status s = uapp::storage::sqlite::open_sqlite_block_store(block_options,
                                                                           blocks);
            !is_ok(s)) {
            return s;
        }

        uapp::storage::sqlite::SqliteDatabaseStoreOptions db_options;
        db_options.path = path();
        return uapp::storage::sqlite::open_sqlite_database_store(db_options, dbs);
    }

    /// Close both connections and open them again on the same file. This is
    /// the persistence check: nothing survives in memory across it.
    [[nodiscard]] Status reopen() {
        dbs.reset();
        blocks.reset();
        return open_stores();
    }
};

/// The five-row corpus, identical in shape to the one view_test.cpp uses.
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
                       .build());
    return rows;
}

Status seed(Harness& h, const Database& db, const std::vector<Row>& rows) {
    if (const Status s = h.dbs->create_database(db); !is_ok(s)) { return s; }
    for (const Row& row : rows) {
        if (const Status s = h.dbs->insert_row(row, kNow); !is_ok(s)) { return s; }
    }
    return Status::Ok;
}

}  // namespace

// --- schema round-trip -----------------------------------------------------

UAPP_TEST("database.store: a six-type schema survives storage exactly") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    UAPP_REQUIRE(h.dbs != nullptr);

    const Database original = uapp::test::task_database();
    UAPP_REQUIRE_OK(h.dbs->create_database(original));

    Database loaded;
    UAPP_REQUIRE_OK(h.dbs->get_database(original.id, loaded));

    UAPP_CHECK(loaded.id == original.id);
    UAPP_CHECK(loaded.page_id == original.page_id);
    UAPP_CHECK(loaded.name == "Tasks");
    UAPP_REQUIRE_MSG(loaded.properties.size() == original.properties.size(),
                     std::string("got ") + std::to_string(loaded.properties.size()) +
                         " properties");

    // COLUMN ORDER is part of the schema, so it is checked, not just the set.
    for (std::size_t i = 0; i < original.properties.size(); ++i) {
        const PropertyDefinition& want = original.properties[i];
        const PropertyDefinition& got  = loaded.properties[i];
        UAPP_CHECK_MSG(got.id == want.id, "property " + std::to_string(i) + ": wrong id");
        UAPP_CHECK_MSG(got.name == want.name,
                       "property " + std::to_string(i) + ": name is " + got.name);
        UAPP_CHECK_MSG(got.type == want.type,
                       "property " + std::to_string(i) + ": type is " +
                           uapp::database::property_type_name(got.type));
        UAPP_REQUIRE_MSG(got.options.size() == want.options.size(),
                         "property " + std::to_string(i) + ": option count is " +
                             std::to_string(got.options.size()));
        // OPTION ORDER is part of the schema too -- it is the Select sort key.
        for (std::size_t j = 0; j < want.options.size(); ++j) {
            UAPP_CHECK(got.options[j].id == want.options[j].id);
            UAPP_CHECK(got.options[j].name == want.options[j].name);
        }
    }
}

UAPP_TEST("database.store: a database must be anchored to a page that exists") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_stores());  // deliberately no page
    UAPP_REQUIRE(h.dbs != nullptr);

    // The malformed-input path a caller hits by holding on to the id of a
    // page someone else deleted.
    UAPP_REQUIRE_STATUS(h.dbs->create_database(uapp::test::task_database()),
                        Status::NotFound);

    std::size_t count = 999;
    UAPP_REQUIRE_OK(h.dbs->database_count(count));
    UAPP_CHECK_MSG(count == 0, "a rejected create left a row behind");
}

UAPP_TEST("database.store: databases are listed per host page") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());

    Database first = uapp::test::task_database();
    UAPP_REQUIRE_OK(h.dbs->create_database(first));

    // A second database on the SAME page. Possible exactly because a database
    // is anchored to a page rather than BEING one (ADR-0004): a page-keyed
    // schema could hold only one.
    Database second = uapp::test::task_database();
    second.id   = fixture_id<DatabaseId>(3);
    second.name = "Meetings";
    second.properties.clear();
    second.properties.push_back(uapp::test::date_property());
    // date_property() returns a fixed PropertyId (kPropDue) for fixture
    // convenience, and `first` already has one at that id as part of
    // task_database()'s six properties. db_property.id is a globally unique
    // primary key (every id in this codebase is globally unique by
    // construction -- a real IdGenerator would never produce two properties
    // with the same id across different databases), so reusing the literal
    // fixture id here is a test-fixture collision, not a production
    // scenario: give this property its own distinct id.
    second.properties.back().id = fixture_id<PropertyId>(30);
    UAPP_REQUIRE_OK(h.dbs->create_database(second));

    std::vector<DatabaseId> ids;
    UAPP_REQUIRE_OK(
        h.dbs->list_databases_on_page(fixture_id<uapp::blocks::PageId>(uapp::test::kFixturePage), ids));
    UAPP_REQUIRE_MSG(ids.size() == 2, std::string("got ") + std::to_string(ids.size()));
    UAPP_CHECK(ids[0] == first.id);
    UAPP_CHECK(ids[1] == second.id);

    std::vector<DatabaseId> none;
    UAPP_REQUIRE_OK(h.dbs->list_databases_on_page(fixture_id<uapp::blocks::PageId>(77), none));
    UAPP_CHECK(none.empty());
}

UAPP_TEST("database.store: a schema the domain rejects never reaches the file") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());

    {
        // A Relation column: declared in the enum, NOT IMPLEMENTED.
        Database db = uapp::test::task_database();
        PropertyDefinition relation;
        relation.id   = fixture_id<PropertyId>(50);
        relation.name = "Project";
        relation.type = PropertyType::Relation;
        db.properties.push_back(std::move(relation));
        UAPP_REQUIRE_STATUS(h.dbs->create_database(db),
                            Status::DatabasePropertyTypeUnsupported);
    }
    {
        Database db = uapp::test::task_database();
        db.properties.push_back(uapp::test::text_property());  // duplicate
        UAPP_REQUIRE_STATUS(h.dbs->create_database(db),
                            Status::DatabasePropertyDuplicated);
    }
    {
        Database db = uapp::test::task_database();
        db.name = std::string(uapp::database::kMaxDatabaseNameBytes + 1, 'n');
        UAPP_REQUIRE_STATUS(h.dbs->create_database(db), Status::DatabaseNameTooLong);
    }

    std::size_t count = 999;
    UAPP_REQUIRE_OK(h.dbs->database_count(count));
    UAPP_CHECK_MSG(count == 0, "a rejected create left a row behind");
}

// --- typed values ----------------------------------------------------------

UAPP_TEST("database.store: every implemented value type round-trips with its type") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());

    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(h.dbs->create_database(db));

    const Row written = uapp::test::RowBuilder(100)
                            .title("Write the ADR")
                            .priority(-12.5)
                            .done(true)
                            .status(uapp::test::kOptInProgress)
                            .tags({uapp::test::kOptDocs, uapp::test::kOptUrgent})
                            .due(5)
                            .build();
    UAPP_REQUIRE_OK(h.dbs->insert_row(written, kNow));

    Row loaded;
    UAPP_REQUIRE_OK(h.dbs->get_row(written.id, loaded));
    UAPP_CHECK(loaded.id == written.id);
    UAPP_CHECK(loaded.database_id == db.id);
    UAPP_REQUIRE_MSG(loaded.values.size() == 6,
                     std::string("got ") + std::to_string(loaded.values.size()) +
                         " values");

    // Each value comes back as its OWN type, not as text that happens to
    // parse. A stringly-typed store would pass a `== "−12.5"` check and fail
    // every one of these.
    const auto* title = std::get_if<TextValue>(
        uapp::database::find_value(loaded, fixture_id<PropertyId>(uapp::test::kPropTitle)));
    UAPP_REQUIRE(title != nullptr);
    UAPP_CHECK(title->text == "Write the ADR");

    const auto* priority = std::get_if<NumberValue>(uapp::database::find_value(
        loaded, fixture_id<PropertyId>(uapp::test::kPropPriority)));
    UAPP_REQUIRE(priority != nullptr);
    UAPP_CHECK_MSG(priority->number == -12.5,
                   std::string("got ") + std::to_string(priority->number));

    const auto* done = std::get_if<CheckboxValue>(
        uapp::database::find_value(loaded, fixture_id<PropertyId>(uapp::test::kPropDone)));
    UAPP_REQUIRE(done != nullptr);
    UAPP_CHECK(done->checked);

    const auto* status = std::get_if<SelectValue>(uapp::database::find_value(
        loaded, fixture_id<PropertyId>(uapp::test::kPropStatus)));
    UAPP_REQUIRE(status != nullptr);
    UAPP_CHECK(status->option == fixture_id<OptionId>(uapp::test::kOptInProgress));

    const auto* tags = std::get_if<MultiSelectValue>(
        uapp::database::find_value(loaded, fixture_id<PropertyId>(uapp::test::kPropTags)));
    UAPP_REQUIRE(tags != nullptr);
    // The user's chosen ORDER is preserved, not normalised to schema order:
    // docs was selected first.
    UAPP_REQUIRE_MSG(tags->options.size() == 2,
                     std::string("got ") + std::to_string(tags->options.size()) + " tags");
    UAPP_CHECK(tags->options[0] == fixture_id<OptionId>(uapp::test::kOptDocs));
    UAPP_CHECK(tags->options[1] == fixture_id<OptionId>(uapp::test::kOptUrgent));

    const auto* due = std::get_if<DateValue>(
        uapp::database::find_value(loaded, fixture_id<PropertyId>(uapp::test::kPropDue)));
    UAPP_REQUIRE(due != nullptr);
    UAPP_CHECK(due->unix_ms == uapp::test::fixture_date(5));
}

UAPP_TEST("database.store: a sparse row keeps its unset properties unset") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    UAPP_REQUIRE_OK(h.dbs->create_database(uapp::test::task_database()));

    const Row sparse = uapp::test::RowBuilder(100).title("only a title").build();
    UAPP_REQUIRE_OK(h.dbs->insert_row(sparse, kNow));

    Row loaded;
    UAPP_REQUIRE_OK(h.dbs->get_row(sparse.id, loaded));
    UAPP_REQUIRE_MSG(loaded.values.size() == 1,
                     std::string("got ") + std::to_string(loaded.values.size()) +
                         " values; an unset property must not be materialised as a default");
    UAPP_CHECK(uapp::database::find_value(
                   loaded, fixture_id<PropertyId>(uapp::test::kPropDone)) == nullptr);
}

UAPP_TEST("database.store: update_row is a PUT, so an omitted property becomes unset") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    UAPP_REQUIRE_OK(h.dbs->create_database(uapp::test::task_database()));

    const Row full = uapp::test::RowBuilder(100)
                         .title("alpha")
                         .priority(3.0)
                         .tags({uapp::test::kOptUrgent})
                         .build();
    UAPP_REQUIRE_OK(h.dbs->insert_row(full, kNow));

    const Row trimmed = uapp::test::RowBuilder(100).title("alpha renamed").build();
    UAPP_REQUIRE_OK(h.dbs->update_row(trimmed, kNow + 1000));

    Row loaded;
    UAPP_REQUIRE_OK(h.dbs->get_row(full.id, loaded));
    UAPP_REQUIRE_MSG(loaded.values.size() == 1,
                     std::string("got ") + std::to_string(loaded.values.size()) +
                         " values; a PUT must clear what it omits");
    const auto* title = std::get_if<TextValue>(
        uapp::database::find_value(loaded, fixture_id<PropertyId>(uapp::test::kPropTitle)));
    UAPP_REQUIRE(title != nullptr);
    UAPP_CHECK(title->text == "alpha renamed");
    UAPP_CHECK(loaded.modified_at_ms == kNow + 1000);
    // The multi-select membership rows must have gone too, not been orphaned.
    UAPP_CHECK(uapp::database::find_value(
                   loaded, fixture_id<PropertyId>(uapp::test::kPropTags)) == nullptr);
}

UAPP_TEST("database.store: a type-mismatched or out-of-schema value is refused") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(h.dbs->create_database(db));

    {
        Row row = uapp::test::RowBuilder(100).title("x").build();
        row.values[fixture_id<PropertyId>(uapp::test::kPropPriority)] = TextValue{"10"};
        UAPP_REQUIRE_STATUS(h.dbs->insert_row(row, kNow),
                            Status::DatabaseValueTypeMismatch);
    }
    {
        Row row = uapp::test::RowBuilder(101).title("x").build();
        row.values[fixture_id<PropertyId>(uapp::test::kPropStatus)] =
            SelectValue{fixture_id<OptionId>(777)};
        UAPP_REQUIRE_STATUS(h.dbs->insert_row(row, kNow),
                            Status::DatabaseOptionNotInSchema);
    }
    {
        Row row = uapp::test::RowBuilder(102).title("x").build();
        row.values[fixture_id<PropertyId>(555)] = TextValue{"ghost column"};
        UAPP_REQUIRE_STATUS(h.dbs->insert_row(row, kNow),
                            Status::DatabasePropertyNotInSchema);
    }
    {
        Row row = uapp::test::RowBuilder(103).build();
        row.values[fixture_id<PropertyId>(uapp::test::kPropPriority)] =
            NumberValue{std::numeric_limits<double>::quiet_NaN()};
        UAPP_REQUIRE_STATUS(h.dbs->insert_row(row, kNow),
                            Status::DatabaseValueMalformed);
    }
    {
        Row row = uapp::test::RowBuilder(104).build();
        row.values[fixture_id<PropertyId>(uapp::test::kPropTitle)] =
            TextValue{std::string(uapp::database::kMaxPropertyTextBytes + 1, 'x')};
        UAPP_REQUIRE_STATUS(h.dbs->insert_row(row, kNow), Status::DatabaseValueTooLarge);
    }

    std::size_t rows = 999;
    UAPP_REQUIRE_OK(h.dbs->row_count(db.id, rows));
    UAPP_CHECK_MSG(rows == 0, "a rejected insert left a row behind");
}

UAPP_TEST("database.store: deleting a row removes its values and its memberships") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(seed(h, db, corpus()));

    std::size_t rows = 0;
    UAPP_REQUIRE_OK(h.dbs->row_count(db.id, rows));
    UAPP_CHECK(rows == 5);

    UAPP_REQUIRE_OK(h.dbs->delete_row(fixture_id<RowId>(100), kNow + 1));
    UAPP_REQUIRE_OK(h.dbs->row_count(db.id, rows));
    UAPP_CHECK(rows == 4);

    Row gone;
    UAPP_REQUIRE_STATUS(h.dbs->get_row(fixture_id<RowId>(100), gone), Status::NotFound);

    // A view over the rest must not report the deleted row's tag membership.
    const PropertyValue urgent =
        SelectValue{fixture_id<OptionId>(uapp::test::kOptUrgent)};
    std::vector<Row> out;
    UAPP_REQUIRE_OK(h.dbs->query_table_view(
        db.id, filter_config(uapp::test::kPropTags, FilterOperator::Contains, &urgent),
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>{"epsilon"}, describe(out));
}

// --- the Table view, through SQL -------------------------------------------

UAPP_TEST("database.store: a Table view filters by each property type's operators") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(seed(h, db, corpus()));

    std::vector<Row> out;

    // Text: contains, case-sensitively (instr(), not LIKE).
    const PropertyValue a = TextValue{"a"};
    UAPP_REQUIRE_OK(h.dbs->query_table_view(
        db.id, filter_config(uapp::test::kPropTitle, FilterOperator::Contains, &a),
        kNoLimit, out));
    UAPP_REQUIRE_MSG(
        titles_of(out) == std::vector<std::string>({"alpha", "beta", "gamma", "delta"}),
        describe(out));

    const PropertyValue upper_a = TextValue{"A"};
    UAPP_REQUIRE_OK(h.dbs->query_table_view(
        db.id, filter_config(uapp::test::kPropTitle, FilterOperator::Contains, &upper_a),
        kNoLimit, out));
    UAPP_REQUIRE_MSG(out.empty(),
                     "contains must be case-SENSITIVE; SQLite's LIKE is not: " +
                         describe(out));

    // Number: >
    const PropertyValue three = NumberValue{3.0};
    UAPP_REQUIRE_OK(h.dbs->query_table_view(
        db.id, filter_config(uapp::test::kPropPriority, FilterOperator::GreaterThan, &three),
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>{"gamma"}, describe(out));

    // Checkbox: is not -- which must INCLUDE the row that leaves it unset.
    const PropertyValue checked = CheckboxValue{true};
    UAPP_REQUIRE_OK(h.dbs->query_table_view(
        db.id, filter_config(uapp::test::kPropDone, FilterOperator::NotEquals, &checked),
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"alpha", "gamma", "delta"}),
                     describe(out));

    // Select: is
    const PropertyValue done_option =
        SelectValue{fixture_id<OptionId>(uapp::test::kOptDone)};
    UAPP_REQUIRE_OK(h.dbs->query_table_view(
        db.id, filter_config(uapp::test::kPropStatus, FilterOperator::Equals, &done_option),
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"beta", "epsilon"}),
                     describe(out));

    // Multi-select: contains / does not contain (the membership table is what
    // makes these an index seek rather than a blob scan).
    const PropertyValue docs = SelectValue{fixture_id<OptionId>(uapp::test::kOptDocs)};
    UAPP_REQUIRE_OK(h.dbs->query_table_view(
        db.id, filter_config(uapp::test::kPropTags, FilterOperator::Contains, &docs),
        kNoLimit, out));
    UAPP_REQUIRE_MSG(
        titles_of(out) == std::vector<std::string>({"alpha", "delta", "epsilon"}),
        describe(out));

    UAPP_REQUIRE_OK(h.dbs->query_table_view(
        db.id, filter_config(uapp::test::kPropTags, FilterOperator::NotContains, &docs),
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"beta", "gamma"}),
                     describe(out));

    // Date: before / after / on
    const PropertyValue fifth = DateValue{uapp::test::fixture_date(5)};
    UAPP_REQUIRE_OK(h.dbs->query_table_view(
        db.id, filter_config(uapp::test::kPropDue, FilterOperator::LessThan, &fifth),
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>{"beta"}, describe(out));

    UAPP_REQUIRE_OK(h.dbs->query_table_view(
        db.id, filter_config(uapp::test::kPropDue, FilterOperator::GreaterThan, &fifth),
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>{"gamma"}, describe(out));

    UAPP_REQUIRE_OK(h.dbs->query_table_view(
        db.id, filter_config(uapp::test::kPropDue, FilterOperator::Equals, &fifth),
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>{"alpha"}, describe(out));

    // Presence, on a scalar column and on the membership table.
    UAPP_REQUIRE_OK(h.dbs->query_table_view(
        db.id, filter_config(uapp::test::kPropDue, FilterOperator::IsEmpty, nullptr),
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"delta", "epsilon"}),
                     describe(out));

    UAPP_REQUIRE_OK(h.dbs->query_table_view(
        db.id, filter_config(uapp::test::kPropTags, FilterOperator::IsEmpty, nullptr),
        kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>{"gamma"}, describe(out));
}

UAPP_TEST("database.store: a Table view sorts by each property type") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(seed(h, db, corpus()));

    std::vector<Row> out;

    UAPP_REQUIRE_OK(h.dbs->query_table_view(db.id, sort_config(uapp::test::kPropTitle, false),
                                            kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"alpha", "beta", "delta",
                                                                 "epsilon", "gamma"}),
                     describe(out));

    UAPP_REQUIRE_OK(h.dbs->query_table_view(
        db.id, sort_config(uapp::test::kPropPriority, false), kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"beta", "alpha", "delta",
                                                                 "gamma", "epsilon"}),
                     describe(out));

    // Descending must keep the UNSET row last, which `ORDER BY key DESC`
    // alone would not do -- hence the `(key IS NULL) ASC` term in front.
    UAPP_REQUIRE_OK(h.dbs->query_table_view(
        db.id, sort_config(uapp::test::kPropPriority, true), kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"gamma", "alpha", "delta",
                                                                 "beta", "epsilon"}),
                     describe(out));

    UAPP_REQUIRE_OK(h.dbs->query_table_view(db.id, sort_config(uapp::test::kPropDone, false),
                                            kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"alpha", "gamma", "beta",
                                                                 "epsilon", "delta"}),
                     describe(out));

    // Select sorts by the SCHEMA's option order (To do, In progress, Done),
    // which is `db_select_option.sort_index` -- not by option name and not by
    // option id bytes.
    UAPP_REQUIRE_OK(h.dbs->query_table_view(
        db.id, sort_config(uapp::test::kPropStatus, false), kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"gamma", "alpha", "beta",
                                                                 "epsilon", "delta"}),
                     describe(out));

    // Multi-select sorts by its earliest selected option in schema order.
    UAPP_REQUIRE_OK(h.dbs->query_table_view(db.id, sort_config(uapp::test::kPropTags, false),
                                            kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"alpha", "epsilon", "beta",
                                                                 "delta", "gamma"}),
                     describe(out));

    UAPP_REQUIRE_OK(h.dbs->query_table_view(db.id, sort_config(uapp::test::kPropDue, false),
                                            kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"beta", "alpha", "gamma",
                                                                 "delta", "epsilon"}),
                     describe(out));
}

UAPP_TEST("database.store: a limit truncates after the sort, not before") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(seed(h, db, corpus()));

    std::vector<Row> out;
    UAPP_REQUIRE_OK(h.dbs->query_table_view(
        db.id, sort_config(uapp::test::kPropPriority, true), 2, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"gamma", "alpha"}),
                     describe(out));
}

// --- the differential oracle -----------------------------------------------

UAPP_TEST("database.differential: the SQL compiler agrees with the reference evaluator") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    const Database db = uapp::test::task_database();
    const std::vector<Row> rows = corpus();
    UAPP_REQUIRE_OK(seed(h, db, rows));

    // The SAME rows, as the store has them (so timestamps and option order
    // match what SQL will produce).
    std::vector<Row> stored;
    UAPP_REQUIRE_OK(h.dbs->query_table_view(db.id, ViewConfig{}, kNoLimit, stored));
    UAPP_REQUIRE_MSG(stored.size() == rows.size(),
                     std::string("stored ") + std::to_string(stored.size()));

    const PropertyValue three  = NumberValue{3.0};
    const PropertyValue a      = TextValue{"a"};
    const PropertyValue fifth  = DateValue{uapp::test::fixture_date(5)};
    const PropertyValue done_o = SelectValue{fixture_id<OptionId>(uapp::test::kOptDone)};
    const PropertyValue docs   = SelectValue{fixture_id<OptionId>(uapp::test::kOptDocs)};
    const PropertyValue yes    = CheckboxValue{true};

    std::vector<ViewConfig> configs;
    configs.push_back(ViewConfig{});
    // One per operator per type, plus the four sort directions that matter.
    configs.push_back(filter_config(uapp::test::kPropTitle, FilterOperator::Equals, &a));
    configs.push_back(filter_config(uapp::test::kPropTitle, FilterOperator::NotEquals, &a));
    configs.push_back(filter_config(uapp::test::kPropTitle, FilterOperator::Contains, &a));
    configs.push_back(
        filter_config(uapp::test::kPropTitle, FilterOperator::NotContains, &a));
    configs.push_back(filter_config(uapp::test::kPropTitle, FilterOperator::IsEmpty, nullptr));
    configs.push_back(
        filter_config(uapp::test::kPropPriority, FilterOperator::Equals, &three));
    configs.push_back(
        filter_config(uapp::test::kPropPriority, FilterOperator::NotEquals, &three));
    configs.push_back(
        filter_config(uapp::test::kPropPriority, FilterOperator::LessThan, &three));
    configs.push_back(
        filter_config(uapp::test::kPropPriority, FilterOperator::LessOrEqual, &three));
    configs.push_back(
        filter_config(uapp::test::kPropPriority, FilterOperator::GreaterThan, &three));
    configs.push_back(
        filter_config(uapp::test::kPropPriority, FilterOperator::GreaterOrEqual, &three));
    configs.push_back(
        filter_config(uapp::test::kPropPriority, FilterOperator::IsNotEmpty, nullptr));
    configs.push_back(filter_config(uapp::test::kPropDone, FilterOperator::Equals, &yes));
    configs.push_back(filter_config(uapp::test::kPropDone, FilterOperator::NotEquals, &yes));
    configs.push_back(filter_config(uapp::test::kPropDone, FilterOperator::IsEmpty, nullptr));
    configs.push_back(
        filter_config(uapp::test::kPropStatus, FilterOperator::Equals, &done_o));
    configs.push_back(
        filter_config(uapp::test::kPropStatus, FilterOperator::NotEquals, &done_o));
    configs.push_back(
        filter_config(uapp::test::kPropStatus, FilterOperator::IsEmpty, nullptr));
    configs.push_back(filter_config(uapp::test::kPropTags, FilterOperator::Contains, &docs));
    configs.push_back(
        filter_config(uapp::test::kPropTags, FilterOperator::NotContains, &docs));
    configs.push_back(filter_config(uapp::test::kPropTags, FilterOperator::IsEmpty, nullptr));
    configs.push_back(
        filter_config(uapp::test::kPropTags, FilterOperator::IsNotEmpty, nullptr));
    configs.push_back(filter_config(uapp::test::kPropDue, FilterOperator::Equals, &fifth));
    configs.push_back(filter_config(uapp::test::kPropDue, FilterOperator::NotEquals, &fifth));
    configs.push_back(filter_config(uapp::test::kPropDue, FilterOperator::LessThan, &fifth));
    configs.push_back(
        filter_config(uapp::test::kPropDue, FilterOperator::GreaterOrEqual, &fifth));
    configs.push_back(filter_config(uapp::test::kPropDue, FilterOperator::IsEmpty, nullptr));

    const std::uint64_t sortable[] = {uapp::test::kPropTitle, uapp::test::kPropPriority,
                                      uapp::test::kPropDone,  uapp::test::kPropStatus,
                                      uapp::test::kPropTags,  uapp::test::kPropDue};
    for (const std::uint64_t property : sortable) {
        configs.push_back(sort_config(property, false));
        configs.push_back(sort_config(property, true));
    }

    // Two multi-key / mixed configs, because a single-key ORDER BY cannot
    // catch a wrong term ORDER in the compiled statement.
    {
        ViewConfig mixed;
        Filter not_done;
        not_done.property_id = fixture_id<PropertyId>(uapp::test::kPropDone);
        not_done.op          = FilterOperator::NotEquals;
        not_done.operand     = yes;
        mixed.filters.push_back(not_done);
        Sort by_priority_desc;
        by_priority_desc.property_id = fixture_id<PropertyId>(uapp::test::kPropPriority);
        by_priority_desc.descending  = true;
        mixed.sorts.push_back(by_priority_desc);
        Sort by_title;
        by_title.property_id = fixture_id<PropertyId>(uapp::test::kPropTitle);
        mixed.sorts.push_back(by_title);
        configs.push_back(std::move(mixed));
    }
    {
        ViewConfig mixed;
        Sort by_priority;
        by_priority.property_id = fixture_id<PropertyId>(uapp::test::kPropPriority);
        mixed.sorts.push_back(by_priority);
        Sort by_title_desc;
        by_title_desc.property_id = fixture_id<PropertyId>(uapp::test::kPropTitle);
        by_title_desc.descending  = true;
        mixed.sorts.push_back(by_title_desc);
        configs.push_back(std::move(mixed));
    }

    std::size_t compared = 0;
    for (std::size_t i = 0; i < configs.size(); ++i) {
        std::vector<Row> from_sql;
        UAPP_REQUIRE_MSG(
            uapp::is_ok(h.dbs->query_table_view(db.id, configs[i], kNoLimit, from_sql)),
            std::string("config ") + std::to_string(i) + ": the SQL path failed");

        std::vector<Row> from_reference;
        UAPP_REQUIRE_MSG(uapp::is_ok(uapp::database::evaluate_table_view(
                             db, configs[i], stored, kNoLimit, from_reference)),
                         std::string("config ") + std::to_string(i) +
                             ": the reference path failed");

        UAPP_CHECK_MSG(titles_of(from_sql) == titles_of(from_reference),
                       std::string("config ") + std::to_string(i) + ": SQL gave " +
                           describe(from_sql) + " but the reference gave " +
                           describe(from_reference));
        ++compared;
    }
    UAPP_CHECK_MSG(compared == configs.size(),
                   std::string("only compared ") + std::to_string(compared));
    UAPP_CHECK_MSG(configs.size() >= 40,
                   std::string("the differential corpus shrank to ") +
                       std::to_string(configs.size()) + " configs");
}

// --- saved views -----------------------------------------------------------

UAPP_TEST("database.store: a named view configuration saves and reloads intact") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(seed(h, db, corpus()));

    View view;
    view.id          = fixture_id<ViewId>(200);
    view.database_id = db.id;
    view.name        = "Open, by priority";
    view.kind        = ViewKind::Table;
    {
        Filter not_done;
        not_done.property_id = fixture_id<PropertyId>(uapp::test::kPropDone);
        not_done.op          = FilterOperator::NotEquals;
        not_done.operand     = PropertyValue(CheckboxValue{true});
        view.config.filters.push_back(not_done);

        Filter tagged;
        tagged.property_id = fixture_id<PropertyId>(uapp::test::kPropTags);
        tagged.op          = FilterOperator::Contains;
        tagged.operand =
            PropertyValue(SelectValue{fixture_id<OptionId>(uapp::test::kOptDocs)});
        view.config.filters.push_back(tagged);

        Sort by_priority_desc;
        by_priority_desc.property_id = fixture_id<PropertyId>(uapp::test::kPropPriority);
        by_priority_desc.descending  = true;
        view.config.sorts.push_back(by_priority_desc);
    }
    UAPP_REQUIRE_OK(h.dbs->save_view(view, kNow));

    View loaded;
    UAPP_REQUIRE_OK(h.dbs->get_view(view.id, loaded));
    UAPP_CHECK(loaded.id == view.id);
    UAPP_CHECK(loaded.database_id == db.id);
    UAPP_CHECK(loaded.name == "Open, by priority");
    UAPP_CHECK(loaded.kind == ViewKind::Table);
    UAPP_REQUIRE_MSG(loaded.config.filters.size() == 2,
                     std::string("got ") + std::to_string(loaded.config.filters.size()) +
                         " filters");
    // FILTER ORDER is preserved, and each operand comes back with its TYPE:
    // the Checkbox operand is a CheckboxValue, the Multi-select membership
    // probe is a SelectValue naming one option.
    UAPP_CHECK(loaded.config.filters[0].op == FilterOperator::NotEquals);
    UAPP_REQUIRE(loaded.config.filters[0].operand.has_value());
    const auto* flag = std::get_if<CheckboxValue>(&*loaded.config.filters[0].operand);
    UAPP_REQUIRE(flag != nullptr);
    UAPP_CHECK(flag->checked);

    UAPP_CHECK(loaded.config.filters[1].op == FilterOperator::Contains);
    UAPP_REQUIRE(loaded.config.filters[1].operand.has_value());
    const auto* option = std::get_if<SelectValue>(&*loaded.config.filters[1].operand);
    UAPP_REQUIRE(option != nullptr);
    UAPP_CHECK(option->option == fixture_id<OptionId>(uapp::test::kOptDocs));

    UAPP_REQUIRE_MSG(loaded.config.sorts.size() == 1,
                     std::string("got ") + std::to_string(loaded.config.sorts.size()) +
                         " sorts");
    UAPP_CHECK(loaded.config.sorts[0].descending);

    // And running the SAVED view gives the same answer as running its config.
    std::vector<Row> from_saved;
    UAPP_REQUIRE_OK(h.dbs->query_saved_view(view.id, kNoLimit, from_saved));
    std::vector<Row> from_config;
    UAPP_REQUIRE_OK(h.dbs->query_table_view(db.id, view.config, kNoLimit, from_config));
    UAPP_CHECK_MSG(titles_of(from_saved) == titles_of(from_config),
                   describe(from_saved) + " vs " + describe(from_config));
    UAPP_REQUIRE_MSG(titles_of(from_saved) == std::vector<std::string>({"alpha", "delta"}),
                     describe(from_saved));
}

UAPP_TEST("database.store: saving a view again replaces its configuration") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(seed(h, db, corpus()));

    View view;
    view.id          = fixture_id<ViewId>(200);
    view.database_id = db.id;
    view.name        = "First";
    view.config      = sort_config(uapp::test::kPropTitle, false);
    UAPP_REQUIRE_OK(h.dbs->save_view(view, kNow));

    view.name   = "Second";
    view.config = sort_config(uapp::test::kPropPriority, true);
    UAPP_REQUIRE_OK(h.dbs->save_view(view, kNow + 1));

    View loaded;
    UAPP_REQUIRE_OK(h.dbs->get_view(view.id, loaded));
    UAPP_CHECK(loaded.name == "Second");
    UAPP_REQUIRE_MSG(loaded.config.sorts.size() == 1,
                     "the old sort was not replaced: " +
                         std::to_string(loaded.config.sorts.size()) + " sorts");
    UAPP_CHECK(loaded.config.sorts[0].property_id ==
               fixture_id<PropertyId>(uapp::test::kPropPriority));

    std::vector<View> all;
    UAPP_REQUIRE_OK(h.dbs->list_views(db.id, all));
    UAPP_CHECK_MSG(all.size() == 1, "an upsert created a second view");
}

UAPP_TEST("database.store: deleting a view touches no row") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(seed(h, db, corpus()));

    View view;
    view.id          = fixture_id<ViewId>(200);
    view.database_id = db.id;
    view.name        = "Temporary";
    view.config      = sort_config(uapp::test::kPropTitle, false);
    UAPP_REQUIRE_OK(h.dbs->save_view(view, kNow));
    UAPP_REQUIRE_OK(h.dbs->delete_view(view.id));

    View gone;
    UAPP_REQUIRE_STATUS(h.dbs->get_view(view.id, gone), Status::NotFound);

    // This is the property that justifies storing a view's configuration
    // separately from the data at all.
    std::size_t rows = 0;
    UAPP_REQUIRE_OK(h.dbs->row_count(db.id, rows));
    UAPP_CHECK_MSG(rows == 5, std::string("deleting a view cost rows: ") +
                                  std::to_string(rows));

    UAPP_REQUIRE_STATUS(h.dbs->delete_view(view.id), Status::NotFound);
}

UAPP_TEST("database.store: a view whose filter is illegal for the type is refused") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(h.dbs->create_database(db));

    View view;
    view.id          = fixture_id<ViewId>(200);
    view.database_id = db.id;
    view.name        = "Nonsense";
    view.config      = filter_config(uapp::test::kPropDone, FilterOperator::GreaterThan,
                                     nullptr);
    view.config.filters[0].operand = PropertyValue(CheckboxValue{true});
    UAPP_REQUIRE_STATUS(h.dbs->save_view(view, kNow),
                        Status::DatabaseFilterOperatorUnsupported);

    view.kind   = ViewKind::Calendar;
    view.config = ViewConfig{};
    UAPP_REQUIRE_STATUS(h.dbs->save_view(view, kNow), Status::DatabaseViewKindUnsupported);

    std::vector<View> all;
    UAPP_REQUIRE_OK(h.dbs->list_views(db.id, all));
    UAPP_CHECK_MSG(all.empty(), "a rejected save_view left a view behind");
}

UAPP_TEST("database.bounds: the saved-view cap is enforced") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(h.dbs->create_database(db));

    for (std::size_t i = 0; i < uapp::database::kMaxViewsPerDatabase; ++i) {
        View view;
        view.id          = fixture_id<ViewId>(300 + i);
        view.database_id = db.id;
        view.name        = "v" + std::to_string(i);
        UAPP_REQUIRE_MSG(uapp::is_ok(h.dbs->save_view(view, kNow)),
                         std::string("view ") + std::to_string(i) + " was rejected early");
    }

    View one_more;
    one_more.id          = fixture_id<ViewId>(9999);
    one_more.database_id = db.id;
    one_more.name        = "over";
    UAPP_REQUIRE_STATUS(h.dbs->save_view(one_more, kNow),
                        Status::DatabaseViewLimitExceeded);

    // ... but RE-saving an existing view is still allowed at the cap: the
    // limit is on how many exist, not on how often they are edited.
    View existing;
    existing.id          = fixture_id<ViewId>(300);
    existing.database_id = db.id;
    existing.name        = "renamed at the cap";
    UAPP_REQUIRE_OK(h.dbs->save_view(existing, kNow + 1));
}

// --- persistence -----------------------------------------------------------

UAPP_TEST("database.store: database, rows and views all survive a close and reopen") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());

    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(seed(h, db, corpus()));

    View view;
    view.id          = fixture_id<ViewId>(200);
    view.database_id = db.id;
    view.name        = "Urgent first";
    view.config      = filter_config(
        uapp::test::kPropTags, FilterOperator::Contains, nullptr);
    view.config.filters[0].operand =
        PropertyValue(SelectValue{fixture_id<OptionId>(uapp::test::kOptUrgent)});
    Sort by_title;
    by_title.property_id = fixture_id<PropertyId>(uapp::test::kPropTitle);
    view.config.sorts.push_back(by_title);
    UAPP_REQUIRE_OK(h.dbs->save_view(view, kNow));

    // Everything below this line reads from the FILE, not from memory.
    UAPP_REQUIRE_OK(h.reopen());
    UAPP_REQUIRE(h.dbs != nullptr);

    Database reloaded;
    UAPP_REQUIRE_OK(h.dbs->get_database(db.id, reloaded));
    UAPP_CHECK(reloaded.name == "Tasks");
    UAPP_REQUIRE_MSG(reloaded.properties.size() == 6,
                     std::string("got ") + std::to_string(reloaded.properties.size()) +
                         " properties after reopen");
    UAPP_CHECK(reloaded.properties[3].type == PropertyType::Select);
    UAPP_REQUIRE(reloaded.properties[3].options.size() == 3);
    UAPP_CHECK(reloaded.properties[3].options[1].name == "In progress");

    std::size_t rows = 0;
    UAPP_REQUIRE_OK(h.dbs->row_count(db.id, rows));
    UAPP_CHECK_MSG(rows == 5, std::string("got ") + std::to_string(rows) +
                                  " rows after reopen");

    // A typed value, read back after a real file round-trip.
    Row row;
    UAPP_REQUIRE_OK(h.dbs->get_row(fixture_id<RowId>(100), row));
    const auto* priority = std::get_if<NumberValue>(uapp::database::find_value(
        row, fixture_id<PropertyId>(uapp::test::kPropPriority)));
    UAPP_REQUIRE(priority != nullptr);
    UAPP_CHECK(priority->number == 3.0);
    const auto* tags = std::get_if<MultiSelectValue>(
        uapp::database::find_value(row, fixture_id<PropertyId>(uapp::test::kPropTags)));
    UAPP_REQUIRE(tags != nullptr);
    UAPP_REQUIRE(tags->options.size() == 2);
    UAPP_CHECK(tags->options[0] == fixture_id<OptionId>(uapp::test::kOptUrgent));

    View reloaded_view;
    UAPP_REQUIRE_OK(h.dbs->get_view(view.id, reloaded_view));
    UAPP_CHECK(reloaded_view.name == "Urgent first");
    UAPP_REQUIRE(reloaded_view.config.filters.size() == 1);
    UAPP_REQUIRE(reloaded_view.config.sorts.size() == 1);

    std::vector<Row> out;
    UAPP_REQUIRE_OK(h.dbs->query_saved_view(view.id, kNoLimit, out));
    UAPP_REQUIRE_MSG(titles_of(out) == std::vector<std::string>({"alpha", "epsilon"}),
                     describe(out));
}

UAPP_TEST("database.store: deleting a database removes its rows, values and views") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(seed(h, db, corpus()));

    View view;
    view.id          = fixture_id<ViewId>(200);
    view.database_id = db.id;
    view.name        = "Anything";
    view.config      = sort_config(uapp::test::kPropTitle, false);
    UAPP_REQUIRE_OK(h.dbs->save_view(view, kNow));

    UAPP_REQUIRE_OK(h.dbs->delete_database(db.id));
    UAPP_REQUIRE_OK(h.reopen());

    Database gone;
    UAPP_REQUIRE_STATUS(h.dbs->get_database(db.id, gone), Status::NotFound);
    Row orphan;
    UAPP_REQUIRE_STATUS(h.dbs->get_row(fixture_id<RowId>(100), orphan), Status::NotFound);
    View orphan_view;
    UAPP_REQUIRE_STATUS(h.dbs->get_view(view.id, orphan_view), Status::NotFound);

    std::size_t count = 999;
    UAPP_REQUIRE_OK(h.dbs->database_count(count));
    UAPP_CHECK(count == 0);

    UAPP_REQUIRE_STATUS(h.dbs->delete_database(db.id), Status::NotFound);
}

UAPP_TEST("database.store: a read-only connection rejects every write") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(seed(h, db, corpus()));

    h.dbs.reset();
    h.blocks.reset();

    std::unique_ptr<DatabaseStore> ro;
    uapp::storage::sqlite::SqliteDatabaseStoreOptions options;
    options.path      = h.path();
    options.read_only = true;
    UAPP_REQUIRE_OK(uapp::storage::sqlite::open_sqlite_database_store(options, ro));
    UAPP_REQUIRE(ro != nullptr);

    // Reads work.
    Database loaded;
    UAPP_REQUIRE_OK(ro->get_database(db.id, loaded));
    std::vector<Row> out;
    UAPP_REQUIRE_OK(ro->query_table_view(db.id, ViewConfig{}, kNoLimit, out));
    UAPP_CHECK(out.size() == 5);

    // Writes are refused with a status, not silently dropped.
    Database another = db;
    another.id = fixture_id<DatabaseId>(9);
    UAPP_REQUIRE_STATUS(ro->create_database(another), Status::StoreReadOnly);
    UAPP_REQUIRE_STATUS(ro->insert_row(uapp::test::RowBuilder(999).title("x").build(), kNow),
                        Status::StoreReadOnly);
    UAPP_REQUIRE_STATUS(ro->delete_row(fixture_id<RowId>(100), kNow),
                        Status::StoreReadOnly);
    UAPP_REQUIRE_STATUS(ro->delete_database(db.id), Status::StoreReadOnly);
    UAPP_REQUIRE_STATUS(ro->set_database_name(db.id, "nope", kNow), Status::StoreReadOnly);
}

UAPP_TEST("database.store: renaming a database is the one schema mutation available") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(h.dbs->create_database(db));

    UAPP_REQUIRE_OK(h.dbs->set_database_name(db.id, "Backlog", kNow + 5));
    Database loaded;
    UAPP_REQUIRE_OK(h.dbs->get_database(db.id, loaded));
    UAPP_CHECK(loaded.name == "Backlog");
    UAPP_CHECK(loaded.modified_at_ms == kNow + 5);

    UAPP_REQUIRE_STATUS(h.dbs->set_database_name(fixture_id<DatabaseId>(42), "x", kNow),
                        Status::NotFound);
    UAPP_REQUIRE_STATUS(
        h.dbs->set_database_name(
            db.id, std::string(uapp::database::kMaxDatabaseNameBytes + 1, 'n'), kNow),
        Status::DatabaseNameTooLong);
}

UAPP_TEST("database.bounds: malformed ids and absent objects are reported, not guessed") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(h.dbs->create_database(db));

    Database out_db;
    UAPP_REQUIRE_STATUS(h.dbs->get_database(fixture_id<DatabaseId>(0), out_db),
                        Status::IdMalformed);
    UAPP_REQUIRE_STATUS(h.dbs->get_database(fixture_id<DatabaseId>(4242), out_db),
                        Status::NotFound);

    Row out_row;
    UAPP_REQUIRE_STATUS(h.dbs->get_row(fixture_id<RowId>(0), out_row), Status::IdMalformed);
    UAPP_REQUIRE_STATUS(h.dbs->get_row(fixture_id<RowId>(4242), out_row), Status::NotFound);

    View out_view;
    UAPP_REQUIRE_STATUS(h.dbs->get_view(fixture_id<ViewId>(0), out_view),
                        Status::IdMalformed);
    UAPP_REQUIRE_STATUS(h.dbs->get_view(fixture_id<ViewId>(4242), out_view),
                        Status::NotFound);

    std::vector<Row> rows;
    UAPP_REQUIRE_STATUS(
        h.dbs->query_table_view(fixture_id<DatabaseId>(4242), ViewConfig{}, kNoLimit, rows),
        Status::NotFound);

    // A row cannot be inserted into a database that does not exist, and a row
    // cannot be moved between databases by an update.
    Row foreign = uapp::test::RowBuilder(100).title("x").build();
    foreign.database_id = fixture_id<DatabaseId>(4242);
    UAPP_REQUIRE_STATUS(h.dbs->insert_row(foreign, kNow), Status::NotFound);

    UAPP_REQUIRE_OK(h.dbs->insert_row(uapp::test::RowBuilder(100).title("x").build(), kNow));
    Row moved = uapp::test::RowBuilder(100).title("x").build();
    moved.database_id = fixture_id<DatabaseId>(4242);
    UAPP_REQUIRE_STATUS(h.dbs->update_row(moved, kNow), Status::InvalidArgument);

    UAPP_REQUIRE_STATUS(h.dbs->insert_row(uapp::test::RowBuilder(100).title("y").build(),
                                          kNow),
                        Status::AlreadyExists);
    UAPP_REQUIRE_STATUS(h.dbs->delete_row(fixture_id<RowId>(4242), kNow), Status::NotFound);
    UAPP_REQUIRE_STATUS(h.dbs->insert_row(uapp::test::RowBuilder(101).title("z").build(), -1),
                        Status::InvalidArgument);
}

UAPP_TEST("database.bounds: deleting the host page takes its databases with it") {
    Harness h;
    UAPP_REQUIRE(h.dir.valid());
    UAPP_REQUIRE_OK(h.open_with_page());
    const Database db = uapp::test::task_database();
    UAPP_REQUIRE_OK(seed(h, db, corpus()));

    // The anchor is a real foreign key with ON DELETE CASCADE, so a database
    // cannot outlive the page it lives on -- which is the lifetime ADR-0004
    // chose over making a database its own top-level object.
    UAPP_REQUIRE_OK(h.blocks->delete_page(
        fixture_id<uapp::blocks::PageId>(uapp::test::kFixturePage)));

    std::size_t count = 999;
    UAPP_REQUIRE_OK(h.dbs->database_count(count));
    UAPP_CHECK_MSG(count == 0, std::string("the database outlived its page: ") +
                                   std::to_string(count));
}
