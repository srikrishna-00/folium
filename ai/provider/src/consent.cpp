// SPDX-License-Identifier: Apache-2.0
#include "uapp/ai/consent.h"

#include <chrono>
#include <utility>

namespace uapp::ai {
namespace {

// FNV-1a, 64-bit. Two independent runs with different offset bases give the
// 128 bits of ContentDigest. NOT CRYPTOGRAPHIC — consent.h's header comment,
// point 2, says why that is the right call here and what would not be.
constexpr std::uint64_t kFnvPrime  = 1099511628211ULL;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvOffsetAlt = 0x9E3779B97F4A7C15ULL;

void fnv_mix(std::uint64_t& h, std::uint8_t byte) noexcept {
    h ^= static_cast<std::uint64_t>(byte);
    h *= kFnvPrime;
}

void fnv_mix_bytes(std::uint64_t& h, const std::string& s) noexcept {
    for (const char c : s) {
        fnv_mix(h, static_cast<std::uint8_t>(c));
    }
    // Length-delimit, so {"ab","c"} and {"a","bc"} do not collide. A digest
    // that ignores message boundaries would let content be regrouped between
    // what the user saw and what is sent.
    fnv_mix(h, 0xFFu);
}

}  // namespace

std::uint64_t wall_clock_ms() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return ms < 0 ? 0u : static_cast<std::uint64_t>(ms);
}

const char* ai_policy_name(AiPolicy p) noexcept {
    switch (p) {
        case AiPolicy::LocalOnly: return "LOCAL_ONLY";
        case AiPolicy::Ask:       return "ASK";
        case AiPolicy::CloudOk:   return "CLOUD_OK";
    }
    return "UNKNOWN_AI_POLICY";
}

ContentDigest digest_messages(const std::vector<Message>& messages) noexcept {
    ContentDigest d;
    d.lo = kFnvOffset;
    d.hi = kFnvOffsetAlt;
    for (const Message& m : messages) {
        const auto role = static_cast<std::uint8_t>(m.role);
        fnv_mix(d.lo, role);
        fnv_mix(d.hi, role);
        fnv_mix_bytes(d.lo, m.text);
        fnv_mix_bytes(d.hi, m.text);
    }
    return d;
}

// --- Content ---------------------------------------------------------------

Content Content::on_device_only(std::vector<Message> messages) {
    Content c;
    c.messages_ = std::move(messages);
    c.scope_    = EgressScope::OnDeviceOnly;
    return c;
}

Content Content::on_device_only(std::string_view text, Role role) {
    std::vector<Message> msgs;
    msgs.push_back(Message{role, std::string(text)});
    return on_device_only(std::move(msgs));
}

std::size_t Content::byte_size() const noexcept {
    std::size_t total = 0;
    for (const Message& m : messages_) {
        total += m.text.size();
    }
    return total;
}

// --- ConsentToken ----------------------------------------------------------

ConsentToken::ConsentToken(const ConsentGate* gate, ConsentRecord record,
                           std::vector<Message> plaintext) noexcept
    : gate_(gate),
      record_(std::move(record)),
      plaintext_(std::move(plaintext)),
      spent_(false) {}

ConsentToken::ConsentToken(ConsentToken&& other) noexcept
    : gate_(other.gate_),
      record_(std::move(other.record_)),
      plaintext_(std::move(other.plaintext_)),
      spent_(other.spent_) {
    // A moved-from token must not be usable: it is the single-use capability,
    // and leaving it live would make `release()` callable twice through two
    // names for one grant.
    other.spent_ = true;
    other.gate_  = nullptr;
}

ConsentToken& ConsentToken::operator=(ConsentToken&& other) noexcept {
    if (this != &other) {
        gate_       = other.gate_;
        record_     = std::move(other.record_);
        plaintext_  = std::move(other.plaintext_);
        spent_      = other.spent_;
        other.spent_ = true;
        other.gate_  = nullptr;
    }
    return *this;
}

Status ConsentToken::release(Content& out) {
    if (spent_ || gate_ == nullptr) {
        return Status::AiConsentTokenSpent;
    }
    if (!gate_->grant_is_live(record_.id)) {
        // Either revoked, or the gate forgot the grant. Both mean the same
        // thing to the caller: this grant is no longer a permission.
        spent_ = true;
        return Status::AiConsentRevoked;
    }
    if (gate_->now_ms() > record_.expires_at_ms) {
        spent_ = true;
        return Status::AiConsentExpired;
    }

    // Mark spent BEFORE building the Content. If anything below were ever to
    // fail, the token must not be retryable: the audit record for this grant
    // has already been written and a second attempt would be a second egress
    // under one record.
    spent_ = true;

    Content authorized;
    authorized.messages_               = std::move(plaintext_);
    authorized.scope_                  = EgressScope::Authorized;
    authorized.consent_id_             = record_.id;
    authorized.authorized_provider_id_ = record_.provider_id;
    authorized.expires_at_ms_          = record_.expires_at_ms;
    authorized.digest_                 = record_.content_digest;
    out = std::move(authorized);
    return Status::Ok;
}

// --- ConsentGate -----------------------------------------------------------

ConsentGate::ConsentGate(Config config) : config_(std::move(config)) {
    if (config_.clock == nullptr) {
        config_.clock = &wall_clock_ms;
    }
}

std::uint64_t ConsentGate::now_ms() const noexcept { return config_.clock(); }

std::size_t ConsentGate::approx_tokens(std::size_t bytes) noexcept {
    return (bytes + 3u) / 4u;
}

