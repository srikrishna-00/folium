// SPDX-License-Identifier: Apache-2.0
//
// Integration tests for the UNIFIED search layer (spec section 16): one query
// string across notes and pages/blocks, against the real SQLite/FTS5 adapter.
//
// These are not unit tests of the MATCH builder -- tests/storage/fts_query_test.cpp
// already covers that in isolation. What is under test here is the property
// that only exists once the pieces are assembled:
//
//   * a hit is correctly TYPED and carries the ids needed to open it,
//   * the index cannot go stale behind a content edit or delete,
//   * and the unified entry point does not reopen the FTS5 injection surface
//     that fts_query.cpp already closed.
//
// EVERY TEST USES A REAL FILE, NOT ":memory:". Each sqlite3_open of
// ":memory:" makes a PRIVATE database, so a note store and a block store both
// opened on ":memory:" would be two databases that cannot see each other's
// index rows -- and the symptom would be zero results with no error. One
// scratch file per test, shared by all three handles, is what "unified"
// actually means here. See sqlite_search_index.h.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common_fixtures.h"
#include "test_framework.h"
#include "uapp/blocks/block.h"
#include "uapp/blocks/block_store.h"
#include "uapp/blocks/page.h"
#include "uapp/notes/note.h"
#include "uapp/notes/note_store.h"
#include "uapp/object_id.h"
#include "uapp/search/search_index.h"
#include "uapp/search/search_result.h"
#include "uapp/status.h"
#include "uapp/storage/sqlite/sqlite_block_store.h"
#include "uapp/storage/sqlite/sqlite_note_store.h"
#include "uapp/storage/sqlite/sqlite_search_index.h"

namespace {

using uapp::ObjectIdBytes;
using uapp::Status;
using uapp::blocks::BlockContent;
using uapp::blocks::BlockId;
using uapp::blocks::BlockStore;
using uapp::blocks::Page;
using uapp::blocks::PageId;
using uapp::notes::Note;
using uapp::notes::NoteId;
using uapp::notes::NoteStore;
using uapp::search::SearchIndex;
using uapp::search::SearchResult;
using uapp::search::SourceKind;

/// A note store, a block store and a search index, all on ONE database file.
///
/// Non-throwing throughout: every handle is left null when its open fails and
/// `open()` returns the status, which each test REQUIREs.
struct Harness final {
    uapp::test::TempDir          dir;
    std::unique_ptr<NoteStore>   notes;
    std::unique_ptr<BlockStore>  blocks;
    std::unique_ptr<SearchIndex> index;

    [[nodiscard]] Status open() {
        if (!dir.valid()) { return Status::StoreOpenFailed; }
        const std::string path = dir.file("unified.db");

        uapp::storage::sqlite::SqliteNoteStoreOptions note_options;
        note_options.path = path;
        if (const Status s =
                uapp::storage::sqlite::open_sqlite_note_store(note_options, notes);
            !uapp::is_ok(s)) {
            return s;
        }

        uapp::storage::sqlite::SqliteBlockStoreOptions block_options;
        block_options.path = path;
        if (const Status s =
                uapp::storage::sqlite::open_sqlite_block_store(block_options, blocks);
            !uapp::is_ok(s)) {
            return s;
        }

        // Read-only by default, which is the point: this port cannot write,
        // so the connection behind it holds no write permission either.
        uapp::storage::sqlite::SqliteSearchIndexOptions index_options;
        index_options.path = path;
        return uapp::storage::sqlite::open_sqlite_search_index(index_options, index);
    }

    [[nodiscard]] bool ready() const {
        return notes != nullptr && blocks != nullptr && index != nullptr;
    }
};

/// A deterministic id with `n` in its last byte. Sequential ids also make
/// failure output readable.
ObjectIdBytes id_bytes(std::uint8_t n) {
    ObjectIdBytes raw{};
    raw[15] = n;
    return raw;
}

Note make_note(std::uint8_t n, std::string title, std::string body) {
    Note note;
    note.id             = NoteId(id_bytes(n));
    note.title          = std::move(title);
    note.body           = std::move(body);
    note.created_at_ms  = 1'700'000'000'000;
    note.modified_at_ms = 1'700'000'000'000;
    return note;
}

BlockContent para(std::string text) {
    return uapp::blocks::ParagraphBlock{std::move(text)};
}

/// How many results have this kind.
std::size_t count_kind(const std::vector<SearchResult>& results, SourceKind kind) {
    std::size_t n = 0;
    for (const SearchResult& r : results) {
        if (r.kind == kind) { ++n; }
    }
    return n;
}

/// Find the single result of `kind`, or nullptr.
const SearchResult* find_kind(const std::vector<SearchResult>& results, SourceKind kind) {
    for (const SearchResult& r : results) {
        if (r.kind == kind) { return &r; }
    }
    return nullptr;
}

bool has_source_id(const std::vector<SearchResult>& results, const ObjectIdBytes& id) {
    for (const SearchResult& r : results) {
        if (r.source_id == id) { return true; }
    }
    return false;
}

/// Readable failure output: "note:…0001, block:…0003" rather than nothing.
/// Only the last two bytes are printed; the test ids differ only there, and
/// thirty leading zeroes per hit makes the message unreadable.
std::string describe(const std::vector<SearchResult>& results) {
    static const char* const kHex = "0123456789abcdef";
    std::string out;
    for (const SearchResult& r : results) {
        if (!out.empty()) { out += ", "; }
        out += uapp::search::source_kind_name(r.kind);
        out += ":\xe2\x80\xa6";
        for (std::size_t i = r.source_id.size() - 2; i < r.source_id.size(); ++i) {
            out += kHex[(r.source_id[i] >> 4) & 0x0Fu];
            out += kHex[r.source_id[i] & 0x0Fu];
        }
    }
    return out.empty() ? std::string("<no results>") : out;
}

}  // namespace

