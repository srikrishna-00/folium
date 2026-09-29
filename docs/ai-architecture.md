# AI Architecture — Providers, RAG, Agents

**Status:** Phase 0 draft — awaiting human sign-off
**Version:** 0.0.0
**Date:** 2026-09-29

Implements spec §17 (provider abstraction), §18 (RAG), §19 (features), §20 (agents),
§24 (privacy).

---

## 1. Principles

1. **No provider is load-bearing.** Removing any single provider must be a configuration
   change, not a refactor (spec §17).
2. **Local is a first-class provider, not a degraded mode.** The local path must satisfy a
   real user journey end to end, or "privacy-first" is marketing.
3. **Policy is enforced below the feature layer.** Every AI call passes through one
   non-bypassable gate. Features cannot opt out.
4. **Citations are verified, not trusted.** The model proposes; the system validates against
   the chunk store.
5. **Degrade loudly.** If the requested capability is unavailable, fail with a specific code.
   Never silently substitute a weaker model or a cloud provider for a local one.
6. **Everything is a job.** Generation is slow and cancellable.
7. **The server never sees plaintext.** `adr/0002` decision 1 makes E2EE a v1.0 property, so
   no AI or RAG stage may be designed server-side. Indexing, embedding, retrieval and citation
   verification are all on-device; plaintext crosses the network only through the explicit
   per-document consent flow. **See §13 — it qualifies §7 (RAG) throughout.**

## 2. Layering

```
     Feature layer (summarize, extract, flashcards, ask, agent runs)
                              │
     ┌────────────────────────▼──────────────────────────────────┐
     │ AI POLICY GATE  (non-bypassable)                           │
     │  · workspace ai_policy  · per-op consent  · quota          │
     │  · content classification  · audit write                   │
     └────────────────────────┬──────────────────────────────────┘
                              │
     ┌────────────────────────▼──────────────────────────────────┐
     │ ROUTER                                                     │
     │  capability match → policy filter → cost/latency → health  │
     └──────┬──────────────────────┬───────────────────┬─────────┘
            │                      │                   │
   ┌────────▼───────┐    ┌─────────▼────────┐   ┌──────▼─────────┐
   │ LocalProvider  │    │ CloudProvider A  │   │ CloudProvider B│
   │ llama.cpp      │    │ HTTP adapter     │   │ HTTP adapter   │
   │ ONNX Runtime   │    └──────────────────┘   └────────────────┘
   └────────────────┘
            │ all implement AIProviderPort
     ┌──────▼───────────────────────────────────────────────────┐
     │ Shared services: tokenizer · context budgeter ·           │
     │ streaming · retry/backoff · response cache · telemetry    │
     └──────────────────────────────────────────────────────────┘
```

## 3. `AIProviderPort`

Spec §17 lists `generate/chat/embed/summarize/classify/extract/translate`. Making all seven
provider primitives is wrong: no provider implements `summarize` natively, so every adapter
would reimplement the same prompt. We split it:

**Provider primitives (what an adapter must implement):**

```c
typedef struct uapp_ai_provider_s uapp_ai_provider_t;

typedef struct {
    const char* id;                 /* "local:qwen3-1.7b-q4" | "cloud-a:model-x" */
    uint32_t    capabilities;       /* bitmask, see below                        */
    uint32_t    context_tokens;
    uint32_t    max_output_tokens;
    uint32_t    embed_dim;          /* 0 if not an embedding model               */
    bool        supports_streaming;
    bool        supports_tools;
    bool        supports_json_schema;
    bool        supports_vision;
    uint8_t     locality;           /* 0 on-device, 1 our-server, 2 third-party  */
    uint32_t    cost_per_mtok_in;   /* micro-units; 0 for local                  */
    uint32_t    cost_per_mtok_out;
    const char* data_policy_url;    /* published retention/training policy       */
} uapp_ai_model_info_t;

/* CAP_CHAT | CAP_COMPLETE | CAP_EMBED | CAP_RERANK | CAP_TOOLS | CAP_VISION | CAP_JSON */

uapp_status_t uapp_ai_list_models (uapp_ai_provider_t*, uapp_ai_model_info_t**, size_t*);
uapp_status_t uapp_ai_chat        (uapp_ai_provider_t*, const uapp_ai_chat_req_t*,
                                   uapp_ai_stream_cb, void* user);
uapp_status_t uapp_ai_embed       (uapp_ai_provider_t*, const char** texts, size_t n,
                                   float* out /* n * dim */);
uapp_status_t uapp_ai_rerank      (uapp_ai_provider_t*, const char* query,
                                   const char** docs, size_t n, float* scores);  /* optional */
uapp_status_t uapp_ai_health      (uapp_ai_provider_t*, uapp_ai_health_t*);
uapp_status_t uapp_ai_count_tokens(uapp_ai_provider_t*, const char* text, uint32_t*);
```

