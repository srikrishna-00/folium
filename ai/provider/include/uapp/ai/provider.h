// SPDX-License-Identifier: Apache-2.0
//
// ai/provider/provider.h — `AIProvider`, the spec §17 abstraction.
//
// ===========================================================================
// SPEC §17 ASKS FOR EXACTLY SEVEN METHODS. THIS IS THEM.
// ===========================================================================
//   generate() chat() embed() summarize() classify() extract() translate()
//
// ---------------------------------------------------------------------------
// FLAGGED FOR REVIEW: this CONTRADICTS ai-architecture.md §3 as written
// ---------------------------------------------------------------------------
// ai-architecture.md §3 (Phase 0 draft) argues: "Making all seven provider
// primitives is wrong: no provider implements `summarize` natively, so every
// adapter would reimplement the same prompt", and splits the seven into five
// provider primitives (chat/embed/rerank/health/count_tokens) plus four "task
// services" built on top.
//
// That argument is RIGHT about duplication and WRONG about where the
// interface boundary goes, and this header takes the other fork. Three
// reasons, in order of weight:
//
//  1. **The §3 split is only correct for providers whose single primitive is
//     text completion.** The one provider that exists in this tree is not one
//     of those. `ai::local::LocalHeuristicProvider` implements summarize,
//     classify and extract NATIVELY — they are its whole point — and
//     implements chat NOT AT ALL. Under §3's shape it would have to expose a
//     `chat()` it cannot perform so that a task service could build
//     `summarize()` on top of it, which is the exact inversion of the truth.
//     An abstraction that cannot describe its only implementation is the wrong
//     abstraction.
//  2. **A non-LLM provider is not a hypothetical.** ONNX classifiers,
//     extractive summarisers and OCR-adjacent field extractors are all real,
//     all useful on the ADR-0002 decision 3 device floor, and none of them
//     chat. Spec §17's instruction is "do not hard-code the product to one AI
//     provider"; hard-coding it to one provider *shape* is the same mistake
//     one level up.
//  3. **§3's duplication concern is solved by a base class, not by a smaller
//     interface.** The right home for "summarize via a chat round-trip with
//     our prompt template" is a `ChatBackedProvider` intermediate base that a
//     completion-only adapter inherits — write the prompt once, inherit it
//     seven times. That base is NOT IMPLEMENTED here, because it is a prompt
//     template and a response parser with no model to run them against, and
//     writing it blind would be the "pretend it works" failure spec §5
//     forbids. It is recorded in ai-architecture.md §14 as the first thing the
//     first real generative adapter must bring with it.
//
// `rerank`, `health` and `count_tokens` from §3's list are kept, because those
// three are real and §17's seven do not cover them: `health()` is what the
// registry filters on, and the other two are deferred (see the bottom of this
// header). ai-architecture.md §3 is amended rather than ignored — see §14 of
// that document.
//
// ---------------------------------------------------------------------------
// THE SHAPE: NON-VIRTUAL PUBLIC, PURE VIRTUAL PROTECTED
// ---------------------------------------------------------------------------
// The seven public methods are **not virtual**. Each one runs a fixed
// preamble, then dispatches to a protected pure virtual:
//
//     Status AIProvider::summarize(req, out)          <- not overridable
//       1. structural validation      (null out-params, label sets, bounds)
//       2. untrusted-input validation (empty, oversized, NUL, control bytes)
//       3. EGRESS AUTHORISATION       (consent.h — the security property)
//       4. do_summarize(req, out)                     <- the override point
//       5. OUTPUT validation          (the provider's own contract)
//       6. stamp provenance from the descriptor       <- provider cannot lie
//
// Steps 3 and 6 are why this is a non-virtual wrapper and not a convention.
// A provider cannot override its way around the consent check, and cannot
// claim its heuristic output came from a language model. Step 5 exists because
// a provider is ALSO untrusted input once there is a network adapter: a cloud
// response is bytes off a socket, and "the model returned a label that was not
// in my label set" has to be `Status::AiResponseMalformed` at the boundary
// rather than a surprise three layers up.
//
// The seven `do_*` are PURE virtual, with no default. That is deliberate and
// costs every provider seven one-line bodies for the capabilities it does not
// have. The alternative — defaults returning `AiCapabilityNotImplemented` —
// lets a provider author add a provider without ever thinking about four of
// the seven methods, and this project's whole NOT-IMPLEMENTED discipline is
// that absence is declared, not defaulted. Compare `database::PropertyType`:
// the seven unimplemented property types are *listed*, with reasons.
//
// The `capabilities` bitmask in the descriptor is a CLAIM about which `do_*`
// do something. It and the behaviour can drift, so they are checked against
// each other: tests/ai/provider_contract_test.cpp calls all seven methods on
// every provider and requires `AiCapabilityNotImplemented` iff the bit is
// clear. The base class does NOT short-circuit on the bitmask, precisely so
// that the behaviour is what the test observes rather than the claim.

