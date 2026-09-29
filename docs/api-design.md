# API Design

**Status:** Phase 0 draft — awaiting human sign-off
**Version:** 0.0.0
**Date:** 2026-09-29

Implements spec §31. Two distinct API surfaces are described, because conflating them is a
common and expensive mistake:

- **§2–§9: the cloud REST API** (client ↔ server), versioned, public-facing.
- **§10: the core C ABI** (host ↔ C/C++ core), versioned separately.

The endpoint list in spec §31 is a sketch. It is refined below; deviations are justified.

---

## 1. Design rules

1. **Resource-oriented where it is a resource; RPC-style where it is an action.** `/documents`
   is a collection; `/pdf/merge` is a verb. Forcing PDF merge into REST semantics produces a
   worse API. We say which is which and are consistent within each family.
2. **Every mutating endpoint is idempotent** via a client-supplied `Idempotency-Key`. A mobile
   client on a flaky network *will* retry. Without this, retries duplicate documents.
3. **Long operations are jobs, not long requests.** Anything that can exceed ~3 s returns
   `202 Accepted` + a job resource. Conversion, OCR, large PDF ops, bulk AI.
4. **Client-generated IDs.** `POST` accepts an `id` the client already created offline. The
   server does not mint IDs for syncable objects.
5. **One error envelope**, mapping 1:1 to core error codes.
6. **Cursor pagination only.** Offset pagination over a mutating collection silently skips and
   duplicates rows.
7. **The API never becomes a required path for offline features.** If an endpoint's absence
   breaks note-taking, the design is wrong.

## 2. Versioning strategy

**URI-path major version, header-based minor negotiation.**

```
https://api.<domain>/v1/documents
X-API-Minor: 7          (request: highest minor the client understands)
X-API-Minor: 7          (response: minor actually served)
```

Rationale for path-major over header-only or media-type versioning: it is trivially
cacheable, greppable in logs, testable with curl, and mobile clients live in the wild for
years — a version you cannot see in a URL is a version you cannot debug from a CDN log.

**Contract:**

| Change | Allowed in a minor? |
|---|---|
| Add an endpoint | Yes |
| Add an optional request field | Yes |
| Add a response field | Yes — **clients must ignore unknown fields** (stated in the client SDK contract) |
| Add an enum value | Yes — **clients must handle unknown enum values** by degrading, not crashing |
| Make an optional field required | No |
| Remove or rename a field | No |
| Change a field's type or semantics | No |
| Change an error code's meaning | No |
| Tighten validation | No (it breaks previously-valid clients) |

**Deprecation policy.** A major version is supported for **24 months** after its successor
ships. This is not generous; it is the minimum for mobile, where a meaningful tail of users
never updates. Deprecated endpoints return:

```
Deprecation: true
Sunset: Sat, 31 Oct 2026 23:59:59 GMT
Link: <https://docs.<domain>/v2/migration>; rel="deprecation"
```

**Minimum-version enforcement.** The server may return `426 Upgrade Required` with
`{"code":"CLIENT_VERSION_UNSUPPORTED","min_version":"1.4.0"}`. Reserved for security fixes
and protocol-breaking sync changes. The client must render this as an actionable message, not
a generic error.

**Sync protocol versions independently** of the REST version (`sync-architecture.md` §9),
because sync changes for different reasons and at a different rate.

## 3. Conventions

- **Transport:** HTTPS only, TLS 1.3, HSTS, certificate pinning in mobile clients (with a
  backup pin and a documented rotation procedure — see `security-model.md` §7).
- **Auth:** `Authorization: Bearer <access_token>` (JWT, 15 min). Refresh tokens are opaque,
  rotating, and device-bound. See §8.
- **Content type:** `application/json; charset=utf-8`. Binary upload/download via presigned
  object-storage URLs, never through the API tier.
- **Casing:** `snake_case` in JSON.
- **Timestamps:** RFC 3339 UTC with milliseconds (`2026-09-29T11:12:00.000Z`).
- **IDs:** UUIDv7, canonical hyphenated lowercase.
- **Required headers on every request:** `X-Request-Id` (client-generated, echoed, used for
  support), `X-Device-Id`, `User-Agent` with app version and platform.
