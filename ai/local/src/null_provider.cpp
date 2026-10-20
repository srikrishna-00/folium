// SPDX-License-Identifier: Apache-2.0
#include "uapp/ai/local/null_provider.h"

#include <utility>

namespace uapp::ai::local {

NullProvider::NullProvider(std::string id, Health health) : health_(health) {
    descriptor_.id           = std::move(id);
    descriptor_.display_name = "Unavailable provider (no capabilities)";
    descriptor_.capabilities = 0;  // the whole point
    descriptor_.locality     = Locality::OnDevice;
    descriptor_.provenance   = OutputProvenance::HeuristicNoModel;
    descriptor_.embed_dim    = 0;  // must be 0 when Embed is not claimed
    descriptor_.deterministic = true;
}

// Seven one-line bodies. `AIProvider`'s `do_*` are pure virtual precisely so
// that writing these is a conscious act rather than an inherited default —
// see the "WHY PURE VIRTUAL" note in ai/provider/include/uapp/ai/provider.h.
Status NullProvider::do_generate(const GenerateRequest&, GenerateResult&) {
    return Status::AiCapabilityNotImplemented;
}
Status NullProvider::do_chat(const ChatRequest&, ChatResult&) {
    return Status::AiCapabilityNotImplemented;
}
Status NullProvider::do_embed(const EmbedRequest&, EmbedResult&) {
    return Status::AiCapabilityNotImplemented;
}
Status NullProvider::do_summarize(const SummarizeRequest&, SummarizeResult&) {
    return Status::AiCapabilityNotImplemented;
}
Status NullProvider::do_classify(const ClassifyRequest&, ClassifyResult&) {
    return Status::AiCapabilityNotImplemented;
}
Status NullProvider::do_extract(const ExtractRequest&, ExtractResult&) {
    return Status::AiCapabilityNotImplemented;
}
Status NullProvider::do_translate(const TranslateRequest&, TranslateResult&) {
    return Status::AiCapabilityNotImplemented;
}

}  // namespace uapp::ai::local