#ifndef UAPP_AI_PROVIDER_PROVIDER_H
#define UAPP_AI_PROVIDER_PROVIDER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "uapp/ai/ai_types.h"
#include "uapp/ai/consent.h"
#include "uapp/status.h"

namespace uapp::ai {

// --- provider identity -----------------------------------------------------

/// Everything the registry and the UI need to know about a provider without
/// calling it. Flat, copyable, and C-ABI-shaped (ADR-0001) apart from the
/// std::strings, which the eventual ABI will render as `const char*`.
struct ProviderDescriptor final {
    /// Stable, unique, `vendor:model`-shaped. Goes in logs, in the user's
    /// "which model" setting, and in consent records.
    std::string id;

    /// What the UI shows. ai-architecture.md §13.2 step 5 requires a NAMED
    /// provider in the consent prompt, never "our AI partner".
    std::string display_name;

    /// Which of §17's seven this provider claims. See the header: a claim,
    /// cross-checked against behaviour by the contract test.
    CapabilityMask capabilities = 0;

    /// How far content travels. The axis the consent boundary turns on.
    Locality locality = Locality::OnDevice;

    /// What kind of machinery produces the output. The base class copies this
    /// into every result, so a provider cannot present heuristics as a model.
    OutputProvenance provenance = OutputProvenance::HeuristicNoModel;

    /// Embedding dimensionality, or 0 if `Capability::Embed` is not claimed.
    ///
    /// PROVIDER-DEFINED, NOT FIXED BY THIS INTERFACE — and that is a decision,
    /// not an omission. Fixing a dimension here would either exclude every
    /// model that disagrees or force a projection layer that destroys the
    /// vectors' meaning. What the interface fixes instead is that the number
    /// is DECLARED, bounded by `kMaxEmbedDim`, and verified: the base class
    /// rejects an `embed()` result whose width is not this value.
    ///
    /// The one-embedding-model-per-corpus constraint (AIQ-5, ai-architecture
    /// §13.4: vectors from two models are in different spaces and must never
    /// be compared) is a VECTOR STORE invariant, not a provider-interface one.
    /// The vector store does not exist yet (spec §18, NOT IMPLEMENTED); when
    /// it does, it stores `(embedding_model_id, dim)` alongside every vector
    /// and refuses to mix them. Recording that here so the next person does
    /// not "fix" this field into a constant.
    std::uint32_t embed_dim = 0;

    /// Input context window in tokens, or 0 if the concept does not apply
    /// (a non-model provider has no context window — it has `kMaxRequest-
    /// ContentBytes`, which the base class enforces regardless).
    std::uint32_t context_tokens = 0;

    /// Maximum tokens this provider will emit, or 0 if not applicable.
    std::uint32_t max_output_tokens = 0;

    /// True iff identical input yields identical output, always. A heuristic
    /// provider is deterministic; a sampling language model is not. Caching
    /// (ai-architecture.md §10) and differential testing both need to know.
    bool deterministic = false;

    /// The provider's published retention/training policy. REQUIRED to be
    /// non-empty when `locality != OnDevice`: the consent prompt has to show
    /// it, and a provider that cannot state its retention terms cannot be
    /// consented to. Enforced by `validate_descriptor()`.
    std::string data_policy_url;
};

/// Rejects a descriptor that cannot be honoured. Called by the registry on
/// every registration, so a malformed provider cannot be selected.
[[nodiscard]] Status validate_descriptor(const ProviderDescriptor& d) noexcept;

/// ai-architecture.md §5's health input, minus the circuit breaker (which is
/// router state, not provider state, and is NOT IMPLEMENTED).
enum class Health : std::uint8_t {
    Healthy     = 0,
    Degraded    = 1,  ///< usable but slow or reduced; still selectable
    Unavailable = 2,  ///< not usable right now; the registry skips it
};

// --- what a result carries in common ---------------------------------------

/// Attached to every result. The provenance field is set by the base class
/// from the descriptor, never by the provider.
struct ResultMeta final {
    std::string      provider_id;
    OutputProvenance provenance = OutputProvenance::HeuristicNoModel;