// --- the basic cross-type contract -----------------------------------------

UAPP_TEST("search.unified: a note-only term returns exactly one typed note hit") {
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    const Note note = make_note(1, "Q3 planning", "the quarterly perihelion budget");
    UAPP_REQUIRE_OK(h.notes->insert(note));

    // A page whose text shares no term with the note, so a leak would show.
    const PageId page_id(id_bytes(2));
    Page page;
    page.id             = page_id;
    page.title          = "Engineering";
    page.created_at_ms  = 1'700'000'000'000;
    page.modified_at_ms = 1'700'000'000'000;
    UAPP_REQUIRE_OK(h.blocks->create_page(page));
    UAPP_REQUIRE_OK(h.blocks->insert_block(page_id, BlockId(), 0, BlockId(id_bytes(3)),
                                           para("unrelated sprint notes"),
                                           1'700'000'000'000));

    std::vector<SearchResult> results;
    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));

    UAPP_REQUIRE_MSG(results.size() == 1, describe(results));
    UAPP_CHECK(results[0].kind == SourceKind::Note);
    UAPP_CHECK(results[0].source_id == note.id.bytes());
    // A note is its own container, so there is no second id to follow.
    UAPP_CHECK(uapp::detail::object_id_is_nil(results[0].container_id));
    UAPP_CHECK_MSG(results[0].title == "Q3 planning", "got: " + results[0].title);
    // The snippet must actually show the match, delimited.
    UAPP_CHECK_MSG(results[0].snippet.find("[perihelion]") != std::string::npos,
                   "got: " + results[0].snippet);
}

UAPP_TEST("search.unified: a block-only term returns a block hit carrying its page id") {
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    UAPP_REQUIRE_OK(h.notes->insert(make_note(1, "Shopping", "milk and bread")));

    const PageId  page_id(id_bytes(2));
    const BlockId block_id(id_bytes(3));
    Page page;
    page.id             = page_id;
    page.title          = "Roadmap 2027";
    page.created_at_ms  = 1'700'000'000'000;
    page.modified_at_ms = 1'700'000'000'000;
    UAPP_REQUIRE_OK(h.blocks->create_page(page));
    UAPP_REQUIRE_OK(h.blocks->insert_block(page_id, BlockId(), 0, block_id,
                                           para("ship the perihelion milestone"),
                                           1'700'000'000'000));

    std::vector<SearchResult> results;
    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));

    UAPP_REQUIRE_MSG(results.size() == 1, describe(results));
    const SearchResult& hit = results[0];
    UAPP_CHECK(hit.kind == SourceKind::Block);
    // BOTH ids: the block to scroll to, and the page to open. This is the
    // whole argument for block-level granularity -- a page-level index could
    // not produce the first one.
    UAPP_CHECK(hit.source_id == block_id.bytes());
    UAPP_CHECK(hit.container_id == page_id.bytes());
    // And the containing page's title, so a result row is presentable
    // without a second query per hit.
    UAPP_CHECK_MSG(hit.title == "Roadmap 2027", "got: " + hit.title);
    UAPP_CHECK_MSG(hit.snippet.find("[perihelion]") != std::string::npos,
                   "got: " + hit.snippet);
}

