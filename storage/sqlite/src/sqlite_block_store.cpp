// SPDX-License-Identifier: Apache-2.0
//
// The SQLite adapter for the block document model. Together with
// sqlite_note_store.cpp this is one of only two translation units in the
// project that include <sqlite3.h>. See PHASE0_SUMMARY.md H-3.

#include "uapp/storage/sqlite/sqlite_block_store.h"

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "uapp/blocks/block.h"
#include "uapp/blocks/block_store.h"
#include "uapp/blocks/page.h"
#include "uapp/object_id.h"
#include "uapp/search/search_result.h"
#include "document_sql.h"
#include "search_index_sql.h"
#include "sqlite_raii.h"

namespace uapp::storage::sqlite {
namespace {

using blocks::Block;
using blocks::BlockContent;
using blocks::BlockId;
using blocks::BlockType;
using blocks::Page;
using blocks::PageId;

// ---------------------------------------------------------------------------
// Schema
//
// THE DDL ITSELF MOVED to document_sql.h once a SECOND adapter
// (sqlite_database_store.cpp) came to need the `page` table it creates. That
// header carries the full rationale for the shape -- the two documented
// deviations from database-design.md section 3.3 (integer `sort_index`
// instead of a fractional `sort_key`; typed content columns instead of a CBOR
// blob), the absent `page.kind` column, the absent `block.text_cache` column,
// and why there is no UNIQUE constraint on the sibling ordering. Nothing
// about the shape changed in the move, and the move is for one reason: two
// copies of `CREATE TABLE IF NOT EXISTS page` differing by one CHECK means
// whichever adapter opens the file first silently wins.
//
// THIS FILE STILL OWNS THOSE TABLES. It is the only translation unit that
// writes to them, and everything about HOW -- the encode/decode pair, the
// tree assembly, the sibling arithmetic, the schema-version bookkeeping --
// stays here. Density of `sort_index` within a sibling group is an invariant
// this file maintains and the tests assert.
//
// FULL-TEXT INDEXING (was NOT IMPLEMENTED; now is)
// ---------------------------------------------------------------------------
// Every mutating method below also maintains the SHARED unified index defined
// in search_index_sql.h, inside its own existing transaction -- the same
// one-transaction discipline sqlite_note_store.cpp already used for
// `note_fts`, so a block that is stored but not indexed is not a reachable
// state. Granularity is per-BLOCK (one index entry per text-bearing block,
// carrying the page id as its container) plus one per-PAGE entry for the page
// TITLE, which lives in no block. The reasoning for block-level over
// page-level is argued in search_index_sql.h, "DESIGN DECISION 2".
//
// ---------------------------------------------------------------------------

/// Upper bound for the recursive-CTE probes. Legitimate trees never reach it
/// because depth is enforced on every write, so hitting it means the file is
/// corrupt (or cyclic) -- and, crucially, it is what makes the recursion
/// terminate on such a file instead of spinning.
constexpr std::int64_t kDepthProbeCap = static_cast<std::int64_t>(blocks::kMaxBlockDepth) + 2;

/// Map a SQLite result code to our vocabulary for a *write*.
Status write_status_for(int rc) noexcept {
    switch (rc) {
        case SQLITE_OK:
        case SQLITE_DONE:
        case SQLITE_ROW:
            return Status::Ok;
        case SQLITE_READONLY:
            return Status::StoreReadOnly;
        case SQLITE_CONSTRAINT:
            return Status::AlreadyExists;
        default:
            return Status::StoreWriteFailed;
    }
}

Status bind_id(Statement& stmt, int index, const ObjectIdBytes& bytes) noexcept {
    return stmt.bind_blob(index, bytes.data(), kObjectIdBytes);
}

/// Bind a parent id, sending SQL NULL for the nil id. Every query that
/// compares against this must use `parent_id IS ?`, never `=`.
Status bind_parent(Statement& stmt, int index, const BlockId& parent) noexcept {
    if (parent.is_nil()) { return stmt.bind_null(index); }
    return bind_id(stmt, index, parent.bytes());
}

Status bind_index(Statement& stmt, int index, std::size_t value) noexcept {
    if (value > blocks::kMaxBlocksPerPage) { return Status::BlockPositionInvalid; }
    return stmt.bind_int64(index, static_cast<std::int64_t>(value));
}

Status read_count(Statement& stmt, std::size_t& out) noexcept {
    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_ROW) { return Status::StoreReadFailed; }
    const std::int64_t n = stmt.column_int64(0);
    if (n < 0) { return Status::StoreReadFailed; }
    out = static_cast<std::size_t>(n);
    return Status::Ok;
}

// --- content <-> columns ---------------------------------------------------

struct ColumnPayload final {
    std::int32_t     type = 0;
    std::string_view text;      ///< borrows from the BlockContent; bind is TRANSIENT
    std::int64_t     checked = 0;
    std::int64_t     heading_level = 0;
    std::string_view language;
};

/// Project a payload onto the four content columns. Total over the variant:
/// adding an alternative without extending this is caught by the final
/// `return Status::Internal`, and by the tests, not by silence.
Status encode_content(const BlockContent& content, ColumnPayload& out) noexcept {
    ColumnPayload p;
    p.type = static_cast<std::int32_t>(blocks::block_type_of(content));

    // Each alternative gets its own name rather than reusing one in an
    // else-if chain: -Wshadow (a -Werror warning here) fires on the reuse,
    // and the distinct names make a missing case obvious on sight.
    if (const auto* paragraph = std::get_if<blocks::ParagraphBlock>(&content)) {
        p.text = paragraph->text;
    } else if (const auto* heading = std::get_if<blocks::HeadingBlock>(&content)) {
        p.text = heading->text;
        p.heading_level = static_cast<std::int64_t>(heading->level);
    } else if (const auto* bullet = std::get_if<blocks::BulletedListItemBlock>(&content)) {
        p.text = bullet->text;
    } else if (const auto* numbered = std::get_if<blocks::NumberedListItemBlock>(&content)) {
        p.text = numbered->text;
    } else if (const auto* checklist = std::get_if<blocks::ChecklistItemBlock>(&content)) {
        p.text = checklist->text;
        p.checked = checklist->checked ? 1 : 0;
    } else if (const auto* code = std::get_if<blocks::CodeBlock>(&content)) {
        p.text = code->code;
        p.language = code->language;
    } else if (std::get_if<blocks::DividerBlock>(&content) != nullptr) {
        // No payload at all.
    } else if (const auto* quote = std::get_if<blocks::QuoteBlock>(&content)) {
        p.text = quote->text;
    } else {
        return Status::Internal;
    }

    out = p;
    return Status::Ok;
}

/// Rebuild a payload from the columns. Everything here came out of a file on
/// disk, which is attacker-reachable (a synced or restored .db), so the
/// result is validated before it is handed back -- spec section 28, "never
/// assume an input is valid, including one that used to be ours".
Status decode_content(std::int64_t type,
                      std::string text,
                      std::int64_t checked,
                      std::int64_t heading_level,
                      std::string language,
                      BlockContent& out) noexcept {
    if (type < 0 || type >= blocks::kBlockTypeCount) {
        return Status::StoreReadFailed;
    }
    const auto block_type = static_cast<BlockType>(static_cast<std::int32_t>(type));
    if (!blocks::block_type_is_implemented(block_type)) {
        // A row written by a newer build that supports more types. Refusing is
        // the honest answer; rendering it as an empty paragraph would silently
        // destroy the user's content on the next save.
        return Status::BlockTypeUnsupported;
    }

    BlockContent decoded;
    switch (block_type) {
        case BlockType::Paragraph:
            decoded = blocks::ParagraphBlock{std::move(text)};
            break;
        case BlockType::Heading: {
            if (heading_level < blocks::kMinHeadingLevel ||
                heading_level > blocks::kMaxHeadingLevel) {
                return Status::StoreReadFailed;
            }
            blocks::HeadingBlock h;
            h.text  = std::move(text);
            h.level = static_cast<int>(heading_level);
            decoded = std::move(h);
            break;
        }
        case BlockType::BulletedListItem:
            decoded = blocks::BulletedListItemBlock{std::move(text)};
            break;
        case BlockType::NumberedListItem:
            decoded = blocks::NumberedListItemBlock{std::move(text)};
            break;
        case BlockType::ChecklistItem: {
            if (checked != 0 && checked != 1) { return Status::StoreReadFailed; }
            blocks::ChecklistItemBlock c;
            c.text    = std::move(text);
            c.checked = (checked == 1);
            decoded   = std::move(c);
            break;
        }
        case BlockType::Code: {
            blocks::CodeBlock c;
            c.code     = std::move(text);
            c.language = std::move(language);
            decoded    = std::move(c);
            break;
        }
        case BlockType::Divider:
            decoded = blocks::DividerBlock{};
            break;
        case BlockType::Quote:
            decoded = blocks::QuoteBlock{std::move(text)};
            break;
        default:
            return Status::BlockTypeUnsupported;
    }

    if (const Status s = blocks::validate_block_content(decoded); !is_ok(s)) {
        return Status::StoreReadFailed;
    }
    out = std::move(decoded);
    return Status::Ok;
}

// ---------------------------------------------------------------------------

/// The columns of one `block` row, before the tree is reassembled.
struct BlockRow final {
    BlockId     id;
    BlockId     parent_id;  ///< nil when the column was NULL
    PageId      page_id;
    std::size_t sort_index = 0;
};

class SqliteBlockStore final : public blocks::BlockStore {
public:
    SqliteBlockStore(Connection conn, bool read_only) noexcept
        : conn_(std::move(conn)), read_only_(read_only) {}