    /// Token accounting. ai-architecture.md §10 budgets and bills on these,
    /// and §4 step 6's audit record carries an approximate count.
    ///
    /// `tokens_are_estimates` is TRUE whenever the numbers came from the
    /// bytes/4 approximation rather than from a provider's own accounting
    /// (AIQ-6 forecloses a tokenizer dependency). A billing or quota path must
    /// read this flag; treating an estimate as exact is how a quota system
    /// starts lying to users.
    std::size_t input_tokens  = 0;
    std::size_t output_tokens = 0;
    bool        tokens_are_estimates = true;

    /// The consent grant this call ran under, nil for an on-device call.
    ConsentId consent_id;
};

// --- requests and results, one pair per §17 method --------------------------
//
// Every request carries `const Content* content` rather than a `Content` by
// value: `Content` is move-only (consent.h explains why), and a request that
// owns it would force a move at every call site and could not be reused for a
// second call within one consent grant's TTL. A null pointer is
// `Status::InvalidArgument`, checked before anything else.

struct GenerateRequest final {
    const Content* content = nullptr;  ///< typically one User message
    std::uint32_t  max_output_tokens = 0;  ///< 0 = provider default
    float          temperature = 0.0F;
};

struct GenerateResult final {
    std::string text;
    ResultMeta  meta;
};

struct ChatRequest final {
    const Content* content = nullptr;  ///< the full message history, in order
    std::uint32_t  max_output_tokens = 0;
    float          temperature = 0.0F;
};

struct ChatResult final {
    Message    reply;  ///< always Role::Assistant; the base class verifies that
    ResultMeta meta;
};

struct EmbedRequest final {
    /// One `Content` holding one message per text to embed. Modelling a batch
    /// as messages rather than as a `vector<string>` keeps ONE consent
    /// boundary for the whole batch: a batch of 256 chunks is 256 pieces of
    /// the user's plaintext, and giving it a shape that bypasses `Content`
    /// would be the hole the whole design exists to close.
    const Content* content = nullptr;
};

struct EmbedResult final {
    /// Row-major, `count * dim` floats. `dim` is the provider's declared
    /// `embed_dim` and the base class rejects any other width.
    std::vector<float> vectors;
    std::size_t        count = 0;
    std::uint32_t      dim   = 0;
    ResultMeta         meta;

    /// Vector `i` as a `(begin, end)` span into `vectors`. Returns an empty
    /// span for an out-of-range index rather than reading past the end.
    [[nodiscard]] const float* row(std::size_t i) const noexcept;
};

struct SummarizeRequest final {
    const Content* content = nullptr;

    /// How many sentences (extractive) or roughly how many sentences
    /// (abstractive) the summary should hold. Clamped to
    /// `kMaxSummarySentences`; 0 means "the provider chooses".
    std::size_t max_sentences = 3;
};

struct SummarizeResult final {
    std::string text;

    /// TRUE iff every sentence in `text` is a verbatim span of the input.
    ///
    /// This is not decoration. An extractive summary of someone's private
    /// document is THEIR OWN WORDS, selected — it is not "AI-generated text"
    /// and must not be labelled as such, and it is also safe to show with
    /// provenance highlights because every sentence is locatable in the
    /// source. An abstractive summary is new text and needs the opposite
    /// treatment. A UI cannot make that distinction without being told.
    bool is_extractive = false;

    /// For an extractive summary: byte offsets of the selected sentences in
    /// the concatenated input, in document order. Empty when
    /// `is_extractive` is false. This is the groundwork spec §18's citation
    /// verification needs: a span that can be checked against the source.
    struct Span final {
        std::size_t offset = 0;
        std::size_t length = 0;
    };
    std::vector<Span> source_spans;