- **Compression:** `Content-Encoding: zstd` accepted and preferred; gzip fallback.

### 3.1 Error envelope

```json
{
  "error": {
    "code": "PDF_ENCRYPTED_PASSWORD_REQUIRED",
    "domain": "pdf",
    "message": "The document is encrypted and requires a user password.",
    "request_id": "01J9Z...",
    "retryable": false,
    "details": { "revision": 6 },
    "causes": [ { "code": "PDF_OPEN_FAILED", "domain": "pdf" } ]
  }
}
```

- `code` is stable and enumerable. Clients switch on `code`, never on `message`.
- `message` is English, developer-facing, and **never contains document content**. The client
  localises from `code`.
- `retryable` tells the client whether backoff-and-retry is meaningful. This removes the
  guesswork that otherwise produces either retry storms or lost work.

| HTTP | Meaning |
|---|---|
| 400 | Malformed request |
| 401 | Missing/invalid/expired access token |
| 403 | Authenticated but not permitted |
| 404 | Not found **or** not visible to this subject (deliberately indistinguishable) |
| 409 | Conflict — sync version mismatch, idempotency-key reuse with a different body |
| 413 | Payload too large |
| 415 | Unsupported media type |
| 422 | Well-formed but semantically rejected (e.g. unsupported conversion pair) |
| 423 | Resource locked (a job is mutating it) |
| 429 | Rate limited — `Retry-After` and `X-RateLimit-*` always present |
| 451 | Refused for legal reasons |
| 5xx | Server fault; always retryable with backoff |

### 3.2 Pagination

```
GET /v1/documents?workspace_id=...&limit=50&cursor=eyJzZXEiOjQ0Mn0
→ { "items": [...], "next_cursor": "eyJzZXEiOjQ5Mn0", "has_more": true }
```
Cursors are opaque, signed, and encode a stable sort key (`seq` or `(sort_key, id)`).
`limit` default 50, max 200.

### 3.3 Idempotency

```
POST /v1/documents
Idempotency-Key: 8f1c...   (client-generated UUID, unique per logical operation)
```
Server stores `(key, endpoint, request_hash) → response` for 24 h. Replay with the same hash
returns the stored response and `Idempotency-Replayed: true`. Replay with a *different* body
returns `409 IDEMPOTENCY_KEY_REUSED`. **Required on every POST/PATCH/DELETE.**

### 3.4 Job protocol

```
POST /v1/files/convert            → 202 { "job": { "id": "...", "state": "queued" } }
GET  /v1/jobs/{id}                → { "id","state","progress",
                                      "result"|"error","created_at","updated_at" }
DELETE /v1/jobs/{id}              → cancel
```
`state ∈ queued | running | succeeded | failed | cancelled | expired`.
Clients poll with backoff, or subscribe over the sync WebSocket (§7) for push updates.
Results expire after 24 h.

## 4. Endpoint surface

Deviations from spec §31 are marked **[Δ]** with a reason.

### 4.1 Identity & devices

```
POST   /v1/auth/register
POST   /v1/auth/login
POST   /v1/auth/refresh
POST   /v1/auth/logout
POST   /v1/auth/mfa/enroll
POST   /v1/auth/mfa/verify
GET    /v1/me
PATCH  /v1/me
DELETE /v1/me                          -- account deletion; async, with a grace period
GET    /v1/me/export                   -- GDPR data export; returns a job
GET    /v1/devices
POST   /v1/devices                     -- register this device (pubkey, name, platform)
DELETE /v1/devices/{id}                -- revoke; invalidates that device's refresh token
```

### 4.2 Workspaces, documents, blocks

```
GET    /v1/workspaces
POST   /v1/workspaces
GET    /v1/workspaces/{id}
PATCH  /v1/workspaces/{id}
DELETE /v1/workspaces/{id}
GET    /v1/workspaces/{id}/members
POST   /v1/workspaces/{id}/members
DELETE /v1/workspaces/{id}/members/{account_id}

POST   /v1/documents                   -- body carries the client-generated id
GET    /v1/documents/{id}
PATCH  /v1/documents/{id}              -- If-Match: <server_rev> required
DELETE /v1/documents/{id}              -- soft delete
POST   /v1/documents/{id}/restore
GET    /v1/documents/{id}/blocks       -- paginated, tree order
PATCH  /v1/documents/{id}/blocks       -- [Δ] batch block mutation, see below
GET    /v1/documents/{id}/versions
POST   /v1/documents/{id}/versions/{vid}/restore
```

