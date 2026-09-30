// SPDX-License-Identifier: Apache-2.0
//
// Integration tests for the block document model against the real SQLite
// adapter: ordering, nesting, persistence across a close/reopen, the
// mutation operations, and the guard rails.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common_fixtures.h"
#include "test_framework.h"
#include "uapp/blocks/block.h"
#include "uapp/blocks/block_store.h"
#include "uapp/blocks/document_service.h"
#include "uapp/blocks/page.h"
#include "uapp/status.h"
#include "uapp/storage/sqlite/sqlite_block_store.h"

namespace {

using uapp::Status;
using uapp::blocks::Block;
using uapp::blocks::BlockId;
using uapp::blocks::BlockStore;
using uapp::blocks::DocumentService;
using uapp::blocks::Page;
using uapp::blocks::PageId;
using uapp::storage::sqlite::SqliteBlockStoreOptions;

/// Everything a test needs, constructed in one place. Non-throwing: `store`
/// is left null when the open fails and each test REQUIREs it.
struct Harness final {
    uapp::test::FakeSystemClock  clock{1'700'000'000'000};
    uapp::test::CountingIdSource ids;
    std::unique_ptr<BlockStore>  store;

    [[nodiscard]] Status open(const std::string& path) {
        SqliteBlockStoreOptions options;
        options.path = path;
        return uapp::storage::sqlite::open_sqlite_block_store(options, store);
    }
};

uapp::blocks::BlockContent para(const char* text) {
    return uapp::blocks::ParagraphBlock{std::string(text)};
}

/// The plain text of every top-level block, in order. Makes an ordering
/// assertion one comparison instead of a loop of index arithmetic.
std::vector<std::string> top_level_text(const Page& page) {
    std::vector<std::string> out;
    out.reserve(page.blocks.size());
    for (const Block& b : page.blocks) {
        out.emplace_back(uapp::blocks::block_plain_text(b.content));
    }
    return out;
}

std::vector<std::string> child_text(const Block& block) {
    std::vector<std::string> out;
    out.reserve(block.children.size());
    for (const Block& b : block.children) {
        out.emplace_back(uapp::blocks::block_plain_text(b.content));
    }
    return out;
}

}  // namespace

// --- create ----------------------------------------------------------------

UAPP_TEST("blocks.store: create a page and fill it with typed blocks") {
    Harness h;
    UAPP_REQUIRE_OK(h.open(":memory:"));
    UAPP_REQUIRE(h.store != nullptr);
    DocumentService svc(*h.store, h.clock, h.ids);

    Page page;
    UAPP_REQUIRE_OK(svc.create_page("Design notes", page));
    UAPP_CHECK(!page.id.is_nil());
    UAPP_CHECK(page.title == "Design notes");
    UAPP_CHECK(page.created_at_ms == 1'700'000'000'000);
    UAPP_CHECK(page.blocks.empty());

    const BlockId top;  // nil parent == top level
    Block created;
    UAPP_REQUIRE_OK(svc.append_block(page.id, top,
                                     uapp::blocks::HeadingBlock{"Goals", 2}, created));
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("Ship the block model."), created));
    UAPP_REQUIRE_OK(
        svc.append_block(page.id, top,
                         uapp::blocks::ChecklistItemBlock{"Write the tests", true}, created));
    UAPP_REQUIRE_OK(svc.append_block(page.id, top,
                                     uapp::blocks::CodeBlock{"int main() {}", "cpp"}, created));
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, uapp::blocks::DividerBlock{}, created));
    UAPP_REQUIRE_OK(svc.append_block(page.id, top,
                                     uapp::blocks::QuoteBlock{"Simple is better."}, created));

    std::size_t count = 0;
    UAPP_REQUIRE_OK(svc.block_count(page.id, count));
    UAPP_CHECK(count == 6);

    Page loaded;
    UAPP_REQUIRE_OK(svc.open_page(page.id, loaded));
    UAPP_REQUIRE(loaded.blocks.size() == 6);

    // Every payload survives with its real fields, not as a flattened string.
    const auto* heading = std::get_if<uapp::blocks::HeadingBlock>(&loaded.blocks[0].content);
    UAPP_REQUIRE(heading != nullptr);
    UAPP_CHECK(heading->level == 2);
    UAPP_CHECK(heading->text == "Goals");

    const auto* check = std::get_if<uapp::blocks::ChecklistItemBlock>(&loaded.blocks[2].content);
    UAPP_REQUIRE(check != nullptr);
    UAPP_CHECK(check->checked);

    const auto* code = std::get_if<uapp::blocks::CodeBlock>(&loaded.blocks[3].content);
    UAPP_REQUIRE(code != nullptr);
    UAPP_CHECK(code->language == "cpp");
    UAPP_CHECK(code->code == "int main() {}");

    UAPP_CHECK(std::get_if<uapp::blocks::DividerBlock>(&loaded.blocks[4].content) != nullptr);
}