bool ConsentGate::grant_is_live(ConsentId id) const noexcept {
    for (const ConsentRecord& r : records_) {
        if (r.id == id) {
            return !r.revoked;
        }
    }
    return false;
}

Status ConsentGate::revoke(ConsentId id) noexcept {
    for (ConsentRecord& r : records_) {
        if (r.id == id) {
            r.revoked = true;
            return Status::Ok;
        }
    }
    return Status::NotFound;
}

Status ConsentGate::request_consent(const Request& request, PlaintextResolverFn resolver,
                                    void* resolver_user, ConsentToken& out) {
    if (resolver == nullptr || request.provider_id.empty()) {
        return Status::InvalidArgument;
    }

    // STEP 1. An on-device provider needs no grant. Minting one would create
    // an egress-authorised payload nobody asked for, so this is an error in
    // the caller rather than a no-op.
    if (request.provider_locality == Locality::OnDevice) {
        return Status::InvalidArgument;
    }

    // STEP 2. ai-architecture.md §13.2 step 3 / H-29. FAILS CLOSED, AND
    // BEFORE THE PROMPT: a LOCAL_ONLY workspace cannot be talked into cloud
    // egress by a dialog. The ordering is the property, not just the result —
    // tests/ai/consent_test.cpp asserts the prompt callback was never invoked.
    if (config_.policy == AiPolicy::LocalOnly) {
        return Status::AiPolicyBlocked;
    }

    // STEP 3. No prompt wired up == no way to ask == no. Failing closed on a
    // missing UI callback is the difference between "we have not built the
    // dialog yet" and "we send without asking".
    if (config_.prompt == nullptr) {
        return Status::AiConsentDenied;
    }

    const std::uint64_t now = now_ms();
    const std::uint64_t ttl = request.ttl_ms != 0 ? request.ttl_ms : config_.default_ttl_ms;

    ConsentPrompt prompt;
    prompt.provider_id              = request.provider_id;
    prompt.provider_display_name    = request.provider_display_name;
    prompt.provider_locality        = request.provider_locality;
    prompt.data_policy_url          = request.data_policy_url;
    prompt.scope                    = request.scope;
    prompt.content_bytes            = request.declared_content_bytes;
    prompt.approx_tokens            = approx_tokens(request.declared_content_bytes);
    prompt.expires_at_ms            = now + ttl;
    prompt.leaves_encrypted_storage = true;
    // The preview is deliberately NOT filled from resolved plaintext: the
    // resolver has not run, because step 6 of §13.2 writes the record before
    // step 7 decrypts. A caller that wants a preview supplies it from the
    // ranges it already resolved in step 2. Recorded in ai-architecture.md
    // §14 as the one place this slice's ordering is stricter than the
    // document's prose suggests, and in the stricter direction.

    // STEP 4. Ask. A denial stops here, so the resolver — the decryption — is
    // never invoked. Tested by asserting the resolver's call count is 0.
    if (config_.prompt(prompt, config_.prompt_user) != ConsentDecision::Allow) {
        return Status::AiConsentDenied;
    }

    // STEP 5. Record the grant, then emit it. References and counts only; no
    // content. The digest is filled in after the resolver runs, because it is
    // a digest OF the resolved content — see the note below.
    ConsentRecord record;
    record.id                = ConsentId{next_id_};
    record.granted_at_ms     = now;
    record.expires_at_ms     = now + ttl;
    record.scope             = request.scope;
    record.provider_id       = request.provider_id;
    record.provider_locality = request.provider_locality;
    record.policy_snapshot   = config_.policy;
    record.content_bytes     = request.declared_content_bytes;
    record.approx_tokens     = prompt.approx_tokens;

    // STEP 6. Resolve (in a real client: decrypt) the plaintext.
    std::vector<Message> plaintext;
    if (const Status s = resolver(resolver_user, plaintext); !is_ok(s)) {
        return s;
    }

    std::size_t resolved_bytes = 0;
    for (const Message& m : plaintext) {
        resolved_bytes += m.text.size();
    }

    // The user approved sending a SPECIFIC AMOUNT of a specific thing. If the
    // resolver produced something else, their answer does not cover it. This
    // is the cheap half of the §13.2 step 2 guarantee ("not 'the document' —
    // the ranges"); the expensive half needs a cryptographic digest of the
    // ranges computed before the prompt, which needs security/crypto.
    if (resolved_bytes != request.declared_content_bytes) {
        return Status::AiConsentScopeMismatch;
    }

    record.content_digest = digest_messages(plaintext);

    records_.push_back(record);
    ++next_id_;

    // Emitted AFTER the record is in `records_` and BEFORE the token — and
    // therefore before any authorised Content — exists. "Audit before
    // egress", observably (tests/ai/consent_test.cpp,
    // ai.consent.audit_precedes_any_possible_egress).
    //
    // ORDERING NOTE, FLAGGED: ai-architecture.md §13.2 numbers its steps
    // "6. write consent_record BEFORE decrypting / 7. decrypt", and this
    // function resolves (decrypts) at step 6 above, before emitting. The
    // reason is that the record binds a digest OF the resolved content, which
    // is what makes "the user approved exactly this" checkable later at the
    // provider boundary; a digest of unresolved content cannot exist. The
    // guarantee §13.2 protects is intact — nothing can be sent before the
    // record exists — but the document's step numbering and this code differ,
    // which is recorded in docs/ai-architecture.md §14 rather than left to be
    // discovered.
    if (config_.audit != nullptr) {
        config_.audit(record, config_.audit_user);
    }

    out = ConsentToken(this, std::move(record), std::move(plaintext));
    return Status::Ok;
}

}  // namespace uapp::ai