**[Δ] Batch block mutation.** Spec §31 implies per-resource CRUD. A block edit touches many
blocks (split a paragraph = update one, insert one, reparent two). Per-block requests would
mean N round-trips and no atomicity. `PATCH /documents/{id}/blocks` takes an ordered op list
applied in one transaction:

```json
{ "base_rev": 41,
  "ops": [
    {"op":"upsert","id":"018f...","type":"paragraph","sort_key":"a0","content":{...}},
    {"op":"delete","id":"018e..."},
    {"op":"move","id":"018d...","parent_id":"018c...","sort_key":"a0V"}
  ] }
```
Returns the new `rev`, or `409` with the server state for the client to merge.

> **Note:** for a synced client this endpoint is *not* the normal path — block changes flow
> through `/v1/sync` (§7). This endpoint exists for the web client, integrations, and
> server-side agents.

### 4.3 Databases

```
GET    /v1/databases/{document_id}/schema
PATCH  /v1/databases/{document_id}/schema
GET    /v1/databases/{document_id}/rows?view_id=...&cursor=...
POST   /v1/databases/{document_id}/rows
PATCH  /v1/databases/{document_id}/rows/{row_id}
DELETE /v1/databases/{document_id}/rows/{row_id}
POST   /v1/databases/{document_id}/query     -- [Δ] complex filter AST in a body
GET    /v1/databases/{document_id}/views
POST   /v1/databases/{document_id}/views
```
**[Δ]** Filter/sort/group specs exceed practical URL length and contain user data that should
not sit in query strings (and therefore in access logs). `POST .../query` is a read.

### 4.4 Files

```
POST   /v1/files                       -- [Δ] create metadata + get an upload target
GET    /v1/files/{id}
PATCH  /v1/files/{id}
DELETE /v1/files/{id}
GET    /v1/files/{id}/content          -- 302 to a short-lived presigned URL
GET    /v1/files/{id}/preview?page=1&w=1024
GET    /v1/files/{id}/text?page=1
GET    /v1/files/{id}/metadata
POST   /v1/files/{id}/detect           -- re-run type detection
POST   /v1/files/convert               -- job
POST   /v1/files/{id}/ocr              -- job
```

**[Δ] `POST /files/upload` replaced by a three-step flow.** A single multipart upload through
the API tier caps file size at the gateway limit, cannot resume, and burns API-tier bandwidth.
Instead:

```
1. POST /v1/files            { name, size_bytes, content_hash, workspace_id, id }
   → 201 { file: {...},
           upload: { method:"multipart", parts:[{n:1,url:"https://...",range:[0,8388607]}, ...],
                     complete_url:"/v1/files/{id}/upload/complete", expires_at:"..." } }
   → if content_hash already exists in this workspace: { "upload": null, "deduplicated": true }
2. PUT each part directly to object storage (resumable; re-request an expired part URL)
3. POST /v1/files/{id}/upload/complete { parts:[{n,etag}] }
   → server verifies size + hash, then marks the blob available
```
The server **verifies the content hash** before marking the blob available. A client-asserted
hash is never trusted for dedupe across workspaces.

### 4.5 PDF

All are jobs. Inputs are `file_id`s already in the system, not inline bytes.

