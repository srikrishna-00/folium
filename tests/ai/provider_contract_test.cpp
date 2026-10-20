// SPDX-License-Identifier: Apache-2.0
//
// tests/ai/provider_contract_test.cpp — properties of the ABSTRACTION that
// every provider must satisfy, and the base class's two non-bypassable
// boundaries (untrusted input in, untrusted provider output back).
//
// The most valuable case here is `ai.contract.declared_capabilities_match_
// behaviour`: it walks every provider in the tree x all seven §17 methods and
// requires `AiCapabilityNotImplemented` exactly when the descriptor's
// capability bit is clear. That is the check that stops a capability CLAIM
// drifting from what the code does — the same discipline
// tests/file_engine/detection_test.cpp applies to file-format-support.md.

#include <string>
#include <vector>

#include "ai_test_doubles.h"
#include "test_framework.h"
#include "uapp/ai/local/local_heuristic_provider.h"
#include "uapp/ai/local/null_provider.h"
#include "uapp/ai/provider.h"

using uapp::Status;
using uapp::ai::AIProvider;
using uapp::ai::Capability;
using uapp::ai::capability_name;
using uapp::ai::ChatRequest;
using uapp::ai::ChatResult;
using uapp::ai::ClassifyLabel;
using uapp::ai::ClassifyRequest;
using uapp::ai::ClassifyResult;
using uapp::ai::Content;
using uapp::ai::EmbedRequest;
using uapp::ai::EmbedResult;
using uapp::ai::ExtractRequest;
using uapp::ai::ExtractResult;
using uapp::ai::FieldKind;
using uapp::ai::GenerateRequest;
using uapp::ai::GenerateResult;
using uapp::ai::has_capability;
using uapp::ai::kAllCapabilities;
using uapp::ai::kAllFieldKinds;
using uapp::ai::kMaxRequestContentBytes;
using uapp::ai::Locality;
using uapp::ai::Message;
using uapp::ai::OutputProvenance;
using uapp::ai::ProviderDescriptor;
using uapp::ai::Role;
using uapp::ai::SummarizeRequest;
using uapp::ai::SummarizeResult;
using uapp::ai::TranslateRequest;
using uapp::ai::TranslateResult;
using uapp::ai::validate_descriptor;
using uapp::ai::local::LocalHeuristicProvider;
using uapp::ai::local::NullProvider;
using uapp::ai::test_doubles::MisbehavingDouble;

namespace {

std::vector<ClassifyLabel> two_labels() {
    std::vector<ClassifyLabel> labels;
    ClassifyLabel a;
    a.id           = "alpha";
    a.display_name = "Alpha";
    a.hints        = {"alpha"};
    labels.push_back(a);
    ClassifyLabel b;
    b.id           = "beta";
    b.display_name = "Beta";
    b.hints        = {"beta"};
    labels.push_back(b);
    return labels;
}

/// Call one of the seven by capability, returning the Status. Used so the
/// capability-vs-behaviour matrix is one loop rather than seven copies.
Status invoke(AIProvider& p, Capability cap, const Content& content) {
    switch (cap) {
        case Capability::Generate: {
            GenerateRequest req;
            req.content = &content;
            GenerateResult out;
            return p.generate(req, out);
        }
        case Capability::Chat: {
            ChatRequest req;
            req.content = &content;
            ChatResult out;
            return p.chat(req, out);
        }
        case Capability::Embed: {
            EmbedRequest req;
            req.content = &content;
            EmbedResult out;
            return p.embed(req, out);
        }
        case Capability::Summarize: {
            SummarizeRequest req;
            req.content       = &content;
            req.max_sentences = 2;
            SummarizeResult out;
            return p.summarize(req, out);
        }
        case Capability::Classify: {
            ClassifyRequest req;
            req.content = &content;
            req.labels  = two_labels();
            ClassifyResult out;
            return p.classify(req, out);
        }
        case Capability::Extract: {
            ExtractRequest req;
            req.content = &content;
            req.kinds   = kAllFieldKinds;
            ExtractResult out;
            return p.extract(req, out);
        }
        case Capability::Translate: {
            TranslateRequest req;
            req.content         = &content;
            req.target_language = "fr";
            TranslateResult out;
            return p.translate(req, out);
        }
    }
    return Status::Internal;
}

const char* const kSample =
    "Alpha appears here. The quarterly report is attached. "
    "Send questions to ana@example.com before 2026-03-14.";

}  // namespace

// --- the capability claim vs the behaviour ---------------------------------