    [[nodiscard]] Status create_page(const Page& page) noexcept override;
    [[nodiscard]] Status get_page(const PageId& id, Page& out) const noexcept override;
    [[nodiscard]] Status set_page_title(const PageId& id,
                                        std::string_view title,
                                        std::int64_t now_ms) noexcept override;
    [[nodiscard]] Status delete_page(const PageId& id) noexcept override;
    [[nodiscard]] Status page_count(std::size_t& out) const noexcept override;

    [[nodiscard]] Status insert_block(const PageId& page_id,
                                      const BlockId& parent_id,
                                      std::size_t position,
                                      const BlockId& new_id,
                                      const BlockContent& content,
                                      std::int64_t now_ms) noexcept override;
    [[nodiscard]] Status get_block(const BlockId& id, Block& out) const noexcept override;
    [[nodiscard]] Status update_block(const BlockId& id,
                                      const BlockContent& content,
                                      std::int64_t now_ms) noexcept override;
    [[nodiscard]] Status delete_block(const BlockId& id,
                                      std::int64_t now_ms) noexcept override;
    [[nodiscard]] Status move_block(const BlockId& id,
                                    const BlockId& new_parent_id,
                                    std::size_t new_position,
                                    std::int64_t now_ms) noexcept override;
    [[nodiscard]] Status block_count(const PageId& page_id,
                                     std::size_t& out) const noexcept override;

private:
    [[nodiscard]] Status require_page(const PageId& id) const noexcept;
    [[nodiscard]] Status load_row(const BlockId& id, BlockRow& out) const noexcept;
    [[nodiscard]] Status depth_of(const BlockId& id, std::size_t& out) const noexcept;
    [[nodiscard]] Status subtree_height(const BlockId& id, std::size_t& out) const noexcept;
    [[nodiscard]] Status has_ancestor(const BlockId& node,
                                      const BlockId& candidate,
                                      bool& out) const noexcept;
    [[nodiscard]] Status sibling_count(const PageId& page_id,
                                       const BlockId& parent_id,
                                       const BlockId& exclude,
                                       std::size_t& out) const noexcept;
    /// Shift every sibling at or after `from` by `delta` (+1 to open a gap,
    /// -1 to close one), optionally skipping one block.
    [[nodiscard]] Status shift_siblings(const PageId& page_id,
                                        const BlockId& parent_id,
                                        std::size_t from,
                                        int delta,
                                        const BlockId& exclude) noexcept;
    [[nodiscard]] Status touch_page(const PageId& page_id, std::int64_t now_ms) noexcept;