UAPP_TEST("blocks.store: inserting at a position shifts the siblings after it") {
    Harness h;
    UAPP_REQUIRE_OK(h.open(":memory:"));
    DocumentService svc(*h.store, h.clock, h.ids);

    Page page;
    UAPP_REQUIRE_OK(svc.create_page("ordered", page));
    const BlockId top;
    Block b;
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("a"), b));
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("c"), b));
    UAPP_REQUIRE_OK(svc.add_block(page.id, top, 1, para("b"), b));

    Page loaded;
    UAPP_REQUIRE_OK(svc.open_page(page.id, loaded));
    const std::vector<std::string> want{"a", "b", "c"};
    UAPP_CHECK(top_level_text(loaded) == want);
}

// --- persistence -----------------------------------------------------------

UAPP_TEST("blocks.store: a close and reopen preserves order and nesting exactly") {
    uapp::test::TempDir dir;
    UAPP_REQUIRE(dir.valid());
    const std::string path = dir.file("blocks.db");

    PageId page_id;
    {
        Harness h;
        UAPP_REQUIRE_OK(h.open(path));
        DocumentService svc(*h.store, h.clock, h.ids);

        Page page;
        UAPP_REQUIRE_OK(svc.create_page("Outline", page));
        page_id = page.id;

        const BlockId top;
        Block one;
        Block two;
        Block three;
        UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("one"), one));
        UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("two"), two));
        UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("three"), three));

        // Nest under "two": two children, and a grandchild under the first.
        Block child_a;
        Block child_b;
        Block grandchild;
        UAPP_REQUIRE_OK(svc.append_block(page.id, two.id, para("two.a"), child_a));
        UAPP_REQUIRE_OK(svc.append_block(page.id, two.id, para("two.b"), child_b));
        UAPP_REQUIRE_OK(
            svc.append_block(page.id, child_a.id, para("two.a.i"), grandchild));

        // And one child under "one", to prove sibling groups do not bleed.
        Block one_child;
        UAPP_REQUIRE_OK(svc.append_block(page.id, one.id, para("one.a"), one_child));
    }  // store destroyed: the connection is closed here

    Harness reopened;
    UAPP_REQUIRE_OK(reopened.open(path));
    DocumentService svc(*reopened.store, reopened.clock, reopened.ids);

    Page loaded;
    UAPP_REQUIRE_OK(svc.open_page(page_id, loaded));
    UAPP_CHECK(loaded.title == "Outline");

    const std::vector<std::string> want_top{"one", "two", "three"};
    UAPP_CHECK(top_level_text(loaded) == want_top);
    UAPP_REQUIRE(loaded.blocks.size() == 3);

    const std::vector<std::string> want_one{"one.a"};
    UAPP_CHECK(child_text(loaded.blocks[0]) == want_one);

    const std::vector<std::string> want_two{"two.a", "two.b"};
    UAPP_CHECK(child_text(loaded.blocks[1]) == want_two);
    UAPP_REQUIRE(loaded.blocks[1].children.size() == 2);

    const std::vector<std::string> want_grandchild{"two.a.i"};
    UAPP_CHECK(child_text(loaded.blocks[1].children[0]) == want_grandchild);

    UAPP_CHECK(loaded.blocks[2].children.empty());
    // one, two, three, one.a, two.a, two.b, two.a.i
    UAPP_CHECK(uapp::blocks::block_tree_size(loaded.blocks) == 7);
    UAPP_CHECK(uapp::blocks::block_tree_depth(loaded.blocks) == 3);

    // Parent links round-trip too, not just the shape.
    UAPP_CHECK(loaded.blocks[1].children[0].parent_id == loaded.blocks[1].id);
    UAPP_CHECK(loaded.blocks[0].parent_id.is_nil());
}