UAPP_TEST("ai.contract.declared_capabilities_match_behaviour") {
    LocalHeuristicProvider local;
    NullProvider           nothing("null:none");

    AIProvider* providers[] = {&local, &nothing};

    for (AIProvider* p : providers) {
        const ProviderDescriptor& d = p->descriptor();
        for (const Capability cap : kAllCapabilities) {
            Content content = Content::on_device_only(kSample, Role::Document);
            const Status s  = invoke(*p, cap, content);
            const bool claimed = has_capability(d.capabilities, cap);

            if (claimed) {
                UAPP_CHECK_MSG(s != Status::AiCapabilityNotImplemented,
                               d.id + " claims " + capability_name(cap) +
                                   " but returned AI_CAPABILITY_NOT_IMPLEMENTED");
            } else {
                UAPP_CHECK_MSG(s == Status::AiCapabilityNotImplemented,
                               d.id + " does NOT claim " + capability_name(cap) +
                                   " but returned " + uapp::status_code(s));
            }
        }
    }
}

UAPP_TEST("ai.contract.local_provider_declares_exactly_four_capabilities") {
    LocalHeuristicProvider local;
    const ProviderDescriptor& d = local.descriptor();

    // The four it really implements...
    UAPP_CHECK(has_capability(d.capabilities, Capability::Embed));
    UAPP_CHECK(has_capability(d.capabilities, Capability::Summarize));
    UAPP_CHECK(has_capability(d.capabilities, Capability::Classify));
    UAPP_CHECK(has_capability(d.capabilities, Capability::Extract));

    // ...and the three that need a real language model, which it does not
    // have and does not pretend to have.
    UAPP_CHECK(!has_capability(d.capabilities, Capability::Generate));
    UAPP_CHECK(!has_capability(d.capabilities, Capability::Chat));
    UAPP_CHECK(!has_capability(d.capabilities, Capability::Translate));

    // The honesty field. Nothing derived from this provider may be labelled
    // AI-generated.
    UAPP_CHECK(d.provenance == OutputProvenance::HeuristicNoModel);
    UAPP_CHECK(d.locality == Locality::OnDevice);
    UAPP_CHECK(d.deterministic);
}

UAPP_TEST("ai.contract.generative_methods_return_a_specific_status_not_fake_text") {
    LocalHeuristicProvider local;
    Content content = Content::on_device_only(kSample, Role::Document);

    GenerateResult gen;
    GenerateRequest greq;
    greq.content = &content;
    UAPP_REQUIRE_STATUS(local.generate(greq, gen), Status::AiCapabilityNotImplemented);
    UAPP_CHECK_MSG(gen.text.empty(),
                   "generate() produced text from a provider with no model");

    ChatResult chat;
    ChatRequest creq;
    creq.content = &content;
    UAPP_REQUIRE_STATUS(local.chat(creq, chat), Status::AiCapabilityNotImplemented);
    UAPP_CHECK(chat.reply.text.empty());

    TranslateResult tr;
    TranslateRequest treq;
    treq.content         = &content;
    treq.target_language = "fr";
    UAPP_REQUIRE_STATUS(local.translate(treq, tr), Status::AiCapabilityNotImplemented);
    UAPP_CHECK_MSG(tr.text.empty(),
                   "translate() produced text from a provider that cannot translate");
}

UAPP_TEST("ai.contract.provenance_is_stamped_by_the_base_class") {
    // The MisbehavingDouble declares LanguageModel provenance and its bodies
    // never touch `meta`. If provenance were the provider's to set, this would
    // come back as the default HeuristicNoModel.
    MisbehavingDouble bad;
    Content content = Content::on_device_only(kSample, Role::Document);
    GenerateRequest req;
    req.content = &content;
    GenerateResult out;
    UAPP_REQUIRE_OK(bad.generate(req, out));
    UAPP_CHECK(out.meta.provenance == OutputProvenance::LanguageModel);
    UAPP_CHECK(out.meta.provider_id == "misbehaving-double:v1");
    UAPP_CHECK(out.meta.input_tokens > 0);
}

// --- untrusted input in ----------------------------------------------------

UAPP_TEST("ai.contract.bounds_null_request_content") {
    LocalHeuristicProvider local;
    SummarizeRequest req;  // content left null
    SummarizeResult out;
    UAPP_REQUIRE_STATUS(local.summarize(req, out), Status::InvalidArgument);
}

