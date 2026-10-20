// SPDX-License-Identifier: Apache-2.0
//
// ai/local/null_provider.h — a provider that declares it can do nothing.
//
// ---------------------------------------------------------------------------
// WHY THIS IS SHIPPED CODE AND NOT A TEST FIXTURE
// ---------------------------------------------------------------------------
// It is the registry's degenerate case, and a registry that has never been
// run against one is untested where it matters. Three real situations produce
// exactly this shape:
//
//   1. **A provider whose backing model is not installed.** A local llama.cpp
//      adapter with no downloaded weights can do nothing until the download
//      finishes, and ai-architecture.md §4's answer for that is
//      `AI_NO_LOCAL_MODEL` plus an offer to download — NOT a cloud call. An
//      empty-capability provider that is nonetheless registered and
//      enumerable is how the settings UI lists a model the user could install.
//   2. **A cloud provider with no API key configured.** Registered so the UI
//      can show it; capable of nothing until credentials exist.
//   3. **A build where an optional integration was compiled out.** The same
//      pattern `UAPP_PDF_ENABLED` and `UAPP_CRYPTO_ENABLED` use, one level up.
//
// The property it proves: a provider that can do nothing is handled
// GRACEFULLY. `ProviderRegistry::select()` never offers it, failover skips
// past it to a provider that can work, and calling any of the seven methods
// on it directly returns `Status::AiCapabilityNotImplemented` rather than
// crashing, hanging or returning Ok with an empty result.
//
// It is NOT a mock that returns canned success. It returns canned FAILURE,
// which is the honest thing for a component that cannot do the job.

#ifndef UAPP_AI_LOCAL_NULL_PROVIDER_H
#define UAPP_AI_LOCAL_NULL_PROVIDER_H

#include <string>

#include "uapp/ai/provider.h"

namespace uapp::ai::local {

class NullProvider final : public AIProvider {
public:
    /// `id` so several can coexist in one registry (the realistic case: two
    /// uninstalled models). `health` so the "registered but unavailable"
    /// case — situation 1 above — can be expressed, which is what
    /// `ProviderRegistry::candidates()` filters on.
    explicit NullProvider(std::string id, Health health = Health::Healthy);

    [[nodiscard]] const ProviderDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }
    [[nodiscard]] Health health() const noexcept override { return health_; }

    void set_health(Health h) noexcept { health_ = h; }

protected:
    [[nodiscard]] Status do_generate(const GenerateRequest&, GenerateResult&) override;
    [[nodiscard]] Status do_chat(const ChatRequest&, ChatResult&) override;
    [[nodiscard]] Status do_embed(const EmbedRequest&, EmbedResult&) override;
    [[nodiscard]] Status do_summarize(const SummarizeRequest&, SummarizeResult&) override;
    [[nodiscard]] Status do_classify(const ClassifyRequest&, ClassifyResult&) override;
    [[nodiscard]] Status do_extract(const ExtractRequest&, ExtractResult&) override;
    [[nodiscard]] Status do_translate(const TranslateRequest&, TranslateResult&) override;

private:
    ProviderDescriptor descriptor_;
    Health             health_;
};

}  // namespace uapp::ai::local

#endif  // UAPP_AI_LOCAL_NULL_PROVIDER_H
