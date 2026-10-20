// SPDX-License-Identifier: Apache-2.0
//
// tests/ai/consent_test.cpp — the E2EE consent boundary (ADR-0002 decision 1,
// ai-architecture.md §13.2).
//
// This file tests a SECURITY PROPERTY, so it is written in two halves that
// fail in two different ways.
//
// HALF ONE: COMPILE-TIME. The static_asserts below are the real enforcement.
// They state, as conditions the compiler checks on every build of this file,
// that there is no publicly reachable way to construct an egress-authorised
// payload. If someone later adds a convenience constructor, a setter or a
// "trust me" factory to `Content` or `ConsentToken`, THIS FILE STOPS
// COMPILING. That is deliberately more annoying than a failing assertion,
// because the property it protects cannot be restored by a bug fix after the
// data has left the device.
//
// There is a deliberate limit to what static_assert can say, and it is worth
// naming rather than overclaiming: `std::is_constructible` proves that a
// specific signature is not callable from here, not that no path exists at
// all. The genuinely exhaustive check is that `Content`'s authorising members
// are private with exactly one friend (`ConsentToken`) and `ConsentToken`'s
// real constructor is private with exactly one friend (`ConsentGate`) — which
// is a two-line review of one header, and is what these asserts pin down so
// that the review stays valid.
//
// HALF TWO: RUNTIME. Every way the boundary can be approached wrongly, driven
// through a real off-device provider double, asserting both the status AND
// — the part that matters — that the provider's body never executed.

#include <string>
#include <type_traits>
#include <vector>

#include "ai_test_doubles.h"
#include "test_framework.h"
#include "uapp/ai/consent.h"
#include "uapp/ai/local/local_heuristic_provider.h"
#include "uapp/ai/provider.h"

using uapp::Status;
using uapp::ai::Capability;
using uapp::ai::ConsentDecision;
using uapp::ai::ConsentGate;
using uapp::ai::ConsentId;
using uapp::ai::ConsentPrompt;
using uapp::ai::ConsentRecord;
using uapp::ai::ConsentToken;
using uapp::ai::Content;
using uapp::ai::EgressScope;
using uapp::ai::Locality;
using uapp::ai::Message;
using uapp::ai::Role;
using uapp::ai::SummarizeRequest;
using uapp::ai::SummarizeResult;
using uapp::ai::test_doubles::fake_clock;
using uapp::ai::test_doubles::g_fake_now_ms;
using uapp::ai::test_doubles::OffDeviceDouble;

// ===========================================================================
// HALF ONE — the type-level property, checked by the compiler
// ===========================================================================

// A ConsentToken cannot be forged by calling its real constructor: it is
// private, with `ConsentGate` as its only friend.
static_assert(!std::is_constructible_v<ConsentToken, const ConsentGate*, ConsentRecord,
                                       std::vector<Message>>,
              "ConsentToken's gate-only constructor must not be publicly reachable");

// A ConsentToken cannot be copied, so a single-use grant cannot be duplicated
// and replayed.
static_assert(!std::is_copy_constructible_v<ConsentToken>,
              "a consent grant must not be copyable");
static_assert(!std::is_copy_assignable_v<ConsentToken>,
              "a consent grant must not be copy-assignable");
static_assert(std::is_move_constructible_v<ConsentToken>,
              "a consent grant must be movable, so a gate can hand one back");

// The null token exists only as an out-parameter and must be inert. Its
// inertness is also asserted at runtime below (`ai.consent.null_token`),
// because "is default constructible" alone would be satisfied by a token that
// authorises everything.
static_assert(std::is_default_constructible_v<ConsentToken>,
              "a caller needs to be able to declare an out-parameter");

// `Content` cannot be built from raw text plus an egress scope, or from a
// consent id, or from any shape that would let a caller stamp it as
// authorised. The only public factory is `on_device_only`.
static_assert(!std::is_constructible_v<Content, std::vector<Message>>,
              "Content must not be constructible straight from messages");
static_assert(!std::is_constructible_v<Content, std::vector<Message>, EgressScope>,
              "Content must not be constructible with a caller-chosen egress scope");
static_assert(!std::is_constructible_v<Content, std::vector<Message>, EgressScope,
                                       ConsentId>,
              "Content must not be constructible with a caller-supplied consent id");
static_assert(!std::is_constructible_v<Content, const char*, EgressScope>,
              "Content must not be constructible with a caller-chosen egress scope");

