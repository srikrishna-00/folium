// SPDX-License-Identifier: Apache-2.0
//
// tools/loadtest/storage_loadtest.cpp — scale/stress harness for the five
// data-shaped engines: core/notes, core/blocks, core/search, core/database and
// core/conversion, each over the real storage/sqlite adapter (except
// conversion, which is pure in-memory by design).
//
// WHY ONE BINARY FOR THESE FIVE AND NOT ALL SEVEN MODULES. The project
// enforces module boundaries on the link line, not by review: ocr/engine links
// only uapp::core_common and Tesseract, and security/crypto links only
// libsodium. Dragging either into a binary that also links SQLite would make
// this harness the one place in the tree where that isolation is broken. So
// crypto and OCR get their own executables (crypto_loadtest.cpp,
// ocr_loadtest.cpp) with their own link lines, and they are also the two
// modules whose presence is optional at configure time.
//
// SEPARATE PROCESSES ARE ALSO THE POINT FOR MEMORY. Peak RSS is a per-process
// high-water mark. Measuring OCR's 19-megapixel peak in the same process that
// just built a 100 000-note SQLite database would report the max of the two and
// attribute it to neither.
//
// EVERY SCALE POINT GETS A FRESH DATABASE FILE, except the unified-search
// runner, where a GROWING corpus is exactly the thing under test.
//
// Usage:
//   uapp-loadtest-storage --dir <scratch-dir> [notes|blocks|search|database|
//                                              conversion|all]...

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "loadtest_util.h"

#include "uapp/blocks/block.h"
#include "uapp/blocks/block_store.h"
#include "uapp/blocks/document_service.h"
#include "uapp/blocks/page.h"
#include "uapp/conversion/fidelity.h"
#include "uapp/conversion/markdown.h"
#include "uapp/database/database.h"
#include "uapp/database/database_store.h"
#include "uapp/database/property.h"
#include "uapp/database/view.h"
#include "uapp/notes/note.h"
#include "uapp/notes/note_service.h"
#include "uapp/notes/ports.h"
#include "uapp/object_id.h"
#include "uapp/ports.h"
#include "uapp/search/search_index.h"
#include "uapp/search/search_result.h"
#include "uapp/status.h"
#include "uapp/storage/sqlite/sqlite_block_store.h"
#include "uapp/storage/sqlite/sqlite_database_store.h"
#include "uapp/storage/sqlite/sqlite_note_store.h"
#include "uapp/storage/sqlite/sqlite_search_index.h"

namespace {

using uapp::ObjectIdBytes;
using uapp::Status;
using uapp::is_ok;
using uapp::loadtest::Timer;
using uapp::loadtest::die;
using uapp::loadtest::emit_row;
using uapp::loadtest::kScaleCount;
using uapp::loadtest::kScales;
using uapp::loadtest::note;
using uapp::loadtest::status_text;

// ---------------------------------------------------------------------------
// Scratch files
// ---------------------------------------------------------------------------

/// WAL mode means three files per database, and a stale -wal from a previous
/// run would silently carry rows into the next scale point.
void drop_db(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

[[nodiscard]] long long file_bytes(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) { return 0; }
    std::fseek(f, 0, SEEK_END);
    const long long n = std::ftell(f);
    std::fclose(f);
    return n < 0 ? 0 : n;
}

/// Total on-disk footprint of a WAL-mode database, which is the number that
/// matters on a 16 GB Tier D device, not the main file alone.
[[nodiscard]] long long db_bytes(const std::string& path) {
    return file_bytes(path) + file_bytes(path + "-wal");
}

// ---------------------------------------------------------------------------
// Deterministic ids for the stores that take an id from the caller
// ---------------------------------------------------------------------------

/// DatabaseStore and BlockStore::insert_block take ids from the caller. Using
/// a counter rather than the CSPRNG keeps every run byte-identical and keeps
/// id generation out of the measured time.
template <class IdType>
[[nodiscard]] IdType counter_id(std::uint64_t n) {
    ObjectIdBytes raw{};
    for (std::size_t i = 0; i < 8; ++i) {
        raw[15 - i] = static_cast<std::uint8_t>((n >> (i * 8)) & 0xFFu);
    }
    return IdType(raw);
}

// ---------------------------------------------------------------------------
// Synthetic content
// ---------------------------------------------------------------------------

/// ~240 bytes of prose with one token unique to this index (`zqNNNNNN`) and
/// several tokens shared by every record. That gives the search runners both a
/// needle (one hit, whatever N is) and a haystack query (N hits), which is the
/// distinction that turns out to drive FTS5's scale behaviour.
[[nodiscard]] std::string body_for(long long i) {
    char head[64];
    std::snprintf(head, sizeof head, "zq%06lld ", i);
    std::string out = head;
    out += "lorem ipsum planning roadmap review quarterly storage slice "
           "the platform team ships the engine first and the bindings after. "
           "Context: a document model, a block tree, a property database and "
           "one unified index over all of it. Measured, not asserted.";
    return out;
}

[[nodiscard]] std::string title_for(long long i) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "Planning note %lld", i);
    return buf;
}

// ---------------------------------------------------------------------------
// 1. core/notes + storage/sqlite
// ---------------------------------------------------------------------------

