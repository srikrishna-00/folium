// SPDX-License-Identifier: Apache-2.0
//
// End-to-end tests for the Phase 1 vertical slice against a real SQLite
// database:  CREATE -> STORE -> SEARCH -> OPEN -> EDIT -> SAVE.
//
// These are integration tests in substance (domain + adapter + SQLite) but
// they run in the unit-test binary because they are fast and hermetic: each
// case gets its own scratch directory and its own database file.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "uapp/notes/note.h"
#include "uapp/notes/note_service.h"
#include "uapp/notes/note_store.h"
#include "uapp/notes/ports.h"
#include "uapp/status.h"
#include "uapp/storage/sqlite/sqlite_note_store.h"

#include "test_fixtures.h"
#include "test_framework.h"

using uapp::Status;
using uapp::notes::Note;
using uapp::notes::NoteId;
using uapp::notes::NoteService;
using uapp::notes::NoteStore;
using uapp::notes::SearchHit;
using uapp::storage::sqlite::open_sqlite_note_store;
using uapp::storage::sqlite::SqliteNoteStoreOptions;
using uapp::test::CountingIdGenerator;
using uapp::test::FakeClock;
using uapp::test::TempDir;

namespace {

constexpr std::int64_t kT0 = 1'700'000'000'000;  // a fixed, plausible epoch ms

Status open_store(const std::string& path, bool read_only,
                  std::unique_ptr<NoteStore>& out) {
    SqliteNoteStoreOptions opts;
    opts.path = path;
    opts.read_only = read_only;
    return open_sqlite_note_store(opts, out);
}

}  // namespace

// ---------------------------------------------------------------------------
// CREATE + STORE
// ---------------------------------------------------------------------------

UAPP_TEST("slice.create stores a note with stamped id and timestamps") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());

    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));

    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    Note created;
    UAPP_REQUIRE_OK(svc.create("Meeting notes", "Discuss the storage layer.", created));

    UAPP_CHECK(!created.id.is_nil());
    UAPP_CHECK(created.title == "Meeting notes");
    UAPP_CHECK(created.body == "Discuss the storage layer.");
    UAPP_CHECK(created.created_at_ms == kT0);
    UAPP_CHECK(created.modified_at_ms == kT0);

    std::size_t n = 0;
    UAPP_REQUIRE_OK(svc.count(n));
    UAPP_CHECK(n == 1);
}

UAPP_TEST("slice.create allocates a distinct id per note") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));

    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    Note a;
    Note b;
    UAPP_REQUIRE_OK(svc.create("A", "first", a));
    UAPP_REQUIRE_OK(svc.create("B", "second", b));
    UAPP_CHECK(a.id != b.id);

    std::size_t n = 0;
    UAPP_REQUIRE_OK(svc.count(n));
    UAPP_CHECK(n == 2);
}

// ---------------------------------------------------------------------------
// PERSIST AND RELOAD — the note must survive the process, not just the handle
// ---------------------------------------------------------------------------

UAPP_TEST("slice.note survives closing and reopening the database") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    const std::string db = dir.file("notes.db");

    NoteId id;
    {
        std::unique_ptr<NoteStore> store;
        UAPP_REQUIRE_OK(open_store(db, false, store));
        FakeClock clock(kT0);
        CountingIdGenerator ids;
        NoteService svc(*store, clock, ids);

        Note created;
        UAPP_REQUIRE_OK(svc.create("Persisted", "This text must outlive the connection.",
                                   created));
        id = created.id;
    }  // store destroyed: connection closed, WAL checkpointed on close

    {
        std::unique_ptr<NoteStore> store;
        UAPP_REQUIRE_OK(open_store(db, false, store));
        FakeClock clock(kT0 + 999);
        CountingIdGenerator ids;
        NoteService svc(*store, clock, ids);

        Note loaded;
        UAPP_REQUIRE_OK(svc.open(id, loaded));
        UAPP_CHECK(loaded.id == id);
        UAPP_CHECK(loaded.title == "Persisted");
        UAPP_CHECK(loaded.body == "This text must outlive the connection.");
        UAPP_CHECK(loaded.created_at_ms == kT0);
        UAPP_CHECK(loaded.modified_at_ms == kT0);
    }
}