UAPP_TEST("ai.contract.bounds_empty_and_whitespace_input") {
    LocalHeuristicProvider local;

    Content empty = Content::on_device_only(std::vector<Message>{});
    SummarizeRequest r1;
    r1.content = &empty;
    SummarizeResult o1;
    UAPP_REQUIRE_STATUS(local.summarize(r1, o1), Status::AiInputEmpty);

    Content blank = Content::on_device_only("   \n\t  \r\n  ");
    SummarizeRequest r2;
    r2.content = &blank;
    SummarizeResult o2;
    UAPP_REQUIRE_STATUS(local.summarize(r2, o2), Status::AiInputEmpty);

    Content zero_len = Content::on_device_only("");
    SummarizeRequest r3;
    r3.content = &zero_len;
    SummarizeResult o3;
    UAPP_REQUIRE_STATUS(local.summarize(r3, o3), Status::AiInputEmpty);
}

UAPP_TEST("ai.contract.bounds_embedded_nul_and_control_bytes") {
    LocalHeuristicProvider local;

    std::string with_nul = "before";
    with_nul.push_back('\0');
    with_nul += "after";
    Content c_nul = Content::on_device_only(with_nul);
    SummarizeRequest r1;
    r1.content = &c_nul;
    SummarizeResult o1;
    UAPP_REQUIRE_STATUS(local.summarize(r1, o1), Status::AiInputMalformed);

    // ESC — a terminal escape sequence in any log that echoes this text.
    std::string with_esc = "colour\x1B[31mme";
    Content c_esc = Content::on_device_only(with_esc);
    SummarizeRequest r2;
    r2.content = &c_esc;
    SummarizeResult o2;
    UAPP_REQUIRE_STATUS(local.summarize(r2, o2), Status::AiInputMalformed);

    // BEL in the middle of otherwise fine prose. The literal is SPLIT on
    // purpose: "\x07dong" would be read as the single hex escape \x07d
    // ('}'), i.e. no control byte at all, and the case would pass for the
    // wrong reason. (It did, once.)
    Content c_bel = Content::on_device_only(std::string("ding\x07" "dong"));
    SummarizeRequest r3;
    r3.content = &c_bel;
    SummarizeResult o3;
    UAPP_REQUIRE_STATUS(local.summarize(r3, o3), Status::AiInputMalformed);
}

UAPP_TEST("ai.contract.bounds_allows_tab_newline_carriage_return") {
    LocalHeuristicProvider local;
    Content c = Content::on_device_only(std::string("line one.\r\n\tline two is here."));
    SummarizeRequest req;
    req.content       = &c;
    req.max_sentences = 2;
    SummarizeResult out;
    UAPP_REQUIRE_OK(local.summarize(req, out));
    UAPP_CHECK(!out.text.empty());
}

UAPP_TEST("ai.contract.bounds_oversized_input") {
    LocalHeuristicProvider local;

    // One byte past the cap.
    std::string huge(kMaxRequestContentBytes + 1, 'a');
    Content c_huge = Content::on_device_only(huge);
    SummarizeRequest r1;
    r1.content = &c_huge;
    SummarizeResult o1;
    UAPP_REQUIRE_STATUS(local.summarize(r1, o1), Status::AiInputTooLarge);

    // Exactly at the cap is accepted — the boundary is inclusive, and a cap
    // that rejected its own stated value would be off by one.
    std::string at_cap(kMaxRequestContentBytes, 'a');
    at_cap[100] = ' ';
    at_cap[200] = '.';
    Content c_cap = Content::on_device_only(at_cap);
    SummarizeRequest r2;
    r2.content       = &c_cap;
    r2.max_sentences = 1;
    SummarizeResult o2;
    UAPP_REQUIRE_OK(local.summarize(r2, o2));

    // Split across many messages: the cap is on the TOTAL, not per message,
    // so a caller cannot get past it by chunking.
    std::vector<Message> many;
    for (int i = 0; i < 20; ++i) {
        many.push_back(Message{Role::Document, std::string(60000, 'b')});
    }
    Content c_many = Content::on_device_only(std::move(many));
    SummarizeRequest r3;
    r3.content = &c_many;
    SummarizeResult o3;
    UAPP_REQUIRE_STATUS(local.summarize(r3, o3), Status::AiInputTooLarge);
}

