// SPDX-License-Identifier: Apache-2.0
//
// ai/provider/consent.h — the E2EE consent boundary, as a type.
//
// ===========================================================================
// WHAT THIS HEADER IS FOR
// ===========================================================================
// ADR-0002 decision 1 makes end-to-end encryption a v1.0 property: the server
// never sees plaintext. ai-architecture.md §13.1 states the consequence as one
// rule:
//
//   "Plaintext leaves the device only when the user has, for this specific
//    scope, in this session, explicitly said so."
//
// A boolean `bool user_consented` on a request struct cannot carry that rule.
// It defaults to something, it is copied around, it is set by whichever layer
// is most convenient, and the day someone writes `req.user_consented = true;`
// to get a test passing, the product's central privacy promise is gone and
// nothing fails to compile.
//
// So consent here is not a flag. It is the only way to CONSTRUCT the value a
// provider needs:
//
//   * `Content` is the carrier for every piece of text any provider method
//     takes. It is stamped with an `EgressScope` at construction and has no
//     mutators.
//   * The only public constructor is `Content::on_device_only(...)`, which
//     stamps `EgressScope::OnDeviceOnly`.
//   * The ONLY way to obtain a `Content` stamped `EgressScope::Authorized` is
//     `ConsentToken::release()`, which consumes the token.
//   * `ConsentToken` has no public constructor, is move-only, and is
//     single-use. The only thing that can build one is `ConsentGate`.
//   * `ConsentGate::request_consent()` fails closed under a LOCAL_ONLY policy
//     WITHOUT prompting, calls the UI prompt callback, writes an audit record,
//     and only then invokes the caller's plaintext resolver.
//   * `AIProvider`'s public methods (provider.h) are NON-VIRTUAL. They check
//     the stamp against the provider's own `Locality` before dispatching to
//     the overridable implementation. A provider cannot override its way past
//     the check, and an off-device provider handed on-device-only content gets
//     `Status::AiConsentRequired` and never sees the bytes.
//
// The property this buys: **there is no expression in the codebase that sends
// content to an off-device provider without a `ConsentGate` having run.** Not
// "we agreed not to"; there is no such expression to write. That is what
// tests/ai/consent_test.cpp checks, with static_asserts for the type-level
// half and real calls for the runtime half.
//
// ===========================================================================
// WHAT THIS IS *NOT*, AND MUST NOT BE MISTAKEN FOR
// ===========================================================================
//  1. **It is not the decryption path.** Nothing here decrypts anything. The
//     caller's `PlaintextResolverFn` is where a real client will call into
//     security/keys and security/crypto. This module deliberately does not
//     link either (see ai/provider/CMakeLists.txt), so that the consent
//     boundary can be reasoned about and tested without a key hierarchy, and
//     so an accidental "just decrypt it here" fails to compile.
//  2. **`ContentDigest` is NOT a cryptographic hash.** It is 128 bits of
//     FNV-1a, and it exists to catch a MISTAKE — content substituted between
//     what the user was shown and what gets sent — not an adversary. An
//     attacker with code execution in this process does not need to forge a
//     digest; they can call the protected virtual directly. The persisted,
//     hash-chained `consent_record` of ai-architecture.md §13.2 step 6 needs a
//     real cryptographic hash and belongs in a module that links
//     security/crypto. NOT IMPLEMENTED here. Flagged in ai-architecture.md §14.
//  3. **Consent records are in-memory and per-gate.** No persistence, no
//     hash-chained audit log, no cross-process uniqueness of `ConsentId`
//     (which is a per-gate sequence number, not a random 128-bit id, precisely
//     so that this module needs no CSPRNG). AIQ-7's retention and revocation
//     semantics are only half-answered here: `revoke()` exists and is honest
//     about stopping only FUTURE egress.
//  4. **There is no cloud provider in this tree.** Nothing exercises the
//     authorised path against a real network, because this environment has
//     none. The path is exercised against a test-double off-device provider,
//     which proves the boundary, not the adapter.

#ifndef UAPP_AI_PROVIDER_CONSENT_H
#define UAPP_AI_PROVIDER_CONSENT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "uapp/ai/ai_types.h"
#include "uapp/status.h"

