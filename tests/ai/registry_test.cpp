// SPDX-License-Identifier: Apache-2.0
//
// tests/ai/registry_test.cpp — "multiple providers / user-selectable models"
// (spec §17) and the failover rules of ai-architecture.md §5.
//
// The cases worth reading first:
//   * `ai.registry.locality_ceiling_is_not_a_preference` — the policy filter.
//     A LOCAL_ONLY selection must make an off-device provider INELIGIBLE, not
//     merely unpopular. This is H-29 expressed in the registry.
//   * `ai.registry.pinned_provider_failure_is_never_a_silent_substitution` —
//     §1 rule 5. Pinning a model that cannot do the job is an error with a
//     specific code, not a quiet downgrade.
//   * `ai.registry.content_policy_refusal_is_never_retried` — §5, and the
//     reason `is_failover_eligible` exists at all.

#include <string>
#include <vector>

#include "ai_test_doubles.h"
#include "test_framework.h"
#include "uapp/ai/local/local_heuristic_provider.h"
#include "uapp/ai/local/null_provider.h"
#include "uapp/ai/registry.h"

using uapp::Status;
using uapp::ai::AIProvider;
using uapp::ai::Capability;
using uapp::ai::Content;
using uapp::ai::Health;
using uapp::ai::is_failover_eligible;
using uapp::ai::Locality;
using uapp::ai::ProviderRegistry;
using uapp::ai::Role;
using uapp::ai::Selection;
using uapp::ai::SummarizeRequest;
using uapp::ai::SummarizeResult;
using uapp::ai::local::kLocalHeuristicProviderId;
using uapp::ai::local::LocalHeuristicProvider;
using uapp::ai::local::NullProvider;
using uapp::ai::test_doubles::FailingDouble;
using uapp::ai::test_doubles::OffDeviceDouble;

namespace {

const char* const kDoc =
    "The ledger reconciliation completed overnight. "
    "Three entries required manual adjustment before the close.";

Selection local_summarize() {
    Selection sel;
    sel.required     = Capability::Summarize;
    sel.max_locality = Locality::OnDevice;
    return sel;
}

}  // namespace

// --- registration ----------------------------------------------------------

UAPP_TEST("ai.registry.registration_and_lookup") {
    ProviderRegistry reg;
    LocalHeuristicProvider local;
    NullProvider           nothing("null:none");

    UAPP_CHECK(reg.size() == 0);
    UAPP_REQUIRE_OK(reg.add(&local));
    UAPP_REQUIRE_OK(reg.add(&nothing));
    UAPP_CHECK(reg.size() == 2);

    AIProvider* found = nullptr;
    UAPP_REQUIRE_OK(reg.find(kLocalHeuristicProviderId, &found));
    UAPP_CHECK(found == &local);

    UAPP_REQUIRE_STATUS(reg.find("nope:0", &found), Status::AiProviderNotRegistered);
    UAPP_CHECK(found == nullptr);

    // Same id twice is a composition-root bug: two providers answering to one
    // name would make a consent record ambiguous about which one got the data.
    LocalHeuristicProvider duplicate;
    UAPP_REQUIRE_STATUS(reg.add(&duplicate), Status::AiProviderDuplicated);

    UAPP_REQUIRE_STATUS(reg.add(nullptr), Status::InvalidArgument);

    UAPP_REQUIRE_OK(reg.remove("null:none"));
    UAPP_CHECK(reg.size() == 1);
    UAPP_REQUIRE_STATUS(reg.remove("null:none"), Status::AiProviderNotRegistered);
}

UAPP_TEST("ai.registry.refuses_a_provider_it_could_not_honour") {
    // An off-device provider with no retention policy cannot be consented to
    // (ai-architecture.md §13.2 step 5), so it must not be registerable. The
    // check is validate_descriptor's, and the registry is where it is
    // unavoidable.
    ProviderRegistry reg;

    // The double supplies a policy URL on purpose, so it registers fine...
    OffDeviceDouble ok_cloud("cloud-ok:v1");
    UAPP_REQUIRE_OK(reg.add(&ok_cloud));

    // ...and the descriptor the registry would have had to accept for one
    // without a policy URL is rejected at the gate `add()` runs.
    uapp::ai::ProviderDescriptor bad;
    bad.id           = "cloud-bad:v1";
    bad.display_name = "No retention policy";
    bad.locality     = Locality::ThirdParty;
    UAPP_REQUIRE_STATUS(uapp::ai::validate_descriptor(bad), Status::InvalidArgument);
}

// --- selection -------------------------------------------------------------

