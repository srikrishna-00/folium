// SPDX-License-Identifier: Apache-2.0
#include "uapp/blocks/page.h"

namespace uapp::blocks {

Status validate_page_title(std::string_view title) noexcept {
    if (title.size() > kMaxPageTitleBytes) {
        return Status::PageTitleTooLong;
    }
    if (title.find('\0') != std::string_view::npos) {
        return Status::InvalidArgument;
    }
    return Status::Ok;
}

Status validate_page_metadata(const Page& page) noexcept {
    if (page.id.is_nil()) {
        return Status::IdMalformed;
    }
    if (const Status s = validate_page_title(page.title); !is_ok(s)) {
        return s;
    }
    if (page.created_at_ms < 0 || page.modified_at_ms < 0) {
        return Status::InvalidArgument;
    }
    return Status::Ok;
}

Status validate_block_tree(const PageId& page_id, const std::vector<Block>& blocks) noexcept {
    if (page_id.is_nil()) {
        return Status::IdMalformed;
    }

    // Iterative walk with an explicit stack. Recursing here would defeat the
    // point: this is the function that decides whether the tree is shallow
    // enough to recurse into.
    struct Frame final {
        const std::vector<Block>* level;
        const BlockId*            parent;  // nullptr at the page's top level
        std::size_t               depth;
    };

    std::size_t seen = 0;
    std::vector<Frame> stack;
    stack.push_back(Frame{&blocks, nullptr, 1});

    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();

        if (frame.depth > kMaxBlockDepth) {
            return Status::BlockDepthExceeded;
        }

        for (const Block& b : *frame.level) {
            ++seen;
            if (seen > kMaxBlocksPerPage) {
                return Status::PageBlockLimitExceeded;
            }
            if (b.id.is_nil()) {
                return Status::IdMalformed;
            }
            if (b.page_id != page_id) {
                return Status::BlockMalformed;
            }
            // The stored parent link must agree with the position the block
            // actually occupies in the tree. A tree where they disagree is
            // the shape a corrupted or hand-edited file takes.
            const bool parent_ok = (frame.parent == nullptr) ? b.parent_id.is_nil()
                                                             : (b.parent_id == *frame.parent);
            if (!parent_ok) {
                return Status::BlockMalformed;
            }
            if (b.parent_id == b.id) {
                return Status::BlockCycleDetected;
            }
            if (const Status s = validate_block_content(b.content); !is_ok(s)) {
                return s;
            }
            if (b.created_at_ms < 0 || b.modified_at_ms < 0) {
                return Status::InvalidArgument;
            }
            if (!b.children.empty()) {
                stack.push_back(Frame{&b.children, &b.id, frame.depth + 1});
            }
        }
    }

    return Status::Ok;
}

}  // namespace uapp::blocks