void run_notes(const std::string& dir) {
    note("== notes ==");

    auto clock = uapp::notes::make_system_clock();
    auto ids   = uapp::notes::make_random_id_generator();
    if (clock == nullptr || ids == nullptr) { die("notes ports", Status::Internal); }

    for (std::size_t si = 0; si < kScaleCount; ++si) {
        const long long n    = kScales[si];
        const std::string db = dir + "/notes.db";
        drop_db(db);

        uapp::storage::sqlite::SqliteNoteStoreOptions options;
        options.path = db;
        std::unique_ptr<uapp::notes::NoteStore> store;
        if (const Status s = uapp::storage::sqlite::open_sqlite_note_store(options, store);
            !is_ok(s)) {
            die("open note store", s);
        }
        uapp::notes::NoteService svc(*store, *clock, *ids);

        // --- CREATE (insert + FTS index maintenance, one txn each) ---------
        std::vector<uapp::notes::NoteId> sampled;
        Timer t;
        bool create_ok = true;
        Status create_err = Status::Ok;
        for (long long i = 0; i < n; ++i) {
            uapp::notes::Note created;
            if (const Status s = svc.create(title_for(i), body_for(i), created); !is_ok(s)) {
                create_ok  = false;
                create_err = s;
                break;
            }
            if (i % 997 == 0) { sampled.push_back(created.id); }
        }
        const double create_ms = t.ms();
        emit_row("notes", "create", n, n, create_ms, create_ok ? "ok" : "FAIL",
                 create_ok ? "" : status_text(create_err).c_str());
        if (!create_ok) { continue; }

        // --- correctness: the store holds exactly what we put in it --------
        std::size_t counted = 0;
        if (const Status s = svc.count(counted); !is_ok(s)) { die("count", s); }
        char detail[160];
        std::snprintf(detail, sizeof detail, "count=%zu main_bytes=%lld wal_bytes=%lld",
                      counted, file_bytes(db), file_bytes(db + "-wal"));
        emit_row("notes", "count", n, 1, 0.0,
                 counted == static_cast<std::size_t>(n) ? "ok" : "FAIL", detail);

        // --- SEARCH, needle: one unique token AND one shared token ---------
        // fts_query.cpp prefix-matches only the LAST term, so putting the
        // unique token first keeps it an exact-term lookup.
        const long long target = n / 2;
        char needle[64];
        std::snprintf(needle, sizeof needle, "zq%06lld lorem", target);
        constexpr int kReps = 20;
        std::vector<uapp::notes::SearchHit> hits;
        std::size_t needle_hits = 0;
        t.reset();
        for (int r = 0; r < kReps; ++r) {
            if (const Status s = svc.search(needle, hits, 20); !is_ok(s)) {
                die("search needle", s);
            }
            needle_hits = hits.size();
        }
        const double needle_ms = t.ms();
        std::snprintf(detail, sizeof detail, "hits=%zu", needle_hits);
        emit_row("notes", "search_needle", n, kReps, needle_ms,
                 needle_hits == 1 ? "ok" : "FAIL", detail);

        // --- SEARCH, needle alone: the selective token IS the last term ----
        // The contrast with search_needle is the measurement. fts_query.cpp
        // prefix-matches the LAST term unconditionally, so this is
        // `"zqNNNNNN"*` -- one selective prefix scan and nothing else -- while
        // search_needle above is that AND `"lorem"*`, a prefix term every
        // record carries. If the two curves separate as N grows, the trailing
        // `*` on a common word is what costs, not the query's selectivity.
        char needle_only[32];
        std::snprintf(needle_only, sizeof needle_only, "zq%06lld", target);
        std::size_t only_hits = 0;
        t.reset();
        for (int r = 0; r < kReps; ++r) {
            if (const Status s = svc.search(needle_only, hits, 20); !is_ok(s)) {
                die("search needle only", s);
            }
            only_hits = hits.size();
        }
        const double only_ms = t.ms();
        std::snprintf(detail, sizeof detail, "hits=%zu query=%s", only_hits, needle_only);
        emit_row("notes", "search_needle_only", n, kReps, only_ms,
                 only_hits >= 1 ? "ok" : "FAIL", detail);

        // --- SEARCH, haystack: a token every record shares -----------------
        t.reset();
        std::size_t broad_hits = 0;
        for (int r = 0; r < kReps; ++r) {
            if (const Status s = svc.search("planning roadmap", hits, 20); !is_ok(s)) {
                die("search broad", s);
            }
            broad_hits = hits.size();
        }
        const double broad_ms = t.ms();
        std::snprintf(detail, sizeof detail, "returned=%zu limit=20", broad_hits);
        emit_row("notes", "search_broad", n, kReps, broad_ms, broad_hits > 0 ? "ok" : "FAIL",
                 detail);

        // --- OPEN / EDIT / SAVE on a sample --------------------------------
        bool rt_ok = true;
        t.reset();
        for (const uapp::notes::NoteId& id : sampled) {
            uapp::notes::Note opened;
            if (const Status s = svc.open(id, opened); !is_ok(s)) { rt_ok = false; break; }
            opened.body += " edited";
            if (const Status s = svc.save(opened); !is_ok(s)) { rt_ok = false; break; }
            uapp::notes::Note reread;
            if (const Status s = svc.open(id, reread); !is_ok(s)) { rt_ok = false; break; }
            if (reread.body != opened.body) { rt_ok = false; break; }
        }
        const double rt_ms = t.ms();
        emit_row("notes", "open_edit_save", n, static_cast<long long>(sampled.size()), rt_ms,
                 rt_ok ? "ok" : "FAIL", "round-trip verified");

        store.reset();
        drop_db(db);
    }
}

// ---------------------------------------------------------------------------
// 2. core/blocks + storage/sqlite
// ---------------------------------------------------------------------------

