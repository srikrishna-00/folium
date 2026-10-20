// SPDX-License-Identifier: Apache-2.0
#include "uapp/ai/registry.h"

namespace uapp::ai {
namespace {

/// Exactly one bit set.
[[nodiscard]] bool is_single_capability(Capability c) noexcept {
    const CapabilityMask m = cap_bit(c);
    return m != 0u && (m & (m - 1u)) == 0u;
}

[[nodiscard]] bool capability_is_known(Capability c) noexcept {
    for (const Capability known : kAllCapabilities) {
        if (known == c) { return true; }
    }
    return false;
}

}  // namespace

bool is_failover_eligible(Status s) noexcept {
    // A CLOSED, EXPLICIT allow-list rather than a deny-list. A new status code
    // must be considered deliberately before it can cause a second egress of
    // the user's content; defaulting to "retry" is the direction that leaks.
    switch (s) {
        case Status::AiProviderUnavailable:
        case Status::AiCapabilityNotImplemented:
            return true;
        default:
            return false;
    }
}

Status ProviderRegistry::add(AIProvider* provider) {
    if (provider == nullptr) {
        return Status::InvalidArgument;
    }
    if (entries_.size() >= kMaxRegisteredProviders) {
        // Status::InvalidArgument rather than a dedicated code, deliberately.
        // A build with 33 providers is a composition-root bug, not a user
        // condition and not something a UI can explain; the 12xx block's own
        // rule is that a code earns its existence by being actionable.
        return Status::InvalidArgument;
    }
    const ProviderDescriptor& d = provider->descriptor();
    if (const Status s = validate_descriptor(d); !is_ok(s)) {
        return s;
    }
    for (const AIProvider* existing : entries_) {
        if (existing->descriptor().id == d.id) {
            return Status::AiProviderDuplicated;
        }
    }
    entries_.push_back(provider);
    return Status::Ok;
}

Status ProviderRegistry::remove(std::string_view provider_id) noexcept {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i]->descriptor().id == provider_id) {
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
            return Status::Ok;
        }
    }
    return Status::AiProviderNotRegistered;
}

Status ProviderRegistry::find(std::string_view provider_id,
                              AIProvider** out) const noexcept {
    if (out == nullptr) { return Status::InvalidArgument; }
    *out = nullptr;
    for (AIProvider* p : entries_) {
        if (p->descriptor().id == provider_id) {
            *out = p;
            return Status::Ok;
        }
    }
    return Status::AiProviderNotRegistered;
}

Status ProviderRegistry::candidates(const Selection& sel,
                                    std::vector<AIProvider*>& out) const {
    out.clear();
    if (!is_single_capability(sel.required) || !capability_is_known(sel.required)) {
        return Status::InvalidArgument;
    }

    // Pass 1: eligibility. Three filters, and the locality one is a CEILING
    // (registry.h explains why that is the policy filter rather than a
    // preference).
    for (AIProvider* p : entries_) {
        const ProviderDescriptor& d = p->descriptor();
        if (!has_capability(d.capabilities, sel.required)) { continue; }
        if (static_cast<std::uint8_t>(d.locality) >
            static_cast<std::uint8_t>(sel.max_locality)) {
            continue;
        }
        const Health h = p->health();
        if (h == Health::Unavailable) { continue; }
        if (h == Health::Degraded && !sel.allow_degraded) { continue; }
        out.push_back(p);
    }

    // Pass 2: stable insertion sort on (not-pinned, locality, health). Stable
    // so registration order is the final tiebreak, which is what makes
    // selection explainable and reproducible. At most
    // kMaxRegisteredProviders entries, so O(n^2) is irrelevant and the
    // simplicity is worth more than the asymptotics.
    const auto rank = [&sel](const AIProvider* p) -> std::uint32_t {
        const ProviderDescriptor& d = p->descriptor();
        const std::uint32_t pinned =
            (!sel.pinned_provider_id.empty() && d.id == sel.pinned_provider_id) ? 0u : 1u;
        const auto loc    = static_cast<std::uint32_t>(d.locality);
        const auto health = static_cast<std::uint32_t>(p->health());
        return (pinned << 16) | (loc << 8) | health;
    };
    for (std::size_t i = 1; i < out.size(); ++i) {
        AIProvider* const  key      = out[i];
        const std::uint32_t key_rank = rank(key);
        std::size_t j = i;
        while (j > 0 && rank(out[j - 1]) > key_rank) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = key;
    }
    return Status::Ok;
}

Status ProviderRegistry::select(const Selection& sel, AIProvider** out) const {
    if (out == nullptr) { return Status::InvalidArgument; }
    *out = nullptr;

    // A pinned id is checked against the WHOLE registry first, so that
    // "the model you pinned is gone" and "the model you pinned cannot do this"
    // are distinguishable from "nothing is eligible". ai-architecture.md §1
    // rule 5 forbids silently substituting another provider in either case.
    if (!sel.pinned_provider_id.empty()) {
        AIProvider* pinned = nullptr;
        if (const Status s = find(sel.pinned_provider_id, &pinned); !is_ok(s)) {
            return s;  // AiProviderNotRegistered
        }
        const ProviderDescriptor& d = pinned->descriptor();
        if (!has_capability(d.capabilities, sel.required)) {
            return Status::AiCapabilityNotImplemented;
        }
        if (static_cast<std::uint8_t>(d.locality) >
            static_cast<std::uint8_t>(sel.max_locality)) {
            // The user pinned a cloud model and the workspace policy forbids
            // egress. Fails closed and says which rule won.
            return Status::AiPolicyBlocked;
        }
        if (pinned->health() == Health::Unavailable) {
            return Status::AiProviderUnavailable;
        }
        *out = pinned;
        return Status::Ok;
    }

    std::vector<AIProvider*> ranked;
    if (const Status s = candidates(sel, ranked); !is_ok(s)) { return s; }
    if (ranked.empty()) { return Status::AiNoEligibleProvider; }
    *out = ranked.front();
    return Status::Ok;
}

Status ProviderRegistry::summarize_with_failover(const Selection& sel,
                                                 const SummarizeRequest& req,
                                                 SummarizeResult& out,
                                                 std::string& chosen_provider_id) const {
    chosen_provider_id.clear();
    out = SummarizeResult{};

    std::vector<AIProvider*> ranked;
    if (const Status s = candidates(sel, ranked); !is_ok(s)) { return s; }
    if (ranked.empty()) { return Status::AiNoEligibleProvider; }

    Status last = Status::AiNoEligibleProvider;
    for (AIProvider* p : ranked) {
        last = p->summarize(req, out);
        if (is_ok(last)) {
            chosen_provider_id = p->descriptor().id;
            return Status::Ok;
        }
        if (!is_failover_eligible(last)) {
            // Stop and report. The request itself is the problem, or the
            // provider refused the content, and trying the next candidate
            // would be a second egress of the same bytes for the same answer.
            return last;
        }
    }
    return last;
}

}  // namespace uapp::ai