namespace uapp::ai {

// --- clock -----------------------------------------------------------------

/// Milliseconds from some fixed epoch. A plain function pointer rather than a
/// `std::function` or a virtual Clock interface: it has to be injectable (TTL
/// expiry is untestable against a real clock without sleeping) and this module
/// is compiled -fno-exceptions/-fno-rtti with a C ABI in its future, so the
/// cheapest injectable thing wins.
using ClockFn = std::uint64_t (*)() noexcept;

/// Wall-clock milliseconds. Used when a caller passes no clock.
///
/// WHY WALL CLOCK AND NOT steady_clock: a consent grant's expiry is a
/// user-visible time ("expires at 14:32"), and a grant must not survive a
/// suspend/resume cycle just because the monotonic clock stopped. The cost is
/// that a clock jump backwards extends a grant; the mitigation is that grants
/// are short and single-token, not that the clock is trusted.
[[nodiscard]] std::uint64_t wall_clock_ms() noexcept;

// --- workspace policy ------------------------------------------------------

/// ai-architecture.md §4 step 2. The workspace-level setting the gate reads
/// first, before anything else happens.
enum class AiPolicy : std::uint8_t {
    /// Only `Locality::OnDevice` providers are eligible. **Fails closed: there
    /// is no consent prompt.** ai-architecture.md §13.2 step 3 and H-29 — "a
    /// LOCAL_ONLY workspace cannot be talked into cloud egress by a dialog".
    /// This is the single behaviour that distinguishes a privacy promise from
    /// a privacy claim, so it is the first thing `request_consent()` checks
    /// and the prompt callback is provably not invoked.
    LocalOnly = 0,

    /// Off-device providers are allowed only with a fresh, matching grant.
    /// This is the default, and the only value under which a prompt appears.
    Ask = 1,

    /// Off-device providers are allowed.
    ///
    /// NOTE: this does NOT mean "no prompt". ai-architecture.md §4 step 3
    /// forces ASK behaviour even under CLOUD_OK when local content
    /// classification flags credentials or payment data. That classifier is
    /// NOT IMPLEMENTED here, so in this slice CLOUD_OK still prompts — the
    /// conservative direction, and the one that does not have to be revisited
    /// when the classifier lands.
    CloudOk = 2,
};

[[nodiscard]] const char* ai_policy_name(AiPolicy p) noexcept;

// --- identity and integrity of a grant -------------------------------------

/// A per-gate sequence number identifying one consent grant.
///
/// NOT globally unique and NOT random. See the header's point 3: a random
/// 128-bit id would make this module depend on a CSPRNG, and the real
/// persisted consent store (NOT IMPLEMENTED) is where global uniqueness has
/// to come from anyway.
struct ConsentId final {
    std::uint64_t value = 0;

    [[nodiscard]] bool is_nil() const noexcept { return value == 0; }

    friend bool operator==(ConsentId a, ConsentId b) noexcept { return a.value == b.value; }
    friend bool operator!=(ConsentId a, ConsentId b) noexcept { return !(a == b); }
};

/// 128 bits of FNV-1a over the exact bytes of a message list, including each
/// message's role. **Not a cryptographic hash** — see the header's point 2.
struct ContentDigest final {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;