/// kMaxBlocksPerPage is 50 000, so the ladder's 100 000 point cannot be
/// reached by design. The ladder is walked to 50 000 and then the limit itself
/// is probed: one block past the cap must be a Status, not a crash.
void run_blocks(const std::string& dir) {
    note("== blocks ==");

    const long long block_scales[] = {10, 100, 1000, 10000,
                                      static_cast<long long>(uapp::blocks::kMaxBlocksPerPage)};

    auto clock = uapp::make_system_clock();
    auto ids   = uapp::make_random_id_source();
    if (clock == nullptr || ids == nullptr) { die("blocks ports", Status::Internal); }

    for (const long long n : block_scales) {
        const std::string db = dir + "/blocks.db";
        drop_db(db);

        uapp::storage::sqlite::SqliteBlockStoreOptions options;
        options.path = db;
        std::unique_ptr<uapp::blocks::BlockStore> store;
        if (const Status s = uapp::storage::sqlite::open_sqlite_block_store(options, store);
            !is_ok(s)) {
            die("open block store", s);
        }
        uapp::blocks::DocumentService svc(*store, *clock, *ids);

        uapp::blocks::Page page;
        if (const Status s = svc.create_page("Load test page", page); !is_ok(s)) {
            die("create_page", s);
        }

        // --- APPEND n blocks at the page root ------------------------------
        uapp::blocks::BlockId first_block;
        uapp::blocks::BlockId last_block;
        Timer t;
        bool append_ok    = true;
        Status append_err = Status::Ok;
        for (long long i = 0; i < n; ++i) {
            uapp::blocks::Block out;
            const uapp::blocks::BlockContent content =
                uapp::blocks::ParagraphBlock{body_for(i)};
            if (const Status s = svc.append_block(page.id, uapp::blocks::BlockId{}, content, out);
                !is_ok(s)) {
                append_ok  = false;
                append_err = s;
                break;
            }
            if (i == 0) { first_block = out.id; }
            last_block = out.id;
        }
        const double append_ms = t.ms();
        emit_row("blocks", "append", n, n, append_ms, append_ok ? "ok" : "FAIL",
                 append_ok ? "" : status_text(append_err).c_str());
        if (!append_ok) {
            store.reset();
            drop_db(db);
            continue;
        }

        // --- OPEN the whole page (get_page loads the full tree) ------------
        std::size_t loaded = 0;
        t.reset();
        constexpr int kOpenReps = 3;
        for (int r = 0; r < kOpenReps; ++r) {
            uapp::blocks::Page opened;
            if (const Status s = svc.open_page(page.id, opened); !is_ok(s)) {
                die("open_page", s);
            }
            loaded = uapp::blocks::block_tree_size(opened.blocks);
        }
        const double open_ms = t.ms();
        char detail[160];
        std::snprintf(detail, sizeof detail, "tree_size=%zu db_bytes=%lld", loaded,
                      db_bytes(db));
        emit_row("blocks", "open_page", n, kOpenReps, open_ms,
                 loaded == static_cast<std::size_t>(n) ? "ok" : "FAIL", detail);

        // --- REORDER within the root (last block to the middle) ------------
        constexpr int kReorderReps = 50;
        const std::size_t mid = static_cast<std::size_t>(n / 2);
        bool reorder_ok = true;
        t.reset();
        for (int r = 0; r < kReorderReps; ++r) {
            if (const Status s = svc.reorder_block(last_block, mid); !is_ok(s)) {
                reorder_ok = false;
                break;
            }
        }
        const double reorder_ms = t.ms();
        emit_row("blocks", "reorder", n, kReorderReps, reorder_ms, reorder_ok ? "ok" : "FAIL",
                 "last -> middle");

        // --- MOVE across parents (root block under another block) ----------
        constexpr int kMoveReps = 50;
        bool move_ok = true;
        t.reset();
        for (int r = 0; r < kMoveReps; ++r) {
            if (const Status s = svc.move_block(last_block, first_block, 0); !is_ok(s)) {
                move_ok = false;
                break;
            }
            if (const Status s = svc.move_block(last_block, uapp::blocks::BlockId{}, 0);
                !is_ok(s)) {
                move_ok = false;
                break;
            }
        }
        const double move_ms = t.ms();
        emit_row("blocks", "move_parent", n, kMoveReps * 2, move_ms, move_ok ? "ok" : "FAIL",
                 "root <-> nested");

        // --- THE LIMIT, at the top of the ladder only ----------------------
        if (n == static_cast<long long>(uapp::blocks::kMaxBlocksPerPage)) {
            uapp::blocks::Block overflow;
            t.reset();
            const Status s = svc.append_block(page.id, uapp::blocks::BlockId{},
                                              uapp::blocks::ParagraphBlock{"one too many"},
                                              overflow);
            const double limit_ms = t.ms();
            const bool correct    = s == Status::PageBlockLimitExceeded;
            emit_row("blocks", "limit_blocks_per_page", n + 1, 1, limit_ms,
                     correct ? "limit" : "FAIL", status_text(s).c_str());
        }

        store.reset();
        drop_db(db);
    }

    // --- DEPTH: kMaxBlockDepth is a separate ceiling from the count one ----
    {
        const std::string db = dir + "/blocks_depth.db";
        drop_db(db);
        uapp::storage::sqlite::SqliteBlockStoreOptions options;
        options.path = db;
        std::unique_ptr<uapp::blocks::BlockStore> store;
        if (const Status s = uapp::storage::sqlite::open_sqlite_block_store(options, store);
            !is_ok(s)) {
            die("open block store (depth)", s);
        }
        uapp::blocks::DocumentService svc(*store, *clock, *ids);
        uapp::blocks::Page page;
        if (const Status s = svc.create_page("Depth page", page); !is_ok(s)) {
            die("create_page (depth)", s);
        }

        uapp::blocks::BlockId parent;  // nil == page root
        const std::size_t depth_target = uapp::blocks::kMaxBlockDepth;
        Timer t;
        std::size_t reached = 0;
        Status depth_err    = Status::Ok;
        for (std::size_t d = 0; d < depth_target; ++d) {
            uapp::blocks::Block out;
            if (const Status s = svc.append_block(
                    page.id, parent, uapp::blocks::ParagraphBlock{"nested"}, out);
                !is_ok(s)) {
                depth_err = s;
                break;
            }
            parent = out.id;
            ++reached;
        }
        const double depth_ms = t.ms();
        char detail[128];
        std::snprintf(detail, sizeof detail, "reached_depth=%zu", reached);
        emit_row("blocks", "nest_to_max_depth", static_cast<long long>(depth_target),
                 static_cast<long long>(reached), depth_ms,
                 reached == depth_target ? "ok" : "FAIL",
                 reached == depth_target ? detail : status_text(depth_err).c_str());

        uapp::blocks::Block overflow;
        t.reset();
        const Status s = svc.append_block(page.id, parent,
                                          uapp::blocks::ParagraphBlock{"one too deep"},
                                          overflow);
        const double over_ms = t.ms();
        emit_row("blocks", "limit_block_depth",
                 static_cast<long long>(depth_target) + 1, 1, over_ms,
                 s == Status::BlockDepthExceeded ? "limit" : "FAIL", status_text(s).c_str());

        store.reset();
        drop_db(db);
    }
}

// ---------------------------------------------------------------------------
// 3. core/search — one unified index over a GROWING mixed corpus
// ---------------------------------------------------------------------------

