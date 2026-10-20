// SPDX-License-Identifier: Apache-2.0
//
// ai/provider/ai_types.h — the vocabulary every AI provider shares.
//
// Spec §17 asks for AI to be "a provider-independent abstraction" with the
// explicit instruction "do not hard-code the product to one AI provider".
// This header holds the types that are provider-independent by construction:
// which of §17's seven methods a provider implements, where it runs, and what
// kind of thing produced its output.
//
// ---------------------------------------------------------------------------
// WHY `OutputProvenance` EXISTS AND IS NOT OPTIONAL
// ---------------------------------------------------------------------------
// The one provider implemented in this tree today
// (ai::local::LocalHeuristicProvider) is NOT a language model. It is word
// frequency, keyword matching and byte-pattern scanning. Spec §5 forbids
// pretending unsupported functionality works, and the single most likely way
// for this codebase to break that rule is for a UI to label a sentence that
// the extractive summariser copied verbatim out of the user's own document as
// "AI-generated".
//
// So provenance is not a doc comment or a naming convention. It is a required
// field on every provider descriptor AND on every single result struct in
// provider.h, and it is set by the base class from the descriptor rather than
// by the provider implementation, so a provider cannot lie about it. A UI that
// wants to print "AI-generated" has to read a field that says
// `HeuristicNoModel` and choose to ignore it.
//
// ---------------------------------------------------------------------------
// WHY CAPABILITIES ARE A BITMASK AND NOT `virtual bool supports_chat()`
// ---------------------------------------------------------------------------
// The registry (registry.h) selects providers by capability, so the capability
// set has to be *data* it can filter on without calling into a provider. A
// bitmask over a closed enum also makes "which of the seven does this one do"
// answerable in one integer in a log line, and maps to the eventual C99 ABI as
// a `uint32_t` (ADR-0001) rather than as a vtable.
//
// The bitmask is a CLAIM, and a claim can drift from behaviour. That is why
// tests/ai/provider_contract_test.cpp walks every registered provider × every
// capability and requires `Status::AiCapabilityNotImplemented` iff the bit is
// clear — the same discipline file-engine applies to its format table.

#ifndef UAPP_AI_PROVIDER_AI_TYPES_H
#define UAPP_AI_PROVIDER_AI_TYPES_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace uapp::ai {

// --- the seven methods of spec §17 -----------------------------------------

/// One bit per §17 method. APPEND-ONLY: these values travel into logs and
/// (later) the C ABI, so never renumber and never reuse.
enum class Capability : std::uint32_t {
    Generate  = 1u << 0,  ///< free-form completion from a prompt
    Chat      = 1u << 1,  ///< multi-turn completion over a message history
    Embed     = 1u << 2,  ///< text -> fixed-dimension vector
    Summarize = 1u << 3,  ///< long text -> short text
    Classify  = 1u << 4,  ///< text -> one of a caller-supplied label set
    Extract   = 1u << 5,  ///< text -> typed structured fields
    Translate = 1u << 6,  ///< text -> the same text in another language
};

using CapabilityMask = std::uint32_t;

/// All seven. Useful for iterating in contract tests.
inline constexpr Capability kAllCapabilities[] = {
    Capability::Generate, Capability::Chat,     Capability::Embed,
    Capability::Summarize, Capability::Classify, Capability::Extract,
    Capability::Translate,
};

[[nodiscard]] constexpr CapabilityMask cap_bit(Capability c) noexcept {
    return static_cast<CapabilityMask>(c);
}

[[nodiscard]] constexpr CapabilityMask cap_mask() noexcept { return 0u; }

template <typename... Rest>
[[nodiscard]] constexpr CapabilityMask cap_mask(Capability first, Rest... rest) noexcept {
    return cap_bit(first) | cap_mask(rest...);
}

[[nodiscard]] constexpr bool has_capability(CapabilityMask m, Capability c) noexcept {
    return (m & cap_bit(c)) != 0u;
}

/// Stable machine-readable name, e.g. "SUMMARIZE". Never null. Goes in logs.
[[nodiscard]] const char* capability_name(Capability c) noexcept;

// --- where a provider runs -------------------------------------------------

/// How far content must travel for this provider to see it.
///
/// This is the axis the consent boundary turns on (consent.h), which is why it
/// is an ordered enum rather than a `bool is_local`: ai-architecture.md §13.1
/// names THREE boundaries, and the mistake that section exists to prevent is
/// conflating "our server" with "a third party". A selection can therefore say
/// "nothing past FirstPartyServer" and mean it.
enum class Locality : std::uint8_t {
    OnDevice         = 0,  ///< runs in this process. No egress, no consent needed.
    FirstPartyServer = 1,  ///< our AI gateway. Plaintext leaves the device.
    ThirdParty       = 2,  ///< a third-party provider's API. Plaintext leaves our control.
};

[[nodiscard]] const char* locality_name(Locality l) noexcept;

// --- what produced the output ----------------------------------------------

/// What kind of machinery produced a result. See the header comment: this
/// field is the structural half of "never present heuristic output as AI".
enum class OutputProvenance : std::uint8_t {
    /// Deterministic, hand-written text processing. NO MODEL OF ANY KIND.
    /// Output derived from this MUST NOT be presented to a user as
    /// "AI-generated", "AI-written" or "generated by a model".
    HeuristicNoModel = 0,