// `Content` cannot be copied: one authorised plaintext payload, one holder.
static_assert(!std::is_copy_constructible_v<Content>,
              "authorised plaintext must not be silently duplicated");
static_assert(std::is_move_constructible_v<Content>, "Content must be movable");

// ===========================================================================
// HALF TWO — runtime behaviour
// ===========================================================================

namespace {

// --- prompt / resolver seams, with call counters ---------------------------
// The counters are the point of several cases below: the status alone does not
// distinguish "we refused after asking" from "we refused without asking", and
// ai-architecture.md §13.2 step 3 requires the second.

int g_prompt_calls   = 0;
int g_resolver_calls = 0;
int g_audit_calls    = 0;

ConsentPrompt g_last_prompt;
ConsentRecord g_last_record;

/// True iff the audit sink ran AFTER the plaintext resolver. See
/// `ai.consent.audit_precedes_any_possible_egress` for why that is the
/// documented order and how it relates to ai-architecture.md §13.2 step 6.
bool g_audit_ran_after_resolver = false;

ConsentDecision allow_prompt(const ConsentPrompt& p, void*) {
    ++g_prompt_calls;
    g_last_prompt = p;
    return ConsentDecision::Allow;
}

ConsentDecision deny_prompt(const ConsentPrompt& p, void*) {
    ++g_prompt_calls;
    g_last_prompt = p;
    return ConsentDecision::Deny;
}

void audit_sink(const ConsentRecord& r, void*) {
    ++g_audit_calls;
    g_last_record = r;
    g_audit_ran_after_resolver = (g_resolver_calls > 0);
}

const char* const kSecret = "The merger closes on 2026-01-05. Contact bob@example.com.";

Status resolve_secret(void*, std::vector<Message>& out) {
    ++g_resolver_calls;
    out.clear();
    out.push_back(Message{Role::Document, std::string(kSecret)});
    return Status::Ok;
}

Status resolve_wrong_size(void*, std::vector<Message>& out) {
    ++g_resolver_calls;
    out.clear();
    out.push_back(Message{Role::Document, std::string(kSecret) + " ...and one more line."});
    return Status::Ok;
}

Status resolve_fails(void*, std::vector<Message>&) {
    ++g_resolver_calls;
    return Status::StoreReadFailed;  // stands in for "decryption failed"
}

void reset_counters() {
    g_prompt_calls   = 0;
    g_resolver_calls = 0;
    g_audit_calls    = 0;
    g_audit_ran_after_resolver = false;
    g_fake_now_ms    = 1'000'000;
}

ConsentGate::Config gate_config(uapp::ai::AiPolicy policy, uapp::ai::ConsentPromptFn prompt) {
    ConsentGate::Config cfg;
    cfg.policy = policy;
    cfg.prompt = prompt;
    cfg.audit  = &audit_sink;
    cfg.clock  = &fake_clock;
    cfg.default_ttl_ms = 60'000;
    return cfg;
}

ConsentGate::Request cloud_request(const std::string& provider_id) {
    ConsentGate::Request r;
    r.scope.workspace_ref        = "ws-1";
    r.scope.object_ref           = "page:merger-memo";
    r.provider_id                = provider_id;
    r.provider_display_name      = "Off-device test double";
    r.provider_locality          = Locality::ThirdParty;
    r.data_policy_url            = "https://example.invalid/retention";
    r.declared_content_bytes     = std::string(kSecret).size();
    return r;
}

SummarizeRequest summarize_of(const Content& c) {
    SummarizeRequest req;
    req.content       = &c;
    req.max_sentences = 2;
    return req;
}

}  // namespace

// --- the central property --------------------------------------------------

UAPP_TEST("ai.consent.off_device_refuses_unauthorized_content") {
    reset_counters();
    OffDeviceDouble cloud;
    Content plain = Content::on_device_only(kSecret, Role::Document);

    UAPP_REQUIRE(plain.egress_scope() == EgressScope::OnDeviceOnly);

    SummarizeResult out;
    const SummarizeRequest req = summarize_of(plain);
    UAPP_REQUIRE_STATUS(cloud.summarize(req, out), Status::AiConsentRequired);

    // THE property, and the reason this is not just a status assertion: the
    // provider's own code never ran, so the bytes never reached the thing
    // that would have sent them.
    UAPP_CHECK_MSG(cloud.body_calls() == 0,
                   "off-device provider body executed without consent");
    UAPP_CHECK(out.text.empty());
}