**Task services (built once, on top of primitives, in `ai/`):**
`summarize`, `classify`, `extract`, `translate`, `generate_study_artifacts`, `compare`.
Each owns its prompt templates, output schema, validation and retry. Adding a provider
requires implementing five functions, not thirteen prompts.

### 3.1 Request shape

```c
typedef struct {
    const char*               model_id;
    const uapp_ai_message_t*  messages;   size_t message_count;
    const uapp_ai_tool_t*     tools;      size_t tool_count;
    const char*               json_schema;     /* NULL unless structured output */
    float                     temperature;
    uint32_t                  max_tokens;
    uint32_t                  timeout_ms;
    const char*               idempotency_key;
    uapp_ai_policy_token_t    policy;     /* opaque, issued by the gate; required */
} uapp_ai_chat_req_t;
```

`policy` is an unforgeable token issued by the policy gate. A provider adapter **refuses** a
request without a valid one. This makes bypassing the gate a compile-and-runtime error rather
than a discipline problem.

### 3.2 Message roles and the injection boundary

```
system     — our instructions. Never contains user document content.
user       — the user's actual typed input.
document   — retrieved content. STRUCTURALLY DISTINCT from `user`.
assistant  — prior model output.
tool       — tool results.
```

The `document` role is our own, rendered into whatever the provider supports with explicit
delimiters and a standing instruction that content inside it is **data, never instructions**.
Retrieved text is never concatenated into the system prompt. This does not solve prompt
injection (T-402) but it is the necessary first step, and it makes the boundary auditable.

## 4. Policy gate

Every call, without exception:

```
1. Resolve scope        → which workspace/object does this operate on?
2. Read workspace.ai_policy:
      LOCAL_ONLY   → only locality==0 models are eligible
      ASK          → cloud allowed only with a fresh, matching consent record
      CLOUD_OK     → cloud allowed
3. Content classification (cheap, local, heuristic):
      flags for detected credentials, ID numbers, payment data → force ASK even in CLOUD_OK
4. Consent check        → scope, provider, and grant time must match; grants expire
5. Quota check          → token budget for the account/tier
6. Emit an audit record BEFORE the call:
      { at, actor, action:"ai.egress", provider, model, locality,
        object_ref, approx_tokens, consent_id }
   (the record contains references and counts, never content)
7. Mint the policy token and dispatch
```

**Failure modes are explicit codes, never fallbacks:**
`AI_POLICY_BLOCKED` · `AI_CONSENT_REQUIRED` · `AI_NO_LOCAL_MODEL` · `AI_QUOTA_EXCEEDED` ·
`AI_PROVIDER_UNAVAILABLE`.

> If policy is LOCAL_ONLY and no local model is installed, the answer is
> `AI_NO_LOCAL_MODEL` plus an offer to download one. It is **never** a cloud call. This single
> behaviour is the difference between a privacy promise and a privacy claim.

## 5. Router

Selection is deterministic and explainable (the UI can show *why* a model was used):

```
candidates = models where capability ⊇ required
           ∩ models allowed by policy
           ∩ models currently healthy
           ∩ models whose context ≥ estimated prompt tokens
rank by:   explicit user pin
       →   locality preference (local first when policy is LOCAL_ONLY or ASK-unanswered)
       →   task-quality tier (task-specific default)
       →   cost
       →   observed p95 latency
```

- **Circuit breaker** per provider: 5 consecutive failures ⇒ open for 60 s, half-open probe.
- **Retries** only on 429/5xx/timeout, with jittered exponential backoff, max 3. Never retry a
  content-policy refusal.
- **No cross-provider fallback across a locality boundary.** Falling back from local to cloud
  on an error would violate the policy the user set. Falling back cloud-A → cloud-B is allowed
  only if both are consented for that scope.

## 6. Local inference

