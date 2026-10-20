// SPDX-License-Identifier: Apache-2.0
//
// tests/ai/ai_test_doubles.h — providers that exist only to be tested against.
//
// These are NOT shipped code and are NOT in ai/local for that reason. Each
// exists because a property of the ABSTRACTION cannot be observed with the
// real on-device provider alone:
//
//   * `OffDeviceDouble` has `Locality::ThirdParty`, which no shipped provider
//     in this tree has (there is no network here). Without it, the consent
//     boundary — the whole point of ai/provider/consent.h — would be
//     untestable, since an on-device provider skips the check by design.
//     It makes NO network call: it is a local object that merely CLAIMS to be
//     third-party, which is exactly what is needed to test a check that keys
//     off the claim.
//   * `MisbehavingDouble` breaks its own declared output contract on demand,
//     so that `AIProvider`'s output validation (which a correct provider never
//     triggers) gets exercised. This is the stand-in for "a cloud response is
//     bytes off a socket and must not be trusted".
//   * `FailingDouble` returns a chosen failure status, so the registry's
//     failover rules can be driven through every branch of
//     `ai::is_failover_eligible`.

#ifndef UAPP_TESTS_AI_TEST_DOUBLES_H
#define UAPP_TESTS_AI_TEST_DOUBLES_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "uapp/ai/consent.h"
#include "uapp/ai/provider.h"

namespace uapp::ai::test_doubles {

// --- injectable clock ------------------------------------------------------

inline std::uint64_t g_fake_now_ms = 1'000'000;

inline std::uint64_t fake_clock() noexcept { return g_fake_now_ms; }

// --- an off-device provider that never touches a network -------------------

class OffDeviceDouble final : public AIProvider {
public:
    explicit OffDeviceDouble(std::string id = "cloud-double:v1",
                             Locality locality = Locality::ThirdParty)
        : AIProvider(&fake_clock) {
        descriptor_.id              = std::move(id);
        descriptor_.display_name    = "Off-device test double";
        descriptor_.capabilities    = cap_mask(Capability::Summarize, Capability::Chat,
                                            Capability::Extract);
        descriptor_.locality        = locality;
        descriptor_.provenance      = OutputProvenance::LanguageModel;
        descriptor_.deterministic   = true;
        descriptor_.data_policy_url = "https://example.invalid/retention";
    }

    [[nodiscard]] const ProviderDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }
    [[nodiscard]] Health health() const noexcept override { return Health::Healthy; }

    /// Number of times any `do_*` body ran. The consent property under test is
    /// that this stays ZERO when authorisation is missing: the provider must
    /// not merely refuse to send, it must never see the bytes.
    [[nodiscard]] int body_calls() const noexcept { return body_calls_; }

protected:
    Status do_generate(const GenerateRequest&, GenerateResult&) override {
        ++body_calls_;
        return Status::AiCapabilityNotImplemented;
    }
    Status do_chat(const ChatRequest&, ChatResult& out) override {
        ++body_calls_;
        out.reply.role = Role::Assistant;
        out.reply.text = "ack";
        return Status::Ok;
    }
    Status do_embed(const EmbedRequest&, EmbedResult&) override {
        ++body_calls_;
        return Status::AiCapabilityNotImplemented;
    }
    Status do_summarize(const SummarizeRequest&, SummarizeResult& out) override {
        ++body_calls_;
        out.is_extractive = false;
        out.text          = "a summary from the off-device double";
        return Status::Ok;
    }
    Status do_classify(const ClassifyRequest&, ClassifyResult&) override {
        ++body_calls_;
        return Status::AiCapabilityNotImplemented;
    }
    Status do_extract(const ExtractRequest&, ExtractResult& out) override {
        ++body_calls_;
        out.fields.clear();
        return Status::Ok;
    }
    Status do_translate(const TranslateRequest&, TranslateResult&) override {
        ++body_calls_;
        return Status::AiCapabilityNotImplemented;
    }

private:
    ProviderDescriptor descriptor_;
    int                body_calls_ = 0;
};

// --- a provider that violates its own output contract ----------------------

class MisbehavingDouble final : public AIProvider {
public:
    enum class Misbehaviour {
        None,
        EmptyOkText,         ///< Ok with no text
        WrongEmbedDim,       ///< a width its descriptor does not declare
        NanEmbedding,        ///< a non-finite component
        LabelOutsideSet,     ///< a classification label nobody offered
        AbstainWithLabel,    ///< abstained == true AND a label
        ConfidenceOutOfRange,
        SpanPastEndOfInput,  ///< a summary span that is not in the source
        SpansWithoutExtractive,
        FieldKindNotRequested,
        ReplyRoleNotAssistant,
    };

    MisbehavingDouble() {
        descriptor_.id              = "misbehaving-double:v1";
        descriptor_.display_name    = "Contract-violating test double";
        descriptor_.capabilities    = cap_mask(Capability::Generate, Capability::Chat,
                                            Capability::Embed, Capability::Summarize,
                                            Capability::Classify, Capability::Extract,
                                            Capability::Translate);
        descriptor_.locality        = Locality::OnDevice;
        descriptor_.provenance      = OutputProvenance::LanguageModel;
        descriptor_.embed_dim       = 8;
        descriptor_.deterministic   = false;
    }