UAPP_TEST("ai.registry.selects_a_capable_provider_and_skips_an_incapable_one") {
    ProviderRegistry reg;
    NullProvider           nothing("null:none");
    LocalHeuristicProvider local;

    // Registered in the order that would select the WRONG one if capability
    // filtering did not happen.
    UAPP_REQUIRE_OK(reg.add(&nothing));
    UAPP_REQUIRE_OK(reg.add(&local));

    AIProvider* chosen = nullptr;
    UAPP_REQUIRE_OK(reg.select(local_summarize(), &chosen));
    UAPP_CHECK_MSG(chosen == &local,
                   "selected a provider that declares no capabilities");

    // Nothing in the registry can generate, so selection fails with a
    // specific code rather than returning the no-capability provider.
    Selection gen = local_summarize();
    gen.required  = Capability::Generate;
    UAPP_REQUIRE_STATUS(reg.select(gen, &chosen), Status::AiNoEligibleProvider);
    UAPP_CHECK(chosen == nullptr);
}

UAPP_TEST("ai.registry.locality_ceiling_is_not_a_preference") {
    ProviderRegistry reg;
    OffDeviceDouble cloud("cloud-double:v1");
    UAPP_REQUIRE_OK(reg.add(&cloud));

    // A LOCAL_ONLY-shaped selection with only a cloud provider available must
    // FAIL, not fall back. H-29: a device being short of options is not a
    // reason to weaken a policy the user set.
    AIProvider* chosen = nullptr;
    UAPP_REQUIRE_STATUS(reg.select(local_summarize(), &chosen),
                        Status::AiNoEligibleProvider);
    UAPP_CHECK(chosen == nullptr);

    // Raise the ceiling and the same provider becomes eligible.
    Selection cloud_ok  = local_summarize();
    cloud_ok.max_locality = Locality::ThirdParty;
    UAPP_REQUIRE_OK(reg.select(cloud_ok, &chosen));
    UAPP_CHECK(chosen == &cloud);

    // A first-party ceiling excludes a third party but not a first party.
    OffDeviceDouble gateway("gateway:v1", Locality::FirstPartyServer);
    UAPP_REQUIRE_OK(reg.add(&gateway));
    Selection first_party_only = local_summarize();
    first_party_only.max_locality = Locality::FirstPartyServer;
    UAPP_REQUIRE_OK(reg.select(first_party_only, &chosen));
    UAPP_CHECK(chosen == &gateway);
}

UAPP_TEST("ai.registry.ranks_on_device_first") {
    ProviderRegistry reg;
    OffDeviceDouble        cloud("cloud-double:v1");
    LocalHeuristicProvider local;

    // Cloud registered FIRST, so registration order alone would pick it.
    UAPP_REQUIRE_OK(reg.add(&cloud));
    UAPP_REQUIRE_OK(reg.add(&local));

    Selection sel      = local_summarize();
    sel.max_locality   = Locality::ThirdParty;

    std::vector<AIProvider*> ranked;
    UAPP_REQUIRE_OK(reg.candidates(sel, ranked));
    UAPP_REQUIRE(ranked.size() == 2);
    UAPP_CHECK_MSG(ranked[0] == &local,
                   "an off-device provider outranked an on-device one");

    AIProvider* chosen = nullptr;
    UAPP_REQUIRE_OK(reg.select(sel, &chosen));
    UAPP_CHECK(chosen == &local);
}

UAPP_TEST("ai.registry.skips_unavailable_and_optionally_degraded") {
    ProviderRegistry reg;
    FailingDouble down("down:v1", Status::AiProviderUnavailable, Health::Unavailable);
    FailingDouble slow("slow:v1", Status::AiProviderUnavailable, Health::Degraded);
    UAPP_REQUIRE_OK(reg.add(&down));
    UAPP_REQUIRE_OK(reg.add(&slow));

    std::vector<AIProvider*> ranked;
    UAPP_REQUIRE_OK(reg.candidates(local_summarize(), ranked));
    UAPP_REQUIRE_MSG(ranked.size() == 1, "got " + std::to_string(ranked.size()));
    UAPP_CHECK(ranked[0] == &slow);

    // Degraded means slow, not wrong, so it is included by default and can be
    // excluded explicitly.
    Selection strict      = local_summarize();
    strict.allow_degraded = false;
    UAPP_REQUIRE_OK(reg.candidates(strict, ranked));
    UAPP_CHECK(ranked.empty());

    AIProvider* chosen = nullptr;
    UAPP_REQUIRE_STATUS(reg.select(strict, &chosen), Status::AiNoEligibleProvider);
}

