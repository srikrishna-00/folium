// SPDX-License-Identifier: Apache-2.0
//
// core/sync/spike/element.h — element identity and the wire operation.
//
// ===========================================================================
// THIS DIRECTORY IS A SPIKE. IT IS NOT PART OF THE SHIPPING CORE.
// ===========================================================================
// Nothing here is linked by uapp::core_*, uapp::storage_sqlite, uapp::capi or
// any product target. It exists to answer one question before Phase 13 writes
// merge code for real:
//
//     does the Fugue algorithm (Weidner & Kleppmann, 2023) actually avoid the
//     backward-interleaving defect that spike SP-6 found in classic RGA, and
//     can we implement it correctly?
//
// See docs/spikes/fugue-correctness-spike.md for the findings, and
// docs/adr/0002-phase0-signoff-decisions.md "Amendment (2026-09-29)" for why
// the algorithm choice changed from RGA to Fugue in the first place.
//
// Deliberate non-goals (do not "finish" these here): integration with
// core/blocks or any document model, network transport, encrypted framing,
// run-length/waypoint compression, tombstone GC, persistence. Every one of
// those is Phase 13 production work with its own design obligations in
// docs/sync-architecture.md section 6.
//
// ---------------------------------------------------------------------------
// WHY A SEPARATE STATUS AND ID TYPE RATHER THAN uapp::Status / uapp::ObjectId
// ---------------------------------------------------------------------------
// core/common/status.h is an append-only enum that travels into logs,
// telemetry and the stable C ABI, and its own header says "a code with no code
// path behind it is a promise this enum cannot keep". Adding 8xx sync codes
// for an experiment would put spike-only codes into the production error
// surface before the production design exists. The spike therefore carries its
// own `SpikeStatus` (see spike_status.h), in its own namespace, with the same
// shape — flat enum, code string, message string — so that promoting it later
// is a mechanical move rather than a redesign.
//
// Likewise, an element id here is 8 bytes of (replica, counter), not a 16-byte
// ObjectId: the whole point of a sequence CRDT's cost model is per-character
// metadata, and measuring it against the wrong id width would measure nothing.

#ifndef UAPP_CORE_SYNC_SPIKE_ELEMENT_H
#define UAPP_CORE_SYNC_SPIKE_ELEMENT_H

#include <cstddef>
#include <cstdint>

namespace uapp::sync::spike {

// --- hard limits -----------------------------------------------------------
// Shared by both algorithms in this directory, because both face the same
// problem: every operation is, in production, attacker-influenced data
// arriving from another device (spec section 38).

/// Hard cap on elements in one replicated sequence, tombstones included.
///
/// WHY A LIMIT: elements arrive from other devices, so the count is not under
/// local control. WHY THIS NUMBER: 2^20 characters is ~1 MiB of text, the same
/// ceiling core/blocks already puts on a single text payload
/// (kMaxBlockTextBytes). Tombstones push the node count above the visible
/// length, which is precisely the cost sync-architecture.md section 6.2 says GC
/// must eventually bound; until GC exists, this limit IS the bound.
inline constexpr std::size_t kMaxElements = 1u << 20;

/// Hard cap on operations held waiting for their causal dependencies.
///
/// A peer that sends inserts whose parents it never sends is, without a bound,
/// a free memory-exhaustion primitive (docs/sync-architecture.md section 6.2,
/// risk T-305). 4096 is far more than any real out-of-order window and small
/// enough that the worst case is tens of kilobytes.
inline constexpr std::size_t kMaxPendingOperations = 4096;

/// Replica (device) identifier. Value 0 is RESERVED: see kNullId.
using ReplicaId = std::uint32_t;

/// Element identifier, Algorithm 1 line 3: `ID := (RID x N) u {null}`.
///
/// The paper is explicit that "the exact construction of IDs and their order is
/// not important", only that there IS a total order agreed by every replica.
/// Lexicographic (replica, counter) is used here. That is a legitimate choice
/// because same-side siblings in a Fugue tree are always pairwise CONCURRENT —
/// a replica never creates a node where it already sees a same-side sibling
/// (paper section 4, "Insert") — so the tie-break never has to respect
/// causality.
struct ElementId final {
    ReplicaId     replica = 0;
    std::uint32_t counter = 0;
};

/// The `null` of Algorithm 1 line 3, i.e. the tree root as a parent reference
/// and the `end` sentinel as a right-origin reference. Replica id 0 is reserved
/// for it so that a real replica can never mint an id that collides with it.
inline constexpr ElementId kNullId{0u, 0u};

[[nodiscard]] inline constexpr bool is_null(ElementId id) noexcept {
    return id.replica == 0u;
}

[[nodiscard]] inline constexpr bool operator==(ElementId a, ElementId b) noexcept {
    return a.replica == b.replica && a.counter == b.counter;
}

[[nodiscard]] inline constexpr bool operator!=(ElementId a, ElementId b) noexcept {
    return !(a == b);
}

/// The total order on ids used to break ties between same-side siblings.
[[nodiscard]] inline constexpr bool operator<(ElementId a, ElementId b) noexcept {
    return a.replica != b.replica ? a.replica < b.replica : a.counter < b.counter;
}

/// Flattened id, for hash-map keys. Injective, so two distinct ids can never
/// collide into one slot.
[[nodiscard]] inline constexpr std::uint64_t element_key(ElementId id) noexcept {
    return (static_cast<std::uint64_t>(id.replica) << 32) |
           static_cast<std::uint64_t>(id.counter);
}

/// Which child list of its parent a node sits in (Algorithm 1 line 6).
enum class Side : std::uint8_t { Left = 0, Right = 1 };

enum class OpKind : std::uint8_t { Insert = 0, Delete = 1 };

/// Which of the paper's two algorithms to run.
///
/// `Fugue` is Algorithm 1 verbatim. `FugueMax` is Algorithm 1 with the
/// right-sibling reordering of the paper's Definition 6 (section 5.3).
///
/// They differ ONLY when one node has several right children with DIFFERENT
/// right origins, which needs several interacting concurrent edits; the paper
/// proves that in exactly those situations *some* interleaving is unavoidable
/// for any algorithm, and recommends plain Fugue in practice. Both are built
/// here because `adr/0002` claims "maximal non-interleaving", and that claim is
/// FugueMax's, not Fugue's — see the findings doc.
enum class Variant : std::uint8_t { Fugue = 0, FugueMax = 1 };

/// One broadcast operation. This is the whole wire format of the spike.
///
/// Fields used per kind:
///   Insert: id, parent, side, value, and (FugueMax, side==Right) right_origin
///   Delete: id
///
/// `right_origin` is recorded unconditionally by the generator even for plain
/// Fugue — it costs 8 bytes in a struct that is never persisted here, and
/// carrying it lets one generated history be replayed under BOTH variants,
/// which is what makes the Fugue/FugueMax comparison in the tests possible at
/// all. Production must not copy that: under plain Fugue the field is dead
/// weight on every character.
struct Operation final {
    OpKind    kind         = OpKind::Insert;
    ElementId id{};
    ElementId parent{};
    ElementId right_origin{};
    char      value        = '\0';
    Side      side         = Side::Right;
};

[[nodiscard]] inline bool same_operation(const Operation& a, const Operation& b) noexcept {
    return a.kind == b.kind && a.id == b.id;
}

}  // namespace uapp::sync::spike

#endif  // UAPP_CORE_SYNC_SPIKE_ELEMENT_H