```
POST /v1/pdf/merge        { file_ids:[...], output_name }
POST /v1/pdf/split        { file_id, mode:"pages"|"ranges"|"every_n"|"bookmarks", spec }
POST /v1/pdf/extract      { file_id, pages:"1-3,7" }
POST /v1/pdf/rotate       { file_id, rotations:[{page,degrees}] }
POST /v1/pdf/reorder      { file_id, order:[3,1,2] }
POST /v1/pdf/delete-pages { file_id, pages:"4,9-11" }
POST /v1/pdf/crop         { file_id, boxes:[{page,x0,y0,x1,y1}] }
POST /v1/pdf/compress     { file_id, target:"screen"|"ebook"|"print"|{max_bytes} }
POST /v1/pdf/watermark    { file_id, text|image_file_id, opacity, position, pages }
POST /v1/pdf/protect      { file_id, user_password?, owner_password?, permissions[],
                            algorithm:"aes256" }
POST /v1/pdf/unlock       { file_id, password }
POST /v1/pdf/redact       { file_id, regions:[{page,x0,y0,x1,y1}], also_scrub_metadata:true }
POST /v1/pdf/flatten      { file_id }          -- bake annotations + form fields into content
POST /v1/pdf/to-images    { file_id, format:"png"|"jpeg", dpi }
POST /v1/pdf/from-images  { file_ids:[...], page_size, orientation }
GET  /v1/pdf/{file_id}/annotations
PUT  /v1/pdf/{file_id}/annotations
GET  /v1/pdf/{file_id}/form-fields
PUT  /v1/pdf/{file_id}/form-fields
```

**Redaction is server-side-optional and client-preferred.** Uploading a document in order to
redact it is the opposite of what the feature is for. The client performs redaction locally by
default; the endpoint exists for the web client, which has no local engine.

### 4.6 Scanner & OCR

```
POST /v1/scanner/process   { image_file_ids:[...],
                             options:{ auto_crop, enhance:"auto"|"color"|"gray"|"bw",
                                       deskew, shadow_removal },
                             output:"pdf"|"images" }                      -- job
POST /v1/ocr/process       { file_id, languages:["eng","hin"],
                             output:["text","hocr","searchable_pdf"],
                             detect_tables:false }                        -- job
GET  /v1/ocr/languages     -- available language packs, sizes, and which are on this device
```

### 4.7 Search

```
POST /v1/search
{ "q": "amortised analysis",
  "workspace_ids": ["..."],
  "modes": ["fulltext","semantic"],     // exact|fulltext|fuzzy|semantic|metadata
  "filters": { "kind":["document","file"], "mime":["application/pdf"],
               "updated_after":"2026-01-01T00:00:00Z", "tags":["lecture"] },
  "limit": 25, "cursor": null }
→ { "items":[ { "object_kind","object_id","title","snippet",
                "highlights":[{field,start,end}],
                "score", "score_breakdown":{"bm25":..,"vector":..,"rrf":..},
                "location":{"page":4,"block_id":"..."} } ],
    "next_cursor": null, "degraded": false }
```
`degraded: true` means one retrieval mode was unavailable (e.g. vector index rebuilding) and
results are partial. Silent degradation is a correctness bug.

### 4.8 AI

```
POST /v1/ai/chat           -- SSE stream; conversational, optional RAG grounding
POST /v1/ai/summarize      { object_kind, object_id | text, style, max_tokens }
POST /v1/ai/extract        { object_kind, object_id, schema:{...JSON Schema...} }
POST /v1/ai/translate      { text | object_id, target_lang }
POST /v1/ai/classify       { object_id, labels:[...] | taxonomy_id }
POST /v1/ai/generate       { task:"flashcards"|"quiz"|"notes"|"study_plan", object_ids:[...] }
POST /v1/ai/ask            -- RAG Q&A with citations
GET  /v1/ai/providers      -- available providers/models, capabilities, cost tier, data policy
GET  /v1/ai/usage          -- quota and consumption
```

**Every AI request must carry an explicit egress acknowledgement:**

```json
{ "consent": { "granted": true,
               "scope": "document:018f...",
               "provider": "provider-a",
               "granted_at": "2026-09-29T11:00:00Z" } }
```
Absent or mismatched consent ⇒ `403 AI_CONSENT_REQUIRED`. The server does not infer consent
from the fact that a request arrived.

`/v1/ai/ask` response shape (citation contract):