UAPP_TEST("ai.registry.pinned_provider_wins_when_eligible") {
    ProviderRegistry reg;
    LocalHeuristicProvider local;
    FailingDouble          other("other:v1", Status::AiProviderUnavailable);
    UAPP_REQUIRE_OK(reg.add(&local));
    UAPP_REQUIRE_OK(reg.add(&other));

    Selection sel            = local_summarize();
    sel.pinned_provider_id   = "other:v1";

    AIProvider* chosen = nullptr;
    UAPP_REQUIRE_OK(reg.select(sel, &chosen));
    UAPP_CHECK_MSG(chosen == &other, "the user's pin was ignored");

    std::vector<AIProvider*> ranked;
    UAPP_REQUIRE_OK(reg.candidates(sel, ranked));
    UAPP_REQUIRE(ranked.size() == 2);
    UAPP_CHECK(ranked[0] == &other);
}

UAPP_TEST("ai.registry.pinned_provider_failure_is_never_a_silent_substitution") {
    ProviderRegistry reg;
    LocalHeuristicProvider local;
    NullProvider           nothing("null:none");
    OffDeviceDouble        cloud("cloud-double:v1");
    UAPP_REQUIRE_OK(reg.add(&local));
    UAPP_REQUIRE_OK(reg.add(&nothing));
    UAPP_REQUIRE_OK(reg.add(&cloud));

    AIProvider* chosen = nullptr;

    // Pinned a model that is not installed.
    Selection gone          = local_summarize();
    gone.pinned_provider_id = "a-model-that-is-not-here:v1";
    UAPP_REQUIRE_STATUS(reg.select(gone, &chosen), Status::AiProviderNotRegistered);
    UAPP_CHECK_MSG(chosen == nullptr, "silently substituted another provider");

    // Pinned a model that exists but cannot do this job.
    Selection incapable          = local_summarize();
    incapable.pinned_provider_id = "null:none";
    UAPP_REQUIRE_STATUS(reg.select(incapable, &chosen),
                        Status::AiCapabilityNotImplemented);
    UAPP_CHECK(chosen == nullptr);

    // Pinned a cloud model under a local-only ceiling: the POLICY wins, and
    // the status says so rather than reporting "unavailable".
    Selection blocked          = local_summarize();
    blocked.pinned_provider_id = "cloud-double:v1";
    UAPP_REQUIRE_STATUS(reg.select(blocked, &chosen), Status::AiPolicyBlocked);
    UAPP_CHECK(chosen == nullptr);
}

UAPP_TEST("ai.registry.rejects_a_selection_naming_more_than_one_capability") {
    ProviderRegistry reg;
    LocalHeuristicProvider local;
    UAPP_REQUIRE_OK(reg.add(&local));

    Selection sel = local_summarize();
    sel.required  = static_cast<Capability>(uapp::ai::cap_mask(Capability::Summarize,
                                                               Capability::Classify));
    AIProvider* chosen = nullptr;
    UAPP_REQUIRE_STATUS(reg.select(sel, &chosen), Status::InvalidArgument);

    // And a bit pattern that names no capability at all.
    sel.required = static_cast<Capability>(1u << 20);
    UAPP_REQUIRE_STATUS(reg.select(sel, &chosen), Status::InvalidArgument);
}

// --- failover --------------------------------------------------------------

UAPP_TEST("ai.registry.failover_eligibility_table") {
    // Provider-shaped failures: the next candidate can genuinely differ.
    UAPP_CHECK(is_failover_eligible(Status::AiProviderUnavailable));
    UAPP_CHECK(is_failover_eligible(Status::AiCapabilityNotImplemented));

    // Request-shaped failures: every candidate would fail identically, and
    // retrying only multiplies audit records.
    UAPP_CHECK(!is_failover_eligible(Status::AiInputTooLarge));
    UAPP_CHECK(!is_failover_eligible(Status::AiInputMalformed));
    UAPP_CHECK(!is_failover_eligible(Status::AiInputEmpty));
    UAPP_CHECK(!is_failover_eligible(Status::AiLabelSetInvalid));
    UAPP_CHECK(!is_failover_eligible(Status::InvalidArgument));

    // Consent-shaped failures: a grant is per-provider, so another provider is
    // a NEW egress needing its own consent, never a retry.
    UAPP_CHECK(!is_failover_eligible(Status::AiConsentRequired));
    UAPP_CHECK(!is_failover_eligible(Status::AiConsentScopeMismatch));
    UAPP_CHECK(!is_failover_eligible(Status::AiConsentExpired));
    UAPP_CHECK(!is_failover_eligible(Status::AiPolicyBlocked));

    // §5, explicitly: never retry a content-policy refusal, and do not fail
    // over on throttling.
    UAPP_CHECK(!is_failover_eligible(Status::AiContentPolicyRejected));
    UAPP_CHECK(!is_failover_eligible(Status::AiRateLimited));

    // A provider that broke its own contract is not retried either: the bug
    // is in the adapter, and hiding it behind a fallback is how a broken
    // adapter ships.
    UAPP_CHECK(!is_failover_eligible(Status::AiResponseMalformed));

    UAPP_CHECK(!is_failover_eligible(Status::Ok));
}