/// Unlike every other runner this one never drops the database between scale
/// points: the question is how query latency moves as the combined
/// notes+blocks corpus grows, so the corpus has to be cumulative.
///
/// Blocks are spread over pages of 5 000 rather than piled into one page.
/// That is not an arbitrary choice: `insert_block` consults `block_count` (a
/// COUNT(*) over the page) on every insert to enforce kMaxBlocksPerPage, so
/// filling one page is quadratic in the page's block count. Ten pages of 5 000
/// keeps the INDEX under test rather than that enforcement cost.
void run_search(const std::string& dir) {
    note("== search (unified) ==");

    const std::string db = dir + "/unified.db";
    drop_db(db);

    uapp::storage::sqlite::SqliteNoteStoreOptions note_options;
    note_options.path = db;
    std::unique_ptr<uapp::notes::NoteStore> notes;
    if (const Status s = uapp::storage::sqlite::open_sqlite_note_store(note_options, notes);
        !is_ok(s)) {
        die("open note store (unified)", s);
    }

    uapp::storage::sqlite::SqliteBlockStoreOptions block_options;
    block_options.path = db;
    std::unique_ptr<uapp::blocks::BlockStore> blocks;
    if (const Status s = uapp::storage::sqlite::open_sqlite_block_store(block_options, blocks);
        !is_ok(s)) {
        die("open block store (unified)", s);
    }

    uapp::storage::sqlite::SqliteSearchIndexOptions index_options;
    index_options.path = db;
    std::unique_ptr<uapp::search::SearchIndex> index;
    if (const Status s = uapp::storage::sqlite::open_sqlite_search_index(index_options, index);
        !is_ok(s)) {
        die("open search index", s);
    }

    auto clock = uapp::make_system_clock();
    auto ids   = uapp::make_random_id_source();
    if (clock == nullptr || ids == nullptr) { die("search ports", Status::Internal); }
    uapp::blocks::DocumentService docs(*blocks, *clock, *ids);

    constexpr long long kBlocksPerPage = 5000;
    long long next_note  = 0;
    long long next_block = 0;
    uapp::blocks::Page current_page;
    long long in_current_page = kBlocksPerPage;  // force a page on the first block

    for (std::size_t si = 0; si < kScaleCount; ++si) {
        const long long target = kScales[si];  // total FTS entries wanted
        const long long want_notes  = target / 2;
        const long long want_blocks = target - want_notes;

        Timer t;
        while (next_note < want_notes) {
            uapp::notes::Note n;
            n.id = counter_id<uapp::notes::NoteId>(static_cast<std::uint64_t>(next_note) + 1u);
            n.title          = title_for(next_note);
            n.body           = body_for(next_note);
            n.created_at_ms  = 1'700'000'000'000;
            n.modified_at_ms = 1'700'000'000'000;
            if (const Status s = notes->insert(n); !is_ok(s)) { die("insert note", s); }
            ++next_note;
        }
        while (next_block < want_blocks) {
            if (in_current_page >= kBlocksPerPage) {
                if (const Status s = docs.create_page("Corpus page", current_page);
                    !is_ok(s)) {
                    die("create_page (unified)", s);
                }
                in_current_page = 0;
            }
            uapp::blocks::Block out;
            if (const Status s =
                    docs.append_block(current_page.id, uapp::blocks::BlockId{},
                                      uapp::blocks::ParagraphBlock{body_for(1'000'000 + next_block)},
                                      out);
                !is_ok(s)) {
                die("append_block (unified)", s);
            }
            ++next_block;
            ++in_current_page;
        }
        const double ingest_ms = t.ms();

        std::size_t entries = 0;
        if (const Status s = index->entry_count(entries); !is_ok(s)) { die("entry_count", s); }

        char detail[192];
        std::snprintf(detail, sizeof detail, "entries=%zu notes=%lld blocks=%lld db_bytes=%lld",
                      entries, next_note, next_block, db_bytes(db));
        emit_row("search", "ingest_to_scale", target, target, ingest_ms, "ok", detail);

        std::vector<uapp::search::SearchResult> out;
        constexpr int kReps = 20;

        // --- needle: a token that exists in exactly one entry --------------
        char needle[64];
        std::snprintf(needle, sizeof needle, "zq%06lld lorem", want_notes > 2 ? want_notes / 2 : 0);
        std::size_t needle_hits = 0;
        t.reset();
        for (int r = 0; r < kReps; ++r) {
            if (const Status s = index->query(needle, 20, out); !is_ok(s)) {
                die("unified query (needle)", s);
            }
            needle_hits = out.size();
        }
        const double needle_ms = t.ms();
        std::snprintf(detail, sizeof detail, "hits=%zu", needle_hits);
        emit_row("search", "query_needle", target, kReps, needle_ms,
                 needle_hits >= 1 ? "ok" : "FAIL", detail);

        // --- haystack: a token every entry shares, ranked ------------------
        std::size_t broad_hits = 0;
        bool mixed_kinds = false;
        t.reset();
        for (int r = 0; r < kReps; ++r) {
            if (const Status s = index->query("planning roadmap", 20, out); !is_ok(s)) {
                die("unified query (broad)", s);
            }
            broad_hits = out.size();
        }
        const double broad_ms = t.ms();
        {
            // Two extra unmeasured queries to confirm the index is genuinely
            // unified: a note-only token must come back as a Note and a
            // block-only token as a Block.
            //
            // Querying a token they SHARE would not prove this and would
            // quietly stop proving it at scale: bm25 ties are broken by
            // rowid, so at 100 000 entries a shared-token query returns the
            // first 1 000 notes and no blocks at all. That is correct ranking
            // behaviour, not a missing kind -- the probe has to be built from
            // tokens that are exclusive to one kind.
            bool saw_note  = false;
            bool saw_block = false;
            std::vector<uapp::search::SearchResult> probe;
            char note_token[32];
            std::snprintf(note_token, sizeof note_token, "zq%06lld", want_notes / 2);
            if (const Status s = index->query(note_token, 20, probe); is_ok(s)) {
                for (const uapp::search::SearchResult& r : probe) {
                    if (r.kind == uapp::search::SourceKind::Note) { saw_note = true; }
                }
            }
            char block_token[32];
            std::snprintf(block_token, sizeof block_token, "zq%06lld",
                          1'000'000 + want_blocks / 2);
            if (const Status s = index->query(block_token, 20, probe); is_ok(s)) {
                for (const uapp::search::SearchResult& r : probe) {
                    if (r.kind == uapp::search::SourceKind::Block) { saw_block = true; }
                }
            }
            mixed_kinds = (saw_note || want_notes == 0) && (saw_block || want_blocks == 0);
        }
        std::snprintf(detail, sizeof detail, "returned=%zu mixed_kinds=%d", broad_hits,
                      mixed_kinds ? 1 : 0);
        emit_row("search", "query_broad", target, kReps, broad_ms,
                 (broad_hits > 0 && mixed_kinds) ? "ok" : "FAIL", detail);

        // --- haystack at the result ceiling (kMaxResults = 1000) -----------
        std::size_t big_hits = 0;
        t.reset();
        for (int r = 0; r < kReps; ++r) {
            if (const Status s = index->query("planning roadmap", 1000, out); !is_ok(s)) {
                die("unified query (limit 1000)", s);
            }
            big_hits = out.size();
        }
        const double big_ms = t.ms();
        std::snprintf(detail, sizeof detail, "returned=%zu limit=1000", big_hits);
        emit_row("search", "query_broad_limit1000", target, kReps, big_ms, "ok", detail);
    }

    index.reset();
    blocks.reset();
    notes.reset();
    drop_db(db);
}