    /// Bring the unified index entry for one block in line with `content`.
    ///
    /// A block whose plain text is EMPTY gets no entry -- it is erased if one
    /// exists. Two cases reach that: a DividerBlock, which is pure structure
    /// and has nothing to find; and a paragraph the user cleared. Keeping an
    /// empty row would make the index larger, make `entry_count` meaningless
    /// as a staleness check, and -- because bm25 weights by average row
    /// length -- quietly skew the ranking of every real hit.
    ///
    /// MUST be called inside the caller's transaction.
    [[nodiscard]] Status reindex_block(const BlockId& id,
                                       const PageId& page_id,
                                       const BlockContent& content) noexcept;

    Connection conn_;
    bool       read_only_ = false;
};

// --- private helpers -------------------------------------------------------

Status SqliteBlockStore::require_page(const PageId& id) const noexcept {
    Statement stmt;
    if (const Status s = stmt.prepare(conn_.get(), "SELECT 1 FROM page WHERE id = ?1;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW)  { return Status::Ok; }
    if (rc == SQLITE_DONE) { return Status::NotFound; }
    return Status::StoreReadFailed;
}

Status SqliteBlockStore::load_row(const BlockId& id, BlockRow& out) const noexcept {
    Statement stmt;
    if (const Status s = stmt.prepare(
            conn_.get(),
            "SELECT page_id, parent_id, sort_index FROM block WHERE id = ?1;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) { return Status::NotFound; }
    if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

    BlockRow row;
    row.id = id;

    ObjectIdBytes raw{};
    if (const Status s = stmt.column_fixed_blob(0, raw.data(), raw.size()); !is_ok(s)) {
        return s;
    }
    row.page_id = PageId(raw);

    if (!stmt.column_is_null(1)) {
        ObjectIdBytes parent_raw{};
        if (const Status s = stmt.column_fixed_blob(1, parent_raw.data(), parent_raw.size());
            !is_ok(s)) {
            return s;
        }
        row.parent_id = BlockId(parent_raw);
    }

    const std::int64_t idx = stmt.column_int64(2);
    if (idx < 0) { return Status::StoreReadFailed; }
    row.sort_index = static_cast<std::size_t>(idx);

    out = row;
    return Status::Ok;
}

Status SqliteBlockStore::depth_of(const BlockId& id, std::size_t& out) const noexcept {
    // Walk upwards to the root. `UNION ALL` is safe here only because the
    // depth predicate bounds the recursion -- see kDepthProbeCap.
    Statement stmt;
    if (const Status s = stmt.prepare(
            conn_.get(),
            "WITH RECURSIVE anc(id, parent_id, depth) AS ("
            "  SELECT id, parent_id, 1 FROM block WHERE id = ?1"
            "  UNION ALL"
            "  SELECT b.id, b.parent_id, anc.depth + 1"
            "    FROM block b JOIN anc ON b.id = anc.parent_id"
            "   WHERE anc.depth < ?2"
            ") SELECT MAX(depth) FROM anc;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, id.bytes());        !is_ok(s)) { return s; }
    if (const Status s = stmt.bind_int64(2, kDepthProbeCap);  !is_ok(s)) { return s; }

    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_ROW) { return Status::StoreReadFailed; }
    if (stmt.column_is_null(0)) { return Status::NotFound; }

    const std::int64_t depth = stmt.column_int64(0);
    if (depth <= 0) { return Status::StoreReadFailed; }
    if (depth >= kDepthProbeCap) {
        // Only reachable if the stored tree is already deeper than we allow,
        // or is cyclic. Either way the file is corrupt, not the request.
        return Status::StoreReadFailed;
    }
    out = static_cast<std::size_t>(depth);
    return Status::Ok;
}

Status SqliteBlockStore::subtree_height(const BlockId& id, std::size_t& out) const noexcept {
    // `UNION` (not UNION ALL): on a cyclic file the deduplication plus the
    // depth predicate together guarantee termination.
    Statement stmt;
    if (const Status s = stmt.prepare(
            conn_.get(),
            "WITH RECURSIVE sub(id, depth) AS ("
            "  SELECT id, 1 FROM block WHERE id = ?1"
            "  UNION"
            "  SELECT b.id, sub.depth + 1"
            "    FROM block b JOIN sub ON b.parent_id = sub.id"
            "   WHERE sub.depth < ?2"
            ") SELECT MAX(depth) FROM sub;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, id.bytes());       !is_ok(s)) { return s; }
    if (const Status s = stmt.bind_int64(2, kDepthProbeCap); !is_ok(s)) { return s; }

    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_ROW) { return Status::StoreReadFailed; }
    if (stmt.column_is_null(0)) { return Status::NotFound; }

    const std::int64_t height = stmt.column_int64(0);
    if (height <= 0 || height >= kDepthProbeCap) { return Status::StoreReadFailed; }
    out = static_cast<std::size_t>(height);
    return Status::Ok;
}

Status SqliteBlockStore::has_ancestor(const BlockId& node,
                                      const BlockId& candidate,
                                      bool& out) const noexcept {
    Statement stmt;
    if (const Status s = stmt.prepare(
            conn_.get(),
            "WITH RECURSIVE anc(id, parent_id, depth) AS ("
            "  SELECT id, parent_id, 1 FROM block WHERE id = ?1"
            "  UNION ALL"
            "  SELECT b.id, b.parent_id, anc.depth + 1"
            "    FROM block b JOIN anc ON b.id = anc.parent_id"
            "   WHERE anc.depth < ?3"
            ") SELECT COUNT(*) FROM anc WHERE id = ?2;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, node.bytes());      !is_ok(s)) { return s; }
    if (const Status s = bind_id(stmt, 2, candidate.bytes()); !is_ok(s)) { return s; }
    if (const Status s = stmt.bind_int64(3, kDepthProbeCap);  !is_ok(s)) { return s; }

    std::size_t hits = 0;
    if (const Status s = read_count(stmt, hits); !is_ok(s)) { return s; }
    out = hits > 0;
    return Status::Ok;
}

Status SqliteBlockStore::sibling_count(const PageId& page_id,
                                       const BlockId& parent_id,
                                       const BlockId& exclude,
                                       std::size_t& out) const noexcept {
    Statement stmt;
    if (const Status s = stmt.prepare(
            conn_.get(),
            "SELECT COUNT(*) FROM block "
            "WHERE page_id = ?1 AND parent_id IS ?2 AND (?3 IS NULL OR id <> ?3);");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, page_id.bytes());  !is_ok(s)) { return s; }
    if (const Status s = bind_parent(stmt, 2, parent_id);    !is_ok(s)) { return s; }
    if (const Status s = bind_parent(stmt, 3, exclude);      !is_ok(s)) { return s; }
    return read_count(stmt, out);
}

Status SqliteBlockStore::shift_siblings(const PageId& page_id,
                                        const BlockId& parent_id,
                                        std::size_t from,
                                        int delta,
                                        const BlockId& exclude) noexcept {
    Statement stmt;
    if (const Status s = stmt.prepare(
            conn_.get(),
            "UPDATE block SET sort_index = sort_index + ?4 "
            "WHERE page_id = ?1 AND parent_id IS ?2 AND sort_index >= ?3 "
            "  AND (?5 IS NULL OR id <> ?5);");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, page_id.bytes()); !is_ok(s)) { return s; }
    if (const Status s = bind_parent(stmt, 2, parent_id);   !is_ok(s)) { return s; }
    if (const Status s = bind_index(stmt, 3, from);         !is_ok(s)) { return s; }
    if (const Status s = stmt.bind_int64(4, delta);         !is_ok(s)) { return s; }
    if (const Status s = bind_parent(stmt, 5, exclude);     !is_ok(s)) { return s; }

    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) { return write_status_for(rc); }
    return Status::Ok;
}

Status SqliteBlockStore::touch_page(const PageId& page_id, std::int64_t now_ms) noexcept {
    if (now_ms < 0) { return Status::InvalidArgument; }
    Statement stmt;
    if (const Status s = stmt.prepare(
            conn_.get(), "UPDATE page SET modified_at_ms = ?2 WHERE id = ?1;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, page_id.bytes()); !is_ok(s)) { return s; }
    if (const Status s = stmt.bind_int64(2, now_ms);        !is_ok(s)) { return s; }

    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) { return write_status_for(rc); }
    return Status::Ok;
}

Status SqliteBlockStore::reindex_block(const BlockId& id,
                                       const PageId& page_id,
                                       const BlockContent& content) noexcept {
    const std::string_view text = blocks::block_plain_text(content);
    if (text.empty()) {
        return search_index::erase(conn_.get(), search::SourceKind::Block, id.bytes());
    }
    // The block's own `title` column is empty: a block has no title, and
    // putting the page's title here would duplicate the page's own entry into
    // every block on the page -- which would both bloat the index and make a
    // page-title query return every block on that page.
    return search_index::put(conn_.get(), search::SourceKind::Block, id.bytes(),
                             &page_id.bytes(), std::string_view(), text);
}

// --- pages -----------------------------------------------------------------

Status SqliteBlockStore::create_page(const Page& page) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    // Re-validate at the adapter boundary: the service validates too, but
    // this interface is reachable by anyone holding the pointer, and the
    // schema CHECKs are the last line rather than the only one.
    if (const Status s = blocks::validate_page_metadata(page); !is_ok(s)) { return s; }
    if (!page.blocks.empty()) { return Status::InvalidArgument; }

    // This method used to be a single INSERT and needed no transaction. It
    // writes two rows now (content + index), so it gets one: a page that
    // exists but is unfindable by title is exactly the half-applied state the
    // BlockStore contract rules out.
    Transaction txn;
    if (const Status s = txn.begin(conn_.get()); !is_ok(s)) { return s; }

    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "INSERT INTO page(id, title, created_at_ms, modified_at_ms) "
                "VALUES(?1, ?2, ?3, ?4);");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, page.id.bytes());       !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_text(2, page.title);           !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(3, page.created_at_ms);  !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(4, page.modified_at_ms); !is_ok(s)) { return s; }

        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) { return write_status_for(rc); }
    }

    // An empty title is a legal, common state ("Untitled"), and it is indexed
    // as an empty row anyway rather than skipped: unlike a block, a page is
    // always openable, so its entry is also what every Block hit on it joins
    // to for a display title. Skipping it would make block results titleless.
    if (const Status s = search_index::put(conn_.get(), search::SourceKind::Page,
                                           page.id.bytes(), nullptr,
                                           page.title, std::string_view());
        !is_ok(s)) {
        return s;
    }

    return txn.commit();
}

Status SqliteBlockStore::get_page(const PageId& id, Page& out) const noexcept {
    if (id.is_nil()) { return Status::IdMalformed; }

    Page page;
    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "SELECT title, created_at_ms, modified_at_ms FROM page WHERE id = ?1;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }

        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) { return Status::NotFound; }
        if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