UAPP_TEST("ai.consent.on_device_needs_no_grant") {
    reset_counters();
    uapp::ai::local::LocalHeuristicProvider local;
    Content plain = Content::on_device_only(kSecret, Role::Document);

    SummarizeResult out;
    const SummarizeRequest req = summarize_of(plain);
    UAPP_REQUIRE_OK(local.summarize(req, out));
    UAPP_CHECK(!out.text.empty());
    // An on-device call carries no consent id, because no consent was needed.
    UAPP_CHECK(out.meta.consent_id.is_nil());
}

UAPP_TEST("ai.consent.granted_token_authorizes_exactly_one_payload") {
    reset_counters();
    OffDeviceDouble cloud;
    ConsentGate gate(gate_config(uapp::ai::AiPolicy::Ask, &allow_prompt));

    ConsentToken token;
    UAPP_REQUIRE_OK(gate.request_consent(cloud_request(std::string(cloud.descriptor().id)),
                                         &resolve_secret, nullptr, token));
    UAPP_CHECK(g_prompt_calls == 1);
    UAPP_CHECK(g_resolver_calls == 1);
    UAPP_CHECK(g_audit_calls == 1);

    // The prompt was given everything §13.2 step 5 requires it to show.
    UAPP_CHECK(g_last_prompt.provider_display_name == "Off-device test double");
    UAPP_CHECK(!g_last_prompt.data_policy_url.empty());
    UAPP_CHECK(g_last_prompt.content_bytes == std::string(kSecret).size());
    UAPP_CHECK(g_last_prompt.approx_tokens > 0);
    UAPP_CHECK(g_last_prompt.leaves_encrypted_storage);
    UAPP_CHECK(g_last_prompt.scope.object_ref == "page:merger-memo");

    // The audit record carries references and counts, and NO content.
    UAPP_CHECK(g_last_record.content_bytes == std::string(kSecret).size());
    UAPP_CHECK(!g_last_record.id.is_nil());
    UAPP_CHECK(g_last_record.provider_id == cloud.descriptor().id);

    Content authorized;
    UAPP_REQUIRE_OK(token.release(authorized));
    UAPP_REQUIRE(authorized.egress_scope() == EgressScope::Authorized);
    UAPP_CHECK(authorized.authorized_provider_id() == cloud.descriptor().id);
    UAPP_CHECK(authorized.consent_id() == g_last_record.id);

    SummarizeResult out;
    const SummarizeRequest req = summarize_of(authorized);
    UAPP_REQUIRE_OK(cloud.summarize(req, out));
    UAPP_CHECK(cloud.body_calls() == 1);
    UAPP_CHECK(!out.text.empty());
    // The call is attributable to the grant that permitted it.
    UAPP_CHECK(out.meta.consent_id == g_last_record.id);
    // And the provenance came from the descriptor, not from the provider body.
    UAPP_CHECK(out.meta.provenance == uapp::ai::OutputProvenance::LanguageModel);
}

UAPP_TEST("ai.consent.audit_precedes_any_possible_egress") {
    reset_counters();
    OffDeviceDouble cloud;
    ConsentGate gate(gate_config(uapp::ai::AiPolicy::Ask, &allow_prompt));

    ConsentToken token;
    UAPP_REQUIRE_OK(gate.request_consent(cloud_request(std::string(cloud.descriptor().id)),
                                         &resolve_secret, nullptr, token));

    // THE ORDER THIS SLICE IMPLEMENTS, and it is NOT quite the order
    // ai-architecture.md §13.2 prose gives. §13.2 numbers the steps
    // "6. write consent_record BEFORE decrypting / 7. decrypt", and here the
    // resolver (the decrypt) runs BEFORE the audit sink is called.
    //
    // The reason is that the record binds a digest OF the resolved content —
    // which is what makes "the user approved exactly this" checkable at the
    // provider boundary — and a digest of content that has not been resolved
    // cannot exist. The guarantee §13.2 is actually protecting is preserved
    // and is what this case asserts: the audit record exists before anything
    // CAN be sent, because the token (and therefore the only authorised
    // Content in existence) is created after the audit call. The resolver is
    // a local decrypt with no observable effect outside the process.
    //
    // Flagged in docs/ai-architecture.md §14 so the document and the code
    // agree rather than quietly differing.
    UAPP_CHECK_MSG(g_audit_ran_after_resolver,
                   "expected resolve-then-audit ordering; see this case's comment");
    UAPP_CHECK(g_audit_calls == 1);
    UAPP_CHECK(gate.records().size() == 1);

    // Nothing has egressed yet: no authorised Content exists until release().
    UAPP_CHECK(cloud.body_calls() == 0);
}