UAPP_TEST("slice.unicode content round-trips byte-for-byte") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));
    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    const std::string title = "\xe4\xbc\x9a\xe8\xad\xb0\xe3\x83\xa1\xe3\x83\xa2";  // 会議メモ
    const std::string body =
        "caf\xc3\xa9 \xe2\x80\x94 na\xc3\xafve \xf0\x9f\x93\x9d r\xc3\xa9sum\xc3\xa9";

    Note created;
    UAPP_REQUIRE_OK(svc.create(title, body, created));

    Note loaded;
    UAPP_REQUIRE_OK(svc.open(created.id, loaded));
    UAPP_CHECK(loaded.title == title);
    UAPP_CHECK(loaded.body == body);
}

// ---------------------------------------------------------------------------
// SEARCH — hit and miss
// ---------------------------------------------------------------------------

UAPP_TEST("slice.search finds a note by a body term") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));
    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    Note target;
    Note other;
    UAPP_REQUIRE_OK(svc.create("Architecture review",
                               "We settled on SQLite with FTS5 for local search.", target));
    UAPP_REQUIRE_OK(svc.create("Lunch", "Tacos on Friday.", other));

    std::vector<SearchHit> hits;
    UAPP_REQUIRE_OK(svc.search("SQLite", hits));
    UAPP_REQUIRE(hits.size() == 1);
    UAPP_CHECK(hits[0].id == target.id);
    UAPP_CHECK(hits[0].title == "Architecture review");
    UAPP_CHECK_MSG(!hits[0].snippet.empty(), "snippet should quote the matching text");
}

UAPP_TEST("slice.search finds a note by a title term") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));
    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    Note target;
    UAPP_REQUIRE_OK(svc.create("Onboarding checklist", "Nothing relevant here.", target));

    std::vector<SearchHit> hits;
    UAPP_REQUIRE_OK(svc.search("onboarding", hits));  // case-insensitive
    UAPP_REQUIRE(hits.size() == 1);
    UAPP_CHECK(hits[0].id == target.id);
}

UAPP_TEST("slice.search miss returns OK with zero hits, not NotFound") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));
    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    Note n;
    UAPP_REQUIRE_OK(svc.create("Shopping", "Bread and butter.", n));

    std::vector<SearchHit> hits;
    // "no results" is a successful search. Conflating it with an error is how
    // UIs end up showing a scary dialog for an ordinary empty result.
    UAPP_REQUIRE_OK(svc.search("helicopter", hits));
    UAPP_CHECK(hits.empty());
}

UAPP_TEST("slice.search matches a prefix of the last term") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));
    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    Note n;
    UAPP_REQUIRE_OK(svc.create("Deployment", "Kubernetes rollout plan.", n));

    std::vector<SearchHit> hits;
    UAPP_REQUIRE_OK(svc.search("kuber", hits));  // search-as-you-type
    UAPP_REQUIRE(hits.size() == 1);
    UAPP_CHECK(hits[0].id == n.id);
}

UAPP_TEST("slice.search is unaffected by deleted notes") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));
    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    Note doomed;
    UAPP_REQUIRE_OK(svc.create("Temporary", "ephemeral marker word zebrafish", doomed));

    std::vector<SearchHit> before;
    UAPP_REQUIRE_OK(svc.search("zebrafish", before));
    UAPP_REQUIRE(before.size() == 1);

    UAPP_REQUIRE_OK(svc.remove(doomed.id));

    // The index entry must go with the row. A stale index would return a hit
    // whose note cannot be opened -- a broken search result in the UI.
    std::vector<SearchHit> after;
    UAPP_REQUIRE_OK(svc.search("zebrafish", after));
    UAPP_CHECK_MSG(after.empty(), "FTS index kept a row for a deleted note");
}