        page.id             = id;
        page.title          = stmt.column_text(0);
        page.created_at_ms  = stmt.column_int64(1);
        page.modified_at_ms = stmt.column_int64(2);
    }
    if (const Status s = blocks::validate_page_metadata(page); !is_ok(s)) {
        return Status::StoreReadFailed;
    }

    // Read every block of the page flat, ordered so that each sibling group
    // already arrives in document order, then reassemble the tree in memory.
    // One query, not one per level: a per-level query would be O(depth) round
    // trips and would race with a concurrent writer between levels.
    std::vector<Block> rows;
    std::map<ObjectIdBytes, std::vector<std::size_t>> children_of;
    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "SELECT id, parent_id, type, text, checked, heading_level, language, "
                "       created_at_ms, modified_at_ms "
                "FROM block WHERE page_id = ?1 "
                "ORDER BY parent_id, sort_index;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }

        for (;;) {
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) { break; }
            if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

            if (rows.size() >= blocks::kMaxBlocksPerPage) {
                return Status::PageBlockLimitExceeded;
            }

            Block block;
            ObjectIdBytes raw{};
            if (const Status s = stmt.column_fixed_blob(0, raw.data(), raw.size());
                !is_ok(s)) {
                return s;
            }
            block.id      = BlockId(raw);
            block.page_id = id;

            ObjectIdBytes parent_key{};  // nil == the page's top level
            if (!stmt.column_is_null(1)) {
                if (const Status s =
                        stmt.column_fixed_blob(1, parent_key.data(), parent_key.size());
                    !is_ok(s)) {
                    return s;
                }
                block.parent_id = BlockId(parent_key);
            }

            if (const Status s = decode_content(stmt.column_int64(2),
                                                stmt.column_text(3),
                                                stmt.column_int64(4),
                                                stmt.column_int64(5),
                                                stmt.column_text(6),
                                                block.content);
                !is_ok(s)) {
                return s;
            }
            block.created_at_ms  = stmt.column_int64(7);
            block.modified_at_ms = stmt.column_int64(8);

            children_of[parent_key].push_back(rows.size());
            rows.push_back(std::move(block));
        }
    }

    // Reassemble. Bounded recursion: kMaxBlockDepth frames at worst, and the
    // `visited` marking turns a cyclic file into an error instead of a hang.
    std::vector<bool> visited(rows.size(), false);
    std::size_t attached = 0;

    struct Assembler final {
        std::vector<Block>& flat;
        std::vector<bool>&  taken;
        std::size_t&        placed;
        const std::map<ObjectIdBytes, std::vector<std::size_t>>& by_parent;

        Status run(const ObjectIdBytes& parent, std::size_t depth,
                   std::vector<Block>& dest) const noexcept {
            // Order matters: a block sitting at exactly kMaxBlockDepth is
            // legal, and we reach this call for it with depth == limit + 1.
            // Only the existence of children at that depth is the violation,
            // so look for them before judging the depth.
            const auto it = by_parent.find(parent);
            if (it == by_parent.end()) { return Status::Ok; }
            if (depth > blocks::kMaxBlockDepth) { return Status::BlockDepthExceeded; }

            for (const std::size_t at : it->second) {
                if (at >= flat.size() || taken[at]) {
                    return Status::StoreReadFailed;  // cycle or duplicate parent link
                }
                taken[at] = true;
                ++placed;

                Block node = std::move(flat[at]);
                if (const Status s = run(node.id.bytes(), depth + 1, node.children);
                    !is_ok(s)) {
                    return s;
                }
                dest.push_back(std::move(node));
            }
            return Status::Ok;
        }
    };

    const Assembler assembler{rows, visited, attached, children_of};
    const ObjectIdBytes root_key{};
    if (const Status s = assembler.run(root_key, 1, page.blocks); !is_ok(s)) {
        return s;
    }
    if (attached != rows.size()) {
        // Some row's parent_id points at a block that is not on this page (or
        // at nothing). That is an orphan, i.e. a corrupt file.
        return Status::StoreReadFailed;
    }

    if (const Status s = blocks::validate_block_tree(id, page.blocks); !is_ok(s)) {
        return s;
    }

    out = std::move(page);
    return Status::Ok;
}

