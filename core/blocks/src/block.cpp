// SPDX-License-Identifier: Apache-2.0
#include "uapp/blocks/block.h"

#include <utility>

namespace uapp::blocks {
namespace {

/// Shared text validation for every text-bearing payload.
Status validate_text(std::string_view text, std::size_t limit) noexcept {
    if (text.size() > limit) {
        return Status::BlockTextTooLarge;
    }
    // An embedded NUL truncates the value the moment it crosses any C
    // boundary (sqlite3_bind_text, printf, the future C ABI). Reject once,
    // here, rather than discover a silently shortened block later.
    if (text.find('\0') != std::string_view::npos) {
        return Status::BlockMalformed;
    }
    return Status::Ok;
}

/// The NOT-IMPLEMENTED table. One row per spec section 14 type we declare but
/// do not build, with the reason. Keeping this in code rather than in a
/// comment means `block_type_not_implemented_reason` cannot drift from
/// reality, and a caller (CLI, future UI, future C ABI) can show the reason.
const char* unimplemented_reason(BlockType type) noexcept {
    switch (type) {
        case BlockType::Toggle:
            return "NOT IMPLEMENTED: a toggle is a container whose collapsed/expanded "
                   "state is per-viewer UI state, not document content; it needs the "
                   "view-state store that does not exist yet. Its child-block half is "
                   "already covered by generic nesting.";
        case BlockType::Callout:
            return "NOT IMPLEMENTED: needs the icon/emoji and colour-token vocabulary "
                   "shared with page icons, which is a design-system decision, not a "
                   "data-model one.";
        case BlockType::Table:
            return "NOT IMPLEMENTED: a table block is a view over the database system "
                   "(spec section 15 properties/views), which is explicitly out of scope "
                   "for this run.";
        case BlockType::Image:
        case BlockType::Video:
        case BlockType::Audio:
        case BlockType::File:
            return "NOT IMPLEMENTED: media and file blocks reference blob-store objects "
                   "by content hash; database-design.md principle 6 forbids putting the "
                   "bytes in the database, and the blob store does not exist yet.";
        case BlockType::Equation:
            return "NOT IMPLEMENTED: needs a LaTeX subset parser and a renderer; an "
                   "unvalidated LaTeX string passed to a renderer is an untrusted-input "
                   "surface (spec section 38) that deserves its own fuzz target.";
        case BlockType::Embed:
        case BlockType::Bookmark:
            return "NOT IMPLEMENTED: both fetch remote URLs for previews, which breaks "
                   "the offline-first default and needs the network policy gate and URL "
                   "allow-listing from security-model.md.";
        case BlockType::Database:
            return "NOT IMPLEMENTED: an inline database block is spec section 15, which "
                   "is a later run.";
        case BlockType::Paragraph:
        case BlockType::Heading:
        case BlockType::BulletedListItem:
        case BlockType::NumberedListItem:
        case BlockType::ChecklistItem:
        case BlockType::Code:
        case BlockType::Divider:
        case BlockType::Quote:
            return nullptr;
    }
    return "NOT IMPLEMENTED: unrecognised block type.";
}

}  // namespace

BlockType block_type_of(const BlockContent& content) noexcept {
    // The variant index IS the type tag; the static_asserts in the header
    // guarantee they agree. valueless_by_exception() cannot happen under
    // -fno-exceptions, but index() would return variant_npos if it somehow
    // did, so clamp rather than cast garbage into the enum.
    const std::size_t index = content.index();
    if (index >= std::variant_size_v<BlockContent>) {
        return BlockType::Paragraph;
    }
    return static_cast<BlockType>(static_cast<std::int32_t>(index));
}

const char* block_type_name(BlockType type) noexcept {
    switch (type) {
        case BlockType::Paragraph:        return "paragraph";
        case BlockType::Heading:          return "heading";
        case BlockType::BulletedListItem: return "bulleted_list_item";
        case BlockType::NumberedListItem: return "numbered_list_item";
        case BlockType::ChecklistItem:    return "checklist_item";
        case BlockType::Code:             return "code";
        case BlockType::Divider:          return "divider";
        case BlockType::Quote:            return "quote";
        case BlockType::Toggle:           return "toggle";
        case BlockType::Callout:          return "callout";
        case BlockType::Table:            return "table";
        case BlockType::Image:            return "image";
        case BlockType::Video:            return "video";
        case BlockType::Audio:            return "audio";
        case BlockType::File:             return "file";
        case BlockType::Equation:         return "equation";
        case BlockType::Embed:            return "embed";
        case BlockType::Database:         return "database";
        case BlockType::Bookmark:         return "bookmark";
    }
    return "unknown";
}

bool block_type_is_implemented(BlockType type) noexcept {
    const auto value = static_cast<std::int32_t>(type);
    return value >= 0 && value < kFirstUnimplementedBlockType;
}

const char* block_type_not_implemented_reason(BlockType type) noexcept {
    if (block_type_is_implemented(type)) { return nullptr; }
    return unimplemented_reason(type);
}

std::string_view block_plain_text(const BlockContent& content) noexcept {
    if (const auto* p = std::get_if<ParagraphBlock>(&content))         { return p->text; }
    if (const auto* p = std::get_if<HeadingBlock>(&content))           { return p->text; }
    if (const auto* p = std::get_if<BulletedListItemBlock>(&content))  { return p->text; }
    if (const auto* p = std::get_if<NumberedListItemBlock>(&content))  { return p->text; }
    if (const auto* p = std::get_if<ChecklistItemBlock>(&content))     { return p->text; }
    if (const auto* p = std::get_if<CodeBlock>(&content))              { return p->code; }
    if (const auto* p = std::get_if<QuoteBlock>(&content))             { return p->text; }
    // DividerBlock: pure structure, no text.
    return std::string_view();
}

Status validate_block_content(const BlockContent& content) noexcept {
    if (const auto* p = std::get_if<HeadingBlock>(&content)) {
        if (p->level < kMinHeadingLevel || p->level > kMaxHeadingLevel) {
            return Status::BlockMalformed;
        }
        return validate_text(p->text, kMaxBlockTextBytes);
    }
    if (const auto* p = std::get_if<CodeBlock>(&content)) {
        if (const Status s = validate_text(p->code, kMaxBlockTextBytes); !is_ok(s)) {
            return s;
        }
        if (p->language.size() > kMaxCodeLanguageBytes) {
            return Status::BlockTextTooLarge;
        }
        // Control bytes in a language tag would end up in a CSS class name or
        // a highlighter lookup key. Reject them rather than sanitise later.
        for (const char c : p->language) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20u || byte == 0x7Fu) { return Status::BlockMalformed; }
        }
        return Status::Ok;
    }
    if (std::get_if<DividerBlock>(&content) != nullptr) {
        return Status::Ok;
    }
    return validate_text(block_plain_text(content), kMaxBlockTextBytes);
}

