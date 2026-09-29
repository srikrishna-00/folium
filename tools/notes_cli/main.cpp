// SPDX-License-Identifier: Apache-2.0
//
// uapp-notes — developer harness for the Phase 1 vertical slice.
//
// Two jobs:
//   1. `demo` runs the whole slice end to end against a real on-disk database
//      and verifies each step, so "it works" is a thing you can watch rather
//      than a claim. CI runs this as an integration test.
//   2. The individual subcommands let a human poke at the same code by hand.
//
// std::iostream is on ADR-0001's forbidden list, so output goes through
// <cstdio>. That is not pedantry here: it keeps the harness linking against
// the same restricted subset the library is built with.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "uapp/notes/note.h"
#include "uapp/notes/note_service.h"
#include "uapp/notes/ports.h"
#include "uapp/status.h"
#include "uapp/storage/sqlite/sqlite_note_store.h"

namespace {

using uapp::Status;
using uapp::is_ok;
using uapp::notes::Note;
using uapp::notes::NoteId;
using uapp::notes::NoteService;
using uapp::notes::SearchHit;

constexpr int kExitOk    = 0;
constexpr int kExitUsage = 2;
constexpr int kExitError = 1;

void print_usage() {
    std::fputs(
        "uapp-notes — Phase 1 vertical slice harness\n"
        "\n"
        "Usage:\n"
        "  uapp-notes --db <path> demo\n"
        "  uapp-notes --db <path> create --title <title> [--body <body>]\n"
        "  uapp-notes --db <path> search <query>\n"
        "  uapp-notes --db <path> open <note-id>\n"
        "  uapp-notes --db <path> edit <note-id> [--title <t>] [--body <b>]\n"
        "  uapp-notes --db <path> delete <note-id>\n"
        "  uapp-notes --db <path> count\n"
        "\n"
        "  demo runs CREATE -> STORE -> SEARCH -> OPEN -> EDIT -> SAVE and\n"
        "  verifies each step, exiting non-zero on the first failure.\n",
        stderr);
}

/// Every failure exit goes through here so that no error is ever reported as
/// a bare string (spec section 33: machine-readable code + human message).
int fail(std::string_view stage, Status s) {
    std::fprintf(stderr, "error: %.*s failed: %s (%s)\n",
                 static_cast<int>(stage.size()), stage.data(),
                 uapp::status_code(s), uapp::status_message(s));
    return kExitError;
}

void print_note(const Note& n) {
    std::printf("id        : %s\n", n.id.to_hex().c_str());
    std::printf("title     : %s\n", n.title.c_str());
    std::printf("created   : %lld ms\n", static_cast<long long>(n.created_at_ms));
    std::printf("modified  : %lld ms\n", static_cast<long long>(n.modified_at_ms));
    std::printf("body      : %s\n", n.body.c_str());
}

/// Find `--flag value` in argv. Returns false if absent; reports usage errors
/// (flag present but value missing) by returning false with *bad set.
bool find_option(int argc, char** argv, int from, const char* flag,
                 std::string& out, bool& bad) {
    for (int i = from; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires a value\n", flag);
                bad = true;
                return false;
            }
            out = argv[i + 1];
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// demo: the whole slice, verified step by step.
// ---------------------------------------------------------------------------
int run_demo(NoteService& svc) {
    std::puts("== Phase 1 vertical slice ==");

    // --- 1. CREATE + 2. STORE LOCALLY --------------------------------------
    Note created;
    if (const Status s = svc.create(
            "Quarterly planning",
            "Review the roadmap with the platform team. Ship the storage slice first.",
            created);
        !is_ok(s)) {
        return fail("create", s);
    }
    std::printf("[1] CREATE  ok  id=%s\n", created.id.to_hex().c_str());

    Note distractor;
    if (const Status s = svc.create("Grocery list",
                                    "Oat milk, sourdough, olive oil.", distractor);
        !is_ok(s)) {
        return fail("create (second note)", s);
    }
    std::printf("[2] STORE   ok  a second note exists, so search has to discriminate\n");

    // --- 3. SEARCH ---------------------------------------------------------
    std::vector<SearchHit> hits;
    if (const Status s = svc.search("roadmap", hits); !is_ok(s)) {
        return fail("search", s);
    }
    if (hits.size() != 1 || hits[0].id != created.id) {
        std::fprintf(stderr,
                     "error: search returned %zu hit(s); expected exactly the planning note\n",
                     hits.size());
        return kExitError;
    }
    std::printf("[3] SEARCH  ok  \"roadmap\" -> 1 hit: %s\n", hits[0].snippet.c_str());

    // A query that must NOT match, so we know search discriminates.
    std::vector<SearchHit> misses;
    if (const Status s = svc.search("submarine", misses); !is_ok(s)) {
        return fail("search (miss)", s);
    }
    if (!misses.empty()) {
        std::fputs("error: search for an absent term returned hits\n", stderr);
        return kExitError;
    }
    std::printf("[3b] SEARCH ok  \"submarine\" -> 0 hits\n");

    // --- 4. OPEN -----------------------------------------------------------
    Note opened;
    if (const Status s = svc.open(created.id, opened); !is_ok(s)) {
        return fail("open", s);
    }
    if (opened.title != created.title || opened.body != created.body) {
        std::fputs("error: opened note does not match what was stored\n", stderr);
        return kExitError;
    }
    std::printf("[4] OPEN    ok  title=\"%s\"\n", opened.title.c_str());

    // --- 5. EDIT (in memory) + 6. SAVE (persisted) -------------------------
    opened.title = "Quarterly planning (revised)";
    opened.body += " Decision: FTS5 for local search.";
    const std::int64_t modified_before = opened.modified_at_ms;

    if (const Status s = svc.save(opened); !is_ok(s)) {
        return fail("save", s);
    }
    std::printf("[5] EDIT    ok  title/body changed in memory\n");
    std::printf("[6] SAVE    ok  persisted, modified_at %lld -> %lld\n",
                static_cast<long long>(modified_before),
                static_cast<long long>(opened.modified_at_ms));

    // --- verify the edit survived a fresh read -----------------------------
    Note reread;
    if (const Status s = svc.open(created.id, reread); !is_ok(s)) {
        return fail("open (verify)", s);
    }
    if (reread.title != opened.title || reread.body != opened.body) {
        std::fputs("error: reloaded note does not reflect the saved edit\n", stderr);
        return kExitError;
    }
    if (reread.created_at_ms != created.created_at_ms) {
        std::fputs("error: created_at changed across an edit\n", stderr);
        return kExitError;
    }
    std::puts("[7] VERIFY  ok  reload reflects the edit; created_at preserved");

    // --- and that the index was updated with it ----------------------------
    std::vector<SearchHit> after;
    if (const Status s = svc.search("FTS5", after); !is_ok(s)) {
        return fail("search (post-edit)", s);
    }
    if (after.size() != 1 || after[0].id != created.id) {
        std::fputs("error: search index did not pick up the edited body\n", stderr);
        return kExitError;
    }
    std::puts("[8] VERIFY  ok  search index reflects the edit (same transaction)");

    // --- error path is a first-class outcome, not an afterthought ----------
    NoteId absent;
    if (const Status s = NoteId::parse("00000000000000000000000000000001", absent);
        !is_ok(s)) {
        return fail("parse (absent id)", s);
    }
    Note nothing;
    const Status not_found = svc.open(absent, nothing);
    if (not_found != Status::NotFound) {
        std::fprintf(stderr, "error: opening an unknown id returned %s, expected NOT_FOUND\n",
                     uapp::status_code(not_found));
        return kExitError;
    }
    std::printf("[9] ERRORS  ok  unknown id -> %s\n", uapp::status_code(not_found));

    std::size_t total = 0;
    if (const Status s = svc.count(total); !is_ok(s)) {
        return fail("count", s);
    }
    std::printf("\nslice complete — %zu notes in the store\n", total);
    return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return kExitUsage;
    }

    std::string db_path;
    bool bad = false;
    if (!find_option(argc, argv, 1, "--db", db_path, bad) || bad) {
        if (!bad) { std::fputs("error: --db <path> is required\n", stderr); }
        print_usage();
        return kExitUsage;
    }

    // The subcommand is the first argument that is not --db or its value.
    const char* command = nullptr;
    int command_index = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--db") == 0) { ++i; continue; }
        command = argv[i];
        command_index = i;
        break;
    }
    if (command == nullptr) {
        print_usage();
        return kExitUsage;
    }

