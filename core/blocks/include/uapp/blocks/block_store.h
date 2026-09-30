// SPDX-License-Identifier: Apache-2.0
//
// core/blocks/block_store.h — the persistence PORT for pages and blocks.
//
// Same seam, same rules as core/notes/note_store.h: PHASE0_SUMMARY.md H-3
// ("nothing outside storage/sqlite includes sqlite3.h") and spec section 32
// ("do not tightly couple business logic to one database engine"). core/blocks
// links this abstract interface only; storage/sqlite implements it.
//
// Contract every implementation MUST honour:
//  * Every mutating method is ALL-OR-NOTHING. A partially applied move or a
//    delete that removed a parent but not its children is a corrupt document,
//    not a recoverable error, so each of these runs in one transaction.
//  * Sibling positions are DENSE: within one (page, parent) group they are
//    exactly 0..n-1 after every operation. Callers may rely on this.
//  * No method throws (ADR-0001: -fno-exceptions).
//  * Out-parameters are written only on Status::Ok.
//  * Depth, cycle and size limits are enforced HERE as well as in the
//    service. The service is the polite front door; this interface is
//    reachable by anyone holding the pointer.

#ifndef UAPP_CORE_BLOCKS_BLOCK_STORE_H
#define UAPP_CORE_BLOCKS_BLOCK_STORE_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "uapp/blocks/block.h"
#include "uapp/blocks/page.h"
#include "uapp/status.h"

namespace uapp::blocks {

/// Position sentinel meaning "after the last existing sibling".
inline constexpr std::size_t kAppendPosition = static_cast<std::size_t>(-1);

class BlockStore {
public:
    virtual ~BlockStore();
    BlockStore(const BlockStore&) = delete;
    BlockStore& operator=(const BlockStore&) = delete;

    // --- pages -------------------------------------------------------------

    /// Create a page from its metadata. `page.blocks` MUST be empty --
    /// creating a page and its tree in one call would need a second
    /// transactional shape for no benefit; add blocks afterwards.
    /// Status::AlreadyExists if the id is taken.
    [[nodiscard]] virtual Status create_page(const Page& page) noexcept = 0;

    /// Load a page AND its entire block tree, in document order, with nesting
    /// reconstructed. Status::NotFound if the page is absent.
    [[nodiscard]] virtual Status get_page(const PageId& id, Page& out) const noexcept = 0;

    /// Rename a page. Status::NotFound if absent.
    [[nodiscard]] virtual Status set_page_title(const PageId& id,
                                                std::string_view title,
                                                std::int64_t now_ms) noexcept = 0;

    /// Delete a page and every block on it. Status::NotFound if absent.
    [[nodiscard]] virtual Status delete_page(const PageId& id) noexcept = 0;

    [[nodiscard]] virtual Status page_count(std::size_t& out) const noexcept = 0;

    // --- blocks ------------------------------------------------------------

    /// Insert a new block.
    ///
    /// `parent_id` nil means top level. `position` is the index the block will
    /// occupy among its siblings, or kAppendPosition for the end; any other
    /// value past the end is Status::BlockPositionInvalid (NOT silently
    /// clamped -- a caller who computed a stale index should be told).
    ///
    /// Fails with Status::NotFound if the page or the parent does not exist,
    /// Status::InvalidArgument if the parent belongs to another page, and
    /// Status::BlockDepthExceeded if the parent already sits at kMaxBlockDepth.
    [[nodiscard]] virtual Status insert_block(const PageId& page_id,
                                              const BlockId& parent_id,
                                              std::size_t position,
                                              const BlockId& new_id,
                                              const BlockContent& content,
                                              std::int64_t now_ms) noexcept = 0;

    /// Load one block WITHOUT its children (`out.children` is left empty).
    /// Use get_page for the tree.
    [[nodiscard]] virtual Status get_block(const BlockId& id, Block& out) const noexcept = 0;

    /// Replace a block's content. The block TYPE may change -- replacing a
    /// paragraph with a heading is a normal editor action, and the children
    /// survive it. Status::NotFound if absent.
    [[nodiscard]] virtual Status update_block(const BlockId& id,
                                              const BlockContent& content,
                                              std::int64_t now_ms) noexcept = 0;

    /// Delete a block AND ITS ENTIRE SUBTREE, then close the gap it leaves in
    /// its sibling list.
    ///
    /// Cascade, not reparent: a child block's meaning is bound to its parent
    /// (a nested bullet under a deleted bullet, the body of a deleted toggle),
    /// so promoting orphans into the grandparent produces a document the user
    /// did not write. It also matches what every comparable editor does, and
    /// it is what database-design.md section 3.3 already specifies with
    /// `ON DELETE CASCADE` on `block.parent_id`. Undo (spec section 14) is the
    /// right answer to "I did not mean that", and is NOT IMPLEMENTED here.
    [[nodiscard]] virtual Status delete_block(const BlockId& id,
                                              std::int64_t now_ms) noexcept = 0;

    /// Move a block (with its subtree) to `new_parent_id` at `new_position`.
    /// Nil parent means top level. Same parent = a reorder.
    ///
    /// `new_position` is an index into the sibling list AFTER the block has
    /// been removed from its old place, so moving within one list uses the
    /// final index the caller wants, not a pre-removal one.
    ///
    /// Status::BlockCycleDetected if the new parent is the block itself or one
    /// of its descendants; Status::BlockDepthExceeded if the destination depth
    /// plus the moved subtree's height would exceed kMaxBlockDepth;
    /// Status::InvalidArgument if the new parent is on another page (see
    /// "cross-page move" in the changelog -- NOT IMPLEMENTED).
    [[nodiscard]] virtual Status move_block(const BlockId& id,
                                            const BlockId& new_parent_id,
                                            std::size_t new_position,
                                            std::int64_t now_ms) noexcept = 0;

    /// Total blocks on a page, at every nesting level.
    [[nodiscard]] virtual Status block_count(const PageId& page_id,
                                             std::size_t& out) const noexcept = 0;

protected:
    BlockStore() noexcept;
};

}  // namespace uapp::blocks

#endif  // UAPP_CORE_BLOCKS_BLOCK_STORE_H
