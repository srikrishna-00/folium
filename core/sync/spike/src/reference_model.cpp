// SPDX-License-Identifier: Apache-2.0
//
// core/sync/spike/src/reference_model.cpp — see reference_model.h for why.
//
// SPIKE CODE — NOT PRODUCTION.
//
// Deliberately slow and deliberately structured differently from fugue.cpp:
// whole-set input, precomputed root paths, a pairwise comparator, and an
// O(n^2) rank count. No incremental state, no tree walk, nothing shared.

#include "uapp/sync/spike/reference_model.h"

#include <cstdio>
#include <unordered_map>
#include <utility>

namespace uapp::sync::spike {
namespace {

/// Guard on the FugueMax comparator's recursion. Each level compares two
/// elements that are strictly CAUSALLY PRIOR to the pair at the level above
/// (a right origin always existed before the element that names it), so the
/// recursion terminates at the causal depth of the history. The guard exists
/// so that a malformed input — a right-origin cycle that could not arise from
/// a real execution — is reported rather than crashing the test binary.
constexpr std::size_t kMaxComparatorDepth = 4096;

struct Element final {
    ElementId     id{};
    ElementId     parent{};
    ElementId     right_origin{};
    char          value   = '\0';
    Side          side    = Side::Right;
    bool          deleted = false;
    /// Indices of ancestors from the outermost (a child of the root) down to
    /// and including this element. Empty only for an element whose path could
    /// not be resolved.
    std::vector<std::size_t> path;
};

class Model final {
public:
    Model(Variant variant, std::vector<Element> elements,
          std::unordered_map<std::uint64_t, std::size_t> by_id)
        : variant_(variant), elements_(std::move(elements)), by_id_(std::move(by_id)) {}

    /// Is element `a` before element `b` in the list order?
    [[nodiscard]] bool less(std::size_t a, std::size_t b, std::size_t depth, bool& failed) const {
        if (depth > kMaxComparatorDepth) {
            failed = true;
            return false;
        }
        const std::vector<std::size_t>& pa = elements_[a].path;
        const std::vector<std::size_t>& pb = elements_[b].path;

        std::size_t k = 0;
        while (k < pa.size() && k < pb.size() && pa[k] == pb[k]) { ++k; }

        // `a` is an ancestor of `b`: b sits in one of a's child lists, so the
        // side of b's next step decides. Left children come before the parent,
        // right children after it.
        if (k == pa.size()) { return elements_[pb[k]].side == Side::Right; }
        if (k == pb.size()) { return elements_[pa[k]].side == Side::Left; }

        // Otherwise pa[k] and pb[k] are distinct siblings under a common
        // parent, and whichever of them comes first takes its whole subtree
        // with it.
        const Element& x = elements_[pa[k]];
        const Element& y = elements_[pb[k]];

        if (x.side != y.side) { return x.side == Side::Left; }
        if (x.side == Side::Left) { return x.id < y.id; }
        if (variant_ == Variant::Fugue) { return x.id < y.id; }

        // FugueMax, Definition 6: right siblings in the REVERSE order of their
        // right origins, ties by id. `end` (a null right origin) is the last
        // position in the list, so reversal puts it first.
        if (x.right_origin == y.right_origin) { return x.id < y.id; }
        if (is_null(x.right_origin)) { return true; }
        if (is_null(y.right_origin)) { return false; }

        std::size_t xr = 0;
        std::size_t yr = 0;
        if (!lookup(x.right_origin, xr) || !lookup(y.right_origin, yr)) {
            failed = true;
            return false;
        }
        return less(yr, xr, depth + 1u, failed);
    }

    [[nodiscard]] bool lookup(ElementId id, std::size_t& out) const {
        const auto it = by_id_.find(element_key(id));
        if (it == by_id_.end()) { return false; }
        out = it->second;
        return true;
    }

