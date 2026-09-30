// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the portable block domain: identifiers, the type table,
// payload validation and the tree helpers. No storage, no SQLite.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "test_framework.h"
#include "uapp/blocks/block.h"
#include "uapp/blocks/page.h"
#include "uapp/object_id.h"
#include "uapp/status.h"

namespace {

using uapp::Status;
using uapp::blocks::Block;
using uapp::blocks::BlockId;
using uapp::blocks::BlockType;
using uapp::blocks::PageId;

PageId make_page_id(std::uint8_t tag) {
    uapp::ObjectIdBytes raw{};
    raw[15] = tag;
    return PageId(raw);
}

BlockId make_block_id(std::uint8_t tag) {
    uapp::ObjectIdBytes raw{};
    raw[0]  = 0x0Bu;
    raw[15] = tag;
    return BlockId(raw);
}

}  // namespace

// --- identifiers -----------------------------------------------------------

UAPP_TEST("blocks.id: round-trips through hex") {
    const BlockId id = make_block_id(0x2Au);
    const std::string hex = id.to_hex();
    UAPP_REQUIRE(hex.size() == uapp::kObjectIdHexChars);
    const std::string expected = "0b" + std::string(28, '0') + "2a";
    UAPP_CHECK_MSG(hex == expected, hex);

    BlockId parsed;
    UAPP_REQUIRE_OK(BlockId::parse(hex, parsed));
    UAPP_CHECK(parsed == id);
}

UAPP_TEST("blocks.id: rejects malformed hex and never half-writes") {
    BlockId out = make_block_id(0x77u);
    const BlockId untouched = out;

    UAPP_REQUIRE_STATUS(BlockId::parse("", out), Status::IdMalformed);
    UAPP_REQUIRE_STATUS(BlockId::parse("abc", out), Status::IdMalformed);
    // Right length, one bad character in the last position.
    UAPP_REQUIRE_STATUS(BlockId::parse("0000000000000000000000000000000z", out),
                        Status::IdMalformed);
    UAPP_CHECK(out == untouched);
}

UAPP_TEST("blocks.id: a default id is nil") {
    const BlockId nil;
    UAPP_CHECK(nil.is_nil());
    UAPP_CHECK(!make_block_id(1).is_nil());
}

// --- the type table --------------------------------------------------------

UAPP_TEST("blocks.type: the eight implemented types report themselves as implemented") {
    UAPP_CHECK(uapp::blocks::block_type_is_implemented(BlockType::Paragraph));
    UAPP_CHECK(uapp::blocks::block_type_is_implemented(BlockType::Heading));
    UAPP_CHECK(uapp::blocks::block_type_is_implemented(BlockType::BulletedListItem));
    UAPP_CHECK(uapp::blocks::block_type_is_implemented(BlockType::NumberedListItem));
    UAPP_CHECK(uapp::blocks::block_type_is_implemented(BlockType::ChecklistItem));
    UAPP_CHECK(uapp::blocks::block_type_is_implemented(BlockType::Code));
    UAPP_CHECK(uapp::blocks::block_type_is_implemented(BlockType::Divider));
    UAPP_CHECK(uapp::blocks::block_type_is_implemented(BlockType::Quote));

    for (BlockType t : {BlockType::Paragraph, BlockType::Heading, BlockType::Code}) {
        UAPP_CHECK(uapp::blocks::block_type_not_implemented_reason(t) == nullptr);
    }
}

UAPP_TEST("blocks.type: every deferred type is marked NOT IMPLEMENTED with a reason") {
    const BlockType deferred[] = {
        BlockType::Toggle,   BlockType::Callout,  BlockType::Table,
        BlockType::Image,    BlockType::Video,    BlockType::Audio,
        BlockType::File,     BlockType::Equation, BlockType::Embed,
        BlockType::Database, BlockType::Bookmark,
    };
    for (const BlockType t : deferred) {
        UAPP_CHECK_MSG(!uapp::blocks::block_type_is_implemented(t),
                       uapp::blocks::block_type_name(t));
        const char* reason = uapp::blocks::block_type_not_implemented_reason(t);
        UAPP_REQUIRE(reason != nullptr);
        UAPP_CHECK_MSG(std::string(reason).find("NOT IMPLEMENTED") == 0,
                       uapp::blocks::block_type_name(t));
    }
}