// --- update ----------------------------------------------------------------

UAPP_TEST("blocks.store: updating a block replaces its content and can change its type") {
    Harness h;
    UAPP_REQUIRE_OK(h.open(":memory:"));
    DocumentService svc(*h.store, h.clock, h.ids);

    Page page;
    UAPP_REQUIRE_OK(svc.create_page("edit me", page));
    const BlockId top;
    Block block;
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("draft"), block));

    // A child, to prove an update leaves the subtree alone.
    Block child;
    UAPP_REQUIRE_OK(svc.append_block(page.id, block.id, para("kept"), child));

    h.clock.advance(5000);
    UAPP_REQUIRE_OK(svc.update_block(block.id, uapp::blocks::HeadingBlock{"final", 1}));

    Block reread;
    UAPP_REQUIRE_OK(svc.get_block(block.id, reread));
    const auto* heading = std::get_if<uapp::blocks::HeadingBlock>(&reread.content);
    UAPP_REQUIRE(heading != nullptr);
    UAPP_CHECK(heading->text == "final");
    UAPP_CHECK(heading->level == 1);
    UAPP_CHECK(reread.modified_at_ms == 1'700'000'005'000);
    UAPP_CHECK(reread.created_at_ms == 1'700'000'000'000);

    Page loaded;
    UAPP_REQUIRE_OK(svc.open_page(page.id, loaded));
    UAPP_REQUIRE(loaded.blocks.size() == 1);
    const std::vector<std::string> want{"kept"};
    UAPP_CHECK(child_text(loaded.blocks[0]) == want);
    // The page's own modified stamp moved with the edit.
    UAPP_CHECK(loaded.modified_at_ms == 1'700'000'005'000);
}

UAPP_TEST("blocks.store: an update with an invalid payload is rejected and changes nothing") {
    Harness h;
    UAPP_REQUIRE_OK(h.open(":memory:"));
    DocumentService svc(*h.store, h.clock, h.ids);

    Page page;
    UAPP_REQUIRE_OK(svc.create_page("p", page));
    const BlockId top;
    Block block;
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("original"), block));

    UAPP_REQUIRE_STATUS(svc.update_block(block.id, uapp::blocks::HeadingBlock{"bad", 9}),
                        Status::BlockMalformed);

    Block reread;
    UAPP_REQUIRE_OK(svc.get_block(block.id, reread));
    UAPP_CHECK(uapp::blocks::block_plain_text(reread.content) == "original");
}

// --- delete ----------------------------------------------------------------