// ---------------------------------------------------------------------------
// OPEN -> EDIT -> SAVE
// ---------------------------------------------------------------------------

UAPP_TEST("slice.edit then reload reflects the edit") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    const std::string db = dir.file("notes.db");

    NoteId id;
    {
        std::unique_ptr<NoteStore> store;
        UAPP_REQUIRE_OK(open_store(db, false, store));
        FakeClock clock(kT0);
        CountingIdGenerator ids;
        NoteService svc(*store, clock, ids);

        Note created;
        UAPP_REQUIRE_OK(svc.create("Draft", "Original body.", created));
        id = created.id;

        Note opened;
        UAPP_REQUIRE_OK(svc.open(id, opened));

        opened.title = "Final";
        opened.body  = "Rewritten body with a marker: chrysanthemum.";
        clock.advance(60'000);

        UAPP_REQUIRE_OK(svc.save(opened));
        UAPP_CHECK(opened.modified_at_ms == kT0 + 60'000);
        UAPP_CHECK_MSG(opened.created_at_ms == kT0,
                       "save must not move created_at");
    }

    {
        std::unique_ptr<NoteStore> store;
        UAPP_REQUIRE_OK(open_store(db, false, store));
        FakeClock clock(kT0);
        CountingIdGenerator ids;
        NoteService svc(*store, clock, ids);

        Note reloaded;
        UAPP_REQUIRE_OK(svc.open(id, reloaded));
        UAPP_CHECK(reloaded.title == "Final");
        UAPP_CHECK(reloaded.body == "Rewritten body with a marker: chrysanthemum.");
        UAPP_CHECK(reloaded.created_at_ms == kT0);
        UAPP_CHECK(reloaded.modified_at_ms == kT0 + 60'000);

        // The search index must have been rewritten in the same transaction.
        std::vector<SearchHit> hits;
        UAPP_REQUIRE_OK(svc.search("chrysanthemum", hits));
        UAPP_CHECK_MSG(hits.size() == 1, "edited body was not reindexed");

        std::vector<SearchHit> stale;
        UAPP_REQUIRE_OK(svc.search("Original", stale));
        UAPP_CHECK_MSG(stale.empty(), "FTS index still contains the pre-edit body");
    }
}

UAPP_TEST("slice.save refuses to let a caller rewrite created_at") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));
    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    Note n;
    UAPP_REQUIRE_OK(svc.create("Audit", "body", n));

    n.created_at_ms = 1;  // a doctored value from an untrusted client
    n.title = "Audit (edited)";
    clock.advance(10);
    UAPP_REQUIRE_OK(svc.save(n));
    UAPP_CHECK(n.created_at_ms == kT0);

    Note reloaded;
    UAPP_REQUIRE_OK(svc.open(n.id, reloaded));
    UAPP_CHECK(reloaded.created_at_ms == kT0);
}

// ---------------------------------------------------------------------------
// ERROR PATHS
// ---------------------------------------------------------------------------

UAPP_TEST("error.open a nonexistent note id returns NOT_FOUND") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));
    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    NoteId absent;
    UAPP_REQUIRE_OK(NoteId::parse("deadbeefdeadbeefdeadbeefdeadbeef", absent));

    Note out;
    UAPP_REQUIRE_STATUS(svc.open(absent, out), Status::NotFound);
    UAPP_REQUIRE_STATUS(svc.remove(absent), Status::NotFound);
}

UAPP_TEST("error.open with a nil id is rejected before touching the store") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));
    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    Note out;
    UAPP_REQUIRE_STATUS(svc.open(NoteId(), out), Status::NoteIdMalformed);
}

UAPP_TEST("error.save a note that no longer exists returns NOT_FOUND") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));
    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    Note n;
    UAPP_REQUIRE_OK(svc.create("Doomed", "body", n));
    UAPP_REQUIRE_OK(svc.remove(n.id));

    n.body = "edited after deletion";
    // The important part is that this is an explicit failure and NOT a silent
    // resurrection of the note.
    UAPP_REQUIRE_STATUS(svc.save(n), Status::NotFound);

    std::size_t count = 0;
    UAPP_REQUIRE_OK(svc.count(count));
    UAPP_CHECK(count == 0);
}

