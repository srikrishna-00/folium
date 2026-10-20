# ai/ — spec §17 (provider abstraction), §18 (RAG), §19 (features),
# §20 (agents).
#
# Spec §7's suggested structure for this directory names /llm, /embeddings,
# /rag, /agents, /summarization, /classification, /extraction, /translation.
# This slice does NOT create eight directories, because seven of them would be
# empty and the eighth would be misnamed: the thing built here is ONE
# provider-independent interface covering all seven of §17's methods, plus ONE
# implementation of it. Splitting an interface across /summarization,
# /classification, /extraction and /translation would put four directories
# inside one header.
#
# So the split is by ROLE, which is the split that actually exists:
#
#   ai/provider — the abstraction. AIProvider, the consent boundary, the
#                 registry. Knows about no concrete provider.
#   ai/local    — one real, fully on-device, no-network, no-model-file
#                 implementation, plus the degenerate no-capability provider
#                 the registry is tested against.
#
# §7's /rag and /agents will be real directories when spec §18 and §20 are
# implemented. They are NOT created empty here: an empty directory with a spec
# section number on it reads as "started", and neither has been.
#
# Added to the build after core/ because ai/provider links uapp::core_common
# (Status) and nothing else, and before storage/ and file-engine/ to make the
# direction visible: the AI layer depends on core/common alone, and nothing in
# the tree depends on the AI layer yet.
add_subdirectory(provider)
add_subdirectory(local)