UAPP_TEST("blocks.store: deleting a block cascades to its whole subtree") {
    Harness h;
    UAPP_REQUIRE_OK(h.open(":memory:"));
    DocumentService svc(*h.store, h.clock, h.ids);

    Page page;
    UAPP_REQUIRE_OK(svc.create_page("cascade", page));
    const BlockId top;

    Block keep_before;
    Block doomed;
    Block keep_after;
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("before"), keep_before));
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("doomed"), doomed));
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("after"), keep_after));

    Block child;
    Block grandchild;
    UAPP_REQUIRE_OK(svc.append_block(page.id, doomed.id, para("child"), child));
    UAPP_REQUIRE_OK(svc.append_block(page.id, child.id, para("grandchild"), grandchild));

    std::size_t count = 0;
    UAPP_REQUIRE_OK(svc.block_count(page.id, count));
    UAPP_CHECK(count == 5);

    UAPP_REQUIRE_OK(svc.delete_block(doomed.id));

    UAPP_REQUIRE_OK(svc.block_count(page.id, count));
    UAPP_CHECK_MSG(count == 2, "children must be deleted, not reparented or orphaned");

    // Both descendants are gone, not silently promoted to the top level.
    Block gone;
    UAPP_REQUIRE_STATUS(svc.get_block(child.id, gone), Status::NotFound);
    UAPP_REQUIRE_STATUS(svc.get_block(grandchild.id, gone), Status::NotFound);

    // And the hole in the sibling list was closed.
    Page loaded;
    UAPP_REQUIRE_OK(svc.open_page(page.id, loaded));
    const std::vector<std::string> want{"before", "after"};
    UAPP_CHECK(top_level_text(loaded) == want);
}

UAPP_TEST("blocks.store: deleting a page removes every block on it") {
    Harness h;
    UAPP_REQUIRE_OK(h.open(":memory:"));
    DocumentService svc(*h.store, h.clock, h.ids);

    Page page;
    UAPP_REQUIRE_OK(svc.create_page("doomed page", page));
    const BlockId top;
    Block parent;
    Block child;
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("p"), parent));
    UAPP_REQUIRE_OK(svc.append_block(page.id, parent.id, para("c"), child));

    UAPP_REQUIRE_OK(svc.delete_page(page.id));

    Page gone;
    UAPP_REQUIRE_STATUS(svc.open_page(page.id, gone), Status::NotFound);
    Block orphan;
    UAPP_REQUIRE_STATUS(svc.get_block(child.id, orphan), Status::NotFound);

    std::size_t pages = 0;
    UAPP_REQUIRE_OK(svc.page_count(pages));
    UAPP_CHECK(pages == 0);
}

// --- reorder and move ------------------------------------------------------

UAPP_TEST("blocks.store: reordering keeps the parent and renumbers densely") {
    Harness h;
    UAPP_REQUIRE_OK(h.open(":memory:"));
    DocumentService svc(*h.store, h.clock, h.ids);

    Page page;
    UAPP_REQUIRE_OK(svc.create_page("reorder", page));
    const BlockId top;
    Block a;
    Block b;
    Block c;
    Block d;
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("a"), a));
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("b"), b));
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("c"), c));
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("d"), d));

    // Drag "d" to the front.
    UAPP_REQUIRE_OK(svc.reorder_block(d.id, 0));
    Page loaded;
    UAPP_REQUIRE_OK(svc.open_page(page.id, loaded));
    const std::vector<std::string> want_first{"d", "a", "b", "c"};
    UAPP_CHECK(top_level_text(loaded) == want_first);

    // Drag it back to the end.
    UAPP_REQUIRE_OK(svc.reorder_block(d.id, 3));
    UAPP_REQUIRE_OK(svc.open_page(page.id, loaded));
    const std::vector<std::string> want_last{"a", "b", "c", "d"};
    UAPP_CHECK(top_level_text(loaded) == want_last);

    // Middle-to-middle.
    UAPP_REQUIRE_OK(svc.reorder_block(a.id, 2));
    UAPP_REQUIRE_OK(svc.open_page(page.id, loaded));
    const std::vector<std::string> want_mid{"b", "c", "a", "d"};
    UAPP_CHECK(top_level_text(loaded) == want_mid);

    // Past the end is an error, not a silent clamp.
    UAPP_REQUIRE_STATUS(svc.reorder_block(a.id, 9), Status::BlockPositionInvalid);
}