// ---------------------------------------------------------------------------
// 4. core/database + storage/sqlite
// ---------------------------------------------------------------------------

namespace dbns = uapp::database;

[[nodiscard]] dbns::Database make_schema() {
    dbns::Database db;
    db.id             = counter_id<dbns::DatabaseId>(2);
    db.page_id        = counter_id<uapp::blocks::PageId>(1);
    db.name           = "Load test tasks";
    db.created_at_ms  = 1'700'000'000'000;
    db.modified_at_ms = 1'700'000'000'000;

    dbns::PropertyDefinition title;
    title.id   = counter_id<dbns::PropertyId>(10);
    title.name = "Title";
    title.type = dbns::PropertyType::Text;
    db.properties.push_back(title);

    dbns::PropertyDefinition priority;
    priority.id   = counter_id<dbns::PropertyId>(11);
    priority.name = "Priority";
    priority.type = dbns::PropertyType::Number;
    db.properties.push_back(priority);

    dbns::PropertyDefinition done;
    done.id   = counter_id<dbns::PropertyId>(12);
    done.name = "Done";
    done.type = dbns::PropertyType::Checkbox;
    db.properties.push_back(done);

    dbns::PropertyDefinition status;
    status.id   = counter_id<dbns::PropertyId>(13);
    status.name = "Status";
    status.type = dbns::PropertyType::Select;
    status.options.push_back({counter_id<dbns::OptionId>(20), "To do"});
    status.options.push_back({counter_id<dbns::OptionId>(21), "In progress"});
    status.options.push_back({counter_id<dbns::OptionId>(22), "Done"});
    db.properties.push_back(status);

    dbns::PropertyDefinition tags;
    tags.id   = counter_id<dbns::PropertyId>(14);
    tags.name = "Tags";
    tags.type = dbns::PropertyType::MultiSelect;
    tags.options.push_back({counter_id<dbns::OptionId>(30), "urgent"});
    tags.options.push_back({counter_id<dbns::OptionId>(31), "backend"});
    tags.options.push_back({counter_id<dbns::OptionId>(32), "docs"});
    db.properties.push_back(tags);

    dbns::PropertyDefinition due;
    due.id   = counter_id<dbns::PropertyId>(15);
    due.name = "Due";
    due.type = dbns::PropertyType::Date;
    db.properties.push_back(due);

    return db;
}

[[nodiscard]] dbns::Row make_row(long long i) {
    dbns::Row row;
    row.id             = counter_id<dbns::RowId>(static_cast<std::uint64_t>(i) + 1000u);
    row.database_id    = counter_id<dbns::DatabaseId>(2);
    row.created_at_ms  = 1'700'000'000'000;
    row.modified_at_ms = 1'700'000'000'000;

    char text[96];
    std::snprintf(text, sizeof text, "task %lld alpha beta", i);
    row.values.emplace(counter_id<dbns::PropertyId>(10), dbns::TextValue{text});
    row.values.emplace(counter_id<dbns::PropertyId>(11),
                       dbns::NumberValue{static_cast<double>(i % 1000)});
    row.values.emplace(counter_id<dbns::PropertyId>(12), dbns::CheckboxValue{(i % 2) == 0});
    row.values.emplace(counter_id<dbns::PropertyId>(13),
                       dbns::SelectValue{counter_id<dbns::OptionId>(
                           20u + static_cast<std::uint64_t>(i % 3))});
    dbns::MultiSelectValue tags;
    tags.options.push_back(counter_id<dbns::OptionId>(30u + static_cast<std::uint64_t>(i % 3)));
    if ((i % 7) == 0) {
        // The SECOND tag has to differ from the first: a multi-select value
        // holding one option twice is DatabaseValueMalformed by design
        // (core/database/src/property.cpp), and an earlier version of this
        // generator hit exactly that at i=7.
        tags.options.push_back(
            counter_id<dbns::OptionId>(30u + static_cast<std::uint64_t>((i + 1) % 3)));
    }
    row.values.emplace(counter_id<dbns::PropertyId>(14), tags);
    row.values.emplace(counter_id<dbns::PropertyId>(15),
                       dbns::DateValue{1'767'225'600'000LL + (i % 365) * 86'400'000LL});
    return row;
}