UAPP_TEST("error.oversized title is rejected and nothing is written") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));
    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    Note out;
    const std::string huge(uapp::notes::kMaxTitleBytes + 1, 't');
    UAPP_REQUIRE_STATUS(svc.create(huge, "body", out), Status::NoteTitleTooLong);

    std::size_t count = 0;
    UAPP_REQUIRE_OK(svc.count(count));
    UAPP_CHECK_MSG(count == 0, "a rejected create must leave no row behind");
}

UAPP_TEST("error.DB write failure surfaces as STORE_READ_ONLY, not silent loss") {
    // A deterministic stand-in for "the database cannot be written": open the
    // same file read-only. Exercises the same failure branch a full disk or a
    // revoked file permission would take.
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    const std::string db = dir.file("notes.db");

    NoteId existing;
    {
        std::unique_ptr<NoteStore> writable;
        UAPP_REQUIRE_OK(open_store(db, false, writable));
        FakeClock clock(kT0);
        CountingIdGenerator ids;
        NoteService svc(*writable, clock, ids);
        Note n;
        UAPP_REQUIRE_OK(svc.create("Existing", "readable body", n));
        existing = n.id;
    }

    std::unique_ptr<NoteStore> ro;
    UAPP_REQUIRE_OK(open_store(db, true, ro));
    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*ro, clock, ids);

    // Reads still work...
    Note loaded;
    UAPP_REQUIRE_OK(svc.open(existing, loaded));
    UAPP_CHECK(loaded.title == "Existing");

    std::vector<SearchHit> hits;
    UAPP_REQUIRE_OK(svc.search("readable", hits));
    UAPP_CHECK(hits.size() == 1);

    // ...but every write is refused with a specific, actionable code.
    Note rejected;
    UAPP_REQUIRE_STATUS(svc.create("Nope", "body", rejected), Status::StoreReadOnly);
    loaded.title = "Renamed";
    UAPP_REQUIRE_STATUS(svc.save(loaded), Status::StoreReadOnly);
    UAPP_REQUIRE_STATUS(svc.remove(existing), Status::StoreReadOnly);

    // And the store really is unchanged.
    Note recheck;
    UAPP_REQUIRE_OK(svc.open(existing, recheck));
    UAPP_CHECK(recheck.title == "Existing");
}

UAPP_TEST("error.opening a database at an unwritable path fails cleanly") {
    std::unique_ptr<NoteStore> store;
    // A directory that does not exist. SQLite cannot create the file, and we
    // must report it rather than hand back a half-initialised store.
    const Status s = open_store("/uapp-nonexistent-dir-9f3a/notes.db", false, store);
    UAPP_REQUIRE_STATUS(s, Status::StoreOpenFailed);
    UAPP_CHECK_MSG(store == nullptr, "failed open must not populate the out-parameter");
}

UAPP_TEST("error.empty database path is rejected") {
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_STATUS(open_store("", false, store), Status::InvalidArgument);
    UAPP_CHECK(store == nullptr);
}

UAPP_TEST("error.search with no searchable terms returns SEARCH_QUERY_EMPTY") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));
    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    Note n;
    UAPP_REQUIRE_OK(svc.create("Anything", "anything at all", n));

    std::vector<SearchHit> hits;
    UAPP_REQUIRE_STATUS(svc.search("", hits), Status::SearchQueryEmpty);
    UAPP_REQUIRE_STATUS(svc.search("   ", hits), Status::SearchQueryEmpty);
    UAPP_REQUIRE_STATUS(svc.search("?!?", hits), Status::SearchQueryEmpty);
    UAPP_CHECK(hits.empty());
}