    ResultMeta meta;
};

/// One candidate label for classify(). The caller supplies the vocabulary,
/// because that is what makes the capability provider-independent: a
/// generative provider puts `display_name` + `hints` in a prompt, a heuristic
/// provider matches `hints` literally, an ONNX classifier maps `id` to an
/// output index. None of them owns the vocabulary.
struct ClassifyLabel final {
    std::string              id;            ///< stable, machine-readable
    std::string              display_name;  ///< shown to the user
    std::vector<std::string> hints;         ///< keywords/phrases characterising it
};

struct ClassifyRequest final {
    const Content*                   content = nullptr;
    std::vector<ClassifyLabel>       labels;

    /// Below this confidence the provider must ABSTAIN rather than return its
    /// best guess. Default 0.0 means "never abstain on confidence alone".
    float min_confidence = 0.0F;
};

struct ClassifyResult final {
    /// Winning label id, or EMPTY if the provider abstained. An empty id with
    /// `Status::Ok` is a valid, meaningful answer: "I do not know." Returning
    /// a wrong label confidently is worse than returning none, which is the
    /// same argument `database::property.h` makes about a string that parses
    /// as a number.
    std::string label_id;
    float       confidence = 0.0F;  ///< in [0, 1]
    bool        abstained  = false;

    /// Score for every label the caller offered, same order as the request.
    /// Present so a UI can show "why", per ai-architecture.md §5's
    /// explainability requirement.
    std::vector<float> label_scores;

    ResultMeta meta;
};

/// Kinds of structured field extract() can be asked for. A bitmask for the
/// same reason capabilities are. APPEND-ONLY.
enum class FieldKind : std::uint32_t {
    Email   = 1u << 0,
    Url     = 1u << 1,
    IsoDate = 1u << 2,  ///< YYYY-MM-DD / YYYY/MM/DD, calendar-validated
    Number  = 1u << 3,  ///< integer or decimal, optional sign and separators
    Money   = 1u << 4,  ///< a Number with a recognised currency symbol
};

using FieldKindMask = std::uint32_t;

[[nodiscard]] constexpr FieldKindMask field_bit(FieldKind k) noexcept {
    return static_cast<FieldKindMask>(k);
}
[[nodiscard]] constexpr bool has_field_kind(FieldKindMask m, FieldKind k) noexcept {
    return (m & field_bit(k)) != 0u;
}
[[nodiscard]] const char* field_kind_name(FieldKind k) noexcept;

inline constexpr FieldKindMask kAllFieldKinds =
    field_bit(FieldKind::Email) | field_bit(FieldKind::Url) |
    field_bit(FieldKind::IsoDate) | field_bit(FieldKind::Number) |
    field_bit(FieldKind::Money);

struct ExtractedField final {
    FieldKind   kind = FieldKind::Email;
    std::string value;       ///< exactly as it appeared in the source
    std::string normalized;  ///< canonical form (lowercased email, ISO date, …)
    std::size_t offset = 0;  ///< byte offset in the concatenated input
    std::size_t length = 0;
};

struct ExtractRequest final {
    const Content* content = nullptr;
    FieldKindMask  kinds   = kAllFieldKinds;
};

struct ExtractResult final {
    std::vector<ExtractedField> fields;  ///< in source order

    /// TRUE iff `kMaxExtractedFields` was hit and matches were dropped. The
    /// caller is TOLD rather than silently handed a partial answer.
    bool truncated = false;

    ResultMeta meta;
};

struct TranslateRequest final {
    const Content* content = nullptr;
    std::string    target_language;  ///< BCP-47, e.g. "fr", "pt-BR"
    std::string    source_language;  ///< empty = detect
};

struct TranslateResult final {
    std::string text;
    std::string detected_source_language;
    ResultMeta  meta;
};

// --- the interface ---------------------------------------------------------

/// A provider of spec §17's seven AI capabilities.
///
/// Abstract: `descriptor()`, `health()` and all seven `do_*` are pure virtual.
/// Implementations live in `ai/local` (and, later, in a network adapter that
/// this environment cannot build or test — see the module doc).
class AIProvider {
public:
    virtual ~AIProvider() = default;

    AIProvider(const AIProvider&)            = delete;
    AIProvider& operator=(const AIProvider&) = delete;

    /// Must return a reference stable for the provider's lifetime: the
    /// registry holds descriptors by pointer.
    [[nodiscard]] virtual const ProviderDescriptor& descriptor() const noexcept = 0;

    /// Cheap, non-blocking, no network. The registry filters on it.
    [[nodiscard]] virtual Health health() const noexcept = 0;