std::size_t block_tree_depth(const std::vector<Block>& blocks) noexcept {
    // Iterative, with an explicit stack, on purpose: this function is one of
    // the tools used to decide whether a tree is too deep, so it must not
    // itself recurse into an untrusted tree to find out.
    std::size_t deepest = 0;
    std::vector<std::pair<const std::vector<Block>*, std::size_t>> stack;
    stack.push_back({&blocks, 1});
    while (!stack.empty()) {
        const auto [level, depth] = stack.back();
        stack.pop_back();
        if (level->empty()) { continue; }
        if (depth > deepest) { deepest = depth; }
        if (depth > kMaxBlockDepth) {
            // Far enough to answer "too deep"; do not walk the rest.
            return deepest;
        }
        for (const Block& b : *level) {
            if (!b.children.empty()) { stack.push_back({&b.children, depth + 1}); }
        }
    }
    return deepest;
}

std::size_t block_tree_size(const std::vector<Block>& blocks) noexcept {
    std::size_t total = 0;
    std::vector<const std::vector<Block>*> stack;
    stack.push_back(&blocks);
    while (!stack.empty()) {
        const std::vector<Block>* level = stack.back();
        stack.pop_back();
        total += level->size();
        for (const Block& b : *level) {
            if (!b.children.empty()) { stack.push_back(&b.children); }
        }
    }
    return total;
}

}  // namespace uapp::blocks
