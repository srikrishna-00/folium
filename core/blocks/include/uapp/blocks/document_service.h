// SPDX-License-Identifier: Apache-2.0
//
// core/blocks/document_service.h — the public API of the document module.
//
// This is the "Support:" list from spec section 14, as far as this run goes:
//
//   Drag/drop       -> reorder_block / move_block (the model half of it)
//   Nested blocks   -> IMPLEMENTED, depth-capped at kMaxBlockDepth
//   Copy/paste      -> NOT IMPLEMENTED (needs a clipboard serialisation format)
//   Undo/redo       -> NOT IMPLEMENTED, explicitly out of scope this run. It is
//                      not a method on this class: doing it properly means an
//                      operation log with inverses, which changes how every
//                      mutation below is recorded, so bolting it on later is
//                      cheaper than faking it now.
//   Version history -> NOT IMPLEMENTED (needs the same operation log, plus
//                      snapshots; database-design.md has a `version` namespace)
//   Backlinks       -> NOT IMPLEMENTED (needs mentions + a link index)
//   Mentions        -> NOT IMPLEMENTED (needs inline rich-text spans; today a
//                      block's text is a flat string, see "rich text" below)
//   Comments        -> NOT IMPLEMENTED (database-design.md section 3.x anchors
//                      a comment to a block id, which now exists)
//   Sharing         -> NOT IMPLEMENTED (needs sync + permissions)
//   Permissions     -> NOT IMPLEMENTED
//   Templates       -> NOT IMPLEMENTED (a template is a page copy operation)
//
// Rich text: a block's text is a plain UTF-8 string. Inline bold/italic/link/
// mention spans are NOT IMPLEMENTED; they need an inline run model plus the
// sequence CRDT that database-design.md reserves `block.crdt_state` for.
//
// Thread affinity: use one DocumentService from one thread at a time. It
// holds no mutable state; concurrency is exactly what the injected BlockStore
// provides (architecture.md section 6).

#ifndef UAPP_CORE_BLOCKS_DOCUMENT_SERVICE_H
#define UAPP_CORE_BLOCKS_DOCUMENT_SERVICE_H

#include <cstddef>
#include <string_view>

#include "uapp/blocks/block.h"
#include "uapp/blocks/block_store.h"
#include "uapp/blocks/page.h"
#include "uapp/ports.h"
#include "uapp/status.h"

namespace uapp::blocks {

class DocumentService final {
public:
    /// All three collaborators are borrowed, not owned, and must outlive the
    /// service. References rather than pointers make "null was passed"
    /// unrepresentable instead of something to re-check at runtime.
    DocumentService(BlockStore& store, Clock& clock, IdSource& ids) noexcept;

    DocumentService(const DocumentService&) = delete;
    DocumentService& operator=(const DocumentService&) = delete;

    // --- pages -------------------------------------------------------------

    /// Create an empty page. `out` receives the stored page on success and is
    /// untouched on failure.
    [[nodiscard]] Status create_page(std::string_view title, Page& out) noexcept;

    /// Read a page with its full block tree.
    [[nodiscard]] Status open_page(const PageId& id, Page& out) const noexcept;

    [[nodiscard]] Status rename_page(const PageId& id, std::string_view title) noexcept;

    /// Delete a page and every block on it.
    [[nodiscard]] Status delete_page(const PageId& id) noexcept;

    [[nodiscard]] Status page_count(std::size_t& out) const noexcept;

    // --- blocks ------------------------------------------------------------

    /// Add a block at an explicit sibling index under `parent_id` (nil parent
    /// = top level). `out` receives the created block, without children.
    [[nodiscard]] Status add_block(const PageId& page_id,
                                   const BlockId& parent_id,
                                   std::size_t position,
                                   const BlockContent& content,
                                   Block& out) noexcept;

    /// Add a block after the last existing sibling.
    [[nodiscard]] Status append_block(const PageId& page_id,
                                      const BlockId& parent_id,
                                      const BlockContent& content,
                                      Block& out) noexcept;

    [[nodiscard]] Status get_block(const BlockId& id, Block& out) const noexcept;

    /// Replace a block's content, keeping its children and its place.
    [[nodiscard]] Status update_block(const BlockId& id,
                                      const BlockContent& content) noexcept;

    /// Delete a block and its whole subtree. See BlockStore::delete_block for
    /// why children cascade rather than being reparented.
    [[nodiscard]] Status delete_block(const BlockId& id) noexcept;

    /// Reorder within the current parent: the "drag it up two places" case.
    [[nodiscard]] Status reorder_block(const BlockId& id,
                                       std::size_t new_position) noexcept;

    /// Re-parent and position in one step: the "drag it under that toggle"
    /// case. Nil `new_parent_id` moves the block to the page's top level.
    [[nodiscard]] Status move_block(const BlockId& id,
                                    const BlockId& new_parent_id,
                                    std::size_t new_position) noexcept;

    [[nodiscard]] Status block_count(const PageId& page_id,
                                     std::size_t& out) const noexcept;

private:
    [[nodiscard]] Status new_page_id(PageId& out) noexcept;
    [[nodiscard]] Status new_block_id(BlockId& out) noexcept;

    BlockStore& store_;
    Clock&      clock_;
    IdSource&   ids_;
};

}  // namespace uapp::blocks

#endif  // UAPP_CORE_BLOCKS_DOCUMENT_SERVICE_H