void run_database(const std::string& dir, long long max_scale) {
    note("== database ==");

    for (std::size_t si = 0; si < kScaleCount; ++si) {
        const long long n = kScales[si];
        if (n > max_scale) {
            note("  (skipping scale %lld: above --max-db-scale)", n);
            continue;
        }
        const std::string db = dir + "/dbview.db";
        drop_db(db);

        // The database store does NOT own the `page` table -- a Database is
        // anchored to a page (ADR-0004), so the block store's schema has to
        // exist first and the page row has to be real.
        uapp::storage::sqlite::SqliteBlockStoreOptions block_options;
        block_options.path = db;
        std::unique_ptr<uapp::blocks::BlockStore> blocks;
        if (const Status s =
                uapp::storage::sqlite::open_sqlite_block_store(block_options, blocks);
            !is_ok(s)) {
            die("open block store (database)", s);
        }
        uapp::blocks::Page anchor;
        anchor.id             = counter_id<uapp::blocks::PageId>(1);
        anchor.title          = "Anchor";
        anchor.created_at_ms  = 1'700'000'000'000;
        anchor.modified_at_ms = 1'700'000'000'000;
        if (const Status s = blocks->create_page(anchor); !is_ok(s)) {
            die("create anchor page", s);
        }

        uapp::storage::sqlite::SqliteDatabaseStoreOptions db_options;
        db_options.path = db;
        std::unique_ptr<dbns::DatabaseStore> store;
        if (const Status s =
                uapp::storage::sqlite::open_sqlite_database_store(db_options, store);
            !is_ok(s)) {
            die("open database store", s);
        }

        const dbns::Database schema = make_schema();
        if (const Status s = store->create_database(schema); !is_ok(s)) {
            die("create_database", s);
        }

        // --- INSERT n rows, 6 property types each --------------------------
        Timer t;
        bool insert_ok    = true;
        Status insert_err = Status::Ok;
        for (long long i = 0; i < n; ++i) {
            if (const Status s = store->insert_row(make_row(i), 1'700'000'000'000);
                !is_ok(s)) {
                insert_ok  = false;
                insert_err = s;
                break;
            }
        }
        const double insert_ms = t.ms();
        emit_row("database", "insert_rows", n, n, insert_ms, insert_ok ? "ok" : "FAIL",
                 insert_ok ? "" : status_text(insert_err).c_str());
        if (!insert_ok) {
            store.reset();
            blocks.reset();
            drop_db(db);
            continue;
        }

        std::size_t rows = 0;
        if (const Status s = store->row_count(schema.id, rows); !is_ok(s)) {
            die("row_count", s);
        }
        char detail[192];
        std::snprintf(detail, sizeof detail, "rows=%zu db_bytes=%lld", rows, db_bytes(db));
        emit_row("database", "row_count", n, 1, 0.0,
                 rows == static_cast<std::size_t>(n) ? "ok" : "FAIL", detail);

        std::vector<dbns::Row> out;
        constexpr int kReps = 10;

        // --- VIEW A: unfiltered, unsorted, first page of 50 ----------------
        {
            dbns::ViewConfig cfg;
            t.reset();
            for (int r = 0; r < kReps; ++r) {
                if (const Status s = store->query_table_view(schema.id, cfg, 50, out);
                    !is_ok(s)) {
                    die("view unfiltered", s);
                }
            }
            const double ms = t.ms();
            std::snprintf(detail, sizeof detail, "returned=%zu", out.size());
            emit_row("database", "view_page50_plain", n, kReps, ms, "ok", detail);
        }

        // --- VIEW B: Select filter + Number sort, first page of 50 ---------
        {
            dbns::ViewConfig cfg;
            dbns::Filter f;
            f.property_id = counter_id<dbns::PropertyId>(13);
            f.op          = dbns::FilterOperator::Equals;
            f.operand     = dbns::PropertyValue{dbns::SelectValue{counter_id<dbns::OptionId>(21)}};
            cfg.filters.push_back(f);
            dbns::Sort sort;
            sort.property_id = counter_id<dbns::PropertyId>(11);
            sort.descending  = true;
            cfg.sorts.push_back(sort);
            t.reset();
            for (int r = 0; r < kReps; ++r) {
                if (const Status s = store->query_table_view(schema.id, cfg, 50, out);
                    !is_ok(s)) {
                    die("view filtered+sorted", s);
                }
            }
            const double ms = t.ms();
            std::snprintf(detail, sizeof detail, "returned=%zu", out.size());
            emit_row("database", "view_page50_filter_sort", n, kReps, ms, "ok", detail);
        }

        // --- VIEW C: Text Contains (no index can help) + Date sort ---------
        {
            dbns::ViewConfig cfg;
            dbns::Filter f;
            f.property_id = counter_id<dbns::PropertyId>(10);
            f.op          = dbns::FilterOperator::Contains;
            f.operand     = dbns::PropertyValue{dbns::TextValue{"alpha"}};
            cfg.filters.push_back(f);
            dbns::Sort sort;
            sort.property_id = counter_id<dbns::PropertyId>(15);
            cfg.sorts.push_back(sort);
            t.reset();
            for (int r = 0; r < kReps; ++r) {
                if (const Status s = store->query_table_view(schema.id, cfg, 50, out);
                    !is_ok(s)) {
                    die("view contains", s);
                }
            }
            const double ms = t.ms();
            std::snprintf(detail, sizeof detail, "returned=%zu", out.size());
            emit_row("database", "view_page50_text_contains", n, kReps, ms, "ok", detail);
        }

        // --- VIEW D: three filters + two sorts, materialise 1000 rows ------
        {
            dbns::ViewConfig cfg;
            dbns::Filter f1;
            f1.property_id = counter_id<dbns::PropertyId>(12);
            f1.op          = dbns::FilterOperator::Equals;
            f1.operand     = dbns::PropertyValue{dbns::CheckboxValue{true}};
            cfg.filters.push_back(f1);
            dbns::Filter f2;
            f2.property_id = counter_id<dbns::PropertyId>(11);
            f2.op          = dbns::FilterOperator::GreaterThan;
            f2.operand     = dbns::PropertyValue{dbns::NumberValue{100.0}};
            cfg.filters.push_back(f2);
            dbns::Filter f3;
            f3.property_id = counter_id<dbns::PropertyId>(14);
            f3.op          = dbns::FilterOperator::Contains;
            // The documented exception: `contains` on a Multi-select takes ONE
            // option, so its operand is a SelectValue even though the property
            // is a MultiSelect (core/database/include/.../view.h, Filter::operand).
            f3.operand = dbns::PropertyValue{dbns::SelectValue{counter_id<dbns::OptionId>(31)}};
            cfg.filters.push_back(f3);
            dbns::Sort s1;
            s1.property_id = counter_id<dbns::PropertyId>(13);
            cfg.sorts.push_back(s1);
            dbns::Sort s2;
            s2.property_id = counter_id<dbns::PropertyId>(11);
            s2.descending  = true;
            cfg.sorts.push_back(s2);
            t.reset();
            for (int r = 0; r < kReps; ++r) {
                if (const Status s = store->query_table_view(schema.id, cfg, 1000, out);
                    !is_ok(s)) {
                    die("view complex", s);
                }
            }
            const double ms = t.ms();
            std::snprintf(detail, sizeof detail, "returned=%zu filters=3 sorts=2", out.size());
            emit_row("database", "view_1000_3filter_2sort", n, kReps, ms, "ok", detail);
        }

        // --- VIEW E: materialise everything (worst case a UI can ask for) --
        {
            dbns::ViewConfig cfg;
            t.reset();
            if (const Status s = store->query_table_view(schema.id, cfg,
                                                         dbns::kMaxRowsPerDatabase, out);
                !is_ok(s)) {
                die("view full scan", s);
            }
            const double ms = t.ms();
            std::snprintf(detail, sizeof detail, "returned=%zu", out.size());
            emit_row("database", "view_full_materialise", n, 1, ms,
                     out.size() == static_cast<std::size_t>(n) ? "ok" : "FAIL", detail);
        }

        // --- THE LIMIT, at the top of the ladder only ----------------------
        if (n == static_cast<long long>(dbns::kMaxRowsPerDatabase)) {
            t.reset();
            const Status s = store->insert_row(make_row(n), 1'700'000'000'000);
            const double ms = t.ms();
            emit_row("database", "limit_rows_per_database", n + 1, 1, ms,
                     s == Status::DatabaseRowLimitExceeded ? "limit" : "FAIL",
                     status_text(s).c_str());
        }

        store.reset();
        blocks.reset();
        drop_db(db);
    }
}