Status SqliteBlockStore::set_page_title(const PageId& id,
                                        std::string_view title,
                                        std::int64_t now_ms) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    if (id.is_nil()) { return Status::IdMalformed; }
    if (const Status s = blocks::validate_page_title(title); !is_ok(s)) { return s; }
    if (now_ms < 0) { return Status::InvalidArgument; }

    Transaction txn;
    if (const Status s = txn.begin(conn_.get());  !is_ok(s)) { return s; }
    if (const Status s = require_page(id);        !is_ok(s)) { return s; }

    Statement stmt;
    if (const Status s = stmt.prepare(
            conn_.get(), "UPDATE page SET title = ?2, modified_at_ms = ?3 WHERE id = ?1;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }
    if (const Status s = stmt.bind_text(2, title);     !is_ok(s)) { return s; }
    if (const Status s = stmt.bind_int64(3, now_ms);   !is_ok(s)) { return s; }

    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) { return write_status_for(rc); }

    // `put` REPLACES the row, so the old title stops matching in the same
    // transaction that stops it being the title. The block entries on this
    // page are untouched: they join to this row for their display title, so
    // renaming a page relabels its hits without rewriting any of them.
    if (const Status s = search_index::put(conn_.get(), search::SourceKind::Page,
                                           id.bytes(), nullptr,
                                           title, std::string_view());
        !is_ok(s)) {
        return s;
    }

    return txn.commit();
}

Status SqliteBlockStore::delete_page(const PageId& id) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    if (id.is_nil()) { return Status::IdMalformed; }

    Transaction txn;
    if (const Status s = txn.begin(conn_.get()); !is_ok(s)) { return s; }
    if (const Status s = require_page(id);       !is_ok(s)) { return s; }

    // Blocks are deleted explicitly rather than left to ON DELETE CASCADE.
    // The cascade is declared and enabled, but relying on it would make
    // correctness depend on a per-connection PRAGMA that some future caller
    // could forget to set.
    {
        Statement stmt;
        if (const Status s = stmt.prepare(conn_.get(),
                                          "DELETE FROM block WHERE page_id = ?1;");
            !is_ok(s)) { return s; }
        if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }
        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) { return write_status_for(rc); }
    }
    {
        Statement stmt;
        if (const Status s = stmt.prepare(conn_.get(), "DELETE FROM page WHERE id = ?1;");
            !is_ok(s)) { return s; }
        if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }
        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) { return write_status_for(rc); }
    }

    // One statement pair drops the page's own entry AND every block entry
    // whose container is this page -- addressed by container_id, so it does
    // not depend on the `block` rows that were just deleted above, and does
    // not degrade into 50 000 individual deletes on a large page.
    if (const Status s = search_index::erase_container(conn_.get(),
                                                       search::SourceKind::Page,
                                                       id.bytes());
        !is_ok(s)) {
        return s;
    }

    return txn.commit();
}

Status SqliteBlockStore::page_count(std::size_t& out) const noexcept {
    Statement stmt;
    if (const Status s = stmt.prepare(conn_.get(), "SELECT COUNT(*) FROM page;");
        !is_ok(s)) {
        return s;
    }
    return read_count(stmt, out);
}

