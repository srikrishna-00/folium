// SPDX-License-Identifier: Apache-2.0
//
// ai/provider/registry.h — "multiple providers / user-selectable models"
// (spec §17), and the failover rules of ai-architecture.md §5.
//
// ---------------------------------------------------------------------------
// WHAT THE REGISTRY IS
// ---------------------------------------------------------------------------
// A list of registered providers plus one deterministic, explainable selection
// function over it. Deterministic matters: ai-architecture.md §5 requires the
// UI to be able to show *why* a model was used, which it cannot do if
// selection depends on a hash-map iteration order. So this holds a vector in
// registration order and the ranking is a total order with registration order
// as the final tiebreak.
//
// It does NOT own providers. Registration takes a raw pointer whose lifetime
// the caller guarantees, for the reason core/database holds no SQLite handle:
// a provider's lifetime is a composition-root decision (it may be a static, a
// member of an app object, or a lazily-loaded model), and a registry that
// forced `unique_ptr` would make the lifetime policy for every future adapter
// at a point where there is exactly one adapter.
//
// ---------------------------------------------------------------------------
// WHAT IT IS NOT: the §2 ROUTER
// ---------------------------------------------------------------------------
// ai-architecture.md §2 draws a ROUTER that does "capability match -> policy
// filter -> cost/latency -> health", with per-provider circuit breakers and
// jittered retry. This is the capability-match and health half, plus an
// explicit locality ceiling. NOT IMPLEMENTED here: circuit breakers,
// p95-latency tracking, cost ranking, retry/backoff. All four need either
// observed call history or a cost model, and inventing either with one
// deterministic local provider in the tree would be fiction. The seam they
// attach to is `Selection` plus `Health`.

#ifndef UAPP_AI_PROVIDER_REGISTRY_H
#define UAPP_AI_PROVIDER_REGISTRY_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "uapp/ai/ai_types.h"
#include "uapp/ai/provider.h"
#include "uapp/status.h"

namespace uapp::ai {

/// Maximum providers one registry holds.
///
/// WHY 32: a provider list is a user-facing settings screen ("which model"),
/// and selection is a linear scan run on every AI call. 32 is far above the
/// handful a shipped build will have (one local + the AIQ-1 cloud set) and
/// keeps the scan trivially bounded.
inline constexpr std::size_t kMaxRegisteredProviders = 32;

/// What a caller wants, as data, so selection is testable without a call.
struct Selection final {
    /// The capability the call needs. Exactly one bit; a selection asking for
    /// two is `Status::InvalidArgument`, because "a provider that can do both"
    /// and "a provider for each" are different requests and silently picking
    /// the first reading would hide the ambiguity.
    Capability required = Capability::Summarize;

    /// The furthest content may travel. **This is the policy filter, and it
    /// is a CEILING, not a preference.** Under `AiPolicy::LocalOnly` a caller
    /// passes `Locality::OnDevice` and an off-device provider is not merely
    /// deprioritised, it is ineligible — ai-architecture.md §5's "no
    /// cross-provider fallback across a locality boundary" and H-29's
    /// fail-closed rule are both expressed by this one field.
    Locality max_locality = Locality::OnDevice;

    /// A provider the user pinned in settings. If set and eligible it wins
    /// outright (§5's first ranking key). If set and NOT eligible, selection
    /// FAILS rather than quietly substituting another provider — §1 rule 5,
    /// "never silently substitute a weaker model".
    std::string pinned_provider_id;

    /// Include `Health::Degraded` providers. Default true: degraded means
    /// slow, not wrong, and excluding it by default would turn a slow local
    /// model into `AiNoEligibleProvider`.
    bool allow_degraded = true;
};

/// True iff a failed call MAY be retried against the next candidate provider.
///
/// This encodes two of ai-architecture.md §5's hard rules as data rather than
/// as review comments:
///
///   * "Never retry a content-policy refusal" — `AiContentPolicyRejected` is
///     false. Re-sending refused content to another provider is both futile
///     and a second egress of the same bytes.
///   * Consent and input failures are false. They are properties of the
///     REQUEST, so every candidate would fail identically; retrying only
///     multiplies the audit records.
///   * `AiRateLimited` is false. Failing over on a 429 is how a client turns
///     its own throttling into a thundering herd across providers. Backoff is
///     the right answer and is NOT IMPLEMENTED.
///
/// Note what IS eligible: `AiProviderUnavailable` and
/// `AiCapabilityNotImplemented`. Those two are properties of the PROVIDER, so
/// the next candidate can genuinely differ. Crossing a locality boundary is
/// prevented by `Selection::max_locality`, not by this predicate — the
/// candidate list never contains a provider past the ceiling.
[[nodiscard]] bool is_failover_eligible(Status s) noexcept;

class ProviderRegistry final {
public:
    ProviderRegistry() = default;

    ProviderRegistry(const ProviderRegistry&)            = delete;
    ProviderRegistry& operator=(const ProviderRegistry&) = delete;

    /// Register a provider. The pointer must outlive the registry.
    ///
    /// Validates the descriptor (`validate_descriptor`), so a provider that
    /// claims `Embed` with `embed_dim == 0`, or an off-device provider with no
    /// `data_policy_url`, cannot be registered at all — the earliest possible
    /// point to catch it.
    [[nodiscard]] Status add(AIProvider* provider);

    /// Remove by id. `Status::AiProviderNotRegistered` if absent.
    [[nodiscard]] Status remove(std::string_view provider_id) noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /// Look a provider up by exact id.
    [[nodiscard]] Status find(std::string_view provider_id, AIProvider** out) const noexcept;

    /// Every provider satisfying `sel`, best first. Empty is not an error
    /// here; `select()` is where emptiness becomes a Status.
    ///
    /// Ranking, in order (ai-architecture.md §5, minus the parts that need
    /// observed history):
    ///   1. the pinned provider, if eligible
    ///   2. locality ascending — on-device first, always
    ///   3. `Health::Healthy` before `Health::Degraded`
    ///   4. registration order
    [[nodiscard]] Status candidates(const Selection& sel,
                                    std::vector<AIProvider*>& out) const;

    /// The single best candidate, or `Status::AiNoEligibleProvider`.
    /// Returns `Status::AiProviderNotRegistered` when a pinned id is unknown,
    /// and `Status::AiCapabilityNotImplemented` when the pinned provider
    /// exists but cannot do the job — distinct codes, because "you pinned a
    /// model that is gone" and "you pinned a model that cannot summarise" need
    /// different things said to the user.
    [[nodiscard]] Status select(const Selection& sel, AIProvider** out) const;

    /// Run `summarize` against candidates in order, stopping at the first
    /// success or the first non-failover-eligible failure.
    ///
    /// WHY ONLY SUMMARIZE HAS A HELPER: the failover rule is one rule, and one
    /// concrete use of it is enough to compile, test and pin the behaviour.
    /// Seven near-identical helpers would be six copies of the same loop; a
    /// template over the seven method pointers would be the right shape once a
    /// second caller exists. `is_failover_eligible()` is public precisely so a
    /// caller can write the loop for another capability today.
    /// `chosen_provider_id` reports who answered, so a UI can say so.
    [[nodiscard]] Status summarize_with_failover(const Selection& sel,
                                                 const SummarizeRequest& req,
                                                 SummarizeResult& out,
                                                 std::string& chosen_provider_id) const;

private:
    std::vector<AIProvider*> entries_;
};

}  // namespace uapp::ai

#endif  // UAPP_AI_PROVIDER_REGISTRY_H