UAPP_TEST("search.unified: a page title is findable as its own typed hit") {
    // A page title lives in no block, so without a per-page entry searching
    // for a page by name would silently fail.
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    const PageId page_id(id_bytes(2));
    Page page;
    page.id             = page_id;
    page.title          = "Perihelion retrospective";
    page.created_at_ms  = 1'700'000'000'000;
    page.modified_at_ms = 1'700'000'000'000;
    UAPP_REQUIRE_OK(h.blocks->create_page(page));

    std::vector<SearchResult> results;
    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));

    UAPP_REQUIRE_MSG(results.size() == 1, describe(results));
    UAPP_CHECK(results[0].kind == SourceKind::Page);
    UAPP_CHECK(results[0].source_id == page_id.bytes());
    UAPP_CHECK(uapp::detail::object_id_is_nil(results[0].container_id));
    UAPP_CHECK_MSG(results[0].snippet.find("[Perihelion]") != std::string::npos,
                   "got: " + results[0].snippet);
}

UAPP_TEST("search.unified: one query returns hits from BOTH stores, each correctly typed") {
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    const Note note = make_note(1, "Launch checklist", "confirm the perihelion date");
    UAPP_REQUIRE_OK(h.notes->insert(note));

    const PageId  page_id(id_bytes(2));
    const BlockId block_id(id_bytes(3));
    Page page;
    page.id             = page_id;
    page.title          = "Engineering";
    page.created_at_ms  = 1'700'000'000'000;
    page.modified_at_ms = 1'700'000'000'000;
    UAPP_REQUIRE_OK(h.blocks->create_page(page));
    UAPP_REQUIRE_OK(h.blocks->insert_block(page_id, BlockId(), 0, block_id,
                                           para("perihelion needs a rehearsal"),
                                           1'700'000'000'000));

    std::vector<SearchResult> results;
    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));

    UAPP_REQUIRE_MSG(results.size() == 2, describe(results));
    UAPP_CHECK_MSG(count_kind(results, SourceKind::Note) == 1, describe(results));
    UAPP_CHECK_MSG(count_kind(results, SourceKind::Block) == 1, describe(results));
    UAPP_CHECK(has_source_id(results, note.id.bytes()));
    UAPP_CHECK(has_source_id(results, block_id.bytes()));

    const SearchResult* note_hit  = find_kind(results, SourceKind::Note);
    const SearchResult* block_hit = find_kind(results, SourceKind::Block);
    UAPP_REQUIRE(note_hit != nullptr && block_hit != nullptr);

    // Both ranks come from ONE bm25 over ONE table, which is the reason for
    // the single-table design: scores from two separate FTS5 indexes would
    // not be on the same scale and this comparison would be meaningless.
    UAPP_CHECK_MSG(note_hit->rank < 0.0,
                   "bm25 is negative-is-better; got " + std::to_string(note_hit->rank));
    UAPP_CHECK_MSG(block_hit->rank < 0.0,
                   "bm25 is negative-is-better; got " + std::to_string(block_hit->rank));
    // Results arrive best-first.
    for (std::size_t i = 1; i < results.size(); ++i) {
        UAPP_CHECK_MSG(results[i - 1].rank <= results[i].rank, "results are not ranked");
    }
}

UAPP_TEST("search.unified: results are limited and the limit is validated") {
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    const PageId page_id(id_bytes(2));
    Page page;
    page.id             = page_id;
    page.title          = "Notebook";
    page.created_at_ms  = 1'700'000'000'000;
    page.modified_at_ms = 1'700'000'000'000;
    UAPP_REQUIRE_OK(h.blocks->create_page(page));
    for (std::uint8_t i = 0; i < 5; ++i) {
        UAPP_REQUIRE_OK(h.blocks->insert_block(
            page_id, BlockId(), uapp::blocks::kAppendPosition,
            BlockId(id_bytes(static_cast<std::uint8_t>(10 + i))),
            para("perihelion entry"), 1'700'000'000'000));
    }

    std::vector<SearchResult> results;
    UAPP_REQUIRE_OK(h.index->query("perihelion", 2, results));
    UAPP_CHECK_MSG(results.size() == 2, describe(results));

    // A zero limit is a caller bug, reported rather than answered with an
    // empty list that looks like "nothing matched".
    UAPP_REQUIRE_STATUS(h.index->query("perihelion", 0, results),
                        Status::InvalidArgument);
    UAPP_CHECK_MSG(results.empty(), "out must be cleared even on the failure path");
}

// --- staleness: edits -------------------------------------------------------