// --- blocks ----------------------------------------------------------------

Status SqliteBlockStore::insert_block(const PageId& page_id,
                                      const BlockId& parent_id,
                                      std::size_t position,
                                      const BlockId& new_id,
                                      const BlockContent& content,
                                      std::int64_t now_ms) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    if (page_id.is_nil() || new_id.is_nil()) { return Status::IdMalformed; }
    if (new_id == parent_id) { return Status::BlockCycleDetected; }
    if (now_ms < 0) { return Status::InvalidArgument; }
    if (const Status s = blocks::validate_block_content(content); !is_ok(s)) { return s; }

    ColumnPayload payload;
    if (const Status s = encode_content(content, payload); !is_ok(s)) { return s; }

    Transaction txn;
    if (const Status s = txn.begin(conn_.get()); !is_ok(s)) { return s; }

    // The page must exist. This is the malformed-input path a caller hits by
    // holding on to the id of a page someone else deleted.
    if (const Status s = require_page(page_id); !is_ok(s)) { return s; }

    if (!parent_id.is_nil()) {
        BlockRow parent_row;
        if (const Status s = load_row(parent_id, parent_row); !is_ok(s)) { return s; }
        if (parent_row.page_id != page_id) {
            // Nesting under a block on another page would silently move the
            // new block off the page the caller named.
            return Status::InvalidArgument;
        }

        std::size_t parent_depth = 0;
        if (const Status s = depth_of(parent_id, parent_depth); !is_ok(s)) { return s; }
        if (parent_depth + 1 > blocks::kMaxBlockDepth) {
            return Status::BlockDepthExceeded;
        }
    }

    std::size_t total = 0;
    if (const Status s = block_count(page_id, total); !is_ok(s)) { return s; }
    if (total >= blocks::kMaxBlocksPerPage) { return Status::PageBlockLimitExceeded; }

    const BlockId no_exclusion;
    std::size_t siblings = 0;
    if (const Status s = sibling_count(page_id, parent_id, no_exclusion, siblings);
        !is_ok(s)) {
        return s;
    }

    std::size_t index = position;
    if (position == blocks::kAppendPosition) {
        index = siblings;
    } else if (position > siblings) {
        // Not clamped: a caller working from a stale tree should be told, not
        // quietly given a different position than it asked for.
        return Status::BlockPositionInvalid;
    }

    if (const Status s = shift_siblings(page_id, parent_id, index, +1, no_exclusion);
        !is_ok(s)) {
        return s;
    }

    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "INSERT INTO block(id, page_id, parent_id, type, sort_index, schema_ver, "
                "                  text, checked, heading_level, language, "
                "                  created_at_ms, modified_at_ms) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?11);");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, new_id.bytes());   !is_ok(s)) { return s; }
        if (const Status s = bind_id(stmt, 2, page_id.bytes());  !is_ok(s)) { return s; }
        if (const Status s = bind_parent(stmt, 3, parent_id);    !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(4, payload.type);   !is_ok(s)) { return s; }
        if (const Status s = bind_index(stmt, 5, index);         !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(6, kBlockSchemaVersion);      !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_text(7, payload.text);              !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(8, payload.checked);          !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(9, payload.heading_level);    !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_text(10, payload.language);         !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(11, now_ms);                  !is_ok(s)) { return s; }

        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) { return write_status_for(rc); }
    }

    if (const Status s = reindex_block(new_id, page_id, content); !is_ok(s)) { return s; }
    if (const Status s = touch_page(page_id, now_ms);             !is_ok(s)) { return s; }
    return txn.commit();
}

Status SqliteBlockStore::get_block(const BlockId& id, Block& out) const noexcept {
    if (id.is_nil()) { return Status::IdMalformed; }

    Statement stmt;
    if (const Status s = stmt.prepare(
            conn_.get(),
            "SELECT page_id, parent_id, type, text, checked, heading_level, language, "
            "       created_at_ms, modified_at_ms "
            "FROM block WHERE id = ?1;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, id.bytes()); !is_ok(s)) { return s; }

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) { return Status::NotFound; }
    if (rc != SQLITE_ROW)  { return Status::StoreReadFailed; }

    Block block;
    block.id = id;

    ObjectIdBytes raw{};
    if (const Status s = stmt.column_fixed_blob(0, raw.data(), raw.size()); !is_ok(s)) {
        return s;
    }
    block.page_id = PageId(raw);

    if (!stmt.column_is_null(1)) {
        ObjectIdBytes parent_raw{};
        if (const Status s = stmt.column_fixed_blob(1, parent_raw.data(), parent_raw.size());
            !is_ok(s)) {
            return s;
        }
        block.parent_id = BlockId(parent_raw);
    }

    if (const Status s = decode_content(stmt.column_int64(2), stmt.column_text(3),
                                        stmt.column_int64(4), stmt.column_int64(5),
                                        stmt.column_text(6), block.content);
        !is_ok(s)) {
        return s;
    }
    block.created_at_ms  = stmt.column_int64(7);
    block.modified_at_ms = stmt.column_int64(8);
    if (block.created_at_ms < 0 || block.modified_at_ms < 0) {
        return Status::StoreReadFailed;
    }

    out = std::move(block);
    return Status::Ok;
}

Status SqliteBlockStore::update_block(const BlockId& id,
                                      const BlockContent& content,
                                      std::int64_t now_ms) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    if (id.is_nil()) { return Status::IdMalformed; }
    if (now_ms < 0)  { return Status::InvalidArgument; }
    if (const Status s = blocks::validate_block_content(content); !is_ok(s)) { return s; }

    ColumnPayload payload;
    if (const Status s = encode_content(content, payload); !is_ok(s)) { return s; }

    Transaction txn;
    if (const Status s = txn.begin(conn_.get()); !is_ok(s)) { return s; }

    BlockRow row;
    if (const Status s = load_row(id, row); !is_ok(s)) { return s; }

    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "UPDATE block SET type = ?2, text = ?3, checked = ?4, heading_level = ?5, "
                "                 language = ?6, schema_ver = ?7, modified_at_ms = ?8 "
                "WHERE id = ?1;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, id.bytes());               !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(2, payload.type);           !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_text(3, payload.text);            !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(4, payload.checked);        !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(5, payload.heading_level);  !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_text(6, payload.language);        !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(7, kBlockSchemaVersion);    !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(8, now_ms);                 !is_ok(s)) { return s; }

        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) { return write_status_for(rc); }
    }

    // Replaces the entry, so the PREVIOUS text stops matching here rather
    // than lingering as a stale hit. Also handles a type change that removes
    // the text entirely (paragraph -> divider): reindex_block erases.
    if (const Status s = reindex_block(id, row.page_id, content); !is_ok(s)) { return s; }
    if (const Status s = touch_page(row.page_id, now_ms);         !is_ok(s)) { return s; }
    return txn.commit();
}