    [[nodiscard]] const std::vector<Element>& elements() const noexcept { return elements_; }

private:
    Variant                                        variant_;
    std::vector<Element>                           elements_;
    std::unordered_map<std::uint64_t, std::size_t> by_id_;
};

}  // namespace

ReferenceOrder reference_order(const std::vector<Operation>& ops, Variant variant) {
    ReferenceOrder result;

    // --- 1. collect the element set (the ORDER of `ops` must not matter) ----
    std::vector<Element>                           elements;
    std::unordered_map<std::uint64_t, std::size_t> by_id;

    for (const Operation& op : ops) {
        if (op.kind != OpKind::Insert) { continue; }
        if (is_null(op.id)) {
            result.diagnostic = "insert operation with a null element id";
            return result;
        }
        if (by_id.find(element_key(op.id)) != by_id.end()) { continue; }  // duplicate delivery
        Element e;
        e.id           = op.id;
        e.parent       = op.parent;
        e.right_origin = op.right_origin;
        e.value        = op.value;
        e.side         = op.side;
        by_id.emplace(element_key(op.id), elements.size());
        elements.push_back(std::move(e));
    }
    for (const Operation& op : ops) {
        if (op.kind != OpKind::Delete) { continue; }
        const auto it = by_id.find(element_key(op.id));
        if (it == by_id.end()) {
            result.diagnostic = "delete operation for an element that was never inserted";
            return result;
        }
        elements[it->second].deleted = true;
    }

    const std::size_t n = elements.size();

    // --- 2. root paths -----------------------------------------------------
    // Walk each element's parent chain, bounded by n so that a malformed input
    // containing a cycle is reported instead of looping forever.
    for (std::size_t i = 0; i < n; ++i) {
        std::vector<std::size_t> upward;
        std::size_t             cursor = i;
        bool                    rooted = false;
        for (std::size_t steps = 0; steps <= n; ++steps) {
            upward.push_back(cursor);
            const ElementId parent = elements[cursor].parent;
            if (is_null(parent)) {
                rooted = true;
                break;
            }
            const auto it = by_id.find(element_key(parent));
            if (it == by_id.end()) { break; }
            cursor = it->second;
        }
        if (!rooted) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "element (%u,%u) has no path to the root (missing parent or cycle)",
                          elements[i].id.replica, elements[i].id.counter);
            result.diagnostic = buf;
            return result;
        }
        elements[i].path.assign(upward.rbegin(), upward.rend());
    }

    const Model model(variant, std::move(elements), std::move(by_id));

    // --- 3. ranks by brute force, which also proves the order is total -----
    std::vector<std::size_t> rank(n, 0);
    bool                     comparator_failed = false;
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t lesser = 0;
        for (std::size_t j = 0; j < n; ++j) {
            if (i == j) { continue; }
            if (model.less(j, i, 0, comparator_failed)) { ++lesser; }
        }
        rank[i] = lesser;
    }
    if (comparator_failed) {
        result.diagnostic = "comparator recursion limit or unresolvable right origin";
        return result;
    }

    std::vector<std::size_t> slot(n, n);
    for (std::size_t i = 0; i < n; ++i) {
        if (rank[i] >= n || slot[rank[i]] != n) {
            // Two elements compared equal, or a rank fell outside range. Either
            // way the comparator is not a strict total order and the model
            // refuses to invent an answer.
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "comparator is not a total order: element (%u,%u) got rank %zu",
                          model.elements()[i].id.replica, model.elements()[i].id.counter,
                          rank[i]);
            result.diagnostic = buf;
            return result;
        }
        slot[rank[i]] = i;
    }

    // --- 4. materialise ----------------------------------------------------
    result.order.reserve(n);
    result.text.reserve(n);
    for (std::size_t p = 0; p < n; ++p) {
        const Element& e = model.elements()[slot[p]];
        result.order.push_back(e.id);
        if (!e.deleted) { result.text.push_back(e.value); }
    }
    result.ok = true;
    return result;
}

bool satisfies_strong_list_spec(const std::vector<InsertWitness>& witnesses,
                                const std::vector<ElementId>&     order,
                                std::string&                      why) {
    std::unordered_map<std::uint64_t, std::size_t> position;
    position.reserve(order.size() * 2u);
    for (std::size_t p = 0; p < order.size(); ++p) {
        position.emplace(element_key(order[p]), p);
    }

    const auto position_of = [&position](ElementId id, std::size_t& out) {
        const auto it = position.find(element_key(id));
        if (it == position.end()) { return false; }
        out = it->second;
        return true;
    };

    char buf[256];
    for (const InsertWitness& w : witnesses) {
        std::size_t mine = 0;
        if (!position_of(w.inserted, mine)) {
            std::snprintf(buf, sizeof(buf),
                          "element (%u,%u) was inserted but is absent from the merged order",
                          w.inserted.replica, w.inserted.counter);
            why = buf;
            return false;
        }
        for (const ElementId& b : w.before) {
            std::size_t other = 0;
            if (!position_of(b, other)) {
                std::snprintf(buf, sizeof(buf),
                              "witness for (%u,%u) names (%u,%u), absent from the merged order",
                              w.inserted.replica, w.inserted.counter, b.replica, b.counter);
                why = buf;
                return false;
            }
            if (other >= mine) {
                std::snprintf(buf, sizeof(buf),
                              "strong list spec (b) violated: (%u,%u) was BEFORE the insertion "
                              "position of (%u,%u) but merged at %zu >= %zu",
                              b.replica, b.counter, w.inserted.replica, w.inserted.counter,
                              other, mine);
                why = buf;
                return false;
            }
        }
        for (const ElementId& a : w.after) {
            std::size_t other = 0;
            if (!position_of(a, other)) {
                std::snprintf(buf, sizeof(buf),
                              "witness for (%u,%u) names (%u,%u), absent from the merged order",
                              w.inserted.replica, w.inserted.counter, a.replica, a.counter);
                why = buf;
                return false;
            }
            if (other <= mine) {
                std::snprintf(buf, sizeof(buf),
                              "strong list spec (b) violated: (%u,%u) was AFTER the insertion "
                              "position of (%u,%u) but merged at %zu <= %zu",
                              a.replica, a.counter, w.inserted.replica, w.inserted.counter,
                              other, mine);
                why = buf;
                return false;
            }
        }
    }
    why.clear();
    return true;
}

std::size_t maximal_run_count(const std::string& text, const std::string& charset) {
    bool        member[256] = {};
    for (const char c : charset) {
        member[static_cast<unsigned char>(c)] = true;
    }
    std::size_t runs    = 0;
    bool        in_run  = false;
    for (const char c : text) {
        const bool is_member = member[static_cast<unsigned char>(c)];
        if (is_member && !in_run) { ++runs; }
        in_run = is_member;
    }
    return runs;
}

}  // namespace uapp::sync::spike
