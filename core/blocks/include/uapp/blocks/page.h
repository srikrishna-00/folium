// SPDX-License-Identifier: Apache-2.0
//
// core/blocks/page.h — the Page, the container spec section 14 hangs blocks off.
//
//     Workspace -> Page -> Blocks / Child Pages / Databases / Files
//                          ^^^^^^
//
// What this type covers and what it does not, explicitly:
//
//   Blocks        IMPLEMENTED (ordered, nested; see block.h).
//   Child Pages   NOT IMPLEMENTED. database-design.md section 3.3 models them
//                 as `document.parent_id`, i.e. one nullable column plus the
//                 same ordering machinery blocks already have. Left out of
//                 this run to keep the change reviewable; the schema column is
//                 deliberately absent rather than present-and-ignored, so
//                 nothing can half-work.
//   Databases     NOT IMPLEMENTED. That is spec section 15 (properties, views,
//                 filters, rollups) and is explicitly a later run.
//   Files         NOT IMPLEMENTED. Needs the blob store (database-design.md
//                 principle 6: "blobs are not in the database").
//   Workspace     NOT IMPLEMENTED. Pages currently live in one implicit
//                 workspace. `page` has no workspace_id column yet; adding it
//                 is an additive migration.

#ifndef UAPP_CORE_BLOCKS_PAGE_H
#define UAPP_CORE_BLOCKS_PAGE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "uapp/blocks/block.h"
#include "uapp/status.h"

namespace uapp::blocks {

struct Page final {
    PageId       id;
    std::string  title;
    std::int64_t created_at_ms  = 0;
    std::int64_t modified_at_ms = 0;

    /// Top-level blocks in document order. Populated by a full read; empty in
    /// the metadata-only paths, which say so at their declaration.
    std::vector<Block> blocks;
};

/// An empty title is allowed -- "Untitled" is a real state that users spend
/// most of a page's life in, and database-design.md gives `document.title` a
/// DEFAULT ''. Only the upper bound and embedded NUL are errors.
[[nodiscard]] Status validate_page_title(std::string_view title) noexcept;

/// Validate page metadata: non-nil id, legal title, non-negative timestamps.
/// Does NOT walk `blocks`; use validate_block_tree for that.
[[nodiscard]] Status validate_page_metadata(const Page& page) noexcept;

/// Validate a whole block forest: depth, total size, per-payload rules, and
/// that every node's parent/page links agree with where it actually sits.
/// Used on read, because a database file is attacker-reachable (spec
/// section 28: never assume an input is valid, including one that was ours).
[[nodiscard]] Status validate_block_tree(const PageId& page_id,
                                         const std::vector<Block>& blocks) noexcept;

}  // namespace uapp::blocks

#endif  // UAPP_CORE_BLOCKS_PAGE_H