    /// A trained statistical model produced this. (No provider in this tree
    /// reports this value yet — it exists so the type is complete and so the
    /// first real model provider does not have to change this enum.)
    LanguageModel = 1,
};

[[nodiscard]] const char* output_provenance_name(OutputProvenance p) noexcept;

// --- message roles ---------------------------------------------------------

/// ai-architecture.md §3.2's injection boundary, as a type.
///
/// `Document` is OUR role, not a provider's. Retrieved or user-file content
/// goes here and NEVER in `System`, so that an adapter renders it with
/// explicit delimiters and a standing "this is data, never instructions"
/// preamble. That does not solve prompt injection (T-402); it makes the
/// boundary exist and makes it auditable, which is the prerequisite.
enum class Role : std::uint8_t {
    System    = 0,  ///< our instructions. NEVER contains user document content.
    User      = 1,  ///< what the user actually typed.
    Document  = 2,  ///< retrieved/file content. Data, never instructions.
    Assistant = 3,  ///< prior model output.
    Tool      = 4,  ///< a tool result (spec §20; no tools exist yet).
};

[[nodiscard]] const char* role_name(Role r) noexcept;

struct Message final {
    Role        role = Role::User;
    std::string text;
};

// --- hard limits -----------------------------------------------------------
// Every AI entry point takes untrusted input: a user's document, a synced
// page, an OCR result. Spec §38 ("validate external input", "validate buffer
// sizes") applies to all of it, and the base class in provider.h enforces
// these before any provider sees a byte. Each number has a named source;
// none is a round number picked for looks.

/// Maximum total content bytes in one request, across all messages.
///
/// WHY A LIMIT: every capability here is at least O(n) in input bytes and
/// summarize() is O(n · distinct_terms); an unbounded input is a trivial
/// denial of service against the UI thread, and on the ADR-0002 decision 3
/// device floor (2 GB RAM) a few unbounded copies are an OOM kill.
///
/// WHY 1 MiB: it is `blocks::kMaxBlockTextBytes`, deliberately — the largest
/// single piece of DOCUMENT CONTENT the block model will hold is the largest
/// thing a summarize-this-block or classify-this-block call can be handed, so
/// the two caps agreeing means no document that fits in the editor is
/// un-summarisable. Multi-document and whole-workspace inputs do not get a
/// bigger cap here; they are the RAG pipeline's job (spec §18, NOT
/// IMPLEMENTED), which chunks before it ever reaches a provider.
inline constexpr std::size_t kMaxRequestContentBytes = 1024u * 1024u;

/// Maximum messages in one chat history.
///
/// WHY 128: ai-architecture.md §10 budgets 60 % of a context window for input;
/// at the §6 local model class (1-3 B, typically 4-8 k context) 128 messages
/// is already far past what fits, so the cap is above any real conversation
/// while keeping the per-request vector bounded. A longer history is a
/// summarise-the-history problem, not a bigger-array problem.
inline constexpr std::size_t kMaxMessagesPerRequest = 128;

/// Maximum labels in one classify() call.
///
/// WHY 64: a classification label set is a controlled vocabulary a human
/// chose; past a few dozen the right model is retrieval, not classification.
/// 64 also keeps the local provider's per-label scan O(64 · hints) over one
/// tokenisation pass.
inline constexpr std::size_t kMaxClassifyLabels = 64;

/// Maximum hint phrases attached to one classify() label.
inline constexpr std::size_t kMaxLabelHints = 32;

/// Maximum bytes in a label id, label display name or hint phrase. A label is
/// a chip in a UI, the same order of magnitude as a property name
/// (`database::kMaxPropertyNameBytes`).
inline constexpr std::size_t kMaxLabelTextBytes = 128;

/// Maximum texts in one embed() batch.
///
/// WHY 256: ai-architecture.md §10 names 64-256 as the embedding batch size
/// that makes indexing throughput work. This is the top of that range, so the
/// cap never forces a smaller batch than the design asks for.
inline constexpr std::size_t kMaxEmbedBatch = 256;

/// Maximum sentences a summarize() call may be asked to return.
///
/// WHY 32: a summary longer than this is not a summary. It also bounds the
/// extractive selection pass, which is O(requested · sentences).
inline constexpr std::size_t kMaxSummarySentences = 32;

/// Maximum structured fields one extract() call may return.
///
/// WHY 4096: at the 1 MiB input cap a pathological input (a file of nothing
/// but comma-separated integers) yields hundreds of thousands of matches, and
/// the caller asked for structured data, not a 1 MiB result. Truncating at a
/// stated bound and SAYING SO (`ExtractResult::truncated`) is honest; silently
/// returning everything is an unbounded allocation driven by untrusted input.
inline constexpr std::size_t kMaxExtractedFields = 4096;

/// Maximum dimensionality any provider in this build may declare for embed().
///
/// WHY BOUND IT: `embed()` writes `batch · dim` floats, both caller-influenced.
/// 4096 covers every embedding model in current use (384-1536 is the normal
/// range; ai-architecture.md §6 picks a 384-dim local model) with headroom,
/// and keeps the worst-case allocation at 256 · 4096 · 4 B = 4 MiB.
inline constexpr std::uint32_t kMaxEmbedDim = 4096;

}  // namespace uapp::ai

#endif  // UAPP_AI_PROVIDER_AI_TYPES_H