UAPP_TEST("search.unified: editing a note replaces what search finds, with no stale hit") {
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    Note note = make_note(1, "Draft", "the perihelion plan");
    UAPP_REQUIRE_OK(h.notes->insert(note));

    std::vector<SearchResult> results;
    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));
    UAPP_REQUIRE_MSG(results.size() == 1, describe(results));

    note.body           = "the aphelion plan";
    note.modified_at_ms = 1'700'000'001'000;
    UAPP_REQUIRE_OK(h.notes->update(note));

    // The OLD term must be gone. This is the assertion that catches an
    // index-append-instead-of-replace bug, which otherwise looks fine.
    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));
    UAPP_CHECK_MSG(results.empty(), describe(results));

    UAPP_REQUIRE_OK(h.index->query("aphelion", 10, results));
    UAPP_REQUIRE_MSG(results.size() == 1, describe(results));
    UAPP_CHECK(results[0].kind == SourceKind::Note);
    UAPP_CHECK(results[0].source_id == note.id.bytes());

    // And exactly one entry, not two: a replace, not an append.
    std::size_t entries = 0;
    UAPP_REQUIRE_OK(h.index->entry_count(entries));
    UAPP_CHECK_MSG(entries == 1, "entry_count = " + std::to_string(entries));
}

UAPP_TEST("search.unified: editing a block replaces what search finds, with no stale hit") {
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    const PageId  page_id(id_bytes(2));
    const BlockId block_id(id_bytes(3));
    Page page;
    page.id             = page_id;
    page.title          = "Engineering";
    page.created_at_ms  = 1'700'000'000'000;
    page.modified_at_ms = 1'700'000'000'000;
    UAPP_REQUIRE_OK(h.blocks->create_page(page));
    UAPP_REQUIRE_OK(h.blocks->insert_block(page_id, BlockId(), 0, block_id,
                                           para("the perihelion plan"),
                                           1'700'000'000'000));

    std::vector<SearchResult> results;
    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));
    UAPP_REQUIRE_MSG(results.size() == 1, describe(results));

    UAPP_REQUIRE_OK(h.blocks->update_block(block_id, para("the aphelion plan"),
                                           1'700'000'001'000));

    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));
    UAPP_CHECK_MSG(results.empty(), describe(results));

    UAPP_REQUIRE_OK(h.index->query("aphelion", 10, results));
    UAPP_REQUIRE_MSG(results.size() == 1, describe(results));
    UAPP_CHECK(results[0].source_id == block_id.bytes());

    // page entry + one block entry == 2. A third would mean the edit appended.
    std::size_t entries = 0;
    UAPP_REQUIRE_OK(h.index->entry_count(entries));
    UAPP_CHECK_MSG(entries == 2, "entry_count = " + std::to_string(entries));
}

UAPP_TEST("search.unified: renaming a page replaces its title hit and relabels its blocks") {
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    const PageId  page_id(id_bytes(2));
    const BlockId block_id(id_bytes(3));
    Page page;
    page.id             = page_id;
    page.title          = "Perihelion retrospective";
    page.created_at_ms  = 1'700'000'000'000;
    page.modified_at_ms = 1'700'000'000'000;
    UAPP_REQUIRE_OK(h.blocks->create_page(page));
    UAPP_REQUIRE_OK(h.blocks->insert_block(page_id, BlockId(), 0, block_id,
                                           para("a nondescript bullet"),
                                           1'700'000'000'000));

    UAPP_REQUIRE_OK(h.blocks->set_page_title(page_id, "Aphelion retrospective",
                                             1'700'000'001'000));

    std::vector<SearchResult> results;
    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));
    UAPP_CHECK_MSG(results.empty(), describe(results));

    UAPP_REQUIRE_OK(h.index->query("aphelion", 10, results));
    UAPP_REQUIRE_MSG(results.size() == 1, describe(results));
    UAPP_CHECK(results[0].kind == SourceKind::Page);

    // The block was never rewritten, yet it reports the NEW page title --
    // which is what the container join buys, and why a page rename is O(1).
    UAPP_REQUIRE_OK(h.index->query("nondescript", 10, results));
    UAPP_REQUIRE_MSG(results.size() == 1, describe(results));
    UAPP_CHECK(results[0].kind == SourceKind::Block);
    UAPP_CHECK_MSG(results[0].title == "Aphelion retrospective", "got: " + results[0].title);
}

UAPP_TEST("search.unified: changing a block to a divider removes it from the index") {
    // A divider has no text, so keeping an empty row would both inflate the
    // index and skew bm25's average-row-length weighting.
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    const PageId  page_id(id_bytes(2));
    const BlockId block_id(id_bytes(3));
    Page page;
    page.id             = page_id;
    page.title          = "Engineering";
    page.created_at_ms  = 1'700'000'000'000;
    page.modified_at_ms = 1'700'000'000'000;
    UAPP_REQUIRE_OK(h.blocks->create_page(page));
    UAPP_REQUIRE_OK(h.blocks->insert_block(page_id, BlockId(), 0, block_id,
                                           para("perihelion"), 1'700'000'000'000));

    UAPP_REQUIRE_OK(h.blocks->update_block(block_id, uapp::blocks::DividerBlock{},
                                           1'700'000'001'000));

    std::vector<SearchResult> results;
    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));
    UAPP_CHECK_MSG(results.empty(), describe(results));

    std::size_t entries = 0;
    UAPP_REQUIRE_OK(h.index->entry_count(entries));
    UAPP_CHECK_MSG(entries == 1, "only the page entry should remain; got " +
                                     std::to_string(entries));
}

