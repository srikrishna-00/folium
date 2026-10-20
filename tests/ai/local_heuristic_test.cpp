// SPDX-License-Identifier: Apache-2.0
//
// tests/ai/local_heuristic_test.cpp — what LocalHeuristicProvider actually
// does, asserted on real output rather than on "it returned Ok".
//
// The cases that matter are the OUTPUT QUALITY ones. A test that only checks
// `summarize()` returns Ok would pass for an implementation that returns the
// first three sentences, which is truncation dressed up as summarisation. So:
//
//   * `summarize` must pick a high-information sentence from the MIDDLE of a
//     document over a low-information one at the top, and must return them in
//     document order.
//   * `classify` must bucket hand-picked real examples into the right label,
//     and must ABSTAIN on text that matches nothing instead of guessing.
//   * `extract` must find a real email, a real date and a real amount in real
//     prose, at the right byte offsets, and must NOT find a date in an
//     ambiguous `03/04/2026`.
//   * `embed` must put near-duplicates closer together than unrelated texts,
//     and must be bit-for-bit deterministic.
//
// It also drives the internal pieces (ai/local/src/text_analysis.h) directly,
// because the sentence splitter and the scanners have invariants the public
// API only exercises indirectly.

#include <cmath>
#include <string>
#include <vector>

#include "test_framework.h"
#include "text_analysis.h"
#include "uapp/ai/local/local_heuristic_provider.h"

using uapp::Status;
using uapp::ai::ClassifyLabel;
using uapp::ai::ClassifyRequest;
using uapp::ai::ClassifyResult;
using uapp::ai::Content;
using uapp::ai::EmbedRequest;
using uapp::ai::EmbedResult;
using uapp::ai::ExtractedField;
using uapp::ai::ExtractRequest;
using uapp::ai::ExtractResult;
using uapp::ai::FieldKind;
using uapp::ai::field_bit;
using uapp::ai::kAllFieldKinds;
using uapp::ai::Message;
using uapp::ai::OutputProvenance;
using uapp::ai::Role;
using uapp::ai::SummarizeRequest;
using uapp::ai::SummarizeResult;
using uapp::ai::local::builtin_document_labels;
using uapp::ai::local::kLocalHeuristicEmbedDim;
using uapp::ai::local::LocalHeuristicProvider;
namespace ta = uapp::ai::local::detail;

