// SPDX-License-Identifier: Apache-2.0
//
// core/sync/spike/src/rga.cpp — classic RGA, the defective control. See rga.h.
//
// SPIKE CODE — NOT PRODUCTION, AND NOT A CANDIDATE.

#include "uapp/sync/spike/rga.h"

#include <algorithm>
#include <utility>

namespace uapp::sync::spike {

RgaReplica::RgaReplica(ReplicaId replica_id) : replica_(replica_id) {
    Node head;
    head.id     = kNullId;
    head.origin = kNullId;
    nodes_.push_back(std::move(head));
    index_.emplace(element_key(kNullId), 0u);
}

bool RgaReplica::resolve(ElementId id, std::uint32_t& index) const {
    const auto it = index_.find(element_key(id));
    if (it == index_.end()) { return false; }
    index = it->second;
    return true;
}

void RgaReplica::traversal(std::vector<std::uint32_t>& out) const {
    // Depth-first PRE-ORDER: a node, then each child's whole subtree, children
    // taken in descending timestamp order. Iterative, so a long document does
    // not overflow the stack (an RGA tree degenerates into a spine for
    // sequentially typed text, exactly like a Fugue tree).
    out.clear();
    out.reserve(nodes_.size());
    std::vector<std::uint32_t> stack;
    stack.push_back(0u);
    while (!stack.empty()) {
        const std::uint32_t current = stack.back();
        stack.pop_back();
        out.push_back(current);
        const std::vector<std::uint32_t>& kids = nodes_[current].children;
        // children are stored descending; push in reverse so the first child
        // is popped first.
        for (std::size_t i = kids.size(); i > 0; --i) {
            stack.push_back(kids[i - 1u]);
        }
    }
}

std::string RgaReplica::text() const {
    std::vector<std::uint32_t> order;
    traversal(order);
    std::string result;
    result.reserve(order.size());
    for (const std::uint32_t idx : order) {
        if (idx == 0u) { continue; }
        if (!nodes_[idx].deleted) { result.push_back(nodes_[idx].value); }
    }
    return result;
}

std::size_t RgaReplica::visible_size() const {
    std::size_t visible = 0;
    for (std::size_t i = 1; i < nodes_.size(); ++i) {
        if (!nodes_[i].deleted) { ++visible; }
    }
    return visible;
}

SpikeStatus RgaReplica::insert(std::size_t index, char value, RgaOperation& out) {
    std::vector<std::uint32_t> order;
    traversal(order);

    // Origin = the visible predecessor, or the head sentinel at index 0. RGA
    // records this and nothing else; the absence of a right origin is the bug.
    ElementId origin = kNullId;
    if (index > 0) {
        std::size_t seen  = 0;
        bool        found = false;
        for (std::size_t p = 1; p < order.size(); ++p) {
            if (nodes_[order[p]].deleted) { continue; }
            ++seen;
            if (seen == index) {
                origin = nodes_[order[p]].id;
                found  = true;
                break;
            }
        }
        if (!found) { return SpikeStatus::IndexOutOfRange; }
    }

    RgaOperation op;
    op.kind   = OpKind::Insert;
    op.id     = ElementId{replica_, clock_ + 1u};
    op.origin = origin;
    op.value  = value;

    const SpikeStatus st = integrate(op);
    if (!is_ok(st)) { return st; }
    clock_ += 1u;
    out = op;
    return SpikeStatus::Ok;
}

SpikeStatus RgaReplica::erase(std::size_t index, RgaOperation& out) {
    std::vector<std::uint32_t> order;
    traversal(order);
    std::size_t seen = 0;
    for (std::size_t p = 1; p < order.size(); ++p) {
        if (nodes_[order[p]].deleted) { continue; }
        if (seen == index) {
            RgaOperation op;
            op.kind              = OpKind::Delete;
            op.id                = nodes_[order[p]].id;
            nodes_[order[p]].deleted = true;
            out                  = op;
            return SpikeStatus::Ok;
        }
        ++seen;
    }
    return SpikeStatus::IndexOutOfRange;
}

SpikeStatus RgaReplica::apply(const RgaOperation& op) {
    if (is_null(op.id)) { return SpikeStatus::OpMalformed; }

    // Lamport clock: whatever we send next must dominate what we have seen.
    clock_ = std::max(clock_, op.id.counter);

    std::uint32_t index = 0u;
    if (op.kind == OpKind::Delete) {
        if (!resolve(op.id, index)) {
            if (pending_.size() >= kMaxPendingOperations) {
                return SpikeStatus::PendingBufferFull;
            }
            pending_.push_back(op);
            return SpikeStatus::Ok;
        }
        nodes_[index].deleted = true;
        return SpikeStatus::Ok;
    }

    if (resolve(op.id, index)) { return SpikeStatus::Ok; }  // idempotent
    if (!resolve(op.origin, index)) {
        if (pending_.size() >= kMaxPendingOperations) { return SpikeStatus::PendingBufferFull; }
        pending_.push_back(op);
        return SpikeStatus::Ok;
    }

    const SpikeStatus st = integrate(op);
    if (!is_ok(st)) { return st; }
    drain_pending();
    return SpikeStatus::Ok;
}

void RgaReplica::drain_pending() {
    bool progress = true;
    while (progress) {
        progress = false;
        for (std::size_t i = 0; i < pending_.size();) {
            const RgaOperation held = pending_[i];
            std::uint32_t      index = 0u;
            const bool         ready = (held.kind == OpKind::Delete)
                                           ? resolve(held.id, index)
                                           : resolve(held.origin, index);
            if (!ready) {
                ++i;
                continue;
            }
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
            if (held.kind == OpKind::Delete) {
                if (resolve(held.id, index)) { nodes_[index].deleted = true; }
            } else if (!resolve(held.id, index)) {
                if (!is_ok(integrate(held))) {
                    pending_.push_back(held);
                    return;
                }
            }
            progress = true;
        }
    }
}

SpikeStatus RgaReplica::integrate(const RgaOperation& op) {
    if (nodes_.size() - 1u >= kMaxElements) { return SpikeStatus::ElementLimitExceeded; }

    std::uint32_t origin_index = 0u;
    if (!resolve(op.origin, origin_index)) { return SpikeStatus::Internal; }

    Node node;
    node.id     = op.id;
    node.origin = op.origin;
    node.value  = op.value;

    const auto new_index = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(std::move(node));
    index_.emplace(element_key(op.id), new_index);

    // THE DEFECT, in one line: all elements sharing an origin are siblings and
    // are ordered purely by descending timestamp. A word typed right-to-left
    // gives every one of its characters the SAME origin, so a concurrent
    // insertion at that origin sorts into the middle of it.
    std::vector<std::uint32_t>& kids = nodes_[origin_index].children;
    std::size_t                 at   = kids.size();
    for (std::size_t i = 0; i < kids.size(); ++i) {
        if (rga_timestamp_less(nodes_[kids[i]].id, op.id)) {
            at = i;
            break;
        }
    }
    kids.insert(kids.begin() + static_cast<std::ptrdiff_t>(at), new_index);

    return SpikeStatus::Ok;
}

}  // namespace uapp::sync::spike