// --- staleness: deletes -----------------------------------------------------

UAPP_TEST("search.unified: deleting a note removes it from search results") {
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    const Note note = make_note(1, "Draft", "the perihelion plan");
    UAPP_REQUIRE_OK(h.notes->insert(note));

    std::vector<SearchResult> results;
    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));
    UAPP_REQUIRE_MSG(results.size() == 1, describe(results));

    UAPP_REQUIRE_OK(h.notes->remove(note.id));

    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));
    UAPP_CHECK_MSG(results.empty(), describe(results));

    std::size_t entries = 0;
    UAPP_REQUIRE_OK(h.index->entry_count(entries));
    UAPP_CHECK_MSG(entries == 0, "entry_count = " + std::to_string(entries));
}

UAPP_TEST("search.unified: deleting a block removes it AND its subtree from search") {
    // delete_block cascades to the whole subtree. If the index walk ran after
    // the content delete instead of before it, the children's text would stay
    // matchable forever and nothing would report an error.
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    const PageId  page_id(id_bytes(2));
    const BlockId parent_id(id_bytes(3));
    const BlockId child_id(id_bytes(4));
    const BlockId sibling_id(id_bytes(5));
    Page page;
    page.id             = page_id;
    page.title          = "Engineering";
    page.created_at_ms  = 1'700'000'000'000;
    page.modified_at_ms = 1'700'000'000'000;
    UAPP_REQUIRE_OK(h.blocks->create_page(page));
    UAPP_REQUIRE_OK(h.blocks->insert_block(page_id, BlockId(), 0, parent_id,
                                           para("perihelion parent"),
                                           1'700'000'000'000));
    UAPP_REQUIRE_OK(h.blocks->insert_block(page_id, parent_id, 0, child_id,
                                           para("perihelion child"),
                                           1'700'000'000'000));
    UAPP_REQUIRE_OK(h.blocks->insert_block(page_id, BlockId(), 1, sibling_id,
                                           para("perihelion sibling"),
                                           1'700'000'000'000));

    std::vector<SearchResult> results;
    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));
    UAPP_REQUIRE_MSG(results.size() == 3, describe(results));

    UAPP_REQUIRE_OK(h.blocks->delete_block(parent_id, 1'700'000'001'000));

    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));
    UAPP_REQUIRE_MSG(results.size() == 1, describe(results));
    UAPP_CHECK_MSG(results[0].source_id == sibling_id.bytes(),
                   "the surviving sibling should be the only hit: " + describe(results));

    // The child must not be reachable by its own distinctive term either.
    UAPP_REQUIRE_OK(h.index->query("child", 10, results));
    UAPP_CHECK_MSG(results.empty(), describe(results));
}

UAPP_TEST("search.unified: deleting a page removes its title and every block on it") {
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    // A note that also matches, to prove the page delete is scoped: it must
    // not take unrelated content with it.
    const Note note = make_note(1, "Keep me", "perihelion in a note");
    UAPP_REQUIRE_OK(h.notes->insert(note));

    const PageId page_id(id_bytes(2));
    Page page;
    page.id             = page_id;
    page.title          = "Perihelion board";
    page.created_at_ms  = 1'700'000'000'000;
    page.modified_at_ms = 1'700'000'000'000;
    UAPP_REQUIRE_OK(h.blocks->create_page(page));
    UAPP_REQUIRE_OK(h.blocks->insert_block(page_id, BlockId(), 0, BlockId(id_bytes(3)),
                                           para("perihelion one"), 1'700'000'000'000));
    UAPP_REQUIRE_OK(h.blocks->insert_block(page_id, BlockId(), 1, BlockId(id_bytes(4)),
                                           para("perihelion two"), 1'700'000'000'000));

    std::vector<SearchResult> results;
    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));
    UAPP_REQUIRE_MSG(results.size() == 4, describe(results));

    UAPP_REQUIRE_OK(h.blocks->delete_page(page_id));

    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));
    UAPP_REQUIRE_MSG(results.size() == 1, describe(results));
    UAPP_CHECK(results[0].kind == SourceKind::Note);
    UAPP_CHECK(results[0].source_id == note.id.bytes());

    std::size_t entries = 0;
    UAPP_REQUIRE_OK(h.index->entry_count(entries));
    UAPP_CHECK_MSG(entries == 1, "entry_count = " + std::to_string(entries));
}

