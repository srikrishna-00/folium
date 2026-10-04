// SPDX-License-Identifier: Apache-2.0
//
// core/sync/spike/reference_model.h — the independent oracles.
//
// SPIKE CODE — NOT PRODUCTION. See element.h for the scope statement.
//
// ===========================================================================
// WHY THIS FILE EXISTS
// ===========================================================================
// docs/adr/0002 (Amendment) requires non-interleaving to be "verified via
// reference-model differential testing against a known-correct sequence CRDT
// implementation, NOT JUST SELF-CONSISTENCY CHECKS", and SP-6 R-3 asks
// specifically for a naive O(n^2) model "written to read like the published
// algorithm and optimised for nothing", with the real implementation required
// to produce byte-identical documents to it.
//
// The reason that requirement is not pedantry is F-1 of SP-6: convergence,
// idempotence and commutativity all PASS on an interleaved document. A test
// suite that only compares replicas against each other proves the replicas
// agree; it says nothing about whether what they agree on is right. So this
// file provides three oracles that do not share a line of code with
// fugue.cpp:
//
//  1. `reference_order()` — the Fugue order computed by a completely
//     different route. fugue.cpp maintains sorted sibling lists INCREMENTALLY
//     as operations arrive and derives the order by walking the tree.
//     This model takes the whole operation set at once, computes each
//     element's root path, and derives the order from an O(n^2) pairwise
//     comparator with no traversal and no incremental maintenance anywhere.
//     The two agree only if the order itself is right, so a mistake in
//     sibling-insertion indices — historically where hand-written CRDTs break
//     — shows up as a mismatch.
//
//     It also CHECKS TOTALITY as a side effect: the comparator is used to
//     compute each element's rank as "how many elements compare less than
//     me", and the ranks must come out as a permutation of 0..n-1. If the
//     comparator is not a strict total order the ranks collide and the model
//     says so rather than silently producing one of several answers. (That is
//     also why the ranks are computed by brute force instead of handing the
//     comparator to std::sort, where an inconsistent comparator is undefined
//     behaviour rather than a reported failure.)
//
//  2. `satisfies_strong_list_spec()` — the only oracle here that is
//     independent of FUGUE ENTIRELY. It checks the merged order against the
//     Attiya et al. strong list specification, derived from what each replica
//     actually saw when it generated each insert: if a replica inserted at
//     index i, every element then at an index below i must precede the new
//     element in the final merged order, and every element then at index >= i
//     must follow it. This is condition (b) of the paper's Theorem 1, checked
//     from the operation history instead of proved from the algorithm. A
//     version of the algorithm that converged on a wrong order would fail
//     here even if the reference model were wrong in the same way.
//
//  3. `maximal_run_count()` — the non-interleaving detector. Non-interleaving
//     is a property of the merged text, not of the tree: each replica's
//     contribution must appear as ONE contiguous run. The tests prove this
//     detector has teeth by running it against rga.h, which must report more
//     than one run on the backward-insertion scenario.

#ifndef UAPP_CORE_SYNC_SPIKE_REFERENCE_MODEL_H
#define UAPP_CORE_SYNC_SPIKE_REFERENCE_MODEL_H

#include <cstddef>
#include <string>
#include <vector>

#include "uapp/sync/spike/element.h"

namespace uapp::sync::spike {

/// What one replica could see when it generated one insert. Recorded by the
/// test harness at generation time; it is the raw material of the strong-list-
/// specification check and cannot be reconstructed afterwards from the
/// operation log alone.
struct InsertWitness final {
    ElementId              inserted;
    /// Visible elements at indices [0, i) on the generating replica.
    std::vector<ElementId> before;
    /// Visible elements at indices [i, n) on the generating replica.
    std::vector<ElementId> after;
};

struct ReferenceOrder final {
    /// Visible document, tombstones excluded.
    std::string            text;
    /// Every element in order, tombstones INCLUDED.
    std::vector<ElementId> order;
    /// False if the model could not produce an answer it is willing to stand
    /// behind: an unresolvable parent, a cycle, or a comparator that did not
    /// come out a total order. `diagnostic` then says which.
    bool                   ok = false;
    std::string            diagnostic;
};

/// The Fugue (or FugueMax) order of `ops`, computed independently of
/// FugueReplica. `ops` may be in any order and may contain duplicates; only
/// the SET matters, which is the point of a CRDT.
///
/// Intentionally O(n^2) in the element count and unoptimised, per SP-6 R-3.
/// Suitable for the small cases differential testing needs, not for a document.
[[nodiscard]] ReferenceOrder reference_order(const std::vector<Operation>& ops, Variant variant);

/// True iff `order` is a linear extension of every constraint in `witnesses`.
/// `order` must be the tombstone-INCLUSIVE order, because a witness may name
/// an element that was deleted later.
/// On false, `why` describes the first violated constraint.
[[nodiscard]] bool satisfies_strong_list_spec(const std::vector<InsertWitness>& witnesses,
                                              const std::vector<ElementId>&     order,
                                              std::string&                      why);

/// Number of maximal runs of characters drawn from `charset` in `text`.
/// One contiguous insertion yields 1; a character-by-character interleaving of
/// two n-character tokens yields n.
[[nodiscard]] std::size_t maximal_run_count(const std::string& text, const std::string& charset);

}  // namespace uapp::sync::spike

#endif  // UAPP_CORE_SYNC_SPIKE_REFERENCE_MODEL_H
