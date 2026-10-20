// SPDX-License-Identifier: Apache-2.0
//
// See the banner in local_heuristic_provider.h. NOT AI. NOT A MODEL.
#include "uapp/ai/local/local_heuristic_provider.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "text_analysis.h"

namespace uapp::ai::local {
namespace detail_alias = detail;

namespace {

ProviderDescriptor make_descriptor() {
    ProviderDescriptor d;
    d.id           = kLocalHeuristicProviderId;
    d.display_name = "On-device heuristics (no AI model)";
    // The four it really does. Generate/Chat/Translate bits are CLEAR, so the
    // registry never routes to them — the descriptor and the three
    // AiCapabilityNotImplemented bodies say the same thing, and
    // tests/ai/provider_contract_test.cpp requires them to keep agreeing.
    d.capabilities = cap_mask(Capability::Embed, Capability::Summarize,
                              Capability::Classify, Capability::Extract);
    d.locality     = Locality::OnDevice;
    d.provenance   = OutputProvenance::HeuristicNoModel;
    d.embed_dim    = kLocalHeuristicEmbedDim;
    // Zero, not a made-up number. This provider has no context window and no
    // output limit in tokens; its real bound is kMaxRequestContentBytes, which
    // the base class enforces. Inventing "4096 tokens" here would be a
    // capability claim with nothing behind it.
    d.context_tokens    = 0;
    d.max_output_tokens = 0;
    d.deterministic     = true;
    // Empty is correct and is only legal because locality == OnDevice:
    // validate_descriptor() requires a retention policy URL for anything that
    // leaves the device. Nothing leaves the device here, so there is no
    // retention policy to publish.
    d.data_policy_url = "";
    return d;
}

/// Diminishing weight for repeated hint hits: the first occurrence is worth
/// 1.0 and each further one 0.3.
///
/// WHY DIMINISHING: a document that says "invoice" forty times is not forty
/// times more likely to be an invoice than one that says it once, but a
/// linear count would make it beat a document that matches four different
/// hints once each — and matching four distinct hints is much stronger
/// evidence. The shape (not the constant) is the point.
[[nodiscard]] double hint_weight(std::size_t occurrences) noexcept {
    if (occurrences == 0) { return 0.0; }
    return 1.0 + 0.3 * static_cast<double>(occurrences - 1);
}

}  // namespace

std::vector<ClassifyLabel> builtin_document_labels() {
    std::vector<ClassifyLabel> labels;

    ClassifyLabel invoice;
    invoice.id           = "invoice";
    invoice.display_name = "Invoice";
    invoice.hints        = {"invoice", "invoice number", "amount due", "due date",
                            "bill to", "subtotal", "vat", "purchase order",
                            "payment terms", "remit"};
    labels.push_back(std::move(invoice));

    ClassifyLabel receipt;
    receipt.id           = "receipt";
    receipt.display_name = "Receipt";
    receipt.hints        = {"receipt", "thank you for your purchase", "cash",
                            "change due", "card ending", "transaction id",
                            "merchant", "refund policy"};
    labels.push_back(std::move(receipt));

    ClassifyLabel resume;
    resume.id           = "resume";
    resume.display_name = "Resume / CV";
    resume.hints        = {"curriculum vitae", "work experience", "education",
                           "skills", "references", "employment history",
                           "objective", "certifications"};
    labels.push_back(std::move(resume));

    ClassifyLabel notes;
    notes.id           = "meeting_notes";
    notes.display_name = "Meeting notes";
    notes.hints        = {"attendees", "agenda", "action items", "minutes",
                          "next steps", "apologies", "follow up", "discussed"};
    labels.push_back(std::move(notes));

    ClassifyLabel code;
    code.id           = "source_code";
    code.display_name = "Source code";
    code.hints        = {"function", "return", "class", "import", "const",
                         "namespace", "void", "struct", "def"};
    labels.push_back(std::move(code));

    ClassifyLabel letter;
    letter.id           = "correspondence";
    letter.display_name = "Letter / correspondence";
    letter.hints        = {"dear", "sincerely", "yours faithfully", "regards",
                           "enclosed", "writing to inform", "best wishes"};
    labels.push_back(std::move(letter));

    return labels;
}

LocalHeuristicProvider::LocalHeuristicProvider() : descriptor_(make_descriptor()) {}

LocalHeuristicProvider::LocalHeuristicProvider(ClockFn clock)
    : AIProvider(clock), descriptor_(make_descriptor()) {}

// --- the three that need a real language model ------------------------------
//
// Each returns a specific status and nothing else. No canned text, no echo of
// the input, no "first sentence" dressed up as generation. See the banner.

Status LocalHeuristicProvider::do_generate(const GenerateRequest&, GenerateResult&) {
    return Status::AiCapabilityNotImplemented;
}

Status LocalHeuristicProvider::do_chat(const ChatRequest&, ChatResult&) {
    return Status::AiCapabilityNotImplemented;
}

Status LocalHeuristicProvider::do_translate(const TranslateRequest&, TranslateResult&) {
    return Status::AiCapabilityNotImplemented;
}

// --- embed -----------------------------------------------------------------

Status LocalHeuristicProvider::do_embed(const EmbedRequest& req, EmbedResult& out) {
    const std::vector<Message>& msgs = req.content->messages();
    const std::uint32_t dim = kLocalHeuristicEmbedDim;

    out.count = msgs.size();
    out.dim   = dim;
    out.vectors.assign(msgs.size() * static_cast<std::size_t>(dim), 0.0F);

    for (std::size_t i = 0; i < msgs.size(); ++i) {
        float* row = out.vectors.data() + i * static_cast<std::size_t>(dim);
        detail_alias::hash_embed(msgs[i].text, dim, row);
    }
    return Status::Ok;
}

// --- summarize -------------------------------------------------------------

Status LocalHeuristicProvider::do_summarize(const SummarizeRequest& req,
                                            SummarizeResult& out) {
    const std::string text = detail_alias::concat_messages(*req.content);

    std::vector<detail_alias::Span> sentences;
    detail_alias::split_sentences(text, sentences);
    if (sentences.empty()) {
        // admit() already rejected whitespace-only input, so this means the
        // splitter found no span — treat the whole input as one sentence
        // rather than returning an empty summary the base class would reject
        // as malformed.
        sentences.push_back(detail_alias::Span{0, text.size()});
    }

    std::size_t k = req.max_sentences == 0 ? 3 : req.max_sentences;
    k = std::min(k, kMaxSummarySentences);
    k = std::min(k, sentences.size());

    std::vector<double> scores;
    detail_alias::score_sentences_tfidf(text, sentences, scores);

    std::vector<std::size_t> chosen;
    detail_alias::select_top_sentences(scores, k, chosen);

    out.is_extractive = true;
    out.source_spans.clear();
    out.text.clear();
    for (const std::size_t idx : chosen) {
        const detail_alias::Span sp = sentences[idx];
        if (!out.text.empty()) { out.text.push_back(' '); }
        out.text.append(text, sp.offset, sp.length);
        out.source_spans.push_back(SummarizeResult::Span{sp.offset, sp.length});
    }
    if (out.text.empty()) {
        // Only reachable if every selected span was empty, which the splitter
        // does not produce. Reported honestly rather than returning Ok with
        // nothing.
        return Status::AiResponseMalformed;
    }
    return Status::Ok;
}

// --- classify --------------------------------------------------------------

Status LocalHeuristicProvider::do_classify(const ClassifyRequest& req,
                                           ClassifyResult& out) {
    const std::string raw     = detail_alias::concat_messages(*req.content);
    const std::string lowered = detail_alias::lowercase_ascii(raw);

    std::vector<double> raw_scores(req.labels.size(), 0.0);
    double total = 0.0;

    for (std::size_t i = 0; i < req.labels.size(); ++i) {
        const ClassifyLabel& label = req.labels[i];
        double score = 0.0;
        for (const std::string& hint : label.hints) {
            const std::string h = detail_alias::lowercase_ascii(hint);
            score += hint_weight(detail_alias::count_whole_word(lowered, h));
        }
        // The display name counts as an implicit hint. A caller who writes
        // `display_name = "Invoice"` and forgets to add "invoice" to the hints
        // should not get an abstention for a document that says "invoice"
        // twenty times.
        if (!label.display_name.empty()) {
            const std::string dn = detail_alias::lowercase_ascii(label.display_name);
            score += hint_weight(detail_alias::count_whole_word(lowered, dn));
        }
        raw_scores[i] = score;
        total += score;
    }

    std::size_t best = 0;
    for (std::size_t i = 1; i < raw_scores.size(); ++i) {
        // Strict `>`, so ties go to the earlier label and the result is
        // deterministic — which this provider's descriptor claims.
        if (raw_scores[i] > raw_scores[best]) { best = i; }
    }

    out.label_scores.assign(req.labels.size(), 0.0F);
    if (total > 0.0) {
        for (std::size_t i = 0; i < raw_scores.size(); ++i) {
            out.label_scores[i] = static_cast<float>(raw_scores[i] / total);
        }
    }

    const double confidence = total > 0.0 ? raw_scores[best] / total : 0.0;

    // ABSTAIN rather than guess. Two independent gates: nothing matched at
    // all, or the match is below the caller's confidence floor.
    const bool nothing_matched = raw_scores[best] <= 0.0;
    if (nothing_matched || static_cast<float>(confidence) < req.min_confidence) {
        out.abstained  = true;
        out.label_id.clear();
        out.confidence = 0.0F;
        return Status::Ok;
    }

    out.abstained  = false;
    out.label_id   = req.labels[best].id;
    out.confidence = static_cast<float>(std::min(confidence, 1.0));
    return Status::Ok;
}

// --- extract ---------------------------------------------------------------

Status LocalHeuristicProvider::do_extract(const ExtractRequest& req, ExtractResult& out) {
    const std::string text = detail_alias::concat_messages(*req.content);

    std::vector<ExtractedField> found;
    if (has_field_kind(req.kinds, FieldKind::Email)) {
        detail_alias::scan_emails(text, found);
    }
    if (has_field_kind(req.kinds, FieldKind::Url)) {
        detail_alias::scan_urls(text, found);
    }
    if (has_field_kind(req.kinds, FieldKind::IsoDate)) {
        detail_alias::scan_iso_dates(text, found);
    }
    detail_alias::scan_numbers(text, has_field_kind(req.kinds, FieldKind::Number),
                               has_field_kind(req.kinds, FieldKind::Money), found);

    detail_alias::resolve_overlaps(found);

    out.truncated = found.size() > kMaxExtractedFields;
    if (out.truncated) {
        found.resize(kMaxExtractedFields);
    }
    out.fields = std::move(found);
    return Status::Ok;
}

}  // namespace uapp::ai::local