| Concern | Decision |
|---|---|
| Runtime | llama.cpp (MIT) for generative; ONNX Runtime (MIT) for embeddings/classifiers |
| Generative model class | 1–3 B parameters, 4-bit quantised (~0.8–2 GB) |
| Embedding model | 384-dim, ~30–90 MB int8 (e.g. a MiniLM/BGE-small-class model) |
| Acceleration | Metal (iOS/macOS), Vulkan or CPU+NEON (Android), NNAPI/Core ML EPs for ONNX |
| Gating | A device-capability probe (RAM, SoC class, thermal headroom) decides whether the generative local model is offered at all |
| Lifecycle | Models are downloaded on demand, signature-verified, versioned, evictable under storage pressure, and resumable mid-download |

**Honest limits, to be stated in-product:**
- A 1–3 B local model is meaningfully weaker than a frontier cloud model at long-document
  reasoning, multi-hop extraction, and non-English tasks.
- Local summarisation of a 50-page document requires map-reduce chunking and will be slow
  (minutes, not seconds) on mid-tier hardware.
- Sustained generation heats the device and throttles. Long jobs must yield.
- **Embeddings are the local feature that actually works well at this size.** Semantic search
  fully local is realistic; frontier-quality local reasoning is not. Product messaging should
  reflect that split.

Spike SP-4 (`technical-requirements.md` §6) validates the generative side. If it fails, the
honest outcome is: *local = embeddings + classification + search; generation = cloud only, with
the policy gate making that explicit.* That is an acceptable v1.0.

## 7. RAG architecture (spec §18)

```
Documents / pages / files
        │
   ┌────▼──────────────────────────────────────────────┐
   │ 1. EXTRACT — text + structure + provenance        │
   │    file-engine parsers, PDF text layer, OCR       │
   │    Every span keeps (page, block_id, char range)  │
   └────┬──────────────────────────────────────────────┘
   ┌────▼──────────────────────────────────────────────┐
   │ 2. CHUNK — structure-aware                        │
   │    · split on headings/block boundaries first     │
   │    · target 300–500 tokens, 15 % overlap          │
   │    · never split a table row or code block        │
   │    · prepend a breadcrumb: "Doc › H1 › H2"        │
   │    · content_hash per chunk ⇒ incremental re-embed│
   └────┬──────────────────────────────────────────────┘
   ┌────▼──────────────────────────────────────────────┐
   │ 3. EMBED — batched, local by default              │
   │    skip chunks whose content_hash is unchanged    │
   └────┬──────────────────────────────────────────────┘
   ┌────▼──────────────────────────────────────────────┐
   │ 4. STORE — chunk + chunk_vec + embedding_meta      │
   │    same transaction as the content (no drift)      │
   └────┬──────────────────────────────────────────────┘
        │
   QUERY
   ┌────▼──────────────────────────────────────────────┐
   │ 5. RETRIEVE — hybrid                               │
   │    FTS5 BM25 top-50  ∪  vector top-50              │
   │    → Reciprocal Rank Fusion (k=60)                 │
   │    → optional local cross-encoder rerank → top-8   │
   │    → permission filter (ALWAYS, before assembly)   │
   └────┬──────────────────────────────────────────────┘
   ┌────▼──────────────────────────────────────────────┐
   │ 6. ASSEMBLE — budgeted context                     │
   │    dedupe near-identical chunks; order by score;   │
   │    attach [n] markers bound to chunk ids;          │
   │    fill to 60 % of context, reserve 40 % for output│
   └────┬──────────────────────────────────────────────┘
   ┌────▼──────────────────────────────────────────────┐
   │ 7. GENERATE — with a citation-required instruction │
   └────┬──────────────────────────────────────────────┘
   ┌────▼──────────────────────────────────────────────┐
   │ 8. VERIFY CITATIONS  ← the step most systems skip  │
   │    · every [n] must map to a chunk in THIS context │
   │    · the chunk must still exist at the stored rev  │
   │    · unresolvable markers are stripped, and their  │
   │      sentence is moved to unsupported_spans        │
   │    · if zero citations survive → grounded=false    │
   └────┬──────────────────────────────────────────────┘
        ▼
   Answer + verified citations + unsupported spans
```

### 7.1 Why hybrid, not vectors alone