UAPP_TEST("ai.consent.local_only_policy_fails_closed_without_prompting") {
    reset_counters();
    OffDeviceDouble cloud;
    ConsentGate gate(gate_config(uapp::ai::AiPolicy::LocalOnly, &allow_prompt));

    ConsentToken token;
    UAPP_REQUIRE_STATUS(gate.request_consent(cloud_request(std::string(cloud.descriptor().id)),
                                             &resolve_secret, nullptr, token),
                        Status::AiPolicyBlocked);

    // H-29 / §13.2 step 3: a LOCAL_ONLY workspace cannot be talked into cloud
    // egress by a dialog. The dialog is never shown — and the prompt here
    // would have said Allow, so this assertion is the whole test.
    UAPP_CHECK_MSG(g_prompt_calls == 0,
                   "LOCAL_ONLY prompted the user instead of failing closed");
    UAPP_CHECK_MSG(g_resolver_calls == 0, "LOCAL_ONLY resolved plaintext anyway");
    UAPP_CHECK(g_audit_calls == 0);
    UAPP_CHECK(token.spent());
}

UAPP_TEST("ai.consent.denial_never_resolves_plaintext") {
    reset_counters();
    OffDeviceDouble cloud;
    ConsentGate gate(gate_config(uapp::ai::AiPolicy::Ask, &deny_prompt));

    ConsentToken token;
    UAPP_REQUIRE_STATUS(gate.request_consent(cloud_request(std::string(cloud.descriptor().id)),
                                             &resolve_secret, nullptr, token),
                        Status::AiConsentDenied);
    UAPP_CHECK(g_prompt_calls == 1);
    UAPP_CHECK_MSG(g_resolver_calls == 0,
                   "the user said no and the content was decrypted anyway");
    UAPP_CHECK(g_audit_calls == 0);
}

UAPP_TEST("ai.consent.missing_prompt_callback_fails_closed") {
    reset_counters();
    OffDeviceDouble cloud;
    // A gate wired up with no UI. The honest answer is "no", not "yes".
    ConsentGate gate(gate_config(uapp::ai::AiPolicy::CloudOk, nullptr));

    ConsentToken token;
    UAPP_REQUIRE_STATUS(gate.request_consent(cloud_request(std::string(cloud.descriptor().id)),
                                             &resolve_secret, nullptr, token),
                        Status::AiConsentDenied);
    UAPP_CHECK(g_resolver_calls == 0);
}

UAPP_TEST("ai.consent.grant_is_single_use") {
    reset_counters();
    OffDeviceDouble cloud;
    ConsentGate gate(gate_config(uapp::ai::AiPolicy::Ask, &allow_prompt));

    ConsentToken token;
    UAPP_REQUIRE_OK(gate.request_consent(cloud_request(std::string(cloud.descriptor().id)),
                                         &resolve_secret, nullptr, token));

    Content first;
    UAPP_REQUIRE_OK(token.release(first));
    UAPP_CHECK(token.spent());

    Content second;
    UAPP_REQUIRE_STATUS(token.release(second), Status::AiConsentTokenSpent);
    UAPP_CHECK(second.egress_scope() == EgressScope::OnDeviceOnly);
    UAPP_CHECK(second.messages().empty());
}

UAPP_TEST("ai.consent.null_token_authorizes_nothing") {
    ConsentToken token;
    UAPP_CHECK(token.spent());
    Content c;
    UAPP_REQUIRE_STATUS(token.release(c), Status::AiConsentTokenSpent);
    UAPP_CHECK(c.egress_scope() == EgressScope::OnDeviceOnly);
}

UAPP_TEST("ai.consent.moved_from_token_is_inert") {
    reset_counters();
    OffDeviceDouble cloud;
    ConsentGate gate(gate_config(uapp::ai::AiPolicy::Ask, &allow_prompt));

    ConsentToken token;
    UAPP_REQUIRE_OK(gate.request_consent(cloud_request(std::string(cloud.descriptor().id)),
                                         &resolve_secret, nullptr, token));

    ConsentToken moved(std::move(token));
    Content from_moved_from;
    // The husk must not still authorise anything: two names for one grant
    // would be two egresses under one audit record.
    UAPP_REQUIRE_STATUS(token.release(from_moved_from), Status::AiConsentTokenSpent);

    Content ok;
    UAPP_REQUIRE_OK(moved.release(ok));
    UAPP_CHECK(ok.egress_scope() == EgressScope::Authorized);
}