UAPP_TEST("search.unified: deleting then re-adding content does not resurrect the old text") {
    // The regression test for index-delete ORDERING. The index is two tables
    // joined by rowid, and SQLite reuses the rowids of deleted rows. If a
    // delete removed the addressing row before the FTS row, the orphaned FTS
    // row would sit on a rowid the next insert is handed -- and the deleted
    // object's text would come back attached to the new object's entry.
    //
    // Nothing about that is visible immediately after the delete, which is
    // why it needs its own case: the churn has to happen first.
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    const PageId page_id(id_bytes(2));
    Page page;
    page.id             = page_id;
    page.title          = "Engineering";
    page.created_at_ms  = 1'700'000'000'000;
    page.modified_at_ms = 1'700'000'000'000;
    UAPP_REQUIRE_OK(h.blocks->create_page(page));

    // Churn: add three blocks, delete them, add three different ones. Every
    // rowid the second batch is handed was held by the first batch.
    for (std::uint8_t round = 0; round < 3; ++round) {
        for (std::uint8_t i = 0; i < 3; ++i) {
            const BlockId id(id_bytes(static_cast<std::uint8_t>(10 + i)));
            UAPP_REQUIRE_OK(h.blocks->insert_block(
                page_id, BlockId(), uapp::blocks::kAppendPosition, id,
                para(round == 0 ? "perihelion text" : "aphelion text"),
                1'700'000'000'000));
        }
        for (std::uint8_t i = 0; i < 3; ++i) {
            const BlockId id(id_bytes(static_cast<std::uint8_t>(10 + i)));
            UAPP_REQUIRE_OK(h.blocks->delete_block(id, 1'700'000'001'000));
        }
    }

    const BlockId final_id(id_bytes(20));
    UAPP_REQUIRE_OK(h.blocks->insert_block(page_id, BlockId(), 0, final_id,
                                           para("aphelion survivor"),
                                           1'700'000'002'000));

    std::vector<SearchResult> results;
    // The very first round's text must be unreachable, by any route.
    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));
    UAPP_CHECK_MSG(results.empty(), describe(results));

    UAPP_REQUIRE_OK(h.index->query("survivor", 10, results));
    UAPP_REQUIRE_MSG(results.size() == 1, describe(results));
    UAPP_CHECK(results[0].source_id == final_id.bytes());
    // And the snippet must be the SURVIVOR's text, not a deleted block's --
    // which is the exact symptom of an FTS row attached to the wrong entry.
    UAPP_CHECK_MSG(results[0].snippet.find("aphelion") != std::string::npos,
                   "got: " + results[0].snippet);
    UAPP_CHECK_MSG(results[0].snippet.find("perihelion") == std::string::npos,
                   "resurrected deleted text: " + results[0].snippet);

    std::size_t entries = 0;
    UAPP_REQUIRE_OK(h.index->entry_count(entries));
    UAPP_CHECK_MSG(entries == 2, "page + one block expected; got " +
                                     std::to_string(entries));
}

// --- empty / no-match -------------------------------------------------------

UAPP_TEST("search.unified: a non-matching query returns Ok and no results") {
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    UAPP_REQUIRE_OK(h.notes->insert(make_note(1, "Shopping", "milk and bread")));

    std::vector<SearchResult> results;
    // Seed the vector so a failure to clear it would show up here.
    results.resize(3);
    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));
    UAPP_CHECK_MSG(results.empty(), describe(results));
}

UAPP_TEST("search.unified: an empty or punctuation-only query is SEARCH_QUERY_EMPTY") {
    // Distinct from "no results" on purpose: the UI response differs, and
    // collapsing the two is how "search is broken" bug reports happen.
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    UAPP_REQUIRE_OK(h.notes->insert(make_note(1, "Shopping", "milk and bread")));

    std::vector<SearchResult> results;
    UAPP_REQUIRE_STATUS(h.index->query("", 10, results), Status::SearchQueryEmpty);
    UAPP_CHECK(results.empty());
    UAPP_REQUIRE_STATUS(h.index->query("   \t\n ", 10, results), Status::SearchQueryEmpty);
    UAPP_REQUIRE_STATUS(h.index->query("!!! ??? ...", 10, results),
                        Status::SearchQueryEmpty);
    UAPP_REQUIRE_STATUS(h.index->query("\"\"\"", 10, results), Status::SearchQueryEmpty);
}

UAPP_TEST("search.unified: an empty index answers cleanly rather than failing") {
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    std::size_t entries = 0;
    UAPP_REQUIRE_OK(h.index->entry_count(entries));
    UAPP_CHECK(entries == 0);

    std::vector<SearchResult> results;
    UAPP_REQUIRE_OK(h.index->query("anything", 10, results));
    UAPP_CHECK(results.empty());
}