UAPP_TEST("ai.registry.failover_steps_past_a_provider_that_can_do_nothing") {
    ProviderRegistry reg;
    NullProvider           nothing("null:none");
    FailingDouble          broken("broken:v1", Status::AiProviderUnavailable);
    LocalHeuristicProvider local;

    // Deliberate order: a provider with no capabilities, then one that is
    // registered-and-capable but fails, then the one that works.
    UAPP_REQUIRE_OK(reg.add(&nothing));
    UAPP_REQUIRE_OK(reg.add(&broken));
    UAPP_REQUIRE_OK(reg.add(&local));

    Content c = Content::on_device_only(kDoc, Role::Document);
    SummarizeRequest req;
    req.content       = &c;
    req.max_sentences = 1;

    SummarizeResult out;
    std::string     chosen;
    UAPP_REQUIRE_OK(reg.summarize_with_failover(local_summarize(), req, out, chosen));
    UAPP_CHECK_MSG(chosen == kLocalHeuristicProviderId,
                   "answered by '" + chosen + "'");
    UAPP_CHECK(!out.text.empty());
    UAPP_CHECK(broken.summarize_calls() == 1);
}

UAPP_TEST("ai.registry.failover_with_nothing_capable_reports_a_clear_status") {
    ProviderRegistry reg;
    NullProvider a("null-a:v1");
    NullProvider b("null-b:v1");
    UAPP_REQUIRE_OK(reg.add(&a));
    UAPP_REQUIRE_OK(reg.add(&b));

    Content c = Content::on_device_only(kDoc, Role::Document);
    SummarizeRequest req;
    req.content = &c;
    SummarizeResult out;
    std::string     chosen;

    // Neither declares Summarize, so neither is even a candidate. The caller
    // gets one clear status and no crash — which is the whole point of
    // registering a provider that can do nothing.
    UAPP_REQUIRE_STATUS(reg.summarize_with_failover(local_summarize(), req, out, chosen),
                        Status::AiNoEligibleProvider);
    UAPP_CHECK(chosen.empty());
    UAPP_CHECK(out.text.empty());
}

UAPP_TEST("ai.registry.content_policy_refusal_is_never_retried") {
    ProviderRegistry reg;
    FailingDouble refuser("refuser:v1", Status::AiContentPolicyRejected);
    LocalHeuristicProvider local;
    UAPP_REQUIRE_OK(reg.add(&refuser));
    UAPP_REQUIRE_OK(reg.add(&local));

    Content c = Content::on_device_only(kDoc, Role::Document);
    SummarizeRequest req;
    req.content       = &c;
    req.max_sentences = 1;
    SummarizeResult out;
    std::string     chosen;

    // The local provider WOULD have succeeded. It is deliberately not tried:
    // re-sending refused content is both futile and a second egress of the
    // same bytes (ai-architecture.md §5).
    UAPP_REQUIRE_STATUS(reg.summarize_with_failover(local_summarize(), req, out, chosen),
                        Status::AiContentPolicyRejected);
    UAPP_CHECK(chosen.empty());
    UAPP_CHECK(refuser.summarize_calls() == 1);
}

UAPP_TEST("ai.registry.failover_never_crosses_the_locality_ceiling") {
    ProviderRegistry reg;
    FailingDouble   broken("broken:v1", Status::AiProviderUnavailable);
    OffDeviceDouble cloud("cloud-double:v1");
    UAPP_REQUIRE_OK(reg.add(&broken));
    UAPP_REQUIRE_OK(reg.add(&cloud));

    Content c = Content::on_device_only(kDoc, Role::Document);
    SummarizeRequest req;
    req.content       = &c;
    req.max_sentences = 1;
    SummarizeResult out;
    std::string     chosen;

    // The on-device provider fails with a failover-eligible status and the
    // only other candidate is off-device. Falling back would violate the
    // policy the user set, so the call fails with the LAST failure instead.
    UAPP_REQUIRE_STATUS(reg.summarize_with_failover(local_summarize(), req, out, chosen),
                        Status::AiProviderUnavailable);
    UAPP_CHECK_MSG(cloud.body_calls() == 0,
                   "failover crossed a locality boundary");
}
