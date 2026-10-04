// SPDX-License-Identifier: Apache-2.0
//
// tests/sync_spike/property_harness.h — generators and replay for the Fugue
// correctness spike.
//
// SPIKE TEST SUPPORT. The code under test is core/sync/spike, which no product
// target links; see core/sync/spike/include/uapp/sync/spike/element.h.
//
// ===========================================================================
// WHAT A "RANDOM TRIAL" IS HERE
// ===========================================================================
// docs/sync-architecture.md section 6.2 asks for the four properties to hold
// "for ANY random interleaving of operations across N simulated replicas". A
// trial therefore is not "apply random operations to one object"; it is:
//
//  1. GENERATE. N replicas take turns performing local edits and receiving
//     each other's operations, in a random schedule. Delivery during
//     generation is causally consistent, because that is the environment the
//     algorithm is specified for. What each replica had already applied when
//     it generated an operation becomes that operation's dependency set, and
//     what it could SEE at that moment becomes an InsertWitness.
//  2. REPLAY. The log is delivered to several FRESH replicas in several
//     different random *topological* orders of that dependency DAG — i.e.
//     different valid causal orders — and once in a deliberately NON-causal
//     shuffle, which exercises the bounded buffer.
//  3. CHECK. Every replay must agree with every other (P1), with a double
//     delivery of the same log (P2), with the independent reference model, and
//     with the strong list specification derived from the witnesses.
//
// Randomness is a seeded splitmix64 rather than <random>: the <random> engines
// are not required to produce identical sequences across standard libraries,
// so a failing seed from CI would not reproduce on a developer's machine. A
// spike whose counterexamples cannot be replayed is not worth much.

#ifndef UAPP_TESTS_SYNC_SPIKE_PROPERTY_HARNESS_H
#define UAPP_TESTS_SYNC_SPIKE_PROPERTY_HARNESS_H

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "uapp/sync/spike/fugue.h"
#include "uapp/sync/spike/reference_model.h"

namespace uapp::spike_test {

using uapp::sync::spike::ElementId;
using uapp::sync::spike::FugueReplica;
using uapp::sync::spike::InsertWitness;
using uapp::sync::spike::is_ok;
using uapp::sync::spike::OpKind;
using uapp::sync::spike::Operation;
using uapp::sync::spike::ReplicaId;
using uapp::sync::spike::SpikeStatus;
using uapp::sync::spike::spike_status_code;
using uapp::sync::spike::Variant;

/// splitmix64. Fixed, portable, reproducible from a seed.
class Prng final {
public:
    explicit Prng(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = state_;
        z               = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z               = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    /// Uniform-enough in [0, bound). Modulo bias is irrelevant at these bounds,
    /// and a rejection loop would only make a failing seed harder to reason
    /// about.
    std::size_t below(std::size_t bound) {
        return bound == 0 ? 0 : static_cast<std::size_t>(next() % bound);
    }

private:
    std::uint64_t state_;
};

/// One generated concurrent-editing history.
struct History final {
    /// The global operation log, in generation order.
    std::vector<Operation> ops;
    /// deps[k] = indices of operations the generator of ops[k] had already
    /// applied. Every entry is < k, so the DAG is acyclic by construction.
    std::vector<std::vector<std::uint32_t>> deps;
    /// One witness per insert, for the strong-list-specification oracle.
    std::vector<InsertWitness> witnesses;
    std::uint64_t              seed = 0;
};

struct GenConfig final {
    std::size_t replicas = 3;
    std::size_t steps    = 28;
    Variant     variant  = Variant::Fugue;
    /// Percent chance that a local edit is a delete rather than an insert.
    ///
    /// WHY THIS IS A KNOB AND NOT A CONSTANT: mutation testing (see
    /// docs/spikes/fugue-correctness-spike.md) found that a bug in the
    /// TOMBSTONE-INCLUSIVE right-origin rule of Algorithm 1 line 24 needs a
    /// tombstone sitting in a particular place to become visible, and at an
    /// 18% delete rate the suite needed on the order of a thousand trials to
    /// hit it. Raising the rate turns that into tens of trials. A generator
    /// that under-produces tombstones is a generator that under-tests the one
    /// part of the algorithm tombstones are load-bearing for.
    std::size_t delete_percent = 18;
    /// When true, local inserts are biased towards position 0, which is the
    /// shape that produces the deep LEFT-child chains where backward
    /// interleaving lives. Uniform random insertion positions essentially
    /// never generate it (SP-6 R-2), so the bias is not cosmetic.
    bool        prepend_bias = false;
};

/// Record one operation generated locally by replica `r` into the history.
/// Shared by generate_history() and Session so both produce logs the same
/// oracles can read.
class HistoryBuilder final {
public:
    HistoryBuilder(std::size_t replica_count) : applied_(replica_count) {}