    friend bool operator==(ContentDigest a, ContentDigest b) noexcept {
        return a.lo == b.lo && a.hi == b.hi;
    }
    friend bool operator!=(ContentDigest a, ContentDigest b) noexcept { return !(a == b); }
};

/// Digest of a message list. Role is mixed in, so moving text from the
/// `Document` role to the `System` role changes the digest — the §3.2
/// injection boundary is part of what the user consented to.
[[nodiscard]] ContentDigest digest_messages(const std::vector<Message>& messages) noexcept;

/// What the grant is *about*. ai-architecture.md §13.2 step 1: a scope is a
/// document, a selection, or an explicitly named set — never "the workspace"
/// by default.
struct ConsentScope final {
    std::string workspace_ref;  ///< opaque to this module; an id or path the UI shows
    std::string object_ref;     ///< the document/selection this grant covers
};

// --- the prompt ------------------------------------------------------------

/// Everything ai-architecture.md §13.2 step 5 requires the prompt to state,
/// passed to the UI as data so the UI cannot omit a field by forgetting it.
struct ConsentPrompt final {
    std::string         provider_id;
    std::string         provider_display_name;  ///< NAMED. Never "our AI partner".
    Locality            provider_locality = Locality::ThirdParty;
    std::string         data_policy_url;        ///< the provider's retention terms
    ConsentScope        scope;
    std::size_t         content_bytes  = 0;     ///< exact size of what would leave
    std::size_t         approx_tokens  = 0;     ///< estimate; see below
    std::uint64_t       expires_at_ms  = 0;
    std::string         preview;                ///< bounded excerpt of what leaves
    bool                leaves_encrypted_storage = true;  ///< §13.2 step 5, verbatim claim
};

enum class ConsentDecision : std::uint8_t { Deny = 0, Allow = 1 };

/// The UI callback. Returning `Deny` is the correct behaviour for a caller
/// that cannot prompt; a null callback is treated as `Deny`, so "we forgot to
/// wire up the UI" fails closed rather than fails open.
using ConsentPromptFn = ConsentDecision (*)(const ConsentPrompt& prompt, void* user);

/// Produces the exact plaintext that would be sent.
///
/// Invoked by the gate ONLY after the policy check has passed, the user has
/// approved, and the audit record has been emitted — ai-architecture.md §13.2
/// steps 6 and 7 in that order. This callback is where a real client decrypts.
/// Returning non-Ok aborts the grant.
using PlaintextResolverFn = Status (*)(void* user, std::vector<Message>& out);

// --- the audit record ------------------------------------------------------

/// ai-architecture.md §4 step 6 / §13.2 step 6: references and counts, NEVER
/// content. Nothing in this struct is user text, which is the point.
struct ConsentRecord final {
    ConsentId      id;
    std::uint64_t  granted_at_ms = 0;
    std::uint64_t  expires_at_ms = 0;
    ConsentScope   scope;
    std::string    provider_id;
    Locality       provider_locality = Locality::ThirdParty;
    AiPolicy       policy_snapshot   = AiPolicy::Ask;
    ContentDigest  content_digest;
    std::size_t    content_bytes = 0;
    std::size_t    approx_tokens = 0;
    bool           revoked = false;
};

/// Where audit records go. Called BEFORE the plaintext resolver runs, so the
/// ordering "the record exists before the content is touched" is observable.
/// The real sink is the hash-chained log of security-model.md §10 —
/// NOT IMPLEMENTED; this is the seam it will be attached to.
using AuditSinkFn = void (*)(const ConsentRecord& record, void* user);

// --- the carrier -----------------------------------------------------------

/// Whether the content in a `Content` may leave the device.
enum class EgressScope : std::uint8_t {
    /// On-device providers only. The default, and what every public
    /// constructor produces.
    OnDeviceOnly = 0,
    /// A `ConsentGate` granted egress for a named provider, scope and TTL.
    /// Reachable ONLY through `ConsentToken::release()`.
    Authorized = 1,
};

class ConsentToken;
class ConsentGate;

/// The text any provider method operates on, plus its egress stamp.
///
/// Move-only on purpose: a `Content` is the in-memory plaintext of something
/// the user holds encrypted at rest, so duplicating it silently is exactly
/// what ai-architecture.md §13.2 step 7 ("a scratch buffer ... zeroed on
/// completion") is trying to prevent. Move-only also means one authorised
/// payload cannot be quietly handed to two call sites.
///
/// NOT ZEROED ON DESTRUCTION in this slice. §13.2 step 7 requires an
/// arena-allocated, never-swapped, zeroed-on-crash-recovery scratch buffer;
/// that needs `crypto::SecureBytes` (security/crypto), which this module
/// deliberately does not link. The gap is REAL and recorded in
/// ai-architecture.md §14 rather than papered over with a `memset` in a
/// destructor that `std::string` is free to have already moved out of.
class Content final {
public:
    /// An empty, on-device-only payload. Exists so `Content` can be an
    /// out-parameter; carries no authorisation.
    Content() = default;

    Content(const Content&)            = delete;
    Content& operator=(const Content&) = delete;
    Content(Content&&) noexcept        = default;
    Content& operator=(Content&&) noexcept = default;