UAPP_TEST("blocks.store: moving a block to a new parent takes its subtree with it") {
    Harness h;
    UAPP_REQUIRE_OK(h.open(":memory:"));
    DocumentService svc(*h.store, h.clock, h.ids);

    Page page;
    UAPP_REQUIRE_OK(svc.create_page("move", page));
    const BlockId top;
    Block host;
    Block mover;
    Block tail;
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("host"), host));
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("mover"), mover));
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("tail"), tail));

    Block existing_child;
    UAPP_REQUIRE_OK(svc.append_block(page.id, host.id, para("host.a"), existing_child));

    Block carried;
    UAPP_REQUIRE_OK(svc.append_block(page.id, mover.id, para("mover.a"), carried));

    UAPP_REQUIRE_OK(svc.move_block(mover.id, host.id, 0));

    Page loaded;
    UAPP_REQUIRE_OK(svc.open_page(page.id, loaded));

    const std::vector<std::string> want_top{"host", "tail"};
    UAPP_CHECK(top_level_text(loaded) == want_top);
    UAPP_REQUIRE(loaded.blocks.size() == 2);

    const std::vector<std::string> want_host{"mover", "host.a"};
    UAPP_CHECK(child_text(loaded.blocks[0]) == want_host);
    UAPP_REQUIRE(loaded.blocks[0].children.size() == 2);

    const std::vector<std::string> want_carried{"mover.a"};
    UAPP_CHECK_MSG(child_text(loaded.blocks[0].children[0]) == want_carried,
                   "the moved block's own subtree must travel with it");

    // And back out to the top level, at the end.
    UAPP_REQUIRE_OK(svc.move_block(mover.id, BlockId(), 2));
    UAPP_REQUIRE_OK(svc.open_page(page.id, loaded));
    const std::vector<std::string> want_back{"host", "tail", "mover"};
    UAPP_CHECK(top_level_text(loaded) == want_back);
    UAPP_REQUIRE(loaded.blocks.size() == 3);
    const std::vector<std::string> want_host_after{"host.a"};
    UAPP_CHECK(child_text(loaded.blocks[0]) == want_host_after);
}

UAPP_TEST("blocks.store: a block cannot be moved under its own descendant") {
    Harness h;
    UAPP_REQUIRE_OK(h.open(":memory:"));
    DocumentService svc(*h.store, h.clock, h.ids);

    Page page;
    UAPP_REQUIRE_OK(svc.create_page("cycle", page));
    const BlockId top;
    Block root;
    Block child;
    Block grandchild;
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("root"), root));
    UAPP_REQUIRE_OK(svc.append_block(page.id, root.id, para("child"), child));
    UAPP_REQUIRE_OK(svc.append_block(page.id, child.id, para("grandchild"), grandchild));

    UAPP_REQUIRE_STATUS(svc.move_block(root.id, grandchild.id, 0),
                        Status::BlockCycleDetected);
    UAPP_REQUIRE_STATUS(svc.move_block(root.id, root.id, 0), Status::BlockCycleDetected);

    // The tree is untouched.
    Page loaded;
    UAPP_REQUIRE_OK(svc.open_page(page.id, loaded));
    UAPP_CHECK(uapp::blocks::block_tree_depth(loaded.blocks) == 3);
}

// --- guard rails -----------------------------------------------------------