// --- hostile input ----------------------------------------------------------

UAPP_TEST("search.unified: malformed and hostile queries cannot break the MATCH parser") {
    // The same corpus as tests/storage/fts_query_test.cpp, but driven END TO
    // END through the unified port rather than against the builder in
    // isolation. That is the point: the builder was already proved safe, and
    // this asserts the new layer did not route around it. Every one of these
    // is either a clean SEARCH_QUERY_EMPTY or a clean Ok -- never
    // SEARCH_FAILED (which is what a MATCH syntax error surfaces as) and
    // never a crash.
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    const Note note = make_note(1, "Notes", "a note mentioning perihelion");
    UAPP_REQUIRE_OK(h.notes->insert(note));

    const PageId page_id(id_bytes(2));
    Page page;
    page.id             = page_id;
    page.title          = "Engineering";
    page.created_at_ms  = 1'700'000'000'000;
    page.modified_at_ms = 1'700'000'000'000;
    UAPP_REQUIRE_OK(h.blocks->create_page(page));
    UAPP_REQUIRE_OK(h.blocks->insert_block(page_id, BlockId(), 0, BlockId(id_bytes(3)),
                                           para("a block mentioning perihelion"),
                                           1'700'000'000'000));

    const char* hostile[] = {
        "NEAR(",
        "\"unbalanced",
        "perihelion OR nonsense",
        "^perihelion",
        "body:perihelion",
        "title : perihelion",
        "perihelion AND (nonsense",
        "*",
        "**",
        "perihelion - nonsense",
        "'; DROP TABLE search_entry; --",
        "'; DELETE FROM search_fts; --",
        "perihelion\" OR \"1\"=\"1",
        "{body} : perihelion",
        "perihelion NOT nonsense",
        "(((((((((",
        ")))))))))",
        "\\",
        "perihelion*****",
        "perihelion\x01\x02\x7f",
    };

    for (const char* q : hostile) {
        std::vector<SearchResult> results;
        const Status s = h.index->query(q, 10, results);
        if (s == Status::SearchQueryEmpty) {
            // Nothing usable survived sanitising. Correct, and results must
            // still be empty.
            UAPP_CHECK_MSG(results.empty(), std::string("leaked results for: ") + q);
            continue;
        }
        UAPP_CHECK_MSG(uapp::is_ok(s),
                       std::string("status ") + uapp::status_code(s) + " for: " + q);
        if (!uapp::is_ok(s)) { continue; }

        // Operator words and punctuation are neutralised by quoting, so these
        // behave as plain term searches. Nothing may match a term the corpus
        // does not contain, and nothing may return a result that is not one
        // of the two objects stored above.
        for (const SearchResult& r : results) {
            UAPP_CHECK_MSG(r.kind == SourceKind::Note || r.kind == SourceKind::Block ||
                               r.kind == SourceKind::Page,
                           std::string("bogus kind for: ") + q);
        }
    }

    // The index survived all of it: both objects are still there and still
    // findable. A `DROP TABLE` that had actually executed would show up here
    // as SEARCH_FAILED or as an empty result, not as a silent pass.
    std::size_t entries = 0;
    UAPP_REQUIRE_OK(h.index->entry_count(entries));
    UAPP_CHECK_MSG(entries == 3, "entry_count = " + std::to_string(entries));

    std::vector<SearchResult> results;
    UAPP_REQUIRE_OK(h.index->query("perihelion", 10, results));
    UAPP_CHECK_MSG(results.size() == 2, describe(results));
}

UAPP_TEST("search.unified: an oversized query is rejected, not truncated") {
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    std::vector<SearchResult> results;
    const std::string too_long(uapp::search::kMaxQueryBytes + 1, 'a');
    UAPP_REQUIRE_STATUS(h.index->query(too_long, 10, results), Status::InvalidArgument);
    UAPP_CHECK(results.empty());

    const std::string at_limit(uapp::search::kMaxQueryBytes, 'a');
    UAPP_REQUIRE_OK(h.index->query(at_limit, 10, results));
}

UAPP_TEST("search.unified: an embedded NUL in the query terminates the term, not the parse") {
    Harness h;
    UAPP_REQUIRE_OK(h.open());
    UAPP_REQUIRE(h.ready());

    UAPP_REQUIRE_OK(h.notes->insert(make_note(1, "Notes", "perihelion")));

    // std::string_view carries its own length, so the NUL is a byte in the
    // middle of the input rather than the end of it. It is not a term byte,
    // so it separates terms -- it must not be smuggled into the MATCH
    // expression, where it would truncate the C string SQLite sees.
    const std::string with_nul = std::string("peri") + '\0' + std::string("helion");
    std::vector<SearchResult> results;
    const Status s = h.index->query(std::string_view(with_nul.data(), with_nul.size()),
                                    10, results);
    UAPP_CHECK_MSG(uapp::is_ok(s), std::string("status ") + uapp::status_code(s));
    // "peri" prefix-matches nothing on its own (only the LAST term is
    // prefix-matched) so this finds nothing -- but it finds nothing *cleanly*.
    UAPP_CHECK_MSG(results.empty(), describe(results));
}