    /// Content that may be given to `Locality::OnDevice` providers only.
    /// This is the ONLY public way to build a `Content`.
    [[nodiscard]] static Content on_device_only(std::vector<Message> messages);

    /// Convenience for the common single-message case.
    [[nodiscard]] static Content on_device_only(std::string_view text,
                                                Role role = Role::User);

    [[nodiscard]] const std::vector<Message>& messages() const noexcept { return messages_; }
    [[nodiscard]] EgressScope egress_scope() const noexcept { return scope_; }

    /// Total content bytes across all messages. O(n) in message count.
    [[nodiscard]] std::size_t byte_size() const noexcept;

    // --- authorisation metadata. All nil/empty unless `egress_scope()` is
    // `Authorized`. Read by AIProvider's non-virtual entry points and by
    // whatever writes the `ai.egress` record.
    [[nodiscard]] ConsentId     consent_id() const noexcept { return consent_id_; }
    [[nodiscard]] std::string_view authorized_provider_id() const noexcept {
        return authorized_provider_id_;
    }
    [[nodiscard]] std::uint64_t  expires_at_ms() const noexcept { return expires_at_ms_; }
    [[nodiscard]] ContentDigest  digest() const noexcept { return digest_; }

private:
    friend class ConsentToken;

    std::vector<Message> messages_;
    EgressScope          scope_ = EgressScope::OnDeviceOnly;
    ConsentId            consent_id_;
    std::string          authorized_provider_id_;
    std::uint64_t        expires_at_ms_ = 0;
    ContentDigest        digest_;
};

/// A single-use capability to turn resolved plaintext into egress-authorised
/// `Content`.
///
/// No public constructor; `ConsentGate` is the only thing that can make one.
/// Move-only and single-use, so a grant cannot be stashed and replayed: the
/// token IS the one decryption the user approved.
class ConsentToken final {
public:
    /// THE NULL TOKEN: already spent, bound to no gate, grants nothing.
    ///
    /// It exists only so a caller can declare an out-parameter before calling
    /// `ConsentGate::request_consent()`. It is NOT a hole in the design: the
    /// property that matters is "no publicly reachable constructor produces a
    /// token that can authorise anything", and a default-constructed token
    /// answers `spent() == true` and `release()` with
    /// `Status::AiConsentTokenSpent`. Both halves are static_asserted and
    /// runtime-tested in tests/ai/consent_test.cpp.
    ConsentToken() noexcept = default;

    ConsentToken(const ConsentToken&)            = delete;
    ConsentToken& operator=(const ConsentToken&) = delete;
    ConsentToken(ConsentToken&& other) noexcept;
    ConsentToken& operator=(ConsentToken&& other) noexcept;
    ~ConsentToken() = default;

    /// Consume this token, producing authorised `Content`.
    ///
    /// Fails with `Status::AiConsentTokenSpent` on a second call,
    /// `Status::AiConsentExpired` if the grant's TTL has passed by the gate's
    /// clock, and `Status::AiConsentRevoked` if the grant was revoked.
    [[nodiscard]] Status release(Content& out);

    [[nodiscard]] const ConsentRecord& record() const noexcept { return record_; }
    [[nodiscard]] bool spent() const noexcept { return spent_; }

private:
    friend class ConsentGate;

    ConsentToken(const ConsentGate* gate, ConsentRecord record,
                 std::vector<Message> plaintext) noexcept;

    const ConsentGate*   gate_ = nullptr;  ///< for the clock and the revocation check
    ConsentRecord        record_;
    std::vector<Message> plaintext_;

    /// Default TRUE, so the null token above is born spent. Only the private
    /// gate-only constructor clears it.
    bool spent_ = true;
};

/// ai-architecture.md §4 / §13.2, the half that does not need keys.
///
/// One gate per workspace. It owns the policy, the prompt seam, the audit
/// seam, the clock and the in-memory grant list. It is the only thing in the
/// process that can mint a `ConsentToken`.
class ConsentGate final {
public:
    struct Config final {
        AiPolicy          policy = AiPolicy::Ask;
        ConsentPromptFn   prompt = nullptr;  ///< null == always Deny (fails closed)
        void*             prompt_user = nullptr;
        AuditSinkFn       audit = nullptr;
        void*             audit_user = nullptr;
        ClockFn           clock = &wall_clock_ms;