UAPP_TEST("blocks.store: the max-nesting-depth guard rejects one level too many") {
    Harness h;
    UAPP_REQUIRE_OK(h.open(":memory:"));
    DocumentService svc(*h.store, h.clock, h.ids);

    Page page;
    UAPP_REQUIRE_OK(svc.create_page("deep", page));

    // Build a chain exactly kMaxBlockDepth deep. Every one of these must be
    // accepted -- an off-by-one in the guard would show up here first.
    BlockId parent;  // nil == top level
    for (std::size_t depth = 1; depth <= uapp::blocks::kMaxBlockDepth; ++depth) {
        Block created;
        UAPP_REQUIRE_STATUS(svc.append_block(page.id, parent, para("x"), created),
                            Status::Ok);
        parent = created.id;
    }

    std::size_t count = 0;
    UAPP_REQUIRE_OK(svc.block_count(page.id, count));
    UAPP_CHECK(count == uapp::blocks::kMaxBlockDepth);

    // One more is refused.
    Block rejected;
    UAPP_REQUIRE_STATUS(svc.append_block(page.id, parent, para("too deep"), rejected),
                        Status::BlockDepthExceeded);

    // Nothing was written by the failed attempt.
    UAPP_REQUIRE_OK(svc.block_count(page.id, count));
    UAPP_CHECK(count == uapp::blocks::kMaxBlockDepth);

    // And the tree still reads back, at exactly the limit.
    Page loaded;
    UAPP_REQUIRE_OK(svc.open_page(page.id, loaded));
    UAPP_CHECK(uapp::blocks::block_tree_depth(loaded.blocks) == uapp::blocks::kMaxBlockDepth);
}

UAPP_TEST("blocks.store: a move that would push a subtree past the depth limit is refused") {
    Harness h;
    UAPP_REQUIRE_OK(h.open(":memory:"));
    DocumentService svc(*h.store, h.clock, h.ids);

    Page page;
    UAPP_REQUIRE_OK(svc.create_page("deep move", page));

    // A chain of (limit - 1) blocks, plus a separate 2-block subtree at the
    // top level. Moving the 2-block subtree under the chain's tip would make
    // the deepest node sit at limit + 1.
    BlockId tip;
    for (std::size_t depth = 1; depth + 1 <= uapp::blocks::kMaxBlockDepth; ++depth) {
        Block created;
        UAPP_REQUIRE_OK(svc.append_block(page.id, tip, para("chain"), created));
        tip = created.id;
    }

    const BlockId top;
    Block subtree_root;
    Block subtree_child;
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("sub"), subtree_root));
    UAPP_REQUIRE_OK(svc.append_block(page.id, subtree_root.id, para("sub.a"), subtree_child));

    UAPP_REQUIRE_STATUS(svc.move_block(subtree_root.id, tip, 0), Status::BlockDepthExceeded);

    // A single block with no children still fits in that last slot, which is
    // what proves the check measures the SUBTREE and not just the block.
    Block leaf;
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("leaf"), leaf));
    UAPP_REQUIRE_OK(svc.move_block(leaf.id, tip, 0));
}

// --- malformed input -------------------------------------------------------

UAPP_TEST("blocks.store: adding a block to a page that does not exist is NotFound") {
    Harness h;
    UAPP_REQUIRE_OK(h.open(":memory:"));
    DocumentService svc(*h.store, h.clock, h.ids);

    PageId ghost;
    UAPP_REQUIRE_OK(PageId::parse("deadbeefdeadbeefdeadbeefdeadbeef", ghost));

    const BlockId top;
    Block rejected;
    UAPP_REQUIRE_STATUS(svc.append_block(ghost, top, para("nowhere"), rejected),
                        Status::NotFound);

    // Nothing leaked into the database.
    std::size_t pages = 0;
    UAPP_REQUIRE_OK(svc.page_count(pages));
    UAPP_CHECK(pages == 0);
}

UAPP_TEST("blocks.store: nil ids are rejected before they reach SQL") {
    Harness h;
    UAPP_REQUIRE_OK(h.open(":memory:"));
    DocumentService svc(*h.store, h.clock, h.ids);

    const PageId nil_page;
    const BlockId nil_block;
    Block out;
    UAPP_REQUIRE_STATUS(svc.append_block(nil_page, nil_block, para("x"), out),
                        Status::IdMalformed);

    Page page_out;
    UAPP_REQUIRE_STATUS(svc.open_page(nil_page, page_out), Status::IdMalformed);
    UAPP_REQUIRE_STATUS(svc.delete_block(nil_block), Status::IdMalformed);
}