```json
{ "answer": "Amortised analysis bounds the average cost per operation …",
  "citations": [
    { "marker": 1, "chunk_id":"018f...", "object_kind":"file", "object_id":"018e...",
      "page": 4, "char_start": 1120, "char_end": 1384, "quote": "…", "score": 0.81 }
  ],
  "unsupported_spans": [ {"start": 210, "end": 260} ],
  "model": "provider-a:model-x:2026-06",
  "grounded": true }
```
`unsupported_spans` marks answer text not traceable to a retrieved chunk. The client renders
it visually distinct. `grounded: false` means retrieval found nothing usable — the client must
not present the answer as document-derived. **Citations are validated server-side against the
chunk store before the response is emitted; a citation that does not resolve is removed, and
its dependent span is moved to `unsupported_spans`.**

### 4.9 Sync

```
POST /v1/sync/pull   { workspace_id, cursor, limit }
POST /v1/sync/push   { workspace_id, device_id, mutations:[...] }
GET  /v1/sync/state  { workspace_id }
WS   /v1/sync/stream
```
Protocol detail in `sync-architecture.md`. **[Δ]** Spec §31 lists a single `POST /sync`; a
single endpoint doing both directions makes the cursor semantics ambiguous and prevents
pulling without pushing (which is what a read-only or recovering client needs).

### 4.10 Sharing, jobs, system

```
POST   /v1/shares          { resource_kind, resource_id, role, expires_at?, password? }
GET    /v1/shares/{id}
DELETE /v1/shares/{id}
GET    /v1/public/{token}  -- unauthenticated read of a shared resource, heavily rate-limited

GET    /v1/jobs/{id}
DELETE /v1/jobs/{id}
GET    /v1/jobs?state=running

GET    /v1/health          -- liveness, unauthenticated
GET    /v1/capabilities    -- server-declared feature + format support for THIS deployment
```

`/v1/capabilities` exists so the client never hard-codes what the server can do. It returns
the same format-capability matrix shape as the local engine, letting the UI merge local and
remote capability into one answer to "can I convert this?".

## 5. Rate limiting

| Class | Limit | Note |
|---|---|---|
| Auth | 10/min/IP, 5/min/account | Plus exponential lockout on failures |
| Read | 600/min/account | |
| Write | 120/min/account | |
| Sync push | 60/min/device | Batched, so this is generous |
| Jobs (conversion/OCR) | 20/hour/account (free tier) | Tied to monetisation (PQ-1) |
| AI | token-budget-based, not request-based | Requests are a poor proxy for cost |

Headers: `X-RateLimit-Limit`, `X-RateLimit-Remaining`, `X-RateLimit-Reset`, `Retry-After`.

## 6. Webhooks (post-1.0)

Outbound `POST` with `X-Signature: v1=<hex HMAC-SHA256(secret, timestamp + "." + body)>` and
`X-Timestamp`. Receivers must reject timestamps older than 5 minutes (replay defence).
At-least-once delivery with exponential backoff for 24 h; consumers must be idempotent on
`event_id`.

## 7. Realtime channel

`WSS /v1/sync/stream`, authenticated by the access token in the connect frame (not the URL —
URLs end up in logs). Server→client messages: `change_available` (a hint to pull, not the data
itself), `job_update`, `device_revoked`, `quota_warning`. Client→server: `ping`, `subscribe`,
`unsubscribe`.

The channel is a **hint channel**. Data always moves over the REST pull. This keeps the
WebSocket stateless and means a dropped socket degrades to polling, never to data loss.

## 8. Authentication detail

| Token | Lifetime | Storage | Notes |
|---|---|---|---|
| Access (JWT, EdDSA) | 15 min | memory only | Claims: `sub`, `dev`, `ws[]`, `scope[]`, `exp`, `jti` |
| Refresh (opaque, 256-bit) | 60 days, rotating | Keychain / Keystore | Device-bound; single-use. Reuse of a consumed refresh token revokes the whole family and alerts the user. |

- Refresh-token rotation with reuse detection is the mechanism that makes a stolen refresh
  token detectable. It is not optional.