        /// Default grant lifetime. ai-architecture.md §13.2: "per document,
        /// per provider, for this session". 15 minutes is a session in the
        /// sense that matters — long enough to run a multi-step operation on
        /// one document, short enough that a grant does not survive the user
        /// walking away. There is no "remember my choice".
        std::uint64_t default_ttl_ms = 15u * 60u * 1000u;

        /// Bytes of the resolved content shown in the prompt preview.
        /// WHY 512: enough for the user to recognise the document; short
        /// enough that the prompt is not itself a data leak into a log or a
        /// screenshot.
        std::size_t preview_bytes = 512;
    };

    /// What the caller must state before a prompt can be built. Everything
    /// here is shown to the user.
    struct Request final {
        ConsentScope scope;
        std::string  provider_id;
        std::string  provider_display_name;
        Locality     provider_locality = Locality::ThirdParty;
        std::string  data_policy_url;

        /// Size the caller resolved in §13.2 step 2 and will show the user.
        /// The gate requires the resolved plaintext to be EXACTLY this many
        /// bytes, or the grant fails with `AiConsentScopeMismatch`: the user
        /// approved sending a specific amount of a specific thing, and
        /// content that does not match what they were shown is not covered by
        /// their answer.
        std::size_t declared_content_bytes = 0;

        /// 0 means `Config::default_ttl_ms`.
        std::uint64_t ttl_ms = 0;
    };

    explicit ConsentGate(Config config);

    ConsentGate(const ConsentGate&)            = delete;
    ConsentGate& operator=(const ConsentGate&) = delete;

    /// ai-architecture.md §13.2 steps 3-7, in order:
    ///
    ///   1. `Locality::OnDevice` provider  -> `Status::InvalidArgument`. An
    ///      on-device call needs no grant, and minting one would create an
    ///      authorised payload nobody asked for.
    ///   2. policy == LocalOnly            -> `Status::AiPolicyBlocked`,
    ///      **without invoking `prompt`**. Fails closed.
    ///   3. prompt == nullptr              -> `Status::AiConsentDenied`.
    ///   4. prompt returns Deny            -> `Status::AiConsentDenied`,
    ///      **without invoking `resolver`**. The user said no, so the content
    ///      is never even resolved.
    ///   5. record the grant and emit it to `audit`.
    ///   6. invoke `resolver`; require `declared_content_bytes` to match.
    ///   7. hand back a single-use token bound to this grant.
    [[nodiscard]] Status request_consent(const Request& request,
                                         PlaintextResolverFn resolver,
                                         void* resolver_user,
                                         ConsentToken& out);

    /// Stop FUTURE egress under this grant. It cannot recall data already
    /// sent, and this API does not pretend it can — AIQ-7. A later
    /// `ConsentToken::release()` for a revoked grant returns
    /// `Status::AiConsentRevoked`.
    [[nodiscard]] Status revoke(ConsentId id) noexcept;

    [[nodiscard]] AiPolicy policy() const noexcept { return config_.policy; }
    [[nodiscard]] std::uint64_t now_ms() const noexcept;
    [[nodiscard]] const std::vector<ConsentRecord>& records() const noexcept {
        return records_;
    }

    /// True iff `id` names a live, un-revoked grant. Used by
    /// `ConsentToken::release()`.
    [[nodiscard]] bool grant_is_live(ConsentId id) const noexcept;

    /// Rough token count from a byte count. AIQ-6 forecloses a real
    /// tokenizer dependency, so this is bytes/4 — the standard English
    /// approximation — and is labelled "approx" everywhere it surfaces.
    /// A number shown to a user next to the word "approximately" is honest;
    /// the same number presented as exact would not be.
    [[nodiscard]] static std::size_t approx_tokens(std::size_t bytes) noexcept;

private:
    Config                     config_;
    std::vector<ConsentRecord> records_;
    std::uint64_t              next_id_ = 1;
};

}  // namespace uapp::ai

#endif  // UAPP_AI_PROVIDER_CONSENT_H