UAPP_TEST("ai.consent.grant_for_one_provider_does_not_authorize_another") {
    reset_counters();
    OffDeviceDouble provider_a("cloud-a:v1");
    OffDeviceDouble provider_b("cloud-b:v1");
    ConsentGate gate(gate_config(uapp::ai::AiPolicy::Ask, &allow_prompt));

    ConsentToken token;
    UAPP_REQUIRE_OK(gate.request_consent(cloud_request("cloud-a:v1"), &resolve_secret,
                                         nullptr, token));
    Content authorized;
    UAPP_REQUIRE_OK(token.release(authorized));

    SummarizeResult out;
    const SummarizeRequest req = summarize_of(authorized);

    UAPP_REQUIRE_STATUS(provider_b.summarize(req, out), Status::AiConsentScopeMismatch);
    UAPP_CHECK_MSG(provider_b.body_calls() == 0,
                   "a grant for provider A let provider B see the content");

    UAPP_REQUIRE_OK(provider_a.summarize(req, out));
    UAPP_CHECK(provider_a.body_calls() == 1);
}

UAPP_TEST("ai.consent.expired_grant_is_refused_at_the_provider_boundary") {
    reset_counters();
    OffDeviceDouble cloud;
    ConsentGate gate(gate_config(uapp::ai::AiPolicy::Ask, &allow_prompt));

    ConsentToken token;
    UAPP_REQUIRE_OK(gate.request_consent(cloud_request(std::string(cloud.descriptor().id)),
                                         &resolve_secret, nullptr, token));
    Content authorized;
    UAPP_REQUIRE_OK(token.release(authorized));

    // Still inside the TTL: the call works.
    SummarizeResult out;
    const SummarizeRequest req = summarize_of(authorized);
    UAPP_REQUIRE_OK(cloud.summarize(req, out));

    // Past it: refused, even though the Content object is unchanged and still
    // says `Authorized`. Expiry is checked at the boundary on every call, not
    // once at release time.
    g_fake_now_ms += 60'001;
    UAPP_REQUIRE_STATUS(cloud.summarize(req, out), Status::AiConsentExpired);
    UAPP_CHECK(cloud.body_calls() == 1);
}

UAPP_TEST("ai.consent.expired_grant_cannot_be_released") {
    reset_counters();
    ConsentGate gate(gate_config(uapp::ai::AiPolicy::Ask, &allow_prompt));

    ConsentToken token;
    UAPP_REQUIRE_OK(gate.request_consent(cloud_request("cloud-a:v1"), &resolve_secret,
                                         nullptr, token));
    g_fake_now_ms += 60'001;

    Content c;
    UAPP_REQUIRE_STATUS(token.release(c), Status::AiConsentExpired);
    UAPP_CHECK(c.messages().empty());
}

UAPP_TEST("ai.consent.revocation_stops_future_release_only") {
    reset_counters();
    ConsentGate gate(gate_config(uapp::ai::AiPolicy::Ask, &allow_prompt));

    ConsentToken token;
    UAPP_REQUIRE_OK(gate.request_consent(cloud_request("cloud-a:v1"), &resolve_secret,
                                         nullptr, token));
    const ConsentId id = token.record().id;
    UAPP_REQUIRE_OK(gate.revoke(id));
    UAPP_CHECK(!gate.grant_is_live(id));

    Content c;
    UAPP_REQUIRE_STATUS(token.release(c), Status::AiConsentRevoked);

    // AIQ-7, stated as a test rather than only as prose: revoking an unknown
    // grant is NotFound, and revocation says nothing about data already sent —
    // there is deliberately no API here that claims to recall it.
    UAPP_REQUIRE_STATUS(gate.revoke(ConsentId{999999}), Status::NotFound);
}