Status SqliteBlockStore::delete_block(const BlockId& id, std::int64_t now_ms) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    if (id.is_nil()) { return Status::IdMalformed; }
    if (now_ms < 0)  { return Status::InvalidArgument; }

    Transaction txn;
    if (const Status s = txn.begin(conn_.get()); !is_ok(s)) { return s; }

    BlockRow row;
    if (const Status s = load_row(id, row); !is_ok(s)) { return s; }

    // Index entries for the whole subtree go FIRST, because finding them
    // means walking `block.parent_id` -- which the next statement is about to
    // delete. Doing it the other way round would silently leave every
    // descendant's text matchable forever, and nothing would ever fail.
    if (const Status s = search_index::erase_block_subtree(conn_.get(), id.bytes(),
                                                           kDepthProbeCap);
        !is_ok(s)) {
        return s;
    }

    // Delete the block and its whole subtree in one statement. `UNION` (not
    // UNION ALL) plus the depth bound is what keeps this terminating on a
    // cyclic file; without it a corrupt parent chain is an infinite loop
    // inside SQLite, which is the DoS this guard exists to prevent.
    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "WITH RECURSIVE sub(id, depth) AS ("
                "  SELECT id, 1 FROM block WHERE id = ?1"
                "  UNION"
                "  SELECT b.id, sub.depth + 1"
                "    FROM block b JOIN sub ON b.parent_id = sub.id"
                "   WHERE sub.depth < ?2"
                ") DELETE FROM block WHERE id IN (SELECT id FROM sub);");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, id.bytes());       !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(2, kDepthProbeCap); !is_ok(s)) { return s; }

        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) { return write_status_for(rc); }
    }

    // Close the hole in the sibling list so positions stay dense 0..n-1.
    const BlockId no_exclusion;
    if (const Status s =
            shift_siblings(row.page_id, row.parent_id, row.sort_index + 1, -1, no_exclusion);
        !is_ok(s)) {
        return s;
    }

    if (const Status s = touch_page(row.page_id, now_ms); !is_ok(s)) { return s; }
    return txn.commit();
}

Status SqliteBlockStore::move_block(const BlockId& id,
                                    const BlockId& new_parent_id,
                                    std::size_t new_position,
                                    std::int64_t now_ms) noexcept {
    if (read_only_) { return Status::StoreReadOnly; }
    if (id.is_nil()) { return Status::IdMalformed; }
    if (id == new_parent_id) { return Status::BlockCycleDetected; }
    if (now_ms < 0) { return Status::InvalidArgument; }

    Transaction txn;
    if (const Status s = txn.begin(conn_.get()); !is_ok(s)) { return s; }

    BlockRow row;
    if (const Status s = load_row(id, row); !is_ok(s)) { return s; }

    if (!new_parent_id.is_nil()) {
        BlockRow parent_row;
        if (const Status s = load_row(new_parent_id, parent_row); !is_ok(s)) { return s; }

        // Cross-page moves are NOT IMPLEMENTED: the whole subtree's page_id
        // would have to be rewritten, and once backlinks and per-page FTS
        // exist those indexes move with it. Rejecting is honest; a half-moved
        // subtree is a corrupt document.
        if (parent_row.page_id != row.page_id) { return Status::InvalidArgument; }

        // A block cannot become its own descendant's child.
        bool cycle = false;
        if (const Status s = has_ancestor(new_parent_id, id, cycle); !is_ok(s)) { return s; }
        if (cycle) { return Status::BlockCycleDetected; }

        std::size_t parent_depth = 0;
        if (const Status s = depth_of(new_parent_id, parent_depth); !is_ok(s)) { return s; }
        std::size_t height = 0;
        if (const Status s = subtree_height(id, height); !is_ok(s)) { return s; }
        // The whole subtree travels with the block, so the destination has to
        // have room for all of it, not just for the block itself.
        if (parent_depth + height > blocks::kMaxBlockDepth) {
            return Status::BlockDepthExceeded;
        }
    }

    // 1. Close the gap the block leaves behind. It cannot match its own
    //    predicate (its index is not > its index), so no exclusion needed.
    const BlockId no_exclusion;
    if (const Status s =
            shift_siblings(row.page_id, row.parent_id, row.sort_index + 1, -1, no_exclusion);
        !is_ok(s)) {
        return s;
    }

    // 2. Count the destination siblings, excluding the block itself so that a
    //    same-parent reorder is indexed against the list the caller sees.
    std::size_t destination = 0;
    if (const Status s = sibling_count(row.page_id, new_parent_id, id, destination);
        !is_ok(s)) {
        return s;
    }

    std::size_t index = new_position;
    if (new_position == blocks::kAppendPosition) {
        index = destination;
    } else if (new_position > destination) {
        return Status::BlockPositionInvalid;
    }

    // 3. Open the gap, again excluding the block itself: in the same-parent
    //    case its row is still sitting at its old index and must not be
    //    shifted along with the others.
    if (const Status s = shift_siblings(row.page_id, new_parent_id, index, +1, id);
        !is_ok(s)) {
        return s;
    }

    {
        Statement stmt;
        if (const Status s = stmt.prepare(
                conn_.get(),
                "UPDATE block SET parent_id = ?2, sort_index = ?3, modified_at_ms = ?4 "
                "WHERE id = ?1;");
            !is_ok(s)) {
            return s;
        }
        if (const Status s = bind_id(stmt, 1, id.bytes());        !is_ok(s)) { return s; }
        if (const Status s = bind_parent(stmt, 2, new_parent_id); !is_ok(s)) { return s; }
        if (const Status s = bind_index(stmt, 3, index);          !is_ok(s)) { return s; }
        if (const Status s = stmt.bind_int64(4, now_ms);          !is_ok(s)) { return s; }

        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) { return write_status_for(rc); }
    }

    // NO index write here, and that is correct rather than an omission: a move
    // changes `parent_id` and `sort_index`, and neither is in the index. The
    // index addresses a block by (kind, block id) and stores its page as the
    // container; a cross-page move WOULD change the container, which is
    // exactly why it is rejected above as NOT IMPLEMENTED. When it lands, the
    // subtree's container entries have to be rewritten with it -- see
    // search_index::put, which already rewrites container_id for that reason.

    if (const Status s = touch_page(row.page_id, now_ms); !is_ok(s)) { return s; }
    return txn.commit();
}