    uapp::storage::sqlite::SqliteNoteStoreOptions opts;
    opts.path = db_path;

    std::unique_ptr<uapp::notes::NoteStore> store;
    if (const Status s = uapp::storage::sqlite::open_sqlite_note_store(opts, store);
        !is_ok(s)) {
        return fail("open database", s);
    }

    auto clock = uapp::notes::make_system_clock();
    auto ids   = uapp::notes::make_random_id_generator();
    if (!clock || !ids) {
        std::fputs("error: out of memory initialising the note service\n", stderr);
        return kExitError;
    }
    NoteService svc(*store, *clock, *ids);

    if (std::strcmp(command, "demo") == 0) {
        return run_demo(svc);
    }

    if (std::strcmp(command, "create") == 0) {
        std::string title;
        std::string body;
        (void)find_option(argc, argv, command_index, "--title", title, bad);
        (void)find_option(argc, argv, command_index, "--body", body, bad);
        if (bad) { return kExitUsage; }

        Note created;
        if (const Status s = svc.create(title, body, created); !is_ok(s)) {
            return fail("create", s);
        }
        print_note(created);
        return kExitOk;
    }

    if (std::strcmp(command, "count") == 0) {
        std::size_t n = 0;
        if (const Status s = svc.count(n); !is_ok(s)) { return fail("count", s); }
        std::printf("%zu\n", n);
        return kExitOk;
    }