UAPP_TEST("ai.consent.resolved_content_must_match_what_the_user_was_shown") {
    reset_counters();
    ConsentGate gate(gate_config(uapp::ai::AiPolicy::Ask, &allow_prompt));

    // The prompt said N bytes; the resolver produced more. The user's answer
    // does not cover the difference.
    ConsentToken token;
    UAPP_REQUIRE_STATUS(gate.request_consent(cloud_request("cloud-a:v1"),
                                             &resolve_wrong_size, nullptr, token),
                        Status::AiConsentScopeMismatch);
    UAPP_CHECK(g_prompt_calls == 1);
    UAPP_CHECK(g_resolver_calls == 1);
    // No grant was recorded, so nothing can later claim this egress was audited.
    UAPP_CHECK(g_audit_calls == 0);
    UAPP_CHECK(gate.records().empty());
    UAPP_CHECK(token.spent());
}

UAPP_TEST("ai.consent.resolver_failure_grants_nothing") {
    reset_counters();
    ConsentGate gate(gate_config(uapp::ai::AiPolicy::Ask, &allow_prompt));

    ConsentToken token;
    UAPP_REQUIRE_STATUS(gate.request_consent(cloud_request("cloud-a:v1"), &resolve_fails,
                                             nullptr, token),
                        Status::StoreReadFailed);
    UAPP_CHECK(token.spent());
    UAPP_CHECK(gate.records().empty());
}

UAPP_TEST("ai.consent.gate_refuses_to_mint_for_an_on_device_provider") {
    reset_counters();
    ConsentGate gate(gate_config(uapp::ai::AiPolicy::CloudOk, &allow_prompt));

    ConsentGate::Request req = cloud_request("local-heuristic:v1");
    req.provider_locality    = Locality::OnDevice;

    ConsentToken token;
    UAPP_REQUIRE_STATUS(gate.request_consent(req, &resolve_secret, nullptr, token),
                        Status::InvalidArgument);
    UAPP_CHECK_MSG(g_prompt_calls == 0,
                   "the user was asked to authorise egress for a local call");
}

UAPP_TEST("ai.consent.first_party_server_also_needs_a_grant") {
    reset_counters();
    // ai-architecture.md §13.1 names THREE boundaries and exists to stop
    // "our server" being treated as "on device". A FirstPartyServer provider
    // is subject to the same check as a third party.
    OffDeviceDouble first_party("gateway:v1", Locality::FirstPartyServer);
    Content plain = Content::on_device_only(kSecret, Role::Document);

    SummarizeResult out;
    const SummarizeRequest req = summarize_of(plain);
    UAPP_REQUIRE_STATUS(first_party.summarize(req, out), Status::AiConsentRequired);
    UAPP_CHECK(first_party.body_calls() == 0);
}

UAPP_TEST("ai.consent.digest_binds_roles_not_just_bytes") {
    // Moving text from the Document role into the System role changes the
    // digest, so the §3.2 injection boundary is part of what was consented to.
    std::vector<Message> as_document;
    as_document.push_back(Message{Role::Document, "payload"});
    std::vector<Message> as_system;
    as_system.push_back(Message{Role::System, "payload"});

    UAPP_CHECK(uapp::ai::digest_messages(as_document) !=
               uapp::ai::digest_messages(as_system));

    // And regrouping the same bytes across message boundaries changes it too.
    std::vector<Message> split;
    split.push_back(Message{Role::Document, "pay"});
    split.push_back(Message{Role::Document, "load"});
    UAPP_CHECK(uapp::ai::digest_messages(as_document) != uapp::ai::digest_messages(split));

    // Same input, same digest. Determinism is what makes it usable at all.
    UAPP_CHECK(uapp::ai::digest_messages(as_document) ==
               uapp::ai::digest_messages(as_document));
}

UAPP_TEST("ai.consent.approx_tokens_is_labelled_an_estimate") {
    // AIQ-6: there is no tokenizer, so the count is bytes/4 and every result
    // must say so. A quota system that treats this as exact would be lying.
    UAPP_CHECK(ConsentGate::approx_tokens(0) == 0);
    UAPP_CHECK(ConsentGate::approx_tokens(1) == 1);
    UAPP_CHECK(ConsentGate::approx_tokens(4) == 1);
    UAPP_CHECK(ConsentGate::approx_tokens(400) == 100);

    uapp::ai::local::LocalHeuristicProvider local;
    Content plain = Content::on_device_only("hello world, this is some text");
    SummarizeResult out;
    const SummarizeRequest req = summarize_of(plain);
    UAPP_REQUIRE_OK(local.summarize(req, out));
    UAPP_CHECK_MSG(out.meta.tokens_are_estimates,
                   "token counts are presented as exact when they are not");
}