Status SqliteBlockStore::block_count(const PageId& page_id, std::size_t& out) const noexcept {
    if (page_id.is_nil()) { return Status::IdMalformed; }
    Statement stmt;
    if (const Status s = stmt.prepare(conn_.get(),
                                      "SELECT COUNT(*) FROM block WHERE page_id = ?1;");
        !is_ok(s)) {
        return s;
    }
    if (const Status s = bind_id(stmt, 1, page_id.bytes()); !is_ok(s)) { return s; }
    return read_count(stmt, out);
}

// --- open ------------------------------------------------------------------

/// Run a semicolon-separated batch of DDL/PRAGMA statements.
Status exec_batch(sqlite3* db, const char* sql, Status failure) noexcept {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err != nullptr) {
        // Allocated by sqlite3_malloc; ours to free. The text is deliberately
        // not propagated: it can contain statement fragments, and spec
        // section 34 forbids user content in logs.
        sqlite3_free(err);
    }
    return rc == SQLITE_OK ? Status::Ok : failure;
}

Status apply_schema(sqlite3* db) noexcept {
    if (const Status s = exec_batch(db, document_sql::kSchemaSql, Status::StoreSchemaFailed); !is_ok(s)) {
        return s;
    }

    Statement stmt;
    if (const Status s = stmt.prepare(
            db,
            "INSERT INTO schema_meta(key, value) VALUES('block_schema_version', ?1) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
        !is_ok(s)) {
        return Status::StoreSchemaFailed;
    }
    if (const Status s = stmt.bind_int64(1, kBlockSchemaVersion); !is_ok(s)) {
        return Status::StoreSchemaFailed;
    }
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return Status::StoreSchemaFailed;
    }

    // The unified index lives in the same file and is created by whichever
    // adapter opens it first; `IF NOT EXISTS` makes the second a no-op. The
    // DDL is one shared constant (search_index_sql.cpp) so the note store and
    // this one cannot disagree about the shape of the table they share.
    return search_index::apply_schema(db);
}

/// Refuse a database written by a newer build: opening it read-write would
/// silently drop whatever columns we do not know about.
Status check_schema_version(sqlite3* db) noexcept {
    Statement stmt;
    if (const Status s = stmt.prepare(
            db, "SELECT value FROM schema_meta WHERE key = 'block_schema_version';");
        !is_ok(s)) {
        return Status::Ok;  // table absent => fresh database, not an error
    }
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) { return search_index::check_schema_version(db); }
    if (rc != SQLITE_ROW)  { return Status::StoreSchemaFailed; }

    if (stmt.column_int64(0) > kBlockSchemaVersion) { return Status::StoreSchemaFailed; }
    return search_index::check_schema_version(db);
}

}  // namespace

Status open_sqlite_block_store(const SqliteBlockStoreOptions& options,
                               std::unique_ptr<blocks::BlockStore>& out) {
    if (options.path.empty()) {
        return Status::InvalidArgument;
    }
    if (options.path.find('\0') != std::string::npos) {
        // sqlite3_open_v2 takes a NUL-terminated string; an embedded NUL
        // would silently open a different, shorter path than requested.
        return Status::InvalidArgument;
    }
    if (options.busy_timeout_ms < 0) {
        return Status::InvalidArgument;
    }

    const int flags = options.read_only
                          ? SQLITE_OPEN_READONLY
                          : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);

    sqlite3* raw_db = nullptr;
    const int rc = sqlite3_open_v2(options.path.c_str(), &raw_db, flags, nullptr);
    // open_v2 can hand back a handle even on failure; wrapping it
    // unconditionally is what guarantees it gets closed.
    Connection conn(raw_db);
    if (rc != SQLITE_OK || !conn) {
        return Status::StoreOpenFailed;
    }

    if (sqlite3_busy_timeout(conn.get(), options.busy_timeout_ms) != SQLITE_OK) {
        return Status::StoreOpenFailed;
    }

    if (options.read_only) {
        // foreign_keys is a no-op for reads but recursive_triggers/WAL are
        // writer settings, so a read-only connection only checks the version.
        if (const Status s = check_schema_version(conn.get()); !is_ok(s)) { return s; }
    } else {
        if (const Status s = exec_batch(conn.get(),
                                        "PRAGMA journal_mode = WAL;"
                                        "PRAGMA foreign_keys = ON;"
                                        "PRAGMA synchronous  = NORMAL;",
                                        Status::StoreOpenFailed);
            !is_ok(s)) {
            return s;
        }
        if (const Status s = check_schema_version(conn.get()); !is_ok(s)) { return s; }
        if (const Status s = apply_schema(conn.get());         !is_ok(s)) { return s; }
    }

    auto* store = new (std::nothrow) SqliteBlockStore(std::move(conn), options.read_only);
    if (store == nullptr) {
        return Status::StoreOpenFailed;  // allocation failure is checked, not assumed away
    }
    out.reset(store);
    return Status::Ok;
}

}  // namespace uapp::storage::sqlite