    void set(Misbehaviour m) noexcept { mode_ = m; }

    [[nodiscard]] const ProviderDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }
    [[nodiscard]] Health health() const noexcept override { return Health::Healthy; }

protected:
    Status do_generate(const GenerateRequest&, GenerateResult& out) override {
        if (mode_ == Misbehaviour::EmptyOkText) { out.text.clear(); return Status::Ok; }
        out.text = "ok";
        return Status::Ok;
    }

    Status do_chat(const ChatRequest&, ChatResult& out) override {
        out.reply.text = "ok";
        out.reply.role = mode_ == Misbehaviour::ReplyRoleNotAssistant ? Role::System
                                                                     : Role::Assistant;
        return Status::Ok;
    }

    Status do_embed(const EmbedRequest& req, EmbedResult& out) override {
        const std::size_t n = req.content->messages().size();
        out.count = n;
        out.dim   = mode_ == Misbehaviour::WrongEmbedDim ? 7u : 8u;
        out.vectors.assign(n * static_cast<std::size_t>(out.dim), 0.5F);
        if (mode_ == Misbehaviour::NanEmbedding && !out.vectors.empty()) {
            out.vectors[0] = 0.0F / 0.0F;
        }
        return Status::Ok;
    }

    Status do_summarize(const SummarizeRequest& req, SummarizeResult& out) override {
        out.text = "summary";
        if (mode_ == Misbehaviour::SpanPastEndOfInput) {
            out.is_extractive = true;
            out.source_spans.push_back(
                SummarizeResult::Span{req.content->byte_size(), 10});
        } else if (mode_ == Misbehaviour::SpansWithoutExtractive) {
            out.is_extractive = false;
            out.source_spans.push_back(SummarizeResult::Span{0, 1});
        }
        return Status::Ok;
    }

    Status do_classify(const ClassifyRequest& req, ClassifyResult& out) override {
        switch (mode_) {
            case Misbehaviour::LabelOutsideSet:
                out.label_id   = "a-label-nobody-offered";
                out.confidence = 0.9F;
                return Status::Ok;
            case Misbehaviour::AbstainWithLabel:
                out.abstained  = true;
                out.label_id   = req.labels.front().id;
                return Status::Ok;
            case Misbehaviour::ConfidenceOutOfRange:
                out.label_id   = req.labels.front().id;
                out.confidence = 1.5F;
                return Status::Ok;
            default:
                out.label_id   = req.labels.front().id;
                out.confidence = 1.0F;
                return Status::Ok;
        }
    }

    Status do_extract(const ExtractRequest&, ExtractResult& out) override {
        if (mode_ == Misbehaviour::FieldKindNotRequested) {
            ExtractedField f;
            f.kind   = FieldKind::Money;
            f.value  = "$1";
            f.offset = 0;
            f.length = 1;
            out.fields.push_back(std::move(f));
        }
        return Status::Ok;
    }

    Status do_translate(const TranslateRequest&, TranslateResult& out) override {
        out.text = "traduction";
        return Status::Ok;
    }

private:
    ProviderDescriptor descriptor_;
    Misbehaviour       mode_ = Misbehaviour::None;
};

// --- a provider that fails with a chosen status ----------------------------

class FailingDouble final : public AIProvider {
public:
    FailingDouble(std::string id, Status failure, Health health = Health::Healthy)
        : failure_(failure), health_(health) {
        descriptor_.id            = std::move(id);
        descriptor_.display_name  = "Failing test double";
        descriptor_.capabilities  = cap_mask(Capability::Summarize);
        descriptor_.locality      = Locality::OnDevice;
        descriptor_.provenance    = OutputProvenance::HeuristicNoModel;
        descriptor_.deterministic = true;
    }

    [[nodiscard]] const ProviderDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }
    [[nodiscard]] Health health() const noexcept override { return health_; }

    [[nodiscard]] int summarize_calls() const noexcept { return summarize_calls_; }

protected:
    Status do_generate(const GenerateRequest&, GenerateResult&) override {
        return Status::AiCapabilityNotImplemented;
    }
    Status do_chat(const ChatRequest&, ChatResult&) override {
        return Status::AiCapabilityNotImplemented;
    }
    Status do_embed(const EmbedRequest&, EmbedResult&) override {
        return Status::AiCapabilityNotImplemented;
    }
    Status do_summarize(const SummarizeRequest&, SummarizeResult&) override {
        ++summarize_calls_;
        return failure_;
    }
    Status do_classify(const ClassifyRequest&, ClassifyResult&) override {
        return Status::AiCapabilityNotImplemented;
    }
    Status do_extract(const ExtractRequest&, ExtractResult&) override {
        return Status::AiCapabilityNotImplemented;
    }
    Status do_translate(const TranslateRequest&, TranslateResult&) override {
        return Status::AiCapabilityNotImplemented;
    }

private:
    ProviderDescriptor descriptor_;
    Status             failure_;
    Health             health_;
    int                summarize_calls_ = 0;
};

}  // namespace uapp::ai::test_doubles

#endif  // UAPP_TESTS_AI_TEST_DOUBLES_H