// ---------------------------------------------------------------------------
// 5. core/conversion — Page <-> Markdown
// ---------------------------------------------------------------------------

/// Pure in-memory: no store is involved in either direction, so this runner
/// measures the converter and nothing else.
///
/// `write_side` and `read_side` are separable because peak RSS is per-PROCESS.
/// Running both halves together gives the maximum of the two and attributes it
/// to neither, and the read half's peak is the number that has to be compared
/// against the Tier D conversion arena cap (docs/performance-strategy.md §1.1,
/// 96 MB). Pass `conversion_read` or `conversion_write` to isolate one.
void run_conversion(bool write_side, bool read_side) {
    note("== conversion (write=%d read=%d) ==", write_side ? 1 : 0, read_side ? 1 : 0);

    auto clock = uapp::make_system_clock();
    auto ids   = uapp::make_random_id_source();
    if (clock == nullptr || ids == nullptr) { die("conversion ports", Status::Internal); }

  if (write_side) {
    // --- WRITE: Page -> Markdown, at increasing block counts --------------
    const long long write_scales[] = {10, 100, 1000, 10000,
                                      static_cast<long long>(uapp::blocks::kMaxBlocksPerPage)};
    for (const long long n : write_scales) {
        uapp::blocks::Page page;
        page.id             = counter_id<uapp::blocks::PageId>(1);
        page.title          = "Conversion load test";
        page.created_at_ms  = 1'700'000'000'000;
        page.modified_at_ms = 1'700'000'000'000;
        page.blocks.reserve(static_cast<std::size_t>(n));
        for (long long i = 0; i < n; ++i) {
            uapp::blocks::Block b;
            b.id      = counter_id<uapp::blocks::BlockId>(static_cast<std::uint64_t>(i) + 2u);
            b.page_id = page.id;
            // A mix, so the writer exercises more than one emitter.
            if (i % 11 == 0) {
                b.content = uapp::blocks::HeadingBlock{body_for(i), 1 + static_cast<int>(i % 3)};
            } else if (i % 7 == 0) {
                b.content = uapp::blocks::BulletedListItemBlock{body_for(i)};
            } else if (i % 13 == 0) {
                b.content = uapp::blocks::CodeBlock{body_for(i), "cpp"};
            } else {
                b.content = uapp::blocks::ParagraphBlock{body_for(i)};
            }
            b.created_at_ms  = 1'700'000'000'000;
            b.modified_at_ms = 1'700'000'000'000;
            page.blocks.push_back(std::move(b));
        }

        std::string md;
        uapp::conversion::FidelityReport report;
        const uapp::conversion::MarkdownWriteOptions options;
        Timer t;
        const Status s = uapp::conversion::page_to_markdown(page, options, md, report);
        const double ms = t.ms();

        char detail[192];
        if (is_ok(s)) {
            std::snprintf(detail, sizeof detail, "out_bytes=%zu fidelity_entries=%zu",
                          md.size(), report.entries().size());
            emit_row("conversion", "page_to_markdown", n, n, ms, "ok", detail);
        } else {
            std::snprintf(detail, sizeof detail, "%s", status_text(s).c_str());
            emit_row("conversion", "page_to_markdown", n, n, ms,
                     s == Status::ConversionOutputTooLarge ? "limit" : "FAIL", detail);
        }
    }

    // --- WRITE: force the output cap -------------------------------------
    // kMaxMarkdownOutputBytes is 16 MiB. ~1 KiB per block means ~17 000
    // blocks crosses it while staying well under kMaxBlocksPerPage, so the
    // cap under test is the OUTPUT one and not the block-count one.
    {
        std::string filler(1000, 'x');
        uapp::blocks::Page page;
        page.id             = counter_id<uapp::blocks::PageId>(1);
        page.title          = "Output cap";
        page.created_at_ms  = 1'700'000'000'000;
        page.modified_at_ms = 1'700'000'000'000;
        const long long blocks_needed = 18000;
        page.blocks.reserve(static_cast<std::size_t>(blocks_needed));
        for (long long i = 0; i < blocks_needed; ++i) {
            uapp::blocks::Block b;
            b.id      = counter_id<uapp::blocks::BlockId>(static_cast<std::uint64_t>(i) + 2u);
            b.page_id = page.id;
            b.content = uapp::blocks::ParagraphBlock{filler};
            b.created_at_ms  = 1'700'000'000'000;
            b.modified_at_ms = 1'700'000'000'000;
            page.blocks.push_back(std::move(b));
        }
        std::string md;
        uapp::conversion::FidelityReport report;
        const uapp::conversion::MarkdownWriteOptions options;
        Timer t;
        const Status s = uapp::conversion::page_to_markdown(page, options, md, report);
        const double ms = t.ms();
        char detail[192];
        std::snprintf(detail, sizeof detail, "%s out_bytes=%zu", status_text(s).c_str(),
                      md.size());
        emit_row("conversion", "limit_markdown_output_bytes", blocks_needed, 1, ms,
                 s == Status::ConversionOutputTooLarge ? "limit" : "FAIL", detail);
    }
  }

  if (read_side) {
    // --- READ: Markdown -> Page, at increasing input sizes ----------------
    // 400-byte paragraph lines: 16 MiB of input is ~41 900 blocks, which stays
    // under kMaxBlocksPerPage so that the INPUT cap is what gets probed.
    const std::size_t line_bytes = 400;
    const std::size_t read_sizes[] = {
        1u * 1024u,
        100u * 1024u,
        1u * 1024u * 1024u,
        8u * 1024u * 1024u,
        uapp::conversion::kMaxMarkdownInputBytes,
        uapp::conversion::kMaxMarkdownInputBytes + 1u,
    };
    for (const std::size_t want : read_sizes) {
        std::string md;
        md.reserve(want + line_bytes);
        const std::string line(line_bytes - 2u, 'w');
        while (md.size() < want) {
            md += line;
            md += "\n\n";
        }
        md.resize(want);

        uapp::blocks::Page page;
        uapp::conversion::FidelityReport report;
        uapp::conversion::MarkdownReadOptions options;
        options.default_title = "Read load test";
        Timer t;
        const Status s =
            uapp::conversion::markdown_to_page(md, options, *clock, *ids, page, report);
        const double ms = t.ms();

        const bool over = want > uapp::conversion::kMaxMarkdownInputBytes;
        char detail[224];
        if (is_ok(s)) {
            std::snprintf(detail, sizeof detail, "in_bytes=%zu blocks=%zu fidelity_entries=%zu",
                          md.size(), page.blocks.size(), report.entries().size());
            emit_row("conversion", "markdown_to_page", static_cast<long long>(want),
                     static_cast<long long>(page.blocks.size()), ms, over ? "FAIL" : "ok",
                     detail);
        } else {
            std::snprintf(detail, sizeof detail, "%s in_bytes=%zu", status_text(s).c_str(),
                          md.size());
            const bool correct = over && s == Status::ConversionInputTooLarge;
            emit_row("conversion",
                     over ? "limit_markdown_input_bytes" : "markdown_to_page",
                     static_cast<long long>(want), 0, ms, correct ? "limit" : "FAIL", detail);
        }
    }

    // --- ROUND TRIP at a realistic document size -------------------------
    {
        uapp::blocks::Page page;
        page.id             = counter_id<uapp::blocks::PageId>(1);
        page.title          = "Round trip";
        page.created_at_ms  = 1'700'000'000'000;
        page.modified_at_ms = 1'700'000'000'000;
        constexpr long long n = 5000;
        for (long long i = 0; i < n; ++i) {
            uapp::blocks::Block b;
            b.id      = counter_id<uapp::blocks::BlockId>(static_cast<std::uint64_t>(i) + 2u);
            b.page_id = page.id;
            b.content = uapp::blocks::ParagraphBlock{body_for(i)};
            b.created_at_ms  = 1'700'000'000'000;
            b.modified_at_ms = 1'700'000'000'000;
            page.blocks.push_back(std::move(b));
        }
        std::string md;
        uapp::conversion::FidelityReport wr;
        const uapp::conversion::MarkdownWriteOptions wopts;
        Timer t;
        Status s = uapp::conversion::page_to_markdown(page, wopts, md, wr);
        if (!is_ok(s)) { die("round trip write", s); }
        uapp::blocks::Page back;
        uapp::conversion::FidelityReport rr;
        uapp::conversion::MarkdownReadOptions ropts;
        s = uapp::conversion::markdown_to_page(md, ropts, *clock, *ids, back, rr);
        const double ms = t.ms();
        if (!is_ok(s)) { die("round trip read", s); }
        char detail[224];
        std::snprintf(detail, sizeof detail, "md_bytes=%zu blocks_in=%zu blocks_out=%zu",
                      md.size(), page.blocks.size(), back.blocks.size());
        emit_row("conversion", "round_trip", n, n * 2, ms,
                 back.blocks.size() == page.blocks.size() ? "ok" : "FAIL", detail);
    }
  }
}