UAPP_TEST("error.hostile search input cannot break the query or the table") {
    // The point of this case is that none of these return SEARCH_FAILED (an
    // FTS5 syntax error) and the store is still usable afterwards.
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));
    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    Note n;
    UAPP_REQUIRE_OK(svc.create("Survivor", "this note must still be here", n));

    const char* hostile[] = {
        "'; DROP TABLE note; --",
        "\" OR \"\"=\"",
        "NEAR(a b, 10)",
        "note_fts MATCH 'x'",
        "^^^",
        "a AND (b OR",
        "*a*",
    };
    for (const char* q : hostile) {
        std::vector<SearchHit> hits;
        const Status s = svc.search(q, hits);
        UAPP_CHECK_MSG(s == Status::Ok || s == Status::SearchQueryEmpty,
                       std::string("query \"") + q + "\" returned " + uapp::status_code(s));
    }

    std::size_t count = 0;
    UAPP_REQUIRE_OK(svc.count(count));
    UAPP_CHECK_MSG(count == 1, "the note table did not survive hostile search input");

    Note still_there;
    UAPP_REQUIRE_OK(svc.open(n.id, still_there));
    UAPP_CHECK(still_there.title == "Survivor");
}

UAPP_TEST("error.an oversized search query is rejected rather than truncated") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));
    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    std::vector<SearchHit> hits;
    const std::string too_long(uapp::notes::kMaxQueryBytes + 1, 'q');
    UAPP_REQUIRE_STATUS(svc.search(too_long, hits), Status::InvalidArgument);
    UAPP_REQUIRE_STATUS(svc.search("valid", hits, 0), Status::InvalidArgument);
}

UAPP_TEST("error.id generator failure aborts create without writing") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));
    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    ids.fail_next();
    Note out;
    UAPP_REQUIRE_STATUS(svc.create("Title", "body", out), Status::Internal);

    std::size_t count = 0;
    UAPP_REQUIRE_OK(svc.count(count));
    UAPP_CHECK(count == 0);

    // ...and the service recovers on the next call.
    UAPP_REQUIRE_OK(svc.create("Title", "body", out));
    UAPP_REQUIRE_OK(svc.count(count));
    UAPP_CHECK(count == 1);
}

UAPP_TEST("error.inserting a duplicate id is refused by the store") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));
    FakeClock clock(kT0);
    CountingIdGenerator ids;
    NoteService svc(*store, clock, ids);

    Note first;
    UAPP_REQUIRE_OK(svc.create("One", "body", first));

    // Go under the service and re-insert the same id, as a buggy caller or a
    // future sync path might. The UNIQUE constraint must catch it.
    UAPP_REQUIRE_STATUS(store->insert(first), Status::AlreadyExists);

    std::size_t count = 0;
    UAPP_REQUIRE_OK(svc.count(count));
    UAPP_CHECK(count == 1);
}

// ---------------------------------------------------------------------------
// The default (non-fake) ports still work — otherwise every test above could
// pass while the shipping configuration is broken.
// ---------------------------------------------------------------------------

UAPP_TEST("ports.system clock and random ids produce a usable note") {
    TempDir dir;
    UAPP_REQUIRE(dir.valid());
    std::unique_ptr<NoteStore> store;
    UAPP_REQUIRE_OK(open_store(dir.file("notes.db"), false, store));

    auto clock = uapp::notes::make_system_clock();
    auto ids   = uapp::notes::make_random_id_generator();
    UAPP_REQUIRE(clock != nullptr);
    UAPP_REQUIRE(ids != nullptr);

    NoteService svc(*store, *clock, *ids);

    Note a;
    Note b;
    UAPP_REQUIRE_OK(svc.create("Real clock", "body one", a));
    UAPP_REQUIRE_OK(svc.create("Real clock", "body two", b));

    UAPP_CHECK(!a.id.is_nil());
    UAPP_CHECK(a.id != b.id);
    UAPP_CHECK_MSG(a.created_at_ms > 1'600'000'000'000,
                   "system clock returned an implausible timestamp");
    UAPP_CHECK(a.created_at_ms == a.modified_at_ms);
}