Pure vector search fails on exactly the queries document users ask most: exact identifiers
("invoice 4471-B"), proper nouns, rare terms, and code. BM25 nails those and fails on
paraphrase. RRF fuses without needing score normalisation between two incomparable scales:

```
RRF(d) = Σ_r  1 / (k + rank_r(d)),  k = 60
```

### 7.2 Permission filtering placement

Filter **after** retrieval scoring but **before** context assembly, and never rely on the
index being permission-partitioned. Filtering during assembly is a source of leaks when the
retriever is refactored; filtering at index time breaks when permissions change.

### 7.3 Freshness

A chunk stores `source_rev`. At retrieval, a chunk whose `source_rev` is behind the object's
current `rev` is marked stale; stale chunks are still usable (better than nothing offline) but
are re-indexed opportunistically and the citation is flagged "may be outdated".

### 7.4 What we do not build at v1.0

- Query decomposition / multi-hop agentic retrieval — **NOT IMPLEMENTED**. Meaningful latency
  and cost for a modest accuracy gain at our corpus sizes.
- Knowledge-graph-augmented retrieval — **NOT IMPLEMENTED** (spec §16's graph view is P2).
- Fine-tuned rerankers — **TBD**, pending SP-2 results.

## 8. Model licensing (models are dependencies)

This is routinely overlooked. Weights carry licences that are **not** the runtime's licence.

| Class | Typical terms | Our position |
|---|---|---|
| Apache-2.0 / MIT weights | Fully permissive, commercial redistribution OK | **Preferred. Default choice.** |
| "Community"/"open weights" licences with use restrictions (MAU thresholds, acceptable-use clauses, naming requirements) | Permissive-ish but conditional | Allowed **only** after legal review of the specific version; conditions recorded in `THIRD_PARTY_NOTICES` |
| Non-commercial / research-only (CC-BY-NC, some research checkpoints) | Forbids commercial use | **Blocked.** Never ship. |
| Gated / EULA-on-download weights | Bespoke | Case by case; must permit redistribution inside an app bundle or via our CDN |
| Tesseract `tessdata_fast`/`tessdata_best` | Apache-2.0 | Safe |

**Rules:**
1. Every shipped model has a row in `THIRD_PARTY_NOTICES` with its exact version and licence.
2. Every model is signature-verified before load (T-109). A model file is executable input.
3. Model licence review is part of the dependency-approval checklist, not an afterthought.
4. Cloud provider *outputs* may carry terms (e.g. restrictions on using outputs to train
   competing models). Recorded in the provider register.

## 9. Agent framework (spec §20)

### 9.1 Components

```
Agent          — a named, versioned definition: goal template, allowed tool set,
                 model requirements, step and token budgets
Tool           — a typed, first-party, compiled capability. Registry is closed.
Memory         — short-term (run scratchpad) + long-term (retrieved, read-only)
Planner        — decomposes a goal into steps; may be an LLM or a fixed DAG
Executor       — runs steps, enforces budgets, handles retries and cancellation
Evaluator      — checks each step's output against its declared postcondition
PermissionMgr  — issues and checks per-run, scoped, expiring grants
AuditLogger    — writes every decision and invocation before it happens
```

### 9.2 Tool contract

```c
typedef struct {
    const char* name;                   /* "pdf.merge", "search.query", ...        */
    const char* json_schema_in;
    const char* json_schema_out;
    uint32_t    required_grants;        /* READ|WRITE|DELETE|SHARE|NETWORK|AI_EGRESS */
    bool        requires_human_confirm; /* blocks on a UI callback                  */
    bool        is_destructive;
    bool        is_idempotent;
    uint32_t    est_cost_units;
} uapp_tool_desc_t;
```

Hard rules:
- **The registry is closed and compiled in.** No shell tool, no eval tool, no arbitrary HTTP
  tool. v1.0 tools: `search.query`, `doc.read`, `doc.create`, `doc.append`, `file.read_text`,
  `file.convert`, `pdf.*` (non-destructive), `ocr.run`, `db.query`, `db.insert`,
  `ai.summarize`, `ai.extract`, `notify.user`.
- **Every destructive tool requires human confirmation.** There is no auto-approve in v1.0.
- **Grants are per-run, scoped to named resources, and expire with the run.** An agent granted
  `READ` on `document:X` cannot read `document:Y`.
- Tool arguments are schema-validated before execution. A model-proposed call that fails
  validation is rejected and fed back as an error, not coerced.

### 9.3 Execution loop

```
plan = Planner(goal, available_tools, context)
for step in plan:                       # hard cap: 20 steps
    if budget_exceeded(tokens|time|cost): abort(AGENT_BUDGET_EXCEEDED)
    grant = PermissionMgr.check(step.tool, step.args)
    if !grant: abort(AGENT_PERMISSION_DENIED)
    if tool.requires_human_confirm: await user_confirm(step)   # blocking, no timeout-accept
    audit.write(step)                   # BEFORE execution
    result = Executor.run(step)
    ok = Evaluator.check(step.postcondition, result)
    if !ok: replan_once_or_abort()
    memory.append(step, result)
audit.write(completion)
```

- Every run is **cancellable** and leaves a complete audit trail.
- Every run produces a user-visible **transcript**: what it did, what it touched, what it sent
  where. An agent whose actions are not inspectable is not shippable.
- Runs are resumable across app restarts (state persisted), because mobile apps get killed.

### 9.4 Built-in agent: document ingest (spec §20 example)

```
INPUT FILE → detect format → extract content → OCR if needed → classify
  → generate metadata/tags → summarize → chunk+embed → index → notify
```
Note this runs **entirely locally** under a LOCAL_ONLY policy except the classify/summarize
steps, which are skipped (not cloud-substituted) when no local model is available. The file is
still indexed and searchable. **Degradation is partial and explicit, never silent.**

### 9.5 Prompt-injection posture

We reduce blast radius; we do not claim to solve it (T-402, R-05):

1. Retrieved content lives in the `document` role, never the instruction channel.
2. Tool authorisation derives from the **user's** grant, not the model's request.
3. Destructive actions require human confirmation, so the worst autonomous outcome is
   read-and-report, not delete-and-exfiltrate.
4. There is no network tool and no code-execution tool, so exfiltration has no direct channel.
5. A lightweight output check flags text that looks like injected instructions and surfaces it
   to the user.
6. The transcript makes an unusual action visible after the fact.

## 10. Caching and cost control

- **Embedding cache:** keyed by `(content_hash, model_id)`. Re-indexing an unchanged document
  costs nothing.
- **Response cache:** keyed by `(model_id, prompt_hash, params)`, opt-in per task, short TTL.
  Never applied to `chat`.
- **Prompt caching** where a provider supports it: keep the system prompt and document block
  prefix-stable so caching actually hits.
- **Token budgeting** before dispatch: estimate, compare with quota, and tell the user the cost
  *before* an expensive operation rather than after.
- **Batching:** embeddings are always batched (64–256 texts) — the single biggest throughput
  lever in the indexing pipeline.

## 11. Evaluation

Shipping AI features without an eval harness means every prompt change is a blind change.

| Suite | What it measures | Gate |
|---|---|---|
| Retrieval | recall@10 and nDCG@10 on a hand-labelled set of 200 query/document pairs over our fixture corpus | recall@10 ≥ 0.85; a regression blocks release |
| Citation integrity | % of generated citations that resolve to a real chunk containing the claim | ≥ 0.95 after verification (§7.8); **any unverified citation reaching the UI is a P0 bug** |
| Grounding/refusal | % of unanswerable questions correctly refused | ≥ 0.9 |
| Extraction | field-level F1 on 50 labelled receipts/invoices/business cards | tracked; no hard gate at v1.0 |
| Summarisation | pairwise human preference vs. the previous prompt version, n ≥ 30 | no regression |
| Local vs. cloud delta | the same suites on the local model | **published internally**, so product claims stay honest |
| Injection resistance | a red-team corpus of documents containing embedded instructions | 0 unauthorised tool invocations |

Evals run nightly and on any prompt/model/retrieval change.

## 12. Open AI questions requiring sign-off

| # | Question |
|---|---|
| AIQ-1 | Which cloud providers ship at v1.0? (= PQ-3) Each needs a DPA with zero-retention terms. |
| AIQ-2 | Do we ship a generative local model at v1.0, or local-embeddings-only? Depends on SP-4. |
| AIQ-3 | Who pays for cloud AI — bundled credits, BYO API key, or both? BYO key changes the key-storage and abuse model substantially. |
| AIQ-4 | ~~Is server-side RAG in v1.0?~~ **Answered by `adr/0002` decision 1: no. RAG is client-side/local-first. See §13.** The residual question is narrower: does a web client exist at v1.0 at all, given it would have to hold keys and index in-browser? |
| AIQ-5 | Which specific embedding model? It is a one-way door — changing it forces a full re-embed of every user's corpus. **Under §13.4 this is now also a per-device-tier question**: the floor device may not run the same model as Tier A. |
| AIQ-6 | ~~Tokenizer dependency: accept a Rust dep (`tokenizers`)?~~ **Foreclosed by `adr/0002` decision 2** — Rust is out of the mobile build. Remaining choice: a C++ tokenizer, or approximate budgeting with a safety margin. |
| AIQ-7 | **New.** What is the exact consent-record retention and revocation semantic (§13.2)? Revoking consent cannot un-send data already sent; the UI must not imply that it can. |
| AIQ-8 | **New.** Is cloud-offload-with-consent offered for OCR on the low-RAM tier (§13.5), or does OCR stay strictly on-device there even when it is slow? This is a product call about which promise dominates. |

---

## 13. AI and RAG under end-to-end encryption (`adr/0002` decision 1)

**The server never sees plaintext. No AI feature may be designed as if it does.**

This section supersedes any implication elsewhere in this document that indexing, embedding
or retrieval could be done server-side.

### 13.1 The one-line rule

> **Plaintext leaves the device only when the user has, for this specific scope, in this
> session, explicitly said so — and it is encrypted before it crosses any other boundary.**

Three separate boundaries follow, and conflating them is the mistake this section exists to
prevent:

| Boundary | What crosses | Encryption |
|---|---|---|
| Device → our server (sync, blobs) | ciphertext only | **Always.** Document key, client-side. The server has no key. |
| Device → our AI gateway | plaintext, only under consent | TLS only — the gateway must read it to call the provider. **This is the E2EE exception, and it is user-initiated, per-scope, and audited.** |
| Our gateway → third-party provider | plaintext | TLS + a zero-retention DPA (T-403) |

**There is no path where our server decrypts stored content.** It cannot: it has no key. A
cloud AI call is not "the server decrypting your document"; it is "the client decrypting a
document it already holds and sending that plaintext, once, with your permission, through a
gateway that does not store it."

### 13.2 The per-document consent flow

Required before any plaintext crosses boundary 2. This extends §4's policy gate rather than
replacing it — the gate's steps 1–7 still all run.

```
 1. User invokes a cloud-requiring AI action on scope S
       (S = a document, a selection, or an explicitly named set)
 2. Client resolves S → the exact byte ranges that would be sent
       Not "the document". The ranges. The user is shown what leaves.
 3. Policy gate evaluates workspace.ai_policy (§4)
       LOCAL_ONLY → AI_POLICY_BLOCKED. There is no consent prompt.
                    A LOCAL_ONLY workspace cannot be talked into cloud
                    egress by a dialog. Fails closed, always.
 4. Local content classification (§4 step 3) over the resolved ranges
       credentials / ID numbers / payment data detected
         → the prompt names what was found, specifically
 5. CONSENT PROMPT — states, without euphemism:
       · which provider (named, not "our AI partner")
       · what leaves (a preview of the resolved ranges, expandable)
       · approximate size and token count
       · that it leaves ENCRYPTED STORAGE to do so
       · the provider's retention terms (data_policy_url)
       · scope of the grant and when it expires
 6. On approval: write consent_record BEFORE decrypting
       { id, at, actor, scope_ref, byte_range_hash, provider,
         model, expires_at, policy_snapshot }
       Hash-chained into audit_log (security-model.md §10).
 7. Decrypt the resolved ranges into a scratch buffer that is:
       · arena-allocated, never written to disk
       · zeroed on completion AND on the crash-recovery sweep (T-206)
 8. Emit the ai.egress audit record (§4 step 6), mint the policy token, dispatch
 9. Response returns; scratch buffer zeroed; consent grant remains
    valid for its declared scope and TTL, then expires
```

**Grant granularity.** Default is **per document, per provider, for this session**. A
workspace-wide standing grant is available but is a deliberate, separately-confirmed
setting, not a "remember my choice" checkbox on the prompt — because the whole value of the
prompt is that the user sees each escalation.

**Revocation is honest.** Revoking a grant stops *future* egress. It cannot recall data
already sent. The UI must say that. A revoke button that implies deletion at the provider is
a lie the product cannot back up (T-403: the guarantee is contractual, not technical).

**Agents inherit this unchanged.** An agent run (§9) requiring cloud AI over a document the
user has not consented for **blocks on the same prompt**, mid-run. There is no batching of
consent across an agent's steps, and no "approve all" — that would reintroduce exactly the
silent-egress failure T-401 exists to prevent.

### 13.3 RAG becomes client-side and local-first. It was already most of the way there.

The good news: `adr/0002` decision 1 changes **less of §7 than expected**, because the
pipeline was already designed local-first (H-23: embeddings are device-local and never
synced). What changes is that the local path is now the *only* path, and the fallbacks that
quietly assumed a server are removed.

| §7 stage | Under E2EE |
|---|---|
| 1. EXTRACT | **On-device, unchanged.** Parsers already run locally. |
| 2. CHUNK | **On-device, unchanged.** |
| 3. EMBED | **On-device only.** Batch cloud embedding of a corpus is dead: it would require bulk-decrypting an entire workspace and shipping it out, which no per-document consent flow can honestly cover. *Cloud embedding of a single consented query string remains possible.* |
| 4. STORE | **On-device only.** `chunk`, `chunk_vec` and `embedding_meta` live in the SQLCipher database and are **never synced** (H-23 — now load-bearing rather than merely tidy). |
| 5. RETRIEVE | **On-device only.** FTS5 + `sqlite-vec` + RRF, all local. |
| 6. ASSEMBLE | **On-device.** |
| 7. GENERATE | Local model, **or** cloud under §13.2 consent — this is the only stage where plaintext may leave. |
| 8. VERIFY CITATIONS | **On-device**, against the local chunk store. Unchanged, and now unambiguously so: verification cannot be delegated to a server that cannot read the chunks. |

**Consequences that must be planned for, not discovered:**

- **A new device starts with an empty index.** Chunks and vectors do not sync, so a
  freshly-signed-in device must re-extract and re-embed its whole corpus locally. For a large
  workspace this is hours of background work. It needs to be a **visible, resumable,
  checkpointed, thermally-aware job** with honest progress — not a spinner. Search results are
  incomplete until it finishes, and the UI must say so rather than silently under-returning.
- **Re-indexing cost is paid per device, N times**, not once on a server. On the low-RAM
  tier (§13.5) this is the single heaviest background workload the app has.
- **T-406 (embedding inversion) improves.** There are no server-side vectors to invert. The
  residual is device-local only, and the mitigation is the database encryption we already have.
- **Multi-device search consistency is now eventual in a second, subtler way**: two devices
  can disagree about search results purely because one has finished indexing and the other
  has not, with no sync divergence at all. Diagnostics must be able to explain that.

### 13.4 Model choice becomes a device-tier decision

Because embedding now happens on every device rather than once on a server, the embedding
model must run on the **floor** device, not the median one. See
`performance-strategy.md` §1.1 and §2.7 for the budgets.

The one-way-door problem (AIQ-5) gets worse: if Tier A and the low-RAM tier ran *different*
embedding models, their vectors would be in different spaces and a user's two devices would
return different results for the same query. **Therefore: one embedding model across all
tiers, chosen to fit the floor device.** Tier A does not get a better one. This is a real
quality ceiling and should be recorded as such rather than left as an implementation detail.

### 13.5 Cloud-offload with consent as the degradation path

`adr/0002` decision 3 puts low-end devices in scope. Where an on-device AI or OCR path is
infeasible there, the honest options are exactly three, and "silently do it on the server"
is not among them:

1. **Do it locally but slowly**, with visible progress and cancellation (preferred default).
2. **Disable the feature and say why**, naming the constraint
   (`performance-strategy.md` §1: features are disabled with an explanation, never silently slow).
3. **Offer cloud-offload through the §13.2 consent flow** — same prompt, same audit record,
   same LOCAL_ONLY fail-closed behaviour. No separate, weaker consent path exists for
   "it's only OCR". OCR text is A4-class content (`security-model.md` §1) and is treated as
   equal in sensitivity to the source document.

**H-29 is unaffected and non-negotiable: LOCAL_ONLY still fails closed.** A device being
slow is not a reason to weaken a policy the user set. `AI_NO_LOCAL_MODEL` remains the
answer, with an offer to download a smaller model where one fits.