UAPP_TEST("blocks.type: the variant index agrees with the persisted type tag") {
    // This is the invariant the storage layer relies on: the integer in the
    // `type` column is the variant index. A reordered alternative would
    // silently reinterpret every stored block.
    UAPP_CHECK(uapp::blocks::block_type_of(uapp::blocks::ParagraphBlock{}) ==
               BlockType::Paragraph);
    UAPP_CHECK(uapp::blocks::block_type_of(uapp::blocks::HeadingBlock{"h", 1}) ==
               BlockType::Heading);
    UAPP_CHECK(uapp::blocks::block_type_of(uapp::blocks::BulletedListItemBlock{}) ==
               BlockType::BulletedListItem);
    UAPP_CHECK(uapp::blocks::block_type_of(uapp::blocks::NumberedListItemBlock{}) ==
               BlockType::NumberedListItem);
    UAPP_CHECK(uapp::blocks::block_type_of(uapp::blocks::ChecklistItemBlock{}) ==
               BlockType::ChecklistItem);
    UAPP_CHECK(uapp::blocks::block_type_of(uapp::blocks::CodeBlock{}) == BlockType::Code);
    UAPP_CHECK(uapp::blocks::block_type_of(uapp::blocks::DividerBlock{}) ==
               BlockType::Divider);
    UAPP_CHECK(uapp::blocks::block_type_of(uapp::blocks::QuoteBlock{}) == BlockType::Quote);

    UAPP_CHECK(std::string(uapp::blocks::block_type_name(BlockType::BulletedListItem)) ==
               "bulleted_list_item");
}

// --- payload validation ----------------------------------------------------

UAPP_TEST("blocks.validate: heading level must be 1..3") {
    UAPP_REQUIRE_OK(uapp::blocks::validate_block_content(uapp::blocks::HeadingBlock{"a", 1}));
    UAPP_REQUIRE_OK(uapp::blocks::validate_block_content(uapp::blocks::HeadingBlock{"a", 3}));
    UAPP_REQUIRE_STATUS(
        uapp::blocks::validate_block_content(uapp::blocks::HeadingBlock{"a", 0}),
        Status::BlockMalformed);
    UAPP_REQUIRE_STATUS(
        uapp::blocks::validate_block_content(uapp::blocks::HeadingBlock{"a", 4}),
        Status::BlockMalformed);
}

UAPP_TEST("blocks.validate: embedded NUL is rejected in text") {
    uapp::blocks::ParagraphBlock p;
    p.text = std::string("before\0after", 12);
    UAPP_REQUIRE_STATUS(uapp::blocks::validate_block_content(p), Status::BlockMalformed);
}

UAPP_TEST("blocks.validate: oversized text is rejected") {
    uapp::blocks::QuoteBlock q;
    q.text = std::string(uapp::blocks::kMaxBlockTextBytes + 1, 'x');
    UAPP_REQUIRE_STATUS(uapp::blocks::validate_block_content(q), Status::BlockTextTooLarge);
}

UAPP_TEST("blocks.validate: a code language is bounded and control-byte free") {
    UAPP_REQUIRE_OK(
        uapp::blocks::validate_block_content(uapp::blocks::CodeBlock{"x = 1", "python"}));

    uapp::blocks::CodeBlock too_long;
    too_long.language = std::string(uapp::blocks::kMaxCodeLanguageBytes + 1, 'c');
    UAPP_REQUIRE_STATUS(uapp::blocks::validate_block_content(too_long),
                        Status::BlockTextTooLarge);

    uapp::blocks::CodeBlock control;
    control.language = "c\x01";
    UAPP_REQUIRE_STATUS(uapp::blocks::validate_block_content(control),
                        Status::BlockMalformed);
}