- JWTs are verified with a rotating key set at `/v1/.well-known/jwks.json`; `alg` is pinned
  server-side to EdDSA (never trust the token header's `alg`).
- Public share links use a separate, short, high-entropy token with its own scope and no
  account privileges.

## 9. OpenAPI and client generation

- `api/openapi.v1.yaml` is the **source of truth**, checked in and reviewed.
- Handlers are validated against it in CI (request and response), so drift is a build failure.
- Error codes are generated into all clients from one shared enum definition, so the core C
  enum, the Go server, the Kotlin client and the Swift client cannot disagree.
- A contract-test suite replays recorded fixtures against each release to catch accidental
  breaking changes across minors.

## 10. Core C ABI (separate surface, separate versioning)

The on-device API. Not REST; no network. Versioned as `UAPP_ABI_VERSION_MAJOR/MINOR`.

```c
/* ---- lifecycle ---- */
uapp_status_t uapp_init(const uapp_config_t*, const uapp_platform_vtable_t*, uapp_ctx_t**);
void          uapp_shutdown(uapp_ctx_t*);
uint32_t      uapp_abi_version_major(void);
uint32_t      uapp_abi_version_minor(void);

/* ---- errors: caller never frees a returned error; it is owned by the ctx TLS slot ---- */
const uapp_error_t* uapp_last_error(uapp_ctx_t*);

/* ---- buffers: all byte transfer uses an explicit, caller-owned buffer type ---- */
typedef struct { uint8_t* data; size_t len; size_t cap; } uapp_buf_t;
void uapp_buf_free(uapp_buf_t*);

/* ---- jobs: every long operation ---- */
typedef void (*uapp_progress_fn)(void* user, float fraction, const char* stage);
typedef void (*uapp_done_fn)(void* user, uapp_status_t, const uapp_result_t*);
uapp_status_t uapp_job_submit(uapp_ctx_t*, const uapp_job_spec_t*,
                              uapp_progress_fn, uapp_done_fn, void* user, uapp_job_t**);
uapp_status_t uapp_job_cancel(uapp_job_t*);

/* ---- documents ---- */
uapp_status_t uapp_doc_create(uapp_ctx_t*, const uapp_doc_spec_t*, uapp_id_t* out);
uapp_status_t uapp_doc_apply_ops(uapp_ctx_t*, uapp_id_t doc, const uapp_buf_t* fb_ops);
uapp_status_t uapp_doc_read_tree(uapp_ctx_t*, uapp_id_t doc, uapp_buf_t* out_fb);

/* ---- files / pdf / ocr / search / ai: same shape ---- */
uapp_status_t uapp_file_detect(uapp_ctx_t*, const char* path, uapp_file_info_t* out);
uapp_status_t uapp_pdf_open(uapp_ctx_t*, uapp_id_t file, const char* password, uapp_pdf_t**);
uapp_status_t uapp_pdf_render_page(uapp_pdf_t*, int page, const uapp_render_opts_t*,
                                   uapp_bitmap_t* inout);   /* caller-provided buffer */
uapp_status_t uapp_search_query(uapp_ctx_t*, const uapp_buf_t* fb_query, uapp_buf_t* out_fb);
```

**ABI rules:**
1. Only C99 types, fixed-width integers, and opaque pointers cross. No `std::`, no exceptions,
   no C++ ABI.
2. Structs passed across the boundary are versioned by a leading `uint32_t struct_size`.
3. Every allocation crossing the boundary has a matching `uapp_*_free`. No `malloc`/`free`
   across module boundaries.
4. Complex structured payloads (block trees, search results) are FlatBuffers in a `uapp_buf_t`,
   not hand-rolled C structs. This keeps the ABI tiny and lets the payload schema evolve
   independently of the ABI.
5. Symbols are versioned and additive-only within a major. Enforced by an ABI-diff CI job
   against the previous release's exported-symbol list.
6. No function may block indefinitely. Everything long-running is a job.

## 11. Open API questions requiring sign-off

| # | Question |
|---|---|
| AQ-1 | Public/partner API at v1.0, or internal-only? Determines how hard the freeze on `/v1` really is. |
| AQ-2 | Does the web client use the same `/v1` API, or a separate BFF? A BFF would let the mobile API stay narrow. |
| AQ-3 | Is `/v1/public/{token}` (unauthenticated share links) in v1.0? It is a meaningful new attack surface. |
| AQ-4 | If PQ-2 resolves to E2EE, most server-side `/pdf/*`, `/ocr/*`, `/ai/*` and `/search` endpoints become impossible. The API surface must then shrink substantially. |
