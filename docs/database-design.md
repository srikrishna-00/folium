# Database Design — Local (SQLite) + Cloud

**Status:** Phase 0 draft — awaiting human sign-off
**Version:** 0.0.0
**Date:** 2026-09-29

Implements spec §32. The domain separations named there (Identity, Workspace, Documents,
Blocks, Files, File metadata, Database records, Tasks, Comments, Permissions, Versions,
Sync changes, AI metadata, Embeddings) are realised as **schema namespaces**, not as one flat
table soup.

---

## 1. Principles

1. **Local is the source of truth for the user's device.** The server is a durable replica and
   a relay. The client never waits on the server to consider a write done.
2. **One persistence port.** Nothing outside `storage/sqlite` includes `sqlite3.h`. The rest of
   the core sees typed repositories. This is the literal requirement of spec §32 ("do not
   tightly couple business logic to one database engine").
3. **Namespaces, not a monolith.** Each namespace has its own migration file, its own
   repository, and an explicit list of the namespaces it may reference.
4. **Every syncable row carries sync columns.** No exceptions — adding them later is a
   migration nightmare.
5. **IDs are client-generatable.** Offline creation cannot wait for a server-assigned key.
6. **Blobs are not in the database.** The DB stores metadata and a content hash; bytes live in
   the blob store.
7. **Index and content commit together.** FTS and vector rows are written in the same
   transaction as the content they describe, or the index will silently drift.

## 2. Identifiers

- **Object ID:** UUIDv7 rendered as a 16-byte `BLOB`. UUIDv7 is time-ordered, so it gives
  B-tree locality (unlike v4) while remaining client-generatable (unlike a server sequence).
  Stored as BLOB, not TEXT: 16 bytes vs. 36, and it matters at 10⁶ blocks.
- **Device ID:** UUIDv4, generated once at first run, stored in the platform keystore.
- **Blob ID:** `BLAKE2b-256` of the plaintext content, hex. Content-addressed ⇒ free dedupe.
- **Lamport counter:** monotonic `u64` per device, used for causal ordering (see
  `sync-architecture.md`).

```
Rationale for not using integer PKs: rows are created on N devices without coordination.
Any auto-increment scheme requires either server round-trips (breaks offline) or
device-partitioned ranges (fragile). The 8-byte cost per FK is accepted.
```

## 3. Local schema (SQLite)

Pragmas set at open:

```sql
PRAGMA journal_mode      = WAL;        -- concurrent readers + one writer
PRAGMA synchronous       = NORMAL;     -- WAL makes FULL unnecessary for durability-on-commit
PRAGMA foreign_keys      = ON;
PRAGMA busy_timeout      = 5000;
PRAGMA temp_store        = MEMORY;
PRAGMA mmap_size         = 268435456;  -- 256 MB; reduce on 32-bit / low-RAM
PRAGMA cache_size        = -20000;     -- 20 MB page cache
PRAGMA auto_vacuum       = INCREMENTAL;
PRAGMA secure_delete     = ON;         -- spec §23 "secure deletion where practical"
-- SQLCipher:
PRAGMA cipher_page_size  = 4096;
PRAGMA kdf_iter          = 256000;
PRAGMA cipher_memory_security = ON;
```

> `secure_delete=ON` costs write throughput. We accept it because the alternative — plaintext
> remnants of deleted documents in free pages — is exactly the thing an at-rest-encryption
> promise is supposed to prevent. Note it is only meaningful *within* the DB file; see §9.

### 3.0 Sync columns (on every syncable table)

```sql
-- included by every syncable table; shown once, not repeated below
  rev              INTEGER NOT NULL,        -- local revision, monotonic per object
  updated_by_dev   BLOB    NOT NULL,        -- device_id that produced this revision
  updated_lamport  INTEGER NOT NULL,        -- Lamport clock at that device
  updated_at       INTEGER NOT NULL,        -- unix millis, for display ONLY, never ordering
  version_vector   BLOB,                    -- CBOR {device_id -> counter}
  sync_state       INTEGER NOT NULL,        -- 0 local-only 1 dirty 2 in-flight 3 synced 4 conflicted
  deleted_at       INTEGER,                 -- soft delete; NULL = live
  server_rev       TEXT                     -- opaque server ETag, NULL until first push
```

**`updated_at` is never used to order or resolve anything.** Device clocks are wrong. It
exists so the UI can say "yesterday". Ordering uses Lamport + version vectors.

### 3.1 `identity` namespace

```sql
CREATE TABLE identity_account (
  id            BLOB PRIMARY KEY,
  handle        TEXT,                    -- email or opaque id; may be NULL for local-only mode
  display_name  TEXT,
  avatar_blob   TEXT REFERENCES blob_ref(blob_id),
  created_at    INTEGER NOT NULL
);

CREATE TABLE identity_device (
  id             BLOB PRIMARY KEY,
  account_id     BLOB REFERENCES identity_account(id),
  name           TEXT NOT NULL,          -- "Priya's Pixel"
  platform       TEXT NOT NULL,          -- android | ios | macos | windows | linux | web
  pubkey         BLOB NOT NULL,          -- Ed25519 device signing key (public part only)
  registered_at  INTEGER NOT NULL,
  last_seen_at   INTEGER,
  revoked_at     INTEGER
);

-- NO TOKENS HERE. Access/refresh tokens live in Keychain/Keystore, never in the DB.
CREATE TABLE identity_session_meta (
  id             BLOB PRIMARY KEY,
  device_id      BLOB NOT NULL REFERENCES identity_device(id),
  expires_at     INTEGER NOT NULL,
  keyref         TEXT NOT NULL           -- opaque handle into the platform secure store
);
```

### 3.2 `workspace` namespace

```sql
CREATE TABLE workspace (
  id            BLOB PRIMARY KEY,
  name          TEXT NOT NULL,
  icon          TEXT,
  owner_id      BLOB REFERENCES identity_account(id),
  ai_policy     INTEGER NOT NULL DEFAULT 0,  -- 0 local-only, 1 ask-each-time, 2 cloud-allowed
  encryption    INTEGER NOT NULL DEFAULT 0,  -- 0 at-rest only, 1 E2EE (reserved; see PQ-2)
  created_at    INTEGER NOT NULL
  -- + sync columns
);

CREATE TABLE workspace_member (
  workspace_id  BLOB NOT NULL REFERENCES workspace(id) ON DELETE CASCADE,
  account_id    BLOB NOT NULL,
  role          INTEGER NOT NULL,            -- 0 viewer 1 commenter 2 editor 3 admin 4 owner
  PRIMARY KEY (workspace_id, account_id)
  -- + sync columns
);
```

`ai_policy` living on the workspace (not on a global setting) is deliberate: Marcus keeps
client work in a local-only workspace and personal notes in a cloud-allowed one.

### 3.3 `document` + `blocks` namespaces

```sql
CREATE TABLE document (
  id            BLOB PRIMARY KEY,
  workspace_id  BLOB NOT NULL REFERENCES workspace(id),
  parent_id     BLOB REFERENCES document(id),   -- page nesting
  kind          INTEGER NOT NULL,               -- 0 page 1 database 2 file-page
  title         TEXT NOT NULL DEFAULT '',
  icon          TEXT,
  cover_blob    TEXT,
  sort_key      TEXT NOT NULL,                  -- fractional index (see note)
  archived      INTEGER NOT NULL DEFAULT 0
  -- + sync columns
);
CREATE INDEX ix_document_parent ON document(parent_id, sort_key) WHERE deleted_at IS NULL;
CREATE INDEX ix_document_ws     ON document(workspace_id)        WHERE deleted_at IS NULL;

CREATE TABLE block (
  id            BLOB PRIMARY KEY,
  document_id   BLOB NOT NULL REFERENCES document(id) ON DELETE CASCADE,
  parent_id     BLOB REFERENCES block(id),      -- nested blocks
  type          TEXT NOT NULL,                  -- 'paragraph','heading','code','table',...
  sort_key      TEXT NOT NULL,                  -- fractional index between siblings
  schema_ver    INTEGER NOT NULL DEFAULT 1,
  content       BLOB NOT NULL,                  -- CBOR, type-specific payload
  text_cache    TEXT,                           -- denormalised plain text, for FTS + preview
  crdt_state    BLOB                            -- sequence-CRDT state for rich text (Phase 13)
  -- + sync columns
);
CREATE INDEX ix_block_doc ON block(document_id, parent_id, sort_key) WHERE deleted_at IS NULL;
```

**Fractional indexing for `sort_key`.** Ordering siblings by an integer position means an
insert renumbers every following row — catastrophic for sync (it turns a 1-block insert into
an N-block conflict). A fractional index (base-62 string, midpoint-between-neighbours, with a
deterministic tie-break on device id) makes an insert a single-row change. This is the single
most important schema decision for sync sanity.

**Why `content` is CBOR, not columns.** Nineteen block types with disjoint fields would mean
either 19 tables (join-heavy reads of a block tree) or a 60-column sparse table. CBOR keeps
the block table narrow and lets `core/blocks` own schema evolution via `schema_ver`. The cost
is that you cannot query inside `content` in SQL — which is why `text_cache` is denormalised out.

### 3.4 `database_records` namespace (user-defined databases)

> **Deviation from the sketch below, recorded 2026-10-17: see
> `docs/adr/0004-database-anchored-to-page.md`.** This section's `db_schema.document_id
> PRIMARY KEY REFERENCES document(id)` keys a database directly off a document row — shape (A)
> in that ADR, "a Database IS a Page." The implementation instead anchors a database to a host
> page via a foreign key, as its own entity with its own `DatabaseId` (`db_collection.page_id
> REFERENCES page(id)`, not `db_collection.id REFERENCES page(id)` as a shared primary key).
> The reason is concrete, not stylistic: by the time this feature was built, `blocks::Page` was
> a real, narrow type (title + validated block tree) with four real readers — `BlockStore`,
> `DocumentService`, the Markdown converter, and the already-frozen C ABI — none of which could
> safely be taught "some pages are not documents" without a `page.kind` column and a schema
> migration touching a published ABI. The ADR records this as **deferred, not rejected**: once
> child pages and `page.kind` exist, a "full-page database" becomes exactly what this section
> originally specified. Table and column names below (`db_schema`, `document_id`) are therefore
> illustrative of the ORIGINAL sketch; the real tables are `db_collection`, `db_property`,
> `db_row`, `db_value`, `db_view` (see `storage/sqlite/src/sqlite_database_store.cpp`).

```sql
CREATE TABLE db_schema (
  document_id   BLOB PRIMARY KEY REFERENCES document(id) ON DELETE CASCADE
  -- + sync columns
);

CREATE TABLE db_property (
  id            BLOB PRIMARY KEY,
  document_id   BLOB NOT NULL REFERENCES db_schema(document_id) ON DELETE CASCADE,
  name          TEXT NOT NULL,
  type          TEXT NOT NULL,     -- text|number|bool|select|multi_select|date|person|url|
                                   -- email|file|relation|rollup|formula
  config        BLOB,              -- CBOR: select options, relation target, formula source...
  sort_key      TEXT NOT NULL
  -- + sync columns
);

CREATE TABLE db_row (
  id            BLOB PRIMARY KEY,
  document_id   BLOB NOT NULL REFERENCES db_schema(document_id) ON DELETE CASCADE,
  page_id       BLOB REFERENCES document(id),  -- each row may open as a page
  sort_key      TEXT NOT NULL
  -- + sync columns
);

-- EAV, deliberately. See note.
CREATE TABLE db_value (
  row_id        BLOB NOT NULL REFERENCES db_row(id) ON DELETE CASCADE,
  property_id   BLOB NOT NULL REFERENCES db_property(id) ON DELETE CASCADE,
  v_text        TEXT,
  v_num         REAL,
  v_int         INTEGER,      -- also bool (0/1) and unix-millis dates
  v_blob        BLOB,         -- CBOR for multi-valued: multi_select, person[], relation[]
  PRIMARY KEY (row_id, property_id)
) WITHOUT ROWID;
CREATE INDEX ix_dbvalue_prop_text ON db_value(property_id, v_text);
CREATE INDEX ix_dbvalue_prop_num  ON db_value(property_id, v_num);
CREATE INDEX ix_dbvalue_prop_int  ON db_value(property_id, v_int);
```

**EAV is normally a smell. Here it is the right call, and here is why.** The alternative —
a real table per user-defined database with `ALTER TABLE` on every property change — means
DDL executed on user data, on N devices, that must converge under sync. Concurrent
`ALTER TABLE` on two devices is not a solvable merge. EAV makes a schema change an ordinary
row change that the same sync machinery handles.

**The cost is real and bounded:** filtering/sorting an M-property view needs M joins or a
pivot. Mitigation: `core/database` compiles a view's filter/sort AST into a single query with
one `db_value` join per *referenced* property (typically 1–3, not M), and caches the
materialised view ordering in `db_view_cache` keyed by `(view_id, schema_rev)`.
**Budget: 10k rows × 20 properties, filtered+sorted view in ≤ 100 ms.** If SP-benchmarking
fails that, fall back to a per-database *pivot table* maintained by trigger — noted as a known
escape hatch, not the default.

```sql
CREATE TABLE db_view (
  id            BLOB PRIMARY KEY,
  document_id   BLOB NOT NULL REFERENCES db_schema(document_id) ON DELETE CASCADE,
  name          TEXT NOT NULL,
  kind          INTEGER NOT NULL,   -- 0 table 1 board 2 calendar 3 gallery 4 list 5 timeline
  spec          BLOB NOT NULL       -- CBOR: filter AST, sort spec, group-by, visible props
  -- + sync columns
);
```

### 3.5 `files` + `file_metadata` namespaces

```sql
CREATE TABLE file (
  id            BLOB PRIMARY KEY,
  workspace_id  BLOB NOT NULL REFERENCES workspace(id),
  name          TEXT NOT NULL,
  extension     TEXT,
  mime_type     TEXT NOT NULL,          -- DETECTED, not the one the OS claimed
  declared_mime TEXT,                   -- what the OS/upload claimed; kept for forensics
  size_bytes    INTEGER NOT NULL,
  blob_id       TEXT NOT NULL REFERENCES blob_ref(blob_id),
  capabilities  INTEGER NOT NULL,       -- bitmask: VIEW|EDIT|CONVERT|EXPORT|OCR|ANNOTATE|
                                        -- COMPRESS|SIGN|ENCRYPT|EXTRACT
  detect_conf   INTEGER NOT NULL,       -- 0-100; low ⇒ UI shows "type uncertain"
  created_at    INTEGER NOT NULL,
  modified_at   INTEGER NOT NULL
  -- + sync columns
);

CREATE TABLE blob_ref (
  blob_id       TEXT PRIMARY KEY,       -- BLAKE2b-256 hex of PLAINTEXT
  size_bytes    INTEGER NOT NULL,
  local_path    TEXT,                   -- NULL ⇒ not on this device (selective sync)
  remote_key    TEXT,                   -- object-storage key; NULL ⇒ never uploaded
  enc_key_ref   TEXT,                   -- handle into the key store for the file key
  refcount      INTEGER NOT NULL DEFAULT 0,
  pinned        INTEGER NOT NULL DEFAULT 0,  -- user asked to keep offline
  last_access   INTEGER
);
CREATE INDEX ix_blob_evict ON blob_ref(pinned, last_access) WHERE local_path IS NOT NULL;

CREATE TABLE file_metadata (
  file_id       BLOB NOT NULL REFERENCES file(id) ON DELETE CASCADE,
  ns            TEXT NOT NULL,          -- 'exif' | 'xmp' | 'pdfinfo' | 'id3' | 'ooxml'
  key           TEXT NOT NULL,
  value         TEXT,
  PRIMARY KEY (file_id, ns, key)
) WITHOUT ROWID;

-- Extracted, parse-derived content. Rebuildable; excluded from sync.
CREATE TABLE file_text (
  file_id       BLOB NOT NULL REFERENCES file(id) ON DELETE CASCADE,
  page          INTEGER NOT NULL,
  text          TEXT NOT NULL,
  source        INTEGER NOT NULL,       -- 0 embedded 1 ocr
  PRIMARY KEY (file_id, page)
);

CREATE TABLE ocr_block (
  id            BLOB PRIMARY KEY,
  file_id       BLOB NOT NULL REFERENCES file(id) ON DELETE CASCADE,
  page          INTEGER NOT NULL,
  x0 REAL, y0 REAL, x1 REAL, y1 REAL,   -- normalised 0..1, origin top-left
  text          TEXT NOT NULL,
  confidence    REAL NOT NULL,
  lang          TEXT,
  block_index   INTEGER NOT NULL        -- reading order
);
CREATE INDEX ix_ocr_file_page ON ocr_block(file_id, page, block_index);
```

`blob_ref.refcount` + `pinned` + `last_access` is the whole selective-sync cache policy:
evict unpinned, zero-refcount, least-recently-accessed blobs when over quota.

### 3.6 `versions` namespace

```sql
CREATE TABLE version_snapshot (
  id            BLOB PRIMARY KEY,
  object_kind   INTEGER NOT NULL,      -- 0 document 1 db_row 2 file
  object_id     BLOB NOT NULL,
  rev           INTEGER NOT NULL,
  author_dev    BLOB NOT NULL,
  created_at    INTEGER NOT NULL,
  label         TEXT,                  -- NULL = automatic, non-NULL = user-named
  payload       BLOB NOT NULL,         -- zstd(CBOR full state) or a delta
  is_delta      INTEGER NOT NULL DEFAULT 0,
  base_id       BLOB REFERENCES version_snapshot(id)
);
CREATE INDEX ix_version_obj ON version_snapshot(object_kind, object_id, rev DESC);
```

Retention: keep every snapshot for 24 h, then hourly for 7 d, then daily for 30 d, then only
user-labelled ones. Compaction runs as a background job. Full snapshot every 20 deltas so a
restore never walks an unbounded chain.

### 3.7 `sync` namespace

```sql
CREATE TABLE sync_mutation (          -- the durable outbox
  seq           INTEGER PRIMARY KEY AUTOINCREMENT,   -- local-only; strict FIFO
  object_kind   INTEGER NOT NULL,
  object_id     BLOB NOT NULL,
  op            INTEGER NOT NULL,     -- 0 create 1 update 2 delete
  payload       BLOB NOT NULL,        -- CBOR delta
  lamport       INTEGER NOT NULL,
  created_at    INTEGER NOT NULL,
  attempts      INTEGER NOT NULL DEFAULT 0,
  next_retry_at INTEGER,
  last_error    TEXT                  -- error CODE, never a message with user content
);
CREATE INDEX ix_mutation_ready ON sync_mutation(next_retry_at, seq);

CREATE TABLE sync_cursor (
  scope         TEXT PRIMARY KEY,     -- 'workspace:<uuid>' | 'account'
  server_cursor TEXT,
  last_pull_at  INTEGER
);

CREATE TABLE sync_conflict (
  id            BLOB PRIMARY KEY,
  object_kind   INTEGER NOT NULL,
  object_id     BLOB NOT NULL,
  local_payload  BLOB NOT NULL,
  remote_payload BLOB NOT NULL,
  detected_at   INTEGER NOT NULL,
  resolved_at   INTEGER,
  resolution    INTEGER               -- 0 local 1 remote 2 merged 3 both-kept
);

CREATE TABLE sync_device_clock (      -- our view of every device's version vector
  device_id     BLOB PRIMARY KEY,
  lamport       INTEGER NOT NULL
);
```

### 3.8 `ai_metadata` + `embeddings` namespaces

```sql
CREATE TABLE ai_artifact (
  id            BLOB PRIMARY KEY,
  object_kind   INTEGER NOT NULL,
  object_id     BLOB NOT NULL,
  kind          TEXT NOT NULL,         -- 'summary'|'tags'|'classification'|'extraction'
  model_id      TEXT NOT NULL,         -- provider:model:version — required for reproducibility
  provider      TEXT NOT NULL,
  content       BLOB NOT NULL,         -- CBOR
  source_rev    INTEGER NOT NULL,      -- object rev this was derived from; stale if behind
  created_at    INTEGER NOT NULL
  -- + sync columns (summaries are worth syncing; embeddings are not)
);

CREATE TABLE chunk (
  id            BLOB PRIMARY KEY,
  object_kind   INTEGER NOT NULL,
  object_id     BLOB NOT NULL,
  ordinal       INTEGER NOT NULL,
  text          TEXT NOT NULL,
  -- provenance, so a citation can be resolved back to an exact location:
  page          INTEGER,
  block_id      BLOB,
  char_start    INTEGER,
  char_end      INTEGER,
  token_count   INTEGER NOT NULL,
  content_hash  TEXT NOT NULL,         -- skip re-embedding unchanged chunks
  source_rev    INTEGER NOT NULL
);
CREATE INDEX ix_chunk_obj ON chunk(object_kind, object_id, ordinal);

-- sqlite-vec virtual table; dimension is per embedding model
CREATE VIRTUAL TABLE chunk_vec USING vec0(
  chunk_id  TEXT PRIMARY KEY,
  embedding FLOAT[384]
);

CREATE TABLE embedding_meta (
  chunk_id      BLOB PRIMARY KEY REFERENCES chunk(id) ON DELETE CASCADE,
  model_id      TEXT NOT NULL,
  dim           INTEGER NOT NULL,
  created_at    INTEGER NOT NULL
);
```

**Embeddings are device-local and never synced.** Reasons: (a) they are a large,
losslessly-derivable artefact; (b) they are model-version-specific and two devices may be on
different model versions; (c) a vector is a lossy but non-trivial reconstruction of the source
text, so syncing them widens the data-exposure surface for no benefit. Cost: a new device
re-embeds. Accepted, and mitigated by re-embedding lazily on first semantic query.

**Model change ⇒ full re-embed.** `embedding_meta.model_id` makes this detectable rather than
silently returning garbage similarity.

### 3.9 Search indexes

```sql
CREATE VIRTUAL TABLE fts_content USING fts5(
  title, body,
  object_kind UNINDEXED,
  object_id   UNINDEXED,
  workspace_id UNINDEXED,
  tokenize = 'unicode61 remove_diacritics 2',   -- replaced by an ICU tokenizer for CJK
  prefix = '2 3'
);
```

- FTS rows are written in the **same transaction** as the content. A separate indexer thread
  that "catches up" is how search silently rots.
- Deletions use FTS5's `'delete'` command with the original text (external-content pattern),
  handled inside the repository so no caller can get it wrong.
- CJK needs a segmenting tokenizer; `unicode61` will not do. Custom ICU tokenizer registered
  at open. Tracked as a Phase 9 task, not an afterthought.
- Fuzzy search: trigram auxiliary table over titles + a bounded Levenshtein rescore on the
  top-N FTS candidates. We do **not** attempt fuzzy matching over full body text.

### 3.10 `permissions`, `comments`, `tasks`, `audit`

```sql
CREATE TABLE permission (
  id            BLOB PRIMARY KEY,
  resource_kind INTEGER NOT NULL,     -- 0 workspace 1 document 2 file
  resource_id   BLOB NOT NULL,
  subject_kind  INTEGER NOT NULL,     -- 0 account 1 link 2 group
  subject_id    BLOB,
  role          INTEGER NOT NULL,
  inherited     INTEGER NOT NULL DEFAULT 1,
  expires_at    INTEGER
  -- + sync columns
);
CREATE INDEX ix_perm_res ON permission(resource_kind, resource_id);

CREATE TABLE comment (
  id            BLOB PRIMARY KEY,
  document_id   BLOB NOT NULL REFERENCES document(id) ON DELETE CASCADE,
  block_id      BLOB REFERENCES block(id),
  thread_id     BLOB,
  author_id     BLOB NOT NULL,
  body          TEXT NOT NULL,
  resolved_at   INTEGER
  -- + sync columns
);

-- Tasks are a db_schema preset, not a separate table. A "task" is a db_row in a
-- database whose properties include status/due/assignee. This is deliberate: it means
-- every database feature (views, filters, rollups) works on tasks for free.
CREATE TABLE task_index (            -- denormalised read model, rebuildable
  row_id        BLOB PRIMARY KEY REFERENCES db_row(id) ON DELETE CASCADE,
  workspace_id  BLOB NOT NULL,
  status        INTEGER NOT NULL,
  due_at        INTEGER,
  assignee_id   BLOB
);
CREATE INDEX ix_task_due ON task_index(workspace_id, status, due_at);

CREATE TABLE audit_log (             -- local, append-only, tamper-evident
  seq           INTEGER PRIMARY KEY AUTOINCREMENT,
  at            INTEGER NOT NULL,
  actor         TEXT NOT NULL,       -- 'user' | 'agent:<id>' | 'system'
  action        TEXT NOT NULL,       -- 'ai.egress' | 'file.share' | 'data.delete' | ...
  resource_kind INTEGER,
  resource_id   BLOB,
  detail        BLOB,                -- CBOR; MUST NOT contain document content
  prev_hash     BLOB NOT NULL,       -- BLAKE2b(prev_hash || this row) — hash chain
  hash          BLOB NOT NULL
);
```

The audit hash chain makes local tampering detectable (not preventable). A break in the chain
is reported, not silently repaired.

## 4. Migration strategy

- Forward-only numbered migrations: `storage/sqlite/migrations/NNNN_name.sql`, applied in a
  transaction, tracked in `schema_migration(version, applied_at, checksum)`.
- **`user_version` alone is insufficient** — we record a checksum per migration so a tampered
  or diverged migration file is caught at open.
- Every migration ships with (a) a forward script, (b) a *data-integrity assertion* run after
  it, and (c) a fixture DB at version N−1 in `tests/fixtures/db/`.
- **No down-migrations.** Rolling an app back across a schema change is handled by refusing to
  open a DB newer than the binary understands (`STORE_SCHEMA_TOO_NEW`) and telling the user to
  update. Silent downgrade is a data-loss vector.
- Long migrations run with progress reporting; a migration that can exceed 5 s must be
  resumable, because users kill apps.

## 5. Cloud schema (PostgreSQL 16)

Separate Postgres **schemas** mirroring the namespaces: `identity`, `workspace`, `content`,
`files`, `sync`, `ai`. Each service owns its schema; cross-schema access is via views or the
owning service, not ad-hoc joins.

### 5.1 Differences from local, and why

| Aspect | Local (SQLite) | Cloud (Postgres) | Reason |
|---|---|---|---|
| Tenancy | single account | `workspace_id` on every row + **row-level security** | One bug must not cross tenants. RLS is a backstop under the application check, not a replacement for it. |
| Blocks | one row per block | `content.block` **partitioned by `workspace_id` hash** (64 partitions) | Keeps per-tenant working sets separate; makes tenant deletion a partition-local operation. |
| Block content | CBOR blob | `JSONB` | Server-side query/indexing (GIN) is worth the encoding cost; also makes admin debugging possible. |
| Versions | snapshots in DB | snapshot payloads in object storage, pointers in DB | Unbounded growth does not belong in the OLTP store. |
| Search | FTS5 | `tsvector` + GIN; **and** the client still has its own index | Server search is for cross-device/shared content; the client never depends on it. |
| Vectors | `sqlite-vec` | `pgvector` HNSW | Only used for server-side RAG, which only exists for cloud-AI workspaces. |
| Change feed | `sync_mutation` outbox | `sync.change_log` append-only, per-workspace monotonic `seq` | The pull cursor is a `seq`, giving cheap, correct, resumable pagination. |
| Deletes | soft, `deleted_at` | soft + a scheduled hard-delete job honouring the retention SLA | GDPR erasure must actually erase. |

### 5.2 Core cloud tables (abridged)

```sql
CREATE SCHEMA sync;

CREATE TABLE sync.change_log (
  workspace_id  uuid        NOT NULL,
  seq           bigint      NOT NULL,          -- per-workspace monotonic
  object_kind   smallint    NOT NULL,
  object_id     uuid        NOT NULL,
  op            smallint    NOT NULL,
  payload       jsonb       NOT NULL,
  version_vector jsonb      NOT NULL,
  origin_device uuid        NOT NULL,
  created_at    timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (workspace_id, seq)
) PARTITION BY HASH (workspace_id);

-- allocate seq without gaps or races
CREATE TABLE sync.workspace_seq (
  workspace_id uuid PRIMARY KEY,
  next_seq     bigint NOT NULL DEFAULT 1
);

CREATE TABLE files.blob (
  blob_id       text PRIMARY KEY,        -- BLAKE2b-256 of plaintext
  size_bytes    bigint NOT NULL,
  storage_key   text   NOT NULL,
  content_type  text,
  created_at    timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE files.blob_owner (          -- refcount across tenants; dedupe without leakage
  blob_id       text NOT NULL REFERENCES files.blob(blob_id),
  workspace_id  uuid NOT NULL,
  PRIMARY KEY (blob_id, workspace_id)
);
```

**Cross-tenant blob dedupe caveat.** Content-addressed storage shared across tenants enables a
*confirmation-of-a-file* oracle: an attacker who has a candidate file can learn whether it
already exists by observing upload deduplication timing. Mitigation: dedupe scope is
**per-workspace**, not global, unless the blob is already public. Recorded as risk R-06.

### 5.3 Row-level security pattern

```sql
ALTER TABLE content.block ENABLE ROW LEVEL SECURITY;
CREATE POLICY block_tenant ON content.block
  USING (workspace_id = current_setting('app.workspace_id')::uuid);
```
The API sets `app.workspace_id` per transaction from the *verified* token claim. RLS is a
defence-in-depth net; the service still performs its own authorisation check.

## 6. Local ↔ cloud mapping

| Local | Cloud | Synced? | Note |
|---|---|---|---|
| `workspace`, `workspace_member` | `workspace.*` | Yes | |
| `document`, `block` | `content.*` | Yes | Block `content` CBOR ↔ JSONB via a canonical codec with round-trip tests |
| `db_*` | `content.db_*` | Yes | |
| `file`, `file_metadata` | `files.*` | Yes (metadata) | Bytes go via the blob service, not the sync feed |
| `blob_ref` | `files.blob` | Partially | `local_path`, `pinned`, `last_access` are device-local |
| `version_snapshot` | `content.version` | Yes, lazily | Payload to object storage |
| `permission`, `comment` | `workspace.permission`, `content.comment` | Yes | |
| `ai_artifact` | `ai.artifact` | Yes | Summaries/tags are worth carrying to a new device |
| `chunk`, `chunk_vec`, `embedding_meta` | `ai.chunk`, `ai.chunk_vec` | **No** | Local-only (see §3.8). Server-side copies exist only for cloud-AI workspaces and are built server-side. |
| `fts_content`, `task_index`, `db_view_cache` | — | **No** | Derived, rebuildable |
| `sync_*` | `sync.change_log` | n/a | Protocol state |
| `audit_log` | `identity.audit` (subset) | Partially | Security events sync; local-only AI-egress records stay local unless the user opts in |
| `identity_session_meta` | — | **Never** | Secrets live in the platform keystore |

## 7. Capacity assumptions

Sizing so the design is falsifiable:

| Entity | Typical user | Heavy user | Note |
|---|---|---|---|
| Documents | 500 | 20 000 | |
| Blocks | 10 000 | 500 000 | ~400 B/row ⇒ 200 MB at heavy |
| Files | 300 | 20 000 | |
| Blob bytes | 2 GB | 200 GB | Selective sync means ≪ this is on-device |
| Chunks | 5 000 | 400 000 | |
| Vectors (384-dim f32) | 7 MB | 600 MB | **Heavy users must use quantised (int8) vectors** — 150 MB. Flagged. |
| FTS index | ~40 % of text | same | |

At the heavy end the SQLite file approaches 1 GB before blobs. That is workable but demands
the incremental-vacuum and version-compaction jobs actually run. Tracked as risk R-07.

## 8. Transaction and consistency rules

1. One SQLite writer, serialised in the storage adapter. Callers never see `SQLITE_BUSY`.
2. Content + FTS + chunk-invalidation commit in one transaction.
3. A mutation is enqueued to `sync_mutation` **in the same transaction** as the content change.
   Outbox pattern — no "wrote the data but lost the sync event" window.
4. Blob writes are two-phase: write to a temp path, fsync, verify hash, then insert `blob_ref`
   and rename. Crash before commit leaves an orphan temp file, which a startup sweep collects.
5. `PRAGMA wal_checkpoint(TRUNCATE)` on background-entry, so the WAL does not grow unbounded
   on a device that is never "idle".

## 9. What this design does not protect against

Stated plainly:

- **SQLCipher protects the DB file, not the blob store.** File bytes are separately encrypted
  with per-file keys (`security-model.md` §5); if that is not implemented, at-rest encryption
  is a half-promise.
- **`secure_delete` does not cover the filesystem.** On flash storage with wear levelling, an
  overwritten page may persist physically. Full-disk encryption plus key destruction is the
  only real answer, and that is the platform's job.
- **Derived data is not verified against its source.** If `text_cache` drifts from `content`,
  search returns stale results. Mitigation: a background consistency checker comparing
  `source_rev` values, reporting rather than silently rewriting.
- **EAV view performance at extreme property counts is unproven.** See the §3.4 budget and the
  pivot-table escape hatch.
- **Cross-schema referential integrity in Postgres is not enforced by FKs** across service
  boundaries. It is the services' responsibility, and it will occasionally be violated.