UAPP_TEST("ai.contract.bounds_too_many_messages") {
    LocalHeuristicProvider local;
    std::vector<Message> msgs;
    for (std::size_t i = 0; i <= uapp::ai::kMaxMessagesPerRequest; ++i) {
        msgs.push_back(Message{Role::Document, "x"});
    }
    Content c = Content::on_device_only(std::move(msgs));
    SummarizeRequest req;
    req.content = &c;
    SummarizeResult out;
    UAPP_REQUIRE_STATUS(local.summarize(req, out), Status::AiInputTooLarge);
}

UAPP_TEST("ai.contract.bounds_invalid_label_sets") {
    LocalHeuristicProvider local;
    Content c = Content::on_device_only(kSample, Role::Document);

    ClassifyRequest req;
    req.content = &c;
    ClassifyResult out;

    // Empty vocabulary: there is nothing to classify into.
    UAPP_REQUIRE_STATUS(local.classify(req, out), Status::AiLabelSetInvalid);

    // Duplicate ids make the result un-attributable.
    req.labels = two_labels();
    req.labels.push_back(req.labels.front());
    UAPP_REQUIRE_STATUS(local.classify(req, out), Status::AiLabelSetInvalid);

    // An empty label id.
    req.labels = two_labels();
    req.labels[0].id.clear();
    UAPP_REQUIRE_STATUS(local.classify(req, out), Status::AiLabelSetInvalid);

    // Too many labels.
    req.labels.clear();
    for (std::size_t i = 0; i <= uapp::ai::kMaxClassifyLabels; ++i) {
        ClassifyLabel l;
        l.id = "l" + std::to_string(i);
        req.labels.push_back(l);
    }
    UAPP_REQUIRE_STATUS(local.classify(req, out), Status::AiLabelSetInvalid);

    // An oversized hint phrase.
    req.labels = two_labels();
    req.labels[0].hints[0] = std::string(uapp::ai::kMaxLabelTextBytes + 1, 'z');
    UAPP_REQUIRE_STATUS(local.classify(req, out), Status::AiLabelSetInvalid);

    // A confidence floor outside [0, 1].
    req.labels         = two_labels();
    req.min_confidence = 1.5F;
    UAPP_REQUIRE_STATUS(local.classify(req, out), Status::InvalidArgument);
}

UAPP_TEST("ai.contract.bounds_invalid_extract_and_translate_arguments") {
    LocalHeuristicProvider local;
    Content c = Content::on_device_only(kSample, Role::Document);

    ExtractRequest ereq;
    ereq.content = &c;
    ereq.kinds   = 0;  // asked for nothing
    ExtractResult eout;
    UAPP_REQUIRE_STATUS(local.extract(ereq, eout), Status::InvalidArgument);

    ereq.kinds = kAllFieldKinds | 0x8000u;  // a bit that names no kind
    UAPP_REQUIRE_STATUS(local.extract(ereq, eout), Status::InvalidArgument);

    // translate() is NOT IMPLEMENTED by this provider, but argument
    // validation happens in the base class and therefore still runs: a
    // missing target language is InvalidArgument, not
    // AiCapabilityNotImplemented. The ordering matters — a caller debugging a
    // malformed request should be told about the request.
    TranslateRequest treq;
    treq.content = &c;
    TranslateResult tout;
    UAPP_REQUIRE_STATUS(local.translate(treq, tout), Status::InvalidArgument);
}

// --- untrusted provider output back ----------------------------------------