UAPP_TEST("blocks.store: a block cannot be nested under a parent on another page") {
    Harness h;
    UAPP_REQUIRE_OK(h.open(":memory:"));
    DocumentService svc(*h.store, h.clock, h.ids);

    Page first;
    Page second;
    UAPP_REQUIRE_OK(svc.create_page("first", first));
    UAPP_REQUIRE_OK(svc.create_page("second", second));

    const BlockId top;
    Block host;
    UAPP_REQUIRE_OK(svc.append_block(first.id, top, para("host"), host));

    Block rejected;
    UAPP_REQUIRE_STATUS(svc.append_block(second.id, host.id, para("x"), rejected),
                        Status::InvalidArgument);
}

UAPP_TEST("blocks.store: inserting past the end of a sibling list is rejected") {
    Harness h;
    UAPP_REQUIRE_OK(h.open(":memory:"));
    DocumentService svc(*h.store, h.clock, h.ids);

    Page page;
    UAPP_REQUIRE_OK(svc.create_page("bounds", page));
    const BlockId top;
    Block first;
    UAPP_REQUIRE_OK(svc.append_block(page.id, top, para("a"), first));

    Block rejected;
    UAPP_REQUIRE_STATUS(svc.add_block(page.id, top, 5, para("b"), rejected),
                        Status::BlockPositionInvalid);
    // Index == size is legal: it means "append".
    Block appended;
    UAPP_REQUIRE_OK(svc.add_block(page.id, top, 1, para("b"), appended));
}

UAPP_TEST("blocks.store: an unknown block is NotFound rather than an empty block") {
    Harness h;
    UAPP_REQUIRE_OK(h.open(":memory:"));
    DocumentService svc(*h.store, h.clock, h.ids);

    BlockId ghost;
    UAPP_REQUIRE_OK(BlockId::parse("0123456789abcdef0123456789abcdef", ghost));

    Block out;
    UAPP_REQUIRE_STATUS(svc.get_block(ghost, out), Status::NotFound);
    UAPP_REQUIRE_STATUS(svc.update_block(ghost, para("x")), Status::NotFound);
    UAPP_REQUIRE_STATUS(svc.delete_block(ghost), Status::NotFound);
    UAPP_REQUIRE_STATUS(svc.reorder_block(ghost, 0), Status::NotFound);
}

UAPP_TEST("blocks.store: a read-only database refuses every write") {
    uapp::test::TempDir dir;
    UAPP_REQUIRE(dir.valid());
    const std::string path = dir.file("ro.db");

    PageId page_id;
    {
        Harness writable;
        UAPP_REQUIRE_OK(writable.open(path));
        DocumentService svc(*writable.store, writable.clock, writable.ids);
        Page page;
        UAPP_REQUIRE_OK(svc.create_page("locked", page));
        page_id = page.id;
    }

    SqliteBlockStoreOptions options;
    options.path      = path;
    options.read_only = true;
    std::unique_ptr<BlockStore> store;
    UAPP_REQUIRE_OK(uapp::storage::sqlite::open_sqlite_block_store(options, store));

    uapp::test::FakeSystemClock clock(1);
    uapp::test::CountingIdSource ids;
    DocumentService svc(*store, clock, ids);

    const BlockId top;
    Block rejected;
    UAPP_REQUIRE_STATUS(svc.append_block(page_id, top, para("nope"), rejected),
                        Status::StoreReadOnly);
    UAPP_REQUIRE_STATUS(svc.delete_page(page_id), Status::StoreReadOnly);

    // Reads still work.
    Page loaded;
    UAPP_REQUIRE_OK(svc.open_page(page_id, loaded));
    UAPP_CHECK(loaded.title == "locked");
}

UAPP_TEST("blocks.store: an entropy-source failure is surfaced, not papered over") {
    Harness h;
    UAPP_REQUIRE_OK(h.open(":memory:"));
    DocumentService svc(*h.store, h.clock, h.ids);

    h.ids.fail_next();
    Page page;
    UAPP_REQUIRE_STATUS(svc.create_page("x", page), Status::Internal);
}