namespace {

[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

[[nodiscard]] const ExtractedField* find_kind(const ExtractResult& r, FieldKind k,
                                              const std::string& value) {
    for (const ExtractedField& f : r.fields) {
        if (f.kind == k && f.value == value) { return &f; }
    }
    return nullptr;
}

[[nodiscard]] std::size_t count_kind(const ExtractResult& r, FieldKind k) {
    std::size_t n = 0;
    for (const ExtractedField& f : r.fields) {
        if (f.kind == k) { ++n; }
    }
    return n;
}

}  // namespace

// ===========================================================================
// text_analysis internals
// ===========================================================================

UAPP_TEST("ai.text.stopword_list_is_sorted") {
    // `is_stopword` binary-searches this array. An out-of-order entry silently
    // disables every entry after it and nothing else fails, which is exactly
    // the kind of bug that never gets found.
    const std::string_view* it  = ta::stop_list_begin();
    const std::string_view* end = ta::stop_list_end();
    UAPP_REQUIRE(it != end);
    for (const std::string_view* p = it + 1; p != end; ++p) {
        UAPP_CHECK_MSG(*(p - 1) < *p,
                       std::string("stop list out of order at '") + std::string(*p) + "'");
    }
    UAPP_CHECK(ta::is_stopword("the"));
    UAPP_CHECK(ta::is_stopword("would"));
    UAPP_CHECK(!ta::is_stopword("invoice"));
}

UAPP_TEST("ai.text.sentence_splitter") {
    std::vector<ta::Span> spans;

    ta::split_sentences("One. Two! Three? Four", spans);
    UAPP_REQUIRE_MSG(spans.size() == 4, "got " + std::to_string(spans.size()));
    UAPP_CHECK(std::string("One.") == std::string("One."));
    UAPP_CHECK(spans[0].length == 4);  // "One."
    UAPP_CHECK(spans[3].length == 4);  // "Four"

    // A decimal point is not a full stop.
    ta::split_sentences("The total is 3.50 today.", spans);
    UAPP_REQUIRE_MSG(spans.size() == 1, "got " + std::to_string(spans.size()));

    // A blank line is a boundary even with no punctuation.
    ta::split_sentences("Heading\n\nBody text here", spans);
    UAPP_REQUIRE_MSG(spans.size() == 2, "got " + std::to_string(spans.size()));
    UAPP_CHECK(spans[0].length == 7);

    // Whitespace-only produces nothing rather than one empty span.
    ta::split_sentences("   \n  ", spans);
    UAPP_CHECK(spans.empty());

    // Spans never exceed the text, and the text they name is non-empty.
    const std::string text = "Alpha beta. Gamma delta! Epsilon?";
    ta::split_sentences(text, spans);
    for (const ta::Span& s : spans) {
        UAPP_CHECK(s.length > 0);
        UAPP_CHECK(s.offset + s.length <= text.size());
    }
}

UAPP_TEST("ai.text.tfidf_prefers_distinctive_sentences") {
    // Sentence 1 is pure stopwords. Sentence 2 carries four distinctive
    // terms. A real metric must rank 2 above 1; truncation would not.
    const std::string text =
        "It is what it is. "
        "Quarterly revenue in Lisbon exceeded forecast by eleven percent.";

    std::vector<ta::Span> sentences;
    ta::split_sentences(text, sentences);
    UAPP_REQUIRE(sentences.size() == 2);

    std::vector<double> scores;
    ta::score_sentences_tfidf(text, sentences, scores);
    UAPP_REQUIRE(scores.size() == 2);
    UAPP_CHECK_MSG(scores[1] > scores[0],
                   "TF-IDF ranked the stopword-only sentence at or above the "
                   "information-bearing one");
}

UAPP_TEST("ai.text.top_sentence_selection_is_in_document_order") {
    const std::vector<double> scores = {0.1, 5.0, 0.2, 4.0, 0.3};
    std::vector<std::size_t>  chosen;
    ta::select_top_sentences(scores, 2, chosen);
    UAPP_REQUIRE(chosen.size() == 2);
    // The two best are indices 1 and 3, returned in document order.
    UAPP_CHECK(chosen[0] == 1);
    UAPP_CHECK(chosen[1] == 3);

    // k past the end returns everything, still in order.
    ta::select_top_sentences(scores, 99, chosen);
    UAPP_REQUIRE(chosen.size() == 5);
    UAPP_CHECK(chosen[0] == 0);
    UAPP_CHECK(chosen[4] == 4);

    // Ties break towards the earlier sentence, so the result is deterministic.
    const std::vector<double> flat = {1.0, 1.0, 1.0};
    ta::select_top_sentences(flat, 2, chosen);
    UAPP_REQUIRE(chosen.size() == 2);
    UAPP_CHECK(chosen[0] == 0);
    UAPP_CHECK(chosen[1] == 1);

    ta::select_top_sentences(scores, 0, chosen);
    UAPP_CHECK(chosen.empty());
}

UAPP_TEST("ai.text.whole_word_matching_does_not_match_inside_words") {
    UAPP_CHECK(ta::count_whole_word("the cat sat", "cat") == 1);
    UAPP_CHECK(ta::count_whole_word("the catalogue", "cat") == 0);
    UAPP_CHECK(ta::count_whole_word("cat cat cat", "cat") == 3);
    UAPP_CHECK(ta::count_whole_word("amount due now", "amount due") == 1);
    UAPP_CHECK(ta::count_whole_word("", "cat") == 0);
    UAPP_CHECK(ta::count_whole_word("cat", "") == 0);
}

// ===========================================================================
// summarize — extractive, with real selection
// ===========================================================================

UAPP_TEST("ai.local.summarize_is_extractive_and_selects_by_information") {
    LocalHeuristicProvider local;

    // Six sentences. The first and last are near-contentless; the
    // information is in the middle. An implementation that truncated would
    // return the first N.
    const std::string doc =
        "Hi there. "
        "It is what it is, and it is. "
        "Lisbon warehouse throughput rose eleven percent after the conveyor "
        "retrofit completed. "
        "Thanks. "
        "The retrofit budget overran by forty thousand euro because the "
        "conveyor motors were replaced twice. "
        "Regards.";

    Content c = Content::on_device_only(doc, Role::Document);
    SummarizeRequest req;
    req.content       = &c;
    req.max_sentences = 2;
    SummarizeResult out;
    UAPP_REQUIRE_OK(local.summarize(req, out));

    UAPP_CHECK_MSG(out.is_extractive, "the local summary must declare itself extractive");
    UAPP_CHECK(out.meta.provenance == OutputProvenance::HeuristicNoModel);
    UAPP_REQUIRE_MSG(out.source_spans.size() == 2,
                     "got " + std::to_string(out.source_spans.size()) + " spans");

    // It picked the two information-bearing sentences, not the greeting.
    UAPP_CHECK_MSG(contains(out.text, "Lisbon warehouse throughput"),
                   "summary missed the highest-information sentence: " + out.text);
    UAPP_CHECK_MSG(contains(out.text, "retrofit budget overran"),
                   "summary missed the second information-bearing sentence: " + out.text);
    UAPP_CHECK_MSG(!contains(out.text, "Hi there"),
                   "summary led with a contentless greeting, i.e. it truncated: " +
                       out.text);

    // EVERY span is a verbatim slice of the input, at the offset it claims.
    // This is the property that makes the output checkable against the source
    // and is the groundwork spec §18's citation verification needs.
    for (const SummarizeResult::Span& sp : out.source_spans) {
        UAPP_REQUIRE(sp.offset + sp.length <= doc.size());
        const std::string slice = doc.substr(sp.offset, sp.length);
        UAPP_CHECK_MSG(contains(out.text, slice),
                       "span [" + std::to_string(sp.offset) + "," +
                           std::to_string(sp.length) + ") is not in the summary text");
    }

    // Spans are in document order.
    for (std::size_t i = 1; i < out.source_spans.size(); ++i) {
        UAPP_CHECK(out.source_spans[i - 1].offset < out.source_spans[i].offset);
    }
}

UAPP_TEST("ai.local.summarize_is_deterministic_and_honours_max_sentences") {
    LocalHeuristicProvider local;
    const std::string doc =
        "Alpha one is here. Beta two follows. Gamma three arrives. "
        "Delta four concludes. Epsilon five trails.";
    Content c = Content::on_device_only(doc, Role::Document);

    SummarizeResult a;
    SummarizeResult b;
    SummarizeRequest req;
    req.content       = &c;
    req.max_sentences = 3;
    UAPP_REQUIRE_OK(local.summarize(req, a));
    UAPP_REQUIRE_OK(local.summarize(req, b));
    UAPP_CHECK_MSG(a.text == b.text, "descriptor claims deterministic but output varied");
    UAPP_CHECK(a.source_spans.size() == 3);

    req.max_sentences = 1;
    SummarizeResult one;
    UAPP_REQUIRE_OK(local.summarize(req, one));
    UAPP_CHECK(one.source_spans.size() == 1);

    // Asking for more sentences than exist returns what exists, not padding.
    req.max_sentences = 50;
    SummarizeResult all;
    UAPP_REQUIRE_OK(local.summarize(req, all));
    UAPP_CHECK(all.source_spans.size() == 5);
}

UAPP_TEST("ai.local.summarize_handles_a_single_sentence_and_no_punctuation") {
    LocalHeuristicProvider local;

    Content one = Content::on_device_only("Just one sentence here.", Role::Document);
    SummarizeRequest r1;
    r1.content       = &one;
    r1.max_sentences = 3;
    SummarizeResult o1;
    UAPP_REQUIRE_OK(local.summarize(r1, o1));
    UAPP_CHECK(o1.source_spans.size() == 1);

    Content none = Content::on_device_only("no punctuation at all in this text");
    SummarizeRequest r2;
    r2.content = &none;
    SummarizeResult o2;
    UAPP_REQUIRE_OK(local.summarize(r2, o2));
    UAPP_CHECK(!o2.text.empty());
}

// ===========================================================================
// classify — keyword rules, with abstention
// ===========================================================================

UAPP_TEST("ai.local.classify_buckets_hand_picked_examples") {
    LocalHeuristicProvider local;
    const std::vector<ClassifyLabel> labels = builtin_document_labels();

    struct Example final {
        const char* text;
        const char* expected_label;
    };

    static const Example kExamples[] = {
        {"INVOICE 4471-B\nBill to: Acme Ltd\nSubtotal: 1200.00\nVAT: 240.00\n"
         "Amount due: 1440.00\nPayment terms: 30 days",
         "invoice"},
        {"Thank you for your purchase! Card ending 4412. Transaction id 99812. "
         "See our refund policy in store.",
         "receipt"},
        {"Curriculum vitae\nWork experience: six years at Initech\n"
         "Education: BSc Computer Science\nSkills: C++, SQL\nReferences available",
         "resume"},
        {"Attendees: Ana, Bo, Cy\nAgenda: budget, hiring\nAction items: Ana to "
         "circulate the figures\nNext steps: reconvene Friday",
         "meeting_notes"},
        {"namespace demo {\nclass Widget {\n  void render();\n};\n}\n"
         "import os\ndef main():\n  return 0",
         "source_code"},
        {"Dear Ms Kowalski,\n\nI am writing to inform you that the enclosed "
         "documents have been filed.\n\nYours faithfully,\nA. Clerk",
         "correspondence"},
    };

    for (const Example& ex : kExamples) {
        Content c = Content::on_device_only(ex.text, Role::Document);
        ClassifyRequest req;
        req.content = &c;
        req.labels  = labels;
        ClassifyResult out;
        UAPP_REQUIRE_OK(local.classify(req, out));

        UAPP_CHECK_MSG(!out.abstained,
                       std::string("abstained on a clear example of ") +
                           ex.expected_label);
        UAPP_CHECK_MSG(out.label_id == ex.expected_label,
                       std::string("classified as '") + out.label_id + "', wanted '" +
                           ex.expected_label + "'");
        UAPP_CHECK(out.confidence > 0.0F && out.confidence <= 1.0F);
        UAPP_CHECK(out.label_scores.size() == labels.size());
        UAPP_CHECK(out.meta.provenance == OutputProvenance::HeuristicNoModel);
    }
}

UAPP_TEST("ai.local.classify_abstains_rather_than_guessing") {
    LocalHeuristicProvider local;

    // Real prose that matches none of the built-in hints. A classifier that
    // returned its least-bad guess here would attach a wrong category to a
    // user's document, which is worse than attaching none.
    Content c = Content::on_device_only(
        "The heron stood motionless in the shallows while the tide withdrew "
        "across the mudflats.",
        Role::Document);

    ClassifyRequest req;
    req.content = &c;
    req.labels  = builtin_document_labels();
    ClassifyResult out;
    UAPP_REQUIRE_OK(local.classify(req, out));

    UAPP_CHECK_MSG(out.abstained, "classifier guessed instead of abstaining");
    UAPP_CHECK(out.label_id.empty());
    UAPP_CHECK(out.confidence == 0.0F);
}

UAPP_TEST("ai.local.classify_respects_a_confidence_floor") {
    LocalHeuristicProvider local;

    // Matches both labels once each, so confidence is ~0.5. A floor above
    // that must produce an abstention.
    Content c = Content::on_device_only("alpha and beta appear once each here.");

    std::vector<ClassifyLabel> labels;
    ClassifyLabel a;
    a.id    = "alpha";
    a.hints = {"alpha"};
    labels.push_back(a);
    ClassifyLabel b;
    b.id    = "beta";
    b.hints = {"beta"};
    labels.push_back(b);

    ClassifyRequest req;
    req.content        = &c;
    req.labels         = labels;
    req.min_confidence = 0.0F;
    ClassifyResult low;
    UAPP_REQUIRE_OK(local.classify(req, low));
    UAPP_CHECK(!low.abstained);
    UAPP_CHECK_MSG(std::fabs(low.confidence - 0.5F) < 0.01F,
                   "expected ~0.5 confidence on a 1-1 split, got " +
                       std::to_string(low.confidence));

    req.min_confidence = 0.9F;
    ClassifyResult high;
    UAPP_REQUIRE_OK(local.classify(req, high));
    UAPP_CHECK_MSG(high.abstained, "ignored the caller's confidence floor");
}

UAPP_TEST("ai.local.classify_uses_the_callers_vocabulary_not_its_own") {
    // The vocabulary lives in the request, which is what makes classify()
    // provider-independent. A caller's own labels must work with no hints
    // from the provider's built-in set.
    LocalHeuristicProvider local;
    Content c = Content::on_device_only(
        "Blade pitch telemetry shows cyclic imbalance on the port rotor.",
        Role::Document);

    std::vector<ClassifyLabel> labels;
    ClassifyLabel rotor;
    rotor.id           = "rotorcraft_maintenance";
    rotor.display_name = "Rotorcraft maintenance";
    rotor.hints        = {"blade pitch", "rotor", "cyclic"};
    labels.push_back(rotor);
    ClassifyLabel marine;
    marine.id           = "marine_log";
    marine.display_name = "Marine log";
    marine.hints        = {"anchorage", "draught", "bilge"};
    labels.push_back(marine);

    ClassifyRequest req;
    req.content = &c;
    req.labels  = labels;
    ClassifyResult out;
    UAPP_REQUIRE_OK(local.classify(req, out));
    UAPP_CHECK(out.label_id == "rotorcraft_maintenance");
    UAPP_CHECK(out.label_scores.size() == 2);
    UAPP_CHECK(out.label_scores[0] > out.label_scores[1]);
}

// ===========================================================================
// extract — pattern-based, with real offsets
// ===========================================================================

UAPP_TEST("ai.local.extract_finds_real_fields_in_real_prose") {
    LocalHeuristicProvider local;
    const std::string doc =
        "Please remit 1,440.50 by 2026-03-14. Queries to Ana.Lopes+billing@"
        "example.co.uk or see https://billing.example.com/inv/4471 for detail. "
        "The prior balance was $-25.00 and the unit count is 12.";

    Content c = Content::on_device_only(doc, Role::Document);
    ExtractRequest req;
    req.content = &c;
    req.kinds   = kAllFieldKinds;
    ExtractResult out;
    UAPP_REQUIRE_OK(local.extract(req, out));
    UAPP_CHECK(!out.truncated);

    const ExtractedField* email =
        find_kind(out, FieldKind::Email, "Ana.Lopes+billing@example.co.uk");
    UAPP_REQUIRE_MSG(email != nullptr, "did not find the email address");
    UAPP_CHECK_MSG(email->normalized == "ana.lopes+billing@example.co.uk",
                   "email not normalised to lowercase: " + email->normalized);
    // The offset really points at the email in the source.
    UAPP_CHECK(doc.compare(email->offset, email->length, email->value) == 0);

    const ExtractedField* date = find_kind(out, FieldKind::IsoDate, "2026-03-14");
    UAPP_REQUIRE_MSG(date != nullptr, "did not find the ISO date");
    UAPP_CHECK(date->normalized == "2026-03-14");
    UAPP_CHECK(doc.compare(date->offset, date->length, date->value) == 0);

    const ExtractedField* url =
        find_kind(out, FieldKind::Url, "https://billing.example.com/inv/4471");
    UAPP_REQUIRE_MSG(url != nullptr, "did not find the URL");
    UAPP_CHECK(doc.compare(url->offset, url->length, url->value) == 0);

    const ExtractedField* amount = find_kind(out, FieldKind::Number, "1,440.50");
    UAPP_REQUIRE_MSG(amount != nullptr, "did not find the thousands-separated number");
    UAPP_CHECK_MSG(amount->normalized == "1440.50",
                   "separators not stripped: " + amount->normalized);

    const ExtractedField* money = find_kind(out, FieldKind::Money, "$-25.00");
    UAPP_REQUIRE_MSG(money != nullptr, "did not find the currency amount");
    UAPP_CHECK_MSG(money->normalized == "$-25.00",
                   "currency normalisation changed the symbol: " + money->normalized);

    // Fields come back in source order.
    for (std::size_t i = 1; i < out.fields.size(); ++i) {
        UAPP_CHECK(out.fields[i - 1].offset < out.fields[i].offset);
    }

    // The digits inside the email, the URL and the date are NOT also
    // reported as numbers: that is the overlap arbitration working.
    for (const ExtractedField& f : out.fields) {
        if (f.kind != FieldKind::Number && f.kind != FieldKind::Money) { continue; }
        UAPP_CHECK_MSG(f.offset < email->offset || f.offset >= email->offset + email->length,
                       "a number was reported from inside the email address");
        UAPP_CHECK_MSG(f.offset < url->offset || f.offset >= url->offset + url->length,
                       "a number was reported from inside the URL");
        UAPP_CHECK_MSG(f.offset < date->offset || f.offset >= date->offset + date->length,
                       "a number was reported from inside the date");
    }
}

UAPP_TEST("ai.local.extract_refuses_ambiguous_dates_and_invalid_calendar_dates") {
    LocalHeuristicProvider local;
    // 03/04/2026 is 3 April or 4 March depending on where the reader lives.
    // 2026-02-30 and 2026-13-01 do not exist. 2025-02-29 is not a leap year.
    const std::string doc =
        "Ambiguous: 03/04/2026. Impossible: 2026-02-30 and 2026-13-01 and "
        "2025-02-29. Valid: 2024-02-29 and 2026/03/14.";

    Content c = Content::on_device_only(doc, Role::Document);
    ExtractRequest req;
    req.content = &c;
    req.kinds   = field_bit(FieldKind::IsoDate);
    ExtractResult out;
    UAPP_REQUIRE_OK(local.extract(req, out));

    UAPP_REQUIRE_MSG(out.fields.size() == 2,
                     "expected exactly the two valid dates, got " +
                         std::to_string(out.fields.size()));
    UAPP_CHECK(out.fields[0].value == "2024-02-29");      // real leap day
    UAPP_CHECK(out.fields[1].value == "2026/03/14");      // slash form accepted
    UAPP_CHECK_MSG(out.fields[1].normalized == "2026-03-14",
                   "slash date not canonicalised: " + out.fields[1].normalized);
}

UAPP_TEST("ai.local.extract_honours_the_requested_kinds_only") {
    LocalHeuristicProvider local;
    Content c = Content::on_device_only(
        "mail me at x@y.com, due 2026-03-14, total $9.99, count 42", Role::Document);

    ExtractRequest req;
    req.content = &c;
    req.kinds   = field_bit(FieldKind::Email);
    ExtractResult out;
    UAPP_REQUIRE_OK(local.extract(req, out));
    UAPP_CHECK(out.fields.size() == 1);
    UAPP_CHECK(out.fields[0].kind == FieldKind::Email);

    req.kinds = field_bit(FieldKind::Money);
    UAPP_REQUIRE_OK(local.extract(req, out));
    UAPP_CHECK(count_kind(out, FieldKind::Money) == 1);
    UAPP_CHECK(count_kind(out, FieldKind::Number) == 0);
}

UAPP_TEST("ai.local.extract_rejects_near_miss_patterns") {
    LocalHeuristicProvider local;
    // Each of these LOOKS like a field and is not one. A scanner that matched
    // them would produce confident wrong data.
    Content c = Content::on_device_only(
        "not-an-email@ nor @example.com nor a@b nor .lead@example.com "
        "nor trailing@example. nor http:// alone. identifier abc123def.",
        Role::Document);

    ExtractRequest req;
    req.content = &c;
    req.kinds   = kAllFieldKinds;
    ExtractResult out;
    UAPP_REQUIRE_OK(local.extract(req, out));

    for (const ExtractedField& f : out.fields) {
        UAPP_CHECK_MSG(f.kind != FieldKind::Email || f.value == "nor@example.com",
                       "accepted a malformed email: " + f.value);
        UAPP_CHECK_MSG(f.kind != FieldKind::Url,
                       "accepted a bare scheme as a URL: " + f.value);
        UAPP_CHECK_MSG(f.kind != FieldKind::Number,
                       "pulled a number out of an identifier: " + f.value);
    }
}

UAPP_TEST("ai.local.extract_bounds_a_pathological_input") {
    LocalHeuristicProvider local;
    // ~200k comma-separated integers: far more matches than the result cap.
    std::string doc;
    doc.reserve(uapp::ai::kMaxRequestContentBytes);
    while (doc.size() + 8 < uapp::ai::kMaxRequestContentBytes) { doc += "1234 56 "; }

    Content c = Content::on_device_only(doc, Role::Document);
    ExtractRequest req;
    req.content = &c;
    req.kinds   = field_bit(FieldKind::Number);
    ExtractResult out;
    UAPP_REQUIRE_OK(local.extract(req, out));

    UAPP_CHECK(out.fields.size() == uapp::ai::kMaxExtractedFields);
    UAPP_CHECK_MSG(out.truncated,
                   "hit the field cap without telling the caller it truncated");
}

// ===========================================================================
// embed — a real, honest lexical fingerprint
// ===========================================================================

UAPP_TEST("ai.local.embed_shape_and_determinism") {
    LocalHeuristicProvider local;

    std::vector<Message> msgs;
    msgs.push_back(Message{Role::Document, "the quarterly report is attached"});
    msgs.push_back(Message{Role::Document, "an unrelated note about herons"});
    Content c = Content::on_device_only(std::move(msgs));

    EmbedRequest req;
    req.content = &c;
    EmbedResult a;
    EmbedResult b;
    UAPP_REQUIRE_OK(local.embed(req, a));
    UAPP_REQUIRE_OK(local.embed(req, b));

    UAPP_REQUIRE(a.count == 2);
    UAPP_REQUIRE(a.dim == kLocalHeuristicEmbedDim);
    UAPP_REQUIRE(a.vectors.size() == 2 * static_cast<std::size_t>(a.dim));
    UAPP_CHECK_MSG(a.vectors == b.vectors,
                   "descriptor claims deterministic but the vectors differ");

    // L2-normalised, so cosine similarity is a dot product.
    for (std::size_t i = 0; i < a.count; ++i) {
        const double self = ta::dot(a.row(i), a.row(i), a.dim);
        UAPP_CHECK_MSG(std::fabs(self - 1.0) < 1e-4,
                       "vector " + std::to_string(i) + " has norm^2 " +
                           std::to_string(self));
    }

    // Out-of-range row access returns null rather than reading past the end.
    UAPP_CHECK(a.row(2) == nullptr);
    UAPP_CHECK(a.row(99) == nullptr);
}

UAPP_TEST("ai.local.embed_puts_near_duplicates_closer_than_unrelated_text") {
    LocalHeuristicProvider local;

    std::vector<Message> msgs;
    msgs.push_back(Message{Role::Document, "the quarterly revenue report for Lisbon"});
    msgs.push_back(Message{Role::Document, "the quarterly revenue report for Lisboa"});
    msgs.push_back(Message{Role::Document, "a heron stood still in the shallows"});
    Content c = Content::on_device_only(std::move(msgs));

    EmbedRequest req;
    req.content = &c;
    EmbedResult r;
    UAPP_REQUIRE_OK(local.embed(req, r));
    UAPP_REQUIRE(r.count == 3);

    const double near    = ta::dot(r.row(0), r.row(1), r.dim);
    const double unrelated = ta::dot(r.row(0), r.row(2), r.dim);

    UAPP_CHECK_MSG(near > unrelated,
                   "near-duplicate similarity " + std::to_string(near) +
                       " was not above unrelated similarity " +
                       std::to_string(unrelated));
    UAPP_CHECK_MSG(near > 0.5,
                   "a one-word-different sentence scored only " + std::to_string(near));
}

UAPP_TEST("ai.local.embed_is_honestly_lexical_not_semantic") {
    // This case DOCUMENTS A LIMITATION as a test, so that nobody mistakes the
    // fingerprint for a semantic embedding on the strength of the case above.
    // "car" and "automobile" mean the same thing and share almost no
    // characters; a real embedding model puts them close, this does not. If a
    // future change makes this assertion FAIL, that is a signal a real model
    // has been wired in and this test should be deleted, not "fixed".
    LocalHeuristicProvider local;

    std::vector<Message> msgs;
    msgs.push_back(Message{Role::Document, "car"});
    msgs.push_back(Message{Role::Document, "automobile"});
    msgs.push_back(Message{Role::Document, "carpet"});
    Content c = Content::on_device_only(std::move(msgs));

    EmbedRequest req;
    req.content = &c;
    EmbedResult r;
    UAPP_REQUIRE_OK(local.embed(req, r));

    const double synonyms      = ta::dot(r.row(0), r.row(1), r.dim);
    const double shared_prefix = ta::dot(r.row(0), r.row(2), r.dim);
    UAPP_CHECK_MSG(shared_prefix > synonyms,
                   "this provider scored a synonym above a shared prefix, which it "
                   "has no mechanism to do — check whether a real model was wired in");
}

UAPP_TEST("ai.local.embed_bounds_the_batch") {
    LocalHeuristicProvider local;
    std::vector<Message> msgs;
    for (std::size_t i = 0; i <= uapp::ai::kMaxEmbedBatch; ++i) {
        msgs.push_back(Message{Role::Document, "text " + std::to_string(i)});
    }
    Content c = Content::on_device_only(std::move(msgs));
    EmbedRequest req;
    req.content = &c;
    EmbedResult out;
    UAPP_REQUIRE_STATUS(local.embed(req, out), Status::AiInputTooLarge);
}

UAPP_TEST("ai.local.handles_utf8_and_very_long_tokens_without_corrupting_them") {
    LocalHeuristicProvider local;

    // Multi-byte UTF-8 must survive byte-for-byte through an extractive
    // summary, because the summary is a slice of the input. A tokenizer that
    // split a UTF-8 sequence would show up here as mangled output.
    const std::string doc =
        "Première phrase avec des accents. "
        "Deuxième phrase, plus informative, à propos des accents français. "
        "Troisième.";
    Content c = Content::on_device_only(doc, Role::Document);
    SummarizeRequest req;
    req.content       = &c;
    req.max_sentences = 1;
    SummarizeResult out;
    UAPP_REQUIRE_OK(local.summarize(req, out));
    UAPP_REQUIRE(out.source_spans.size() == 1);
    const std::string slice =
        doc.substr(out.source_spans[0].offset, out.source_spans[0].length);
    UAPP_CHECK(out.text == slice);

    // A single 100 KB "word" must not break anything.
    const std::string long_token(100000, 'q');
    Content c2 = Content::on_device_only(long_token + " end.");
    SummarizeRequest r2;
    r2.content = &c2;
    SummarizeResult o2;
    UAPP_REQUIRE_OK(local.summarize(r2, o2));

    ExtractRequest r3;
    r3.content = &c2;
    r3.kinds   = kAllFieldKinds;
    ExtractResult o3;
    UAPP_REQUIRE_OK(local.extract(r3, o3));

    EmbedRequest r4;
    r4.content = &c2;
    EmbedResult o4;
    UAPP_REQUIRE_OK(local.embed(r4, o4));
}
