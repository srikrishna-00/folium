// SPDX-License-Identifier: Apache-2.0
#include "uapp/blocks/document_service.h"

#include <utility>

namespace uapp::blocks {

// Out-of-line so the vtable lands in exactly one translation unit.
BlockStore::BlockStore() noexcept = default;
BlockStore::~BlockStore() = default;

DocumentService::DocumentService(BlockStore& store, Clock& clock, IdSource& ids) noexcept
    : store_(store), clock_(clock), ids_(ids) {}

Status DocumentService::new_page_id(PageId& out) noexcept {
    ObjectIdBytes raw{};
    if (const Status s = ids_.generate(raw); !is_ok(s)) { return s; }
    PageId candidate(raw);
    if (candidate.is_nil()) { return Status::Internal; }
    out = candidate;
    return Status::Ok;
}

Status DocumentService::new_block_id(BlockId& out) noexcept {
    ObjectIdBytes raw{};
    if (const Status s = ids_.generate(raw); !is_ok(s)) { return s; }
    BlockId candidate(raw);
    if (candidate.is_nil()) { return Status::Internal; }
    out = candidate;
    return Status::Ok;
}

Status DocumentService::create_page(std::string_view title, Page& out) noexcept {
    if (const Status s = validate_page_title(title); !is_ok(s)) { return s; }

    Page page;
    if (const Status s = new_page_id(page.id); !is_ok(s)) { return s; }
    page.title          = std::string(title);
    page.created_at_ms  = clock_.now_unix_ms();
    page.modified_at_ms = page.created_at_ms;

    if (const Status s = store_.create_page(page); !is_ok(s)) { return s; }

    out = std::move(page);
    return Status::Ok;
}

Status DocumentService::open_page(const PageId& id, Page& out) const noexcept {
    if (id.is_nil()) { return Status::IdMalformed; }
    return store_.get_page(id, out);
}

Status DocumentService::rename_page(const PageId& id, std::string_view title) noexcept {
    if (id.is_nil()) { return Status::IdMalformed; }
    if (const Status s = validate_page_title(title); !is_ok(s)) { return s; }
    return store_.set_page_title(id, title, clock_.now_unix_ms());
}

Status DocumentService::delete_page(const PageId& id) noexcept {
    if (id.is_nil()) { return Status::IdMalformed; }
    return store_.delete_page(id);
}

Status DocumentService::page_count(std::size_t& out) const noexcept {
    return store_.page_count(out);
}

Status DocumentService::add_block(const PageId& page_id,
                                  const BlockId& parent_id,
                                  std::size_t position,
                                  const BlockContent& content,
                                  Block& out) noexcept {
    if (page_id.is_nil()) { return Status::IdMalformed; }
    if (const Status s = validate_block_content(content); !is_ok(s)) { return s; }

    BlockId id;
    if (const Status s = new_block_id(id); !is_ok(s)) { return s; }

    const std::int64_t now = clock_.now_unix_ms();
    if (const Status s = store_.insert_block(page_id, parent_id, position, id, content, now);
        !is_ok(s)) {
        return s;
    }

    Block created;
    created.id             = id;
    created.parent_id      = parent_id;
    created.page_id        = page_id;
    created.content        = content;
    created.created_at_ms  = now;
    created.modified_at_ms = now;
    out = std::move(created);
    return Status::Ok;
}

Status DocumentService::append_block(const PageId& page_id,
                                     const BlockId& parent_id,
                                     const BlockContent& content,
                                     Block& out) noexcept {
    return add_block(page_id, parent_id, kAppendPosition, content, out);
}

Status DocumentService::get_block(const BlockId& id, Block& out) const noexcept {
    if (id.is_nil()) { return Status::IdMalformed; }
    return store_.get_block(id, out);
}

Status DocumentService::update_block(const BlockId& id, const BlockContent& content) noexcept {
    if (id.is_nil()) { return Status::IdMalformed; }
    if (const Status s = validate_block_content(content); !is_ok(s)) { return s; }
    return store_.update_block(id, content, clock_.now_unix_ms());
}

Status DocumentService::delete_block(const BlockId& id) noexcept {
    if (id.is_nil()) { return Status::IdMalformed; }
    return store_.delete_block(id, clock_.now_unix_ms());
}

Status DocumentService::reorder_block(const BlockId& id, std::size_t new_position) noexcept {
    if (id.is_nil()) { return Status::IdMalformed; }

    // A reorder is a move that keeps the parent. Reading the parent back from
    // the store rather than taking it from the caller means a caller working
    // from a stale tree cannot accidentally re-parent a block while thinking
    // it is only nudging it up one slot.
    Block current;
    if (const Status s = store_.get_block(id, current); !is_ok(s)) { return s; }

    return store_.move_block(id, current.parent_id, new_position, clock_.now_unix_ms());
}

Status DocumentService::move_block(const BlockId& id,
                                   const BlockId& new_parent_id,
                                   std::size_t new_position) noexcept {
    if (id.is_nil()) { return Status::IdMalformed; }
    if (id == new_parent_id) { return Status::BlockCycleDetected; }
    return store_.move_block(id, new_parent_id, new_position, clock_.now_unix_ms());
}

Status DocumentService::block_count(const PageId& page_id, std::size_t& out) const noexcept {
    if (page_id.is_nil()) { return Status::IdMalformed; }
    return store_.block_count(page_id, out);
}

}  // namespace uapp::blocks