// --- durability -------------------------------------------------------------

UAPP_TEST("search.unified: the index survives a close and reopen") {
    uapp::test::TempDir dir;
    UAPP_REQUIRE(dir.valid());
    const std::string path = dir.file("reopen.db");

    {
        std::unique_ptr<NoteStore>  notes;
        std::unique_ptr<BlockStore> blocks;

        uapp::storage::sqlite::SqliteNoteStoreOptions note_options;
        note_options.path = path;
        UAPP_REQUIRE_OK(
            uapp::storage::sqlite::open_sqlite_note_store(note_options, notes));
        UAPP_REQUIRE_OK(notes->insert(make_note(1, "Draft", "perihelion in a note")));

        uapp::storage::sqlite::SqliteBlockStoreOptions block_options;
        block_options.path = path;
        UAPP_REQUIRE_OK(
            uapp::storage::sqlite::open_sqlite_block_store(block_options, blocks));

        const PageId page_id(id_bytes(2));
        Page page;
        page.id             = page_id;
        page.title          = "Engineering";
        page.created_at_ms  = 1'700'000'000'000;
        page.modified_at_ms = 1'700'000'000'000;
        UAPP_REQUIRE_OK(blocks->create_page(page));
        UAPP_REQUIRE_OK(blocks->insert_block(page_id, BlockId(), 0, BlockId(id_bytes(3)),
                                             para("perihelion in a block"),
                                             1'700'000'000'000));
    }  // both stores closed here

    std::unique_ptr<SearchIndex> index;
    uapp::storage::sqlite::SqliteSearchIndexOptions index_options;
    index_options.path = path;
    UAPP_REQUIRE_OK(uapp::storage::sqlite::open_sqlite_search_index(index_options, index));
    UAPP_REQUIRE(index != nullptr);

    std::vector<SearchResult> results;
    UAPP_REQUIRE_OK(index->query("perihelion", 10, results));
    UAPP_REQUIRE_MSG(results.size() == 2, describe(results));
    UAPP_CHECK(count_kind(results, SourceKind::Note) == 1);
    UAPP_CHECK(count_kind(results, SourceKind::Block) == 1);
}

UAPP_TEST("search.unified: opening read-only on a path with no index is an error") {
    // Not "an index with nothing in it". A missing table means the caller was
    // pointed at the wrong file, and answering every query with "no results"
    // forever would hide that completely.
    uapp::test::TempDir dir;
    UAPP_REQUIRE(dir.valid());

    std::unique_ptr<SearchIndex> index;
    uapp::storage::sqlite::SqliteSearchIndexOptions options;
    options.path = dir.file("never-written.db");
    UAPP_REQUIRE_STATUS(uapp::storage::sqlite::open_sqlite_search_index(options, index),
                        Status::StoreOpenFailed);
    UAPP_CHECK(index == nullptr);

    options.path.clear();
    UAPP_REQUIRE_STATUS(uapp::storage::sqlite::open_sqlite_search_index(options, index),
                        Status::InvalidArgument);

    options.path = std::string("a") + '\0' + std::string("b");
    UAPP_REQUIRE_STATUS(uapp::storage::sqlite::open_sqlite_search_index(options, index),
                        Status::InvalidArgument);
}

// --- the kind tag -----------------------------------------------------------

UAPP_TEST("search.unified: source_kind_name covers every kind and never returns null") {
    UAPP_CHECK(std::string_view(uapp::search::source_kind_name(SourceKind::Note)) == "note");
    UAPP_CHECK(std::string_view(uapp::search::source_kind_name(SourceKind::Page)) == "page");
    UAPP_CHECK(std::string_view(uapp::search::source_kind_name(SourceKind::Block)) ==
               "block");

    // An integer cast into the enum is exactly what reading the persisted
    // column does, so it must not be undefined behaviour or a null pointer.
    const auto bogus = static_cast<SourceKind>(uapp::search::kSourceKindCount + 7);
    UAPP_CHECK(uapp::search::source_kind_name(bogus) != nullptr);
    UAPP_CHECK(std::string_view(uapp::search::source_kind_name(bogus)) == "unknown");
    UAPP_CHECK(!uapp::search::source_kind_is_valid(bogus));
    UAPP_CHECK(uapp::search::source_kind_is_valid(SourceKind::Block));
}