    void record(std::size_t r, const Operation& op,
                const std::vector<ElementId>& visible_before, std::size_t at) {
        const std::size_t index = history_.ops.size();

        std::vector<std::uint32_t> dep;
        for (std::uint32_t k = 0; k < applied_[r].size(); ++k) {
            if (applied_[r][k]) { dep.push_back(k); }
        }
        history_.ops.push_back(op);
        history_.deps.push_back(std::move(dep));
        for (std::vector<bool>& seen : applied_) { seen.resize(history_.ops.size(), false); }
        applied_[r][index] = true;

        if (op.kind == OpKind::Insert) {
            InsertWitness w;
            w.inserted = op.id;
            w.before.assign(visible_before.begin(),
                            visible_before.begin() + static_cast<std::ptrdiff_t>(at));
            w.after.assign(visible_before.begin() + static_cast<std::ptrdiff_t>(at),
                           visible_before.end());
            history_.witnesses.push_back(std::move(w));
        }
    }

    [[nodiscard]] bool applied(std::size_t r, std::size_t k) const { return applied_[r][k]; }
    void               mark_applied(std::size_t r, std::size_t k) { applied_[r][k] = true; }
    [[nodiscard]] History&       history() noexcept { return history_; }
    [[nodiscard]] const History& history() const noexcept { return history_; }

private:
    History                        history_;
    std::vector<std::vector<bool>> applied_;
};

/// Generate a history by simulating `config.replicas` replicas exchanging
/// operations under causally consistent delivery.
inline History generate_history(const GenConfig& config, std::uint64_t seed) {
    Prng            rng(seed);
    HistoryBuilder  builder(config.replicas);

    std::vector<FugueReplica> replicas;
    replicas.reserve(config.replicas);
    for (std::size_t r = 0; r < config.replicas; ++r) {
        replicas.emplace_back(static_cast<ReplicaId>(r + 1u), config.variant);
    }

    for (std::size_t step = 0; step < config.steps; ++step) {
        const std::size_t r = rng.below(config.replicas);

        // What replica r could legally be handed right now, i.e. operations
        // all of whose causal dependencies it already holds.
        std::vector<std::uint32_t> deliverable;
        const History&             h = builder.history();
        for (std::uint32_t k = 0; k < h.ops.size(); ++k) {
            if (builder.applied(r, k)) { continue; }
            bool ready = true;
            for (const std::uint32_t d : h.deps[k]) {
                if (!builder.applied(r, d)) {
                    ready = false;
                    break;
                }
            }
            if (ready) { deliverable.push_back(k); }
        }

        if (deliverable.empty() || rng.below(100) < 55) {
            const std::size_t visible = replicas[r].visible_size();
            Operation         op;
            if (visible > 0 && rng.below(100) < config.delete_percent) {
                const std::size_t at = rng.below(visible);
                const std::vector<ElementId> before = replicas[r].visible_ids();
                if (!is_ok(replicas[r].erase(at, op))) { continue; }
                builder.record(r, op, before, at);
            } else {
                // Prepend bias: half the inserts go to index 0 regardless of
                // document length, which builds text right to left.
                const std::size_t at = (config.prepend_bias && rng.below(100) < 60)
                                           ? 0u
                                           : rng.below(visible + 1u);
                const char value = static_cast<char>('a' + static_cast<int>(rng.below(26)));
                const std::vector<ElementId> before = replicas[r].visible_ids();
                if (!is_ok(replicas[r].insert(at, value, op))) { continue; }
                builder.record(r, op, before, at);
            }
        } else {
            const std::uint32_t k = deliverable[rng.below(deliverable.size())];
            if (!is_ok(replicas[r].apply(builder.history().ops[k]))) { continue; }
            builder.mark_applied(r, k);
        }
    }

    History out  = std::move(builder.history());
    out.seed     = seed;
    return out;
}

/// A random topological order of the dependency DAG, i.e. one valid causal
/// delivery order.
inline std::vector<std::uint32_t> random_causal_order(const History& history, Prng& rng) {
    const std::size_t                       n = history.ops.size();
    std::vector<std::size_t>                remaining(n, 0);
    std::vector<std::vector<std::uint32_t>> enables(n);
    for (std::size_t k = 0; k < n; ++k) {
        remaining[k] = history.deps[k].size();
        for (const std::uint32_t d : history.deps[k]) {
            enables[d].push_back(static_cast<std::uint32_t>(k));
        }
    }
    std::vector<std::uint32_t> ready;
    for (std::size_t k = 0; k < n; ++k) {
        if (remaining[k] == 0) { ready.push_back(static_cast<std::uint32_t>(k)); }
    }
    std::vector<std::uint32_t> order;
    order.reserve(n);
    while (!ready.empty()) {
        const std::size_t   pick = rng.below(ready.size());
        const std::uint32_t k    = ready[pick];
        ready[pick]              = ready.back();
        ready.pop_back();
        order.push_back(k);
        for (const std::uint32_t next : enables[k]) {
            if (--remaining[next] == 0) { ready.push_back(next); }
        }
    }
    return order;
}

/// A delivery order that ignores causality entirely. A correct implementation
/// must still converge, by buffering (docs/sync-architecture.md section 6.2).
inline std::vector<std::uint32_t> random_shuffle_order(std::size_t n, Prng& rng) {
    std::vector<std::uint32_t> order(n);
    for (std::size_t i = 0; i < n; ++i) { order[i] = static_cast<std::uint32_t>(i); }
    for (std::size_t i = n; i > 1; --i) {
        const std::size_t   j   = rng.below(i);
        const std::uint32_t tmp = order[i - 1u];
        order[i - 1u]           = order[j];
        order[j]                = tmp;
    }
    return order;
}

struct ReplayResult final {
    std::string            text;
    std::vector<ElementId> order;
    std::size_t            pending = 0;
    SpikeStatus            status  = SpikeStatus::Ok;
};

/// Deliver `history.ops` to a fresh replica in `delivery` order. `repeats`
/// greater than 1 delivers the whole log that many times, which is the
/// idempotence probe (P2).
inline ReplayResult replay(const History& history, const std::vector<std::uint32_t>& delivery,
                           Variant variant, ReplicaId observer, std::size_t repeats = 1) {
    FugueReplica rep(observer, variant);
    ReplayResult out;
    for (std::size_t pass = 0; pass < repeats; ++pass) {
        for (const std::uint32_t k : delivery) {
            const SpikeStatus st = rep.apply(history.ops[k]);
            if (!is_ok(st)) {
                out.status = st;
                return out;
            }
        }
    }
    out.text    = rep.text();
    out.order   = rep.traversal_ids();
    out.pending = rep.pending_count();
    return out;
}

// ---------------------------------------------------------------------------
// Scripted scenarios, for the non-interleaving property.
//
// P4 cannot be reached reliably by random operation generation (SP-6 R-2:
// "random operation interleaving will almost never produce the shape that
// triggers it"), so these build the shape explicitly: replicas that fork from
// a shared base and each write a KNOWN CONTIGUOUS TOKEN at the SAME position,
// one of them built right to left.
// ---------------------------------------------------------------------------

/// Several replicas with an explicit, hand-driven delivery schedule.
class Session final {
public:
    Session(Variant variant, std::size_t replica_count)
        : variant_(variant), builder_(replica_count) {
        replicas_.reserve(replica_count);
        for (std::size_t r = 0; r < replica_count; ++r) {
            replicas_.emplace_back(static_cast<ReplicaId>(r + 1u), variant);
        }
    }