// ---------------------------------------------------------------------------

void print_usage() {
    std::fputs(
        "uapp-loadtest-storage — scale harness for notes/blocks/search/database/conversion\n"
        "\n"
        "Usage:\n"
        "  uapp-loadtest-storage --dir <scratch-dir> [--max-db-scale N] <module>...\n"
        "\n"
        "Modules: notes blocks search database conversion all\n"
        "         conversion_write / conversion_read isolate one direction, so that\n"
        "         peak RSS is attributable to it (peak RSS is per-process)\n"
        "\n"
        "Writes one CSV row per measured point to stdout; commentary to stderr.\n",
        stderr);
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir;
    long long max_db_scale = static_cast<long long>(dbns::kMaxRowsPerDatabase);
    std::vector<std::string> modules;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            dir = argv[++i];
        } else if (std::strcmp(argv[i], "--max-db-scale") == 0 && i + 1 < argc) {
            max_db_scale = std::atoll(argv[++i]);
        } else if (argv[i][0] == '-') {
            print_usage();
            return 2;
        } else {
            modules.emplace_back(argv[i]);
        }
    }
    if (dir.empty() || modules.empty()) {
        print_usage();
        return 2;
    }

    const bool all = modules.size() == 1 && modules[0] == "all";
    const auto wants = [&](const char* name) {
        if (all) { return true; }
        for (const std::string& m : modules) {
            if (m == name) { return true; }
        }
        return false;
    };

    uapp::loadtest::emit_csv_header();
    note("loadtest-storage: scratch dir = %s", dir.c_str());

    if (wants("notes"))      { run_notes(dir); }
    if (wants("blocks"))     { run_blocks(dir); }
    if (wants("search"))     { run_search(dir); }
    if (wants("database"))   { run_database(dir, max_db_scale); }
    const bool conv_write = wants("conversion") || wants("conversion_write");
    const bool conv_read  = wants("conversion") || wants("conversion_read");
    if (conv_write || conv_read) { run_conversion(conv_write, conv_read); }

    note("loadtest-storage: done. process peak RSS = %.2f MiB",
         uapp::loadtest::peak_rss_mib());
    return 0;
}
