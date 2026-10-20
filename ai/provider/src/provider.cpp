// SPDX-License-Identifier: Apache-2.0
#include "uapp/ai/provider.h"

#include <cmath>

namespace uapp::ai {
namespace {

/// Bytes we refuse to carry into any provider.
///
/// A NUL is rejected because every downstream consumer of this text is either
/// a C string (the eventual ABI), a prompt serialiser, or a JSON encoder, and
/// a NUL truncates the first and corrupts the other two. The same argument
/// `conversion::ConversionInputMalformed` makes for Markdown.
///
/// Other C0 control bytes are rejected because they do nothing a document
/// needs and several things a prompt does not survive: ESC begins a terminal
/// escape sequence in any log that echoes this text, and the C1 controls
/// smuggled as UTF-8 are a known prompt-injection vector. TAB, LF and CR are
/// allowed, because they are document structure.
///
/// DEL (0x7F) is allowed through: it is not a C0 control, it occurs in real
/// legacy text, and it has none of the above properties.
[[nodiscard]] bool is_disallowed_control(unsigned char c) noexcept {
    if (c >= 0x20u) { return false; }
    return c != '\t' && c != '\n' && c != '\r';
}

[[nodiscard]] bool is_space(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

[[nodiscard]] Status validate_label_set(const std::vector<ClassifyLabel>& labels) noexcept {
    if (labels.empty() || labels.size() > kMaxClassifyLabels) {
        return Status::AiLabelSetInvalid;
    }
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const ClassifyLabel& l = labels[i];
        if (l.id.empty() || l.id.size() > kMaxLabelTextBytes ||
            l.display_name.size() > kMaxLabelTextBytes ||
            l.hints.size() > kMaxLabelHints) {
            return Status::AiLabelSetInvalid;
        }
        for (const std::string& h : l.hints) {
            if (h.empty() || h.size() > kMaxLabelTextBytes) {
                return Status::AiLabelSetInvalid;
            }
        }
        // Duplicate ids make `label_scores` ambiguous and make a returned
        // `label_id` un-attributable. O(n^2) over at most 64 entries.
        for (std::size_t j = 0; j < i; ++j) {
            if (labels[j].id == l.id) {
                return Status::AiLabelSetInvalid;
            }
        }
    }
    return Status::Ok;
}

}  // namespace

const float* EmbedResult::row(std::size_t i) const noexcept {
    if (dim == 0 || i >= count) { return nullptr; }
    const std::size_t offset = i * static_cast<std::size_t>(dim);
    if (offset + dim > vectors.size()) { return nullptr; }
    return vectors.data() + offset;
}

const char* field_kind_name(FieldKind k) noexcept {
    switch (k) {
        case FieldKind::Email:   return "EMAIL";
        case FieldKind::Url:     return "URL";
        case FieldKind::IsoDate: return "ISO_DATE";
        case FieldKind::Number:  return "NUMBER";
        case FieldKind::Money:   return "MONEY";
    }
    return "UNKNOWN_FIELD_KIND";
}

Status validate_descriptor(const ProviderDescriptor& d) noexcept {
    if (d.id.empty() || d.id.size() > 128 || d.display_name.empty()) {
        return Status::InvalidArgument;
    }

    // A capability bit with no way to honour it is the "claim with no code
    // path" failure, caught at registration rather than at call time.
    if (has_capability(d.capabilities, Capability::Embed)) {
        if (d.embed_dim == 0 || d.embed_dim > kMaxEmbedDim) {
            return Status::InvalidArgument;
        }
    } else if (d.embed_dim != 0) {
        return Status::InvalidArgument;
    }

    // ai-architecture.md §13.2 step 5 requires the consent prompt to show the
    // provider's retention terms. A provider that cannot state them cannot be
    // consented to, so it must not be registerable: refusing here is the only
    // place that check cannot be forgotten.
    if (d.locality != Locality::OnDevice && d.data_policy_url.empty()) {
        return Status::InvalidArgument;
    }

    // An on-device provider that claims LanguageModel provenance is legal
    // (a local llama.cpp adapter is exactly that). The combination that is
    // NOT legal is left unchecked on purpose: there is no descriptor shape
    // that proves provenance, which is precisely why provenance is a declared
    // field the base class propagates rather than something inferred.
    return Status::Ok;
}

// --- the non-overridable preamble ------------------------------------------

Status AIProvider::admit(const Content* content) const noexcept {
    if (content == nullptr) {
        return Status::InvalidArgument;
    }

    const ProviderDescriptor& d = descriptor();
    const std::vector<Message>& msgs = content->messages();

    // EGRESS AUTHORISATION FIRST, before any complaint about the content's
    // shape. Two reasons: an unauthorised off-device call must be reported as
    // unauthorised whatever the content looks like, and the reverse ordering
    // would let a caller probe an off-device provider's input validation
    // without ever obtaining consent.
    if (d.locality != Locality::OnDevice) {
        if (content->egress_scope() != EgressScope::Authorized) {
            return Status::AiConsentRequired;
        }
        if (content->authorized_provider_id() != d.id) {
            return Status::AiConsentScopeMismatch;
        }
        // Defence in depth. `Content` has no mutators today, so this can only
        // fire if one is added later — which is exactly when it should fire.
        if (digest_messages(msgs) != content->digest()) {
            return Status::AiConsentScopeMismatch;
        }
        if (clock_() > content->expires_at_ms()) {
            return Status::AiConsentExpired;
        }
    }

    // --- untrusted input (spec §38) ---------------------------------------
    if (msgs.empty()) {
        return Status::AiInputEmpty;
    }
    if (msgs.size() > kMaxMessagesPerRequest) {
        return Status::AiInputTooLarge;
    }

    std::size_t total = 0;
    bool has_visible  = false;
    for (const Message& m : msgs) {
        total += m.text.size();
        if (total > kMaxRequestContentBytes) {
            return Status::AiInputTooLarge;
        }
        for (const char raw : m.text) {
            const auto c = static_cast<unsigned char>(raw);
            if (is_disallowed_control(c)) {
                return Status::AiInputMalformed;
            }
            if (!is_space(c)) {
                has_visible = true;
            }
        }
    }
    if (!has_visible) {
        // Whitespace-only is not "a small input", it is no input. Every
        // capability here would otherwise return a confident empty answer.
        return Status::AiInputEmpty;
    }
    return Status::Ok;
}

void AIProvider::stamp(ResultMeta& meta, const Content& content,
                       std::size_t output_bytes) const {
    const ProviderDescriptor& d = descriptor();
    meta.provider_id = d.id;
    // Overwritten, not defaulted. Whatever the provider wrote here is
    // discarded: provenance is the descriptor's to state, not the call's.
    meta.provenance           = d.provenance;
    meta.consent_id           = content.consent_id();
    meta.input_tokens         = ConsentGate::approx_tokens(content.byte_size());
    meta.output_tokens        = ConsentGate::approx_tokens(output_bytes);
    meta.tokens_are_estimates = true;
}

// --- the seven -------------------------------------------------------------

Status AIProvider::generate(const GenerateRequest& req, GenerateResult& out) {
    out = GenerateResult{};
    if (const Status s = admit(req.content); !is_ok(s)) { return s; }

    const Status s = do_generate(req, out);
    if (!is_ok(s)) { return s; }

    if (out.text.empty()) {
        // Ok with nothing in it is a broken provider, not an empty answer.
        return Status::AiResponseMalformed;
    }
    stamp(out.meta, *req.content, out.text.size());
    return Status::Ok;
}

Status AIProvider::chat(const ChatRequest& req, ChatResult& out) {
    out = ChatResult{};
    if (const Status s = admit(req.content); !is_ok(s)) { return s; }

    const Status s = do_chat(req, out);
    if (!is_ok(s)) { return s; }

    // The reply must be an assistant turn. A provider that labels its own
    // output `System` or `Document` would corrupt the §3.2 injection boundary
    // on the next round trip, when that history is replayed.
    if (out.reply.role != Role::Assistant || out.reply.text.empty()) {
        return Status::AiResponseMalformed;
    }
    stamp(out.meta, *req.content, out.reply.text.size());
    return Status::Ok;
}

Status AIProvider::embed(const EmbedRequest& req, EmbedResult& out) {
    out = EmbedResult{};
    if (const Status s = admit(req.content); !is_ok(s)) { return s; }
    if (req.content->messages().size() > kMaxEmbedBatch) {
        return Status::AiInputTooLarge;
    }

    const Status s = do_embed(req, out);
    if (!is_ok(s)) { return s; }

    // The provider declared a dimension in its descriptor. A vector store
    // mixing two widths, or holding one NaN, is corrupt in a way that shows
    // up much later as wrong search results, so it is rejected here.
    const ProviderDescriptor& d = descriptor();
    if (out.dim != d.embed_dim || out.count != req.content->messages().size()) {
        return Status::AiResponseMalformed;
    }
    if (out.vectors.size() != out.count * static_cast<std::size_t>(out.dim)) {
        return Status::AiResponseMalformed;
    }
    for (const float v : out.vectors) {
        if (!std::isfinite(v)) {
            return Status::AiResponseMalformed;
        }
    }
    stamp(out.meta, *req.content, 0);
    return Status::Ok;
}

Status AIProvider::summarize(const SummarizeRequest& req, SummarizeResult& out) {
    out = SummarizeResult{};
    if (const Status s = admit(req.content); !is_ok(s)) { return s; }

    const Status s = do_summarize(req, out);
    if (!is_ok(s)) { return s; }

    if (out.text.empty()) {
        return Status::AiResponseMalformed;
    }
    if (out.source_spans.size() > kMaxSummarySentences) {
        return Status::AiResponseMalformed;
    }
    if (!out.is_extractive && !out.source_spans.empty()) {
        // Spans claim "this text is verbatim at this offset". An abstractive
        // summary cannot make that claim, and letting it would feed spec §18's
        // citation verifier a citation that does not mean what it says.
        return Status::AiResponseMalformed;
    }
    const std::size_t input_bytes = req.content->byte_size();
    for (const SummarizeResult::Span& sp : out.source_spans) {
        if (sp.length == 0 || sp.offset > input_bytes ||
            sp.length > input_bytes - sp.offset) {
            return Status::AiResponseMalformed;
        }
    }
    stamp(out.meta, *req.content, out.text.size());
    return Status::Ok;
}

Status AIProvider::classify(const ClassifyRequest& req, ClassifyResult& out) {
    out = ClassifyResult{};
    if (const Status s = admit(req.content); !is_ok(s)) { return s; }
    if (const Status s = validate_label_set(req.labels); !is_ok(s)) { return s; }
    if (req.min_confidence < 0.0F || req.min_confidence > 1.0F) {
        return Status::InvalidArgument;
    }

    const Status s = do_classify(req, out);
    if (!is_ok(s)) { return s; }

    if (!std::isfinite(out.confidence) || out.confidence < 0.0F || out.confidence > 1.0F) {
        return Status::AiResponseMalformed;
    }
    if (!out.label_scores.empty() && out.label_scores.size() != req.labels.size()) {
        return Status::AiResponseMalformed;
    }
    for (const float sc : out.label_scores) {
        if (!std::isfinite(sc)) { return Status::AiResponseMalformed; }
    }
    if (out.abstained) {
        if (!out.label_id.empty()) { return Status::AiResponseMalformed; }
    } else {
        // THE CHECK THAT MATTERS FOR A GENERATIVE PROVIDER: a language model
        // asked to pick from a list will sometimes return a label that is not
        // on it, spelled almost right. Accepting that is how an invented
        // category reaches a user's document. It is AiResponseMalformed here,
        // at the boundary.
        bool in_set = false;
        for (const ClassifyLabel& l : req.labels) {
            if (l.id == out.label_id) { in_set = true; break; }
        }
        if (!in_set) { return Status::AiResponseMalformed; }
    }
    stamp(out.meta, *req.content, out.label_id.size());
    return Status::Ok;
}

Status AIProvider::extract(const ExtractRequest& req, ExtractResult& out) {
    out = ExtractResult{};
    if (const Status s = admit(req.content); !is_ok(s)) { return s; }
    if (req.kinds == 0 || (req.kinds & ~kAllFieldKinds) != 0) {
        return Status::InvalidArgument;
    }

    const Status s = do_extract(req, out);
    if (!is_ok(s)) { return s; }

    if (out.fields.size() > kMaxExtractedFields) {
        return Status::AiResponseMalformed;
    }
    const std::size_t input_bytes = req.content->byte_size();
    std::size_t output_bytes = 0;
    for (const ExtractedField& f : out.fields) {
        // Every field claims a span in the input. A span outside it, or a
        // kind the caller did not ask for, is a fabricated field — the
        // extract() analogue of an unverifiable citation.
        if (f.length == 0 || f.offset > input_bytes ||
            f.length > input_bytes - f.offset) {
            return Status::AiResponseMalformed;
        }
        if (!has_field_kind(req.kinds, f.kind)) {
            return Status::AiResponseMalformed;
        }
        output_bytes += f.value.size() + f.normalized.size();
    }
    stamp(out.meta, *req.content, output_bytes);
    return Status::Ok;
}

Status AIProvider::translate(const TranslateRequest& req, TranslateResult& out) {
    out = TranslateResult{};
    if (const Status s = admit(req.content); !is_ok(s)) { return s; }
    if (req.target_language.empty() || req.target_language.size() > 32 ||
        req.source_language.size() > 32) {
        return Status::InvalidArgument;
    }

    const Status s = do_translate(req, out);
    if (!is_ok(s)) { return s; }

    if (out.text.empty()) {
        return Status::AiResponseMalformed;
    }
    stamp(out.meta, *req.content, out.text.size());
    return Status::Ok;
}

}  // namespace uapp::ai