UAPP_TEST("blocks.validate: a divider carries no payload and always validates") {
    UAPP_REQUIRE_OK(uapp::blocks::validate_block_content(uapp::blocks::DividerBlock{}));
    UAPP_CHECK(uapp::blocks::block_plain_text(uapp::blocks::DividerBlock{}).empty());
}

UAPP_TEST("blocks.page: title is bounded, empty is allowed") {
    UAPP_REQUIRE_OK(uapp::blocks::validate_page_title(""));
    UAPP_REQUIRE_OK(
        uapp::blocks::validate_page_title(std::string(uapp::blocks::kMaxPageTitleBytes, 't')));
    UAPP_REQUIRE_STATUS(
        uapp::blocks::validate_page_title(std::string(uapp::blocks::kMaxPageTitleBytes + 1, 't')),
        Status::PageTitleTooLong);
}

// --- tree helpers ----------------------------------------------------------

namespace {

/// Build a single chain of `depth` blocks, each the only child of the last.
std::vector<Block> make_chain(const PageId& page, std::size_t depth) {
    std::vector<Block> root;
    if (depth == 0) { return root; }

    std::vector<Block>* level = &root;
    BlockId parent;  // nil at the top
    for (std::size_t i = 0; i < depth; ++i) {
        Block b;
        b.id        = make_block_id(static_cast<std::uint8_t>(i + 1));
        b.parent_id = parent;
        b.page_id   = page;
        b.content   = uapp::blocks::ParagraphBlock{"n"};
        parent      = b.id;
        level->push_back(std::move(b));
        level = &level->back().children;
    }
    return root;
}

}  // namespace

UAPP_TEST("blocks.tree: depth and size are measured over the whole forest") {
    const PageId page = make_page_id(1);
    const std::vector<Block> chain = make_chain(page, 5);
    UAPP_CHECK(uapp::blocks::block_tree_depth(chain) == 5);
    UAPP_CHECK(uapp::blocks::block_tree_size(chain) == 5);
    UAPP_CHECK(uapp::blocks::block_tree_depth({}) == 0);
}

UAPP_TEST("blocks.tree: validation accepts a tree at exactly the depth limit") {
    const PageId page = make_page_id(1);
    const std::vector<Block> chain = make_chain(page, uapp::blocks::kMaxBlockDepth);
    UAPP_REQUIRE_OK(uapp::blocks::validate_block_tree(page, chain));
}

UAPP_TEST("blocks.tree: validation rejects a tree one level past the limit") {
    const PageId page = make_page_id(1);
    const std::vector<Block> chain = make_chain(page, uapp::blocks::kMaxBlockDepth + 1);
    UAPP_REQUIRE_STATUS(uapp::blocks::validate_block_tree(page, chain),
                        Status::BlockDepthExceeded);
}

UAPP_TEST("blocks.tree: validation rejects a parent link that contradicts the tree") {
    const PageId page = make_page_id(1);
    std::vector<Block> chain = make_chain(page, 2);
    UAPP_REQUIRE(chain.size() == 1);
    UAPP_REQUIRE(chain[0].children.size() == 1);

    // The child claims a different parent than the one it actually hangs off.
    chain[0].children[0].parent_id = make_block_id(0xFEu);
    UAPP_REQUIRE_STATUS(uapp::blocks::validate_block_tree(page, chain),
                        Status::BlockMalformed);
}

UAPP_TEST("blocks.tree: validation rejects a block belonging to another page") {
    const PageId page = make_page_id(1);
    std::vector<Block> chain = make_chain(page, 1);
    chain[0].page_id = make_page_id(2);
    UAPP_REQUIRE_STATUS(uapp::blocks::validate_block_tree(page, chain),
                        Status::BlockMalformed);
}