    [[nodiscard]] FugueReplica& replica(std::size_t r) { return replicas_[r]; }

    /// Type `s` left to right starting at visible index `at`.
    [[nodiscard]] SpikeStatus type_forward(std::size_t r, std::size_t at, const std::string& s) {
        for (std::size_t i = 0; i < s.size(); ++i) {
            const SpikeStatus st = local_insert(r, at + i, s[i]);
            if (!is_ok(st)) { return st; }
        }
        return SpikeStatus::Ok;
    }

    /// Type `s` RIGHT TO LEFT at visible index `at`: the last character first,
    /// then each preceding character prepended in front of it. The final text
    /// reads `s`, but every character except the last was inserted *before* an
    /// element that already existed — the backward-insertion shape of SP-6 F-1
    /// and of the paper's Figure 2.
    [[nodiscard]] SpikeStatus type_backward(std::size_t r, std::size_t at, const std::string& s) {
        for (std::size_t i = s.size(); i > 0; --i) {
            const SpikeStatus st = local_insert(r, at, s[i - 1u]);
            if (!is_ok(st)) { return st; }
        }
        return SpikeStatus::Ok;
    }

    [[nodiscard]] SpikeStatus local_insert(std::size_t r, std::size_t at, char value) {
        const std::vector<ElementId> before = replicas_[r].visible_ids();
        Operation                    op;
        const SpikeStatus            st = replicas_[r].insert(at, value, op);
        if (!is_ok(st)) { return st; }
        builder_.record(r, op, before, at);
        return SpikeStatus::Ok;
    }