    // --- spec §17's seven. NON-VIRTUAL. See the header comment. ------------
    [[nodiscard]] Status generate(const GenerateRequest& req, GenerateResult& out);
    [[nodiscard]] Status chat(const ChatRequest& req, ChatResult& out);
    [[nodiscard]] Status embed(const EmbedRequest& req, EmbedResult& out);
    [[nodiscard]] Status summarize(const SummarizeRequest& req, SummarizeResult& out);
    [[nodiscard]] Status classify(const ClassifyRequest& req, ClassifyResult& out);
    [[nodiscard]] Status extract(const ExtractRequest& req, ExtractResult& out);
    [[nodiscard]] Status translate(const TranslateRequest& req, TranslateResult& out);

protected:
    /// `clock` is used only to check consent-grant expiry at the provider
    /// boundary. It is injectable because TTL enforcement is otherwise
    /// untestable without sleeping.
    explicit AIProvider(ClockFn clock = &wall_clock_ms) noexcept : clock_(clock) {}

    // --- the override points. Pure virtual on purpose: see the header. ----
    //
    // A provider that does not implement one returns
    // `Status::AiCapabilityNotImplemented` in a one-line body and clears the
    // matching capability bit in its descriptor. It does NOT return a canned
    // string: spec §5 forbids pretending unsupported functionality works, and
    // a fabricated "summary" is the most damaging possible form of that.
    //
    // Everything these see has already been validated: `req.content` is
    // non-null, within bounds, free of NUL and disallowed control bytes, and
    // authorised for this provider's locality.
    [[nodiscard]] virtual Status do_generate(const GenerateRequest&, GenerateResult&) = 0;
    [[nodiscard]] virtual Status do_chat(const ChatRequest&, ChatResult&) = 0;
    [[nodiscard]] virtual Status do_embed(const EmbedRequest&, EmbedResult&) = 0;
    [[nodiscard]] virtual Status do_summarize(const SummarizeRequest&, SummarizeResult&) = 0;
    [[nodiscard]] virtual Status do_classify(const ClassifyRequest&, ClassifyResult&) = 0;
    [[nodiscard]] virtual Status do_extract(const ExtractRequest&, ExtractResult&) = 0;
    [[nodiscard]] virtual Status do_translate(const TranslateRequest&, TranslateResult&) = 0;

private:
    /// Steps 1-3 of the preamble: structural checks, untrusted-input checks,
    /// and the egress-authorisation check. Non-virtual, private, and the only
    /// path to any `do_*`.
    [[nodiscard]] Status admit(const Content* content) const noexcept;

    /// Step 6: overwrite whatever the provider put in `meta` with the truth.
    void stamp(ResultMeta& meta, const Content& content, std::size_t output_bytes) const;

    ClockFn clock_;
};

// ===========================================================================
// NOT IMPLEMENTED in this slice, and deliberately not given API surface
// ===========================================================================
//  * **Streaming.** ai-architecture.md §2 and §3 put an `uapp_ai_stream_cb` in
//    the primitive signature. It is absent here because the only provider is
//    synchronous and deterministic, and a streaming callback with nothing that
//    can stream would be an untested parameter on seven methods. Adding it is
//    a signature change the first generative adapter must make, together with
//    the cancellation seam §2 ("everything is a job") needs.
//  * **`rerank()`, `count_tokens()`.** §3 lists both. `rerank` has no caller
//    until the RAG retriever exists (spec §18); `count_tokens` cannot be
//    implemented honestly without the tokenizer AIQ-6 forecloses, and
//    `ConsentGate::approx_tokens()` is the stated approximation in the
//    meantime, labelled as one.
//  * **Tool calling / JSON-schema output.** Spec §20's agent framework needs
//    both. No tool registry exists, so neither is modelled.
//  * **`list_models()` per provider.** §3 has providers expose several models.
//    Here one provider == one model; the registry is the multi-model surface.
//    This is the simplification most likely to need revisiting for a cloud
//    adapter, and is recorded as such in ai-architecture.md §14.
//  * **The policy GATE's quota and content-classification steps** (§4 steps 3
//    and 5). `ConsentGate` implements policy, prompt, audit and grant; it does
//    not implement the credential/PII classifier or quota accounting.
//  * **RAG (spec §18) and the agent framework (spec §20)** in their entirety.
//    This module is what they will be built on.

}  // namespace uapp::ai

#endif  // UAPP_AI_PROVIDER_PROVIDER_H