UAPP_TEST("ai.contract.rejects_provider_contract_violations") {
    MisbehavingDouble bad;
    Content c = Content::on_device_only(kSample, Role::Document);

    {
        bad.set(MisbehavingDouble::Misbehaviour::EmptyOkText);
        GenerateRequest req;
        req.content = &c;
        GenerateResult out;
        UAPP_REQUIRE_STATUS(bad.generate(req, out), Status::AiResponseMalformed);
    }
    {
        bad.set(MisbehavingDouble::Misbehaviour::ReplyRoleNotAssistant);
        ChatRequest req;
        req.content = &c;
        ChatResult out;
        UAPP_REQUIRE_STATUS(bad.chat(req, out), Status::AiResponseMalformed);
    }
    {
        bad.set(MisbehavingDouble::Misbehaviour::WrongEmbedDim);
        EmbedRequest req;
        req.content = &c;
        EmbedResult out;
        UAPP_REQUIRE_STATUS(bad.embed(req, out), Status::AiResponseMalformed);
    }
    {
        bad.set(MisbehavingDouble::Misbehaviour::NanEmbedding);
        EmbedRequest req;
        req.content = &c;
        EmbedResult out;
        UAPP_REQUIRE_STATUS(bad.embed(req, out), Status::AiResponseMalformed);
    }
    {
        bad.set(MisbehavingDouble::Misbehaviour::LabelOutsideSet);
        ClassifyRequest req;
        req.content = &c;
        req.labels  = two_labels();
        ClassifyResult out;
        UAPP_REQUIRE_STATUS(bad.classify(req, out), Status::AiResponseMalformed);
    }
    {
        bad.set(MisbehavingDouble::Misbehaviour::AbstainWithLabel);
        ClassifyRequest req;
        req.content = &c;
        req.labels  = two_labels();
        ClassifyResult out;
        UAPP_REQUIRE_STATUS(bad.classify(req, out), Status::AiResponseMalformed);
    }
    {
        bad.set(MisbehavingDouble::Misbehaviour::ConfidenceOutOfRange);
        ClassifyRequest req;
        req.content = &c;
        req.labels  = two_labels();
        ClassifyResult out;
        UAPP_REQUIRE_STATUS(bad.classify(req, out), Status::AiResponseMalformed);
    }
    {
        bad.set(MisbehavingDouble::Misbehaviour::SpanPastEndOfInput);
        SummarizeRequest req;
        req.content = &c;
        SummarizeResult out;
        UAPP_REQUIRE_STATUS(bad.summarize(req, out), Status::AiResponseMalformed);
    }
    {
        bad.set(MisbehavingDouble::Misbehaviour::SpansWithoutExtractive);
        SummarizeRequest req;
        req.content = &c;
        SummarizeResult out;
        UAPP_REQUIRE_STATUS(bad.summarize(req, out), Status::AiResponseMalformed);
    }
    {
        bad.set(MisbehavingDouble::Misbehaviour::FieldKindNotRequested);
        ExtractRequest req;
        req.content = &c;
        req.kinds   = uapp::ai::field_bit(FieldKind::Email);
        ExtractResult out;
        UAPP_REQUIRE_STATUS(bad.extract(req, out), Status::AiResponseMalformed);
    }
    {
        // Sanity: with no misbehaviour the same provider passes every check,
        // so the cases above are failing for the reason they claim.
        bad.set(MisbehavingDouble::Misbehaviour::None);
        GenerateRequest req;
        req.content = &c;
        GenerateResult out;
        UAPP_REQUIRE_OK(bad.generate(req, out));
    }
}

// --- descriptor validation -------------------------------------------------

UAPP_TEST("ai.contract.descriptor_validation") {
    ProviderDescriptor d;
    d.id           = "x:1";
    d.display_name = "X";
    UAPP_REQUIRE_OK(validate_descriptor(d));

    ProviderDescriptor no_id = d;
    no_id.id.clear();
    UAPP_REQUIRE_STATUS(validate_descriptor(no_id), Status::InvalidArgument);

    ProviderDescriptor no_name = d;
    no_name.display_name.clear();
    UAPP_REQUIRE_STATUS(validate_descriptor(no_name), Status::InvalidArgument);

    // Claims Embed with no dimension.
    ProviderDescriptor embed_no_dim = d;
    embed_no_dim.capabilities = uapp::ai::cap_bit(Capability::Embed);
    UAPP_REQUIRE_STATUS(validate_descriptor(embed_no_dim), Status::InvalidArgument);

    // Declares a dimension without claiming Embed.
    ProviderDescriptor dim_no_embed = d;
    dim_no_embed.embed_dim = 384;
    UAPP_REQUIRE_STATUS(validate_descriptor(dim_no_embed), Status::InvalidArgument);

    // Past the dimension ceiling.
    ProviderDescriptor huge_dim = d;
    huge_dim.capabilities = uapp::ai::cap_bit(Capability::Embed);
    huge_dim.embed_dim    = uapp::ai::kMaxEmbedDim + 1;
    UAPP_REQUIRE_STATUS(validate_descriptor(huge_dim), Status::InvalidArgument);

    // THE ONE THAT MATTERS: an off-device provider with no published
    // retention policy cannot be registered, because the consent prompt
    // (§13.2 step 5) is required to show one.
    ProviderDescriptor cloud_no_policy = d;
    cloud_no_policy.locality = Locality::ThirdParty;
    UAPP_REQUIRE_STATUS(validate_descriptor(cloud_no_policy), Status::InvalidArgument);

    cloud_no_policy.data_policy_url = "https://example.invalid/terms";
    UAPP_REQUIRE_OK(validate_descriptor(cloud_no_policy));
}