    [[nodiscard]] SpikeStatus local_erase(std::size_t r, std::size_t at) {
        const std::vector<ElementId> before = replicas_[r].visible_ids();
        Operation                    op;
        const SpikeStatus            st = replicas_[r].erase(at, op);
        if (!is_ok(st)) { return st; }
        builder_.record(r, op, before, at);
        return SpikeStatus::Ok;
    }

    /// Deliver exactly one operation, by its index in the global log. Needed
    /// for scenarios where a replica must be kept IGNORANT of an operation —
    /// the paper's Figures 6 and 7 both turn on a replica having seen A and C
    /// but not B.
    [[nodiscard]] SpikeStatus deliver(std::size_t r, std::size_t op_index) {
        const History& h = builder_.history();
        if (op_index >= h.ops.size()) { return SpikeStatus::IndexOutOfRange; }
        if (builder_.applied(r, op_index)) { return SpikeStatus::Ok; }
        const SpikeStatus st = replicas_[r].apply(h.ops[op_index]);
        if (!is_ok(st)) { return st; }
        builder_.mark_applied(r, op_index);
        return SpikeStatus::Ok;
    }

    [[nodiscard]] std::size_t op_count() const noexcept { return builder_.history().ops.size(); }

    /// Deliver everything replica `r` has not seen yet, oldest first (a
    /// causally consistent schedule).
    [[nodiscard]] SpikeStatus sync_to(std::size_t r) {
        const History& h = builder_.history();
        for (std::size_t k = 0; k < h.ops.size(); ++k) {
            if (builder_.applied(r, k)) { continue; }
            const SpikeStatus st = replicas_[r].apply(h.ops[k]);
            if (!is_ok(st)) { return st; }
            builder_.mark_applied(r, k);
        }
        return SpikeStatus::Ok;
    }

    [[nodiscard]] SpikeStatus sync_all() {
        for (std::size_t r = 0; r < replicas_.size(); ++r) {
            const SpikeStatus st = sync_to(r);
            if (!is_ok(st)) { return st; }
        }
        return SpikeStatus::Ok;
    }

    [[nodiscard]] const History& history() const noexcept { return builder_.history(); }
    [[nodiscard]] const std::vector<Operation>& ops() const noexcept {
        return builder_.history().ops;
    }
    [[nodiscard]] Variant variant() const noexcept { return variant_; }

private:
    Variant                   variant_;
    HistoryBuilder            builder_;
    std::vector<FugueReplica> replicas_;
};

/// Read the random-trial count from the environment, so a long soak needs no
/// rebuild: UAPP_FUGUE_TRIALS=200000 ./uapp_sync_spike_tests
inline std::size_t trial_count(std::size_t fallback) {
    const char* raw = std::getenv("UAPP_FUGUE_TRIALS");
    if (raw == nullptr) { return fallback; }
    const long parsed = std::strtol(raw, nullptr, 10);
    if (parsed <= 0) { return fallback; }
    return static_cast<std::size_t>(parsed);
}

}  // namespace uapp::spike_test

#endif  // UAPP_TESTS_SYNC_SPIKE_PROPERTY_HARNESS_H