    // Everything below needs one positional argument after the subcommand.
    if (command_index + 1 >= argc) {
        std::fprintf(stderr, "error: '%s' requires an argument\n", command);
        print_usage();
        return kExitUsage;
    }
    const char* positional = argv[command_index + 1];

    if (std::strcmp(command, "search") == 0) {
        std::vector<SearchHit> hits;
        if (const Status s = svc.search(positional, hits); !is_ok(s)) {
            return fail("search", s);
        }
        std::printf("%zu hit(s)\n", hits.size());
        for (const SearchHit& h : hits) {
            std::printf("  %s  %-40s  %s\n", h.id.to_hex().c_str(),
                        h.title.c_str(), h.snippet.c_str());
        }
        return kExitOk;
    }

    NoteId id;
    if (const Status s = NoteId::parse(positional, id); !is_ok(s)) {
        return fail("parse note id", s);
    }

    if (std::strcmp(command, "open") == 0) {
        Note n;
        if (const Status s = svc.open(id, n); !is_ok(s)) { return fail("open", s); }
        print_note(n);
        return kExitOk;
    }

    if (std::strcmp(command, "edit") == 0) {
        Note n;
        if (const Status s = svc.open(id, n); !is_ok(s)) { return fail("open", s); }

        std::string title;
        std::string body;
        const bool has_title = find_option(argc, argv, command_index, "--title", title, bad);
        const bool has_body  = find_option(argc, argv, command_index, "--body", body, bad);
        if (bad) { return kExitUsage; }
        if (!has_title && !has_body) {
            std::fputs("error: edit needs --title and/or --body\n", stderr);
            return kExitUsage;
        }
        if (has_title) { n.title = title; }
        if (has_body)  { n.body  = body; }

        if (const Status s = svc.save(n); !is_ok(s)) { return fail("save", s); }
        print_note(n);
        return kExitOk;
    }

    if (std::strcmp(command, "delete") == 0) {
        if (const Status s = svc.remove(id); !is_ok(s)) { return fail("delete", s); }
        std::puts("deleted");
        return kExitOk;
    }

    std::fprintf(stderr, "error: unknown command '%s'\n", command);
    print_usage();
    return kExitUsage;
}
