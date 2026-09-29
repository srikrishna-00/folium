# Security Model & Threat Model

**Status:** Phase 0 draft — awaiting human sign-off and external review
**Version:** 0.0.0
**Date:** 2026-09-29

Implements spec §23 (security) and §24 (privacy). This is a *basics* threat model suitable for
starting Phase 1. It is **not** a substitute for the external review required by the v1.0
release gate.

---

## 1. Assets, ranked

| # | Asset | Why it matters | Worst case |
|---|---|---|---|
| A1 | User document content (notes, PDFs, scans) | The product's whole reason to exist; often contains contracts, IDs, medical, financial material | Irreversible confidentiality loss |
| A2 | Master encryption keys / device keys | Compromise ⇒ all of A1 | Total compromise |
| A3 | Auth credentials and refresh tokens | Account takeover ⇒ A1 | Total compromise |
| A4 | Derived content (OCR text, chunks, embeddings, summaries) | Reconstructs much of A1 | Near-equivalent to A1 |
| A5 | Metadata (filenames, sizes, timestamps, workspace graph) | Leaks a lot even without content | Serious privacy harm |
| A6 | Third-party AI provider API keys (server-held) | Financial + abuse | Cost, reputational |
| A7 | Availability of the user's own data | Offline-first means the device copy is often the *only* copy | Data loss |
| A8 | Audit log integrity | The record of what left the device | Undetectable policy violation |

**A4 is routinely under-protected.** Teams encrypt the PDF and leave the OCR text and
embeddings in a plaintext table. Our schema treats derived content as equal in sensitivity to
its source.

## 2. Trust boundaries

```
┌─ Untrusted ─────────────────────────────────────────────────────────┐
│ File bytes from anywhere (share sheet, download, cloud import,      │
│ sync peer). Network responses. AI model output. Plugin code.        │
│ QR/barcode payloads. Deep-link URLs. Clipboard.                     │
└───────────────┬─────────────────────────────────────────────────────┘
                │  TB-1: parse boundary (arena, caps, fuzzed)
┌───────────────▼─────────────────────────────────────────────────────┐
│ Semi-trusted: the core process. Holds plaintext while in use.       │
│ Shares an address space with every parser. See R-02.                │
└───────────────┬─────────────────────────────────────────────────────┘
                │  TB-2: platform keystore boundary (hardware-backed)
┌───────────────▼─────────────────────────────────────────────────────┐
│ Trusted: Secure Enclave / StrongBox / TEE. Key material never       │
│ leaves; we hold handles, not bytes.                                 │
└─────────────────────────────────────────────────────────────────────┘

                │  TB-3: device ↔ server (TLS 1.3, pinned)
┌───────────────▼─────────────────────────────────────────────────────┐
│ Server: trusted-but-breachable. Assume a future breach and design   │
│ so that a database dump is not a content dump.                      │
└───────────────┬─────────────────────────────────────────────────────┘
                │  TB-4: server ↔ AI provider (the privacy-critical hop)
┌───────────────▼─────────────────────────────────────────────────────┐
│ Third-party AI providers: untrusted with respect to A1. Every byte  │
│ crossing here requires recorded consent.                            │
└─────────────────────────────────────────────────────────────────────┘
```

## 3. Threat model (STRIDE, per boundary)

### TB-1 — malicious file input (the highest-likelihood technical threat)

| ID | Threat | Impact | Mitigation | Residual |
|---|---|---|---|---|
| T-101 | Memory-corruption RCE via crafted PDF/TIFF/DOCX | Critical | Fuzz every parser; ASan/UBSan CI; arena allocation with hard caps; bounds-checked wrappers; pin and track upstream CVEs for PDFium/libtiff/Tesseract | **Medium.** Parsers share the app address space (A2-4 rejected on iOS). Risk R-02. |
| T-102 | Decompression bomb (ZIP/GZ/PNG/TIFF) → OOM/DoS | High | Global expansion-ratio cap (100:1) and absolute output cap before materialising; per-job memory ceiling | Low |
| T-103 | XXE / billion-laughs in DOCX/XLSX/SVG/XMP | High | Expat with external entities, DTDs and parameter entities **disabled unconditionally**, not by config | Low |
| T-104 | Zip-slip (`../` in archive entry names) on extract | High | Canonicalise and verify every entry path is inside the target root; reject absolute paths, symlinks and device files | Low |
| T-105 | PDF with embedded JavaScript / auto-actions / launch actions | Medium | PDFium built **without V8**; `/OpenAction`, `/AA`, `/Launch`, `/SubmitForm`, `/ImportData` stripped on open | Low |
| T-106 | SSRF via a PDF/HTML/SVG external reference | Medium | **No network fetch during parsing or rendering, ever.** External refs render as placeholders. | Low |
| T-107 | Polyglot file (valid PDF *and* valid HTML) defeats type-based routing | Medium | Detection records ambiguity; routing uses the detected type, never the extension; ambiguous files get the most restrictive handler | Medium |
| T-108 | Malicious font (embedded in PDF/DOCX) exploiting FreeType | High | FreeType kept current; font parsing inside the arena; reject fonts above a size/complexity bound | Medium |
| T-109 | OCR/ML model file swapped for a malicious payload | High | Models are downloaded over pinned TLS and verified against a signature from our release key before load. A model is code. | Low |

### TB-2 — device compromise / local attacker

| ID | Threat | Impact | Mitigation | Residual |
|---|---|---|---|---|
| T-201 | Lost/stolen unlocked device | Critical | App lock with biometric + passcode fallback; configurable auto-lock; content hidden in the app switcher; clipboard auto-expire for sensitive copies | Low |
| T-202 | Rooted/jailbroken device reads app storage | Critical | Keys in Keystore/Keychain with hardware backing where available and `whenUnlockedThisDeviceOnly`; SQLCipher; blob encryption. **We do not pretend to defeat root.** Optional attestation-based warning, not enforcement. | **High and accepted.** Stated in the privacy policy. |
| T-203 | Backup exfiltration (iCloud/Google backup, ADB backup) | High | `allowBackup=false`; keys marked non-exportable and `ThisDeviceOnly` so a restored backup cannot decrypt; explicit opt-in export instead | Low |
| T-204 | Another app reads our files | High | Scoped storage; app-private directories; no world-readable paths; `FileProvider` with per-URI grants | Low |
| T-205 | Screenshot / screen recording of sensitive content | Medium | `FLAG_SECURE` on sensitive screens (opt-in, since it breaks legitimate screenshots); blur on background | Medium |
| T-206 | Plaintext temp files left after a conversion | High | All temp work in an encrypted scratch directory, unlinked on completion **and** on crash-recovery sweep at startup | Medium — an OOM kill can leave residue |
| T-207 | Secrets in logs or crash reports | High | Logging API cannot accept a document-content type; a compile-time-checked redaction wrapper; crash reports strip heap and file paths | Medium |

### TB-3 — network and server

| ID | Threat | Impact | Mitigation | Residual |
|---|---|---|---|---|
| T-301 | MITM / hostile CA | Critical | TLS 1.3 minimum; certificate pinning with a backup pin; pin rotation ships ahead of cert rotation (see §7) | Low |
| T-302 | Stolen refresh token replayed | Critical | Rotating single-use refresh tokens with reuse detection → revoke the whole family and notify | Low |
| T-303 | Server breach dumps the content DB | Critical | Blobs encrypted at rest with per-file keys; DB encrypted at rest; **but without E2EE the server can decrypt.** | **High until PQ-2 is decided.** Risk R-03. |
| T-304 | Broken object-level authorisation (IDOR) | Critical | Every handler authorises the *subject against the object*, not just authenticates; Postgres RLS as a second net; an automated test asserts every endpoint rejects a cross-tenant id | Low |
| T-305 | A malicious/compromised device pushes poisoned sync data | High | Mutations signed by the device key; server verifies the device is registered and unrevoked; server validates payload schema and size; per-device rate limits; revocation invalidates future pushes | Medium — a compromised device can still corrupt its own user's data; version history is the recovery path |
| T-306 | Presigned URL leaked or shared | Medium | 5-minute expiry, single-use where the storage backend supports it, scoped to one object and one method | Low |
| T-307 | Enumeration of users / resources | Medium | 404 and 403 are indistinguishable for non-visible resources; uniform timing on auth paths; rate limits | Low |
| T-308 | DoS via expensive jobs (100 MB PDF ×1000) | Medium | Per-account job quotas; CPU/mem/time caps per worker; work is queued, never unbounded | Low |

### TB-4 — AI egress (the privacy-critical boundary)

| ID | Threat | Impact | Mitigation | Residual |
|---|---|---|---|---|
| T-401 | Confidential document silently sent to a cloud LLM | Critical (product-killing) | Workspace `ai_policy`; per-operation consent; non-bypassable policy layer between the API facade and the AI module; audit-log entry recording *what* left, *to whom*, *when*; **local-only policy fails closed** | Low **if the policy layer is genuinely non-bypassable** — this is an architectural invariant to test, not a code-review item |
| T-402 | Prompt injection in a document steers an agent ("ignore instructions, email this file") | **High** | Retrieved content is delivered in a clearly-delimited data channel, never concatenated into the instruction channel; tool calls are authorised against the *user's* grant, not the model's request; destructive tools always require human confirmation; agents run with a least-privilege tool allowlist per run | **Medium-High.** There is no complete defence. Risk R-05. |
| T-403 | Provider retains or trains on our users' content | High | Contractual: zero-retention / no-training DPAs with every provider; the provider list is public; a provider without such terms is not shipped | Low — but contractual, not technical |
| T-404 | Model output rendered as trusted HTML/markdown → XSS or a malicious link | Medium | Output is sanitised and rendered as text/limited markdown; links are not auto-opened; no script execution | Low |
| T-405 | Fabricated citations presented as sourced | **High** (trust) | Every citation is validated against the chunk store before rendering; unresolvable citations are stripped and their spans marked unsupported | Low |
| T-406 | Embedding-inversion recovers source text from vectors | Medium | Embeddings are device-local and not synced; server-side vectors exist only for cloud-AI workspaces and inherit the at-rest encryption | Medium |

### Agent-specific (spec §20)

The five prohibitions in spec §20 are enforced mechanically, not by prompt:

| Prohibition | Enforcement |
|---|---|
| No deletion without confirmation | `delete` tools are marked `requires_human_confirm`; the executor blocks on a UI callback. There is no "auto-approve" setting in v1.0. |
| No sharing without permission | The share tool requires a `share` grant scoped to a specific resource. Grants are per-run and expire. |
| No external data send without permission | The AI egress policy layer applies to agents identically to direct user actions. |
| No arbitrary code execution | There is no shell/eval tool. Ever. Tools are a fixed, first-party, compiled registry. |
| No private docs to external AI without consent | Same policy layer as T-401. |

Plus: every tool invocation is written to the audit log **before** it executes, with its
arguments (content-redacted), so a run is reconstructible even if it crashes.

## 4. Authentication and session management

- **Primary:** email + password (Argon2id, `opslimit=3`, `memlimit=64 MiB`, server-side) or
  OIDC federation. Passwords checked against a breached-password list at set time.
- **MFA:** TOTP (RFC 6238) at v1.0; WebAuthn/passkeys strongly preferred and targeted for v1.0
  if schedule allows. Recovery codes, single-use, shown once.
- **Sessions:** 15-minute access JWT (EdDSA), 60-day rotating single-use refresh token bound to
  the device id. Reuse detection revokes the family.
- **Device registration:** each device generates an Ed25519 keypair at first run; the private
  key lives in the keystore and never leaves. Sync mutations are signed with it.
- **Revocation:** revoking a device invalidates its refresh family immediately and adds it to a
  revocation list checked at push time. Access tokens live ≤ 15 min, which bounds the window.
- **Step-up:** changing the password, disabling MFA, revoking a device, exporting all data, or
  changing the AI policy requires re-authentication within the last 5 minutes.

## 5. Key hierarchy

```
Device Keystore / Secure Enclave  (hardware-backed, non-exportable)
   │
   ├── DEK-wrap key (AES-256, biometric- or passcode-gated)
   │       │
   │       ├── Database Key ─────────► SQLCipher page encryption (AES-256-CBC + HMAC)
   │       │
   │       └── Blob Master Key
   │               └── per-file key = HKDF(BlobMasterKey, blob_id)
   │                     └── XChaCha20-Poly1305 over file bytes, 1 MiB chunks,
   │                         nonce = HKDF(file_key, chunk_index) — deterministic,
   │                         never reused, enables random access for large PDFs
   │
   └── Device Identity Key (Ed25519) ─► signs sync mutations, device registration
```

- **Per-file keys derived from the blob id** give crypto-erasure (drop the key ⇒ the bytes are
  gone) and random access without decrypting a whole 500 MB file to read page 300.
- **Chunked AEAD, not whole-file.** Whole-file AEAD makes streaming a 500 MB PDF impossible
  within the memory budget. Each 1 MiB chunk is independently authenticated; the chunk index
  is bound into the nonce **and** the AAD so chunks cannot be reordered or spliced between files.
- **AAD** for each chunk = `blob_id || chunk_index || total_chunks`. This prevents truncation
  and cross-file substitution, which naive chunked encryption allows.
- **Key rotation:** the DEK-wrap key rotates on password change; blob keys are content-derived
  and therefore rotate only via re-encryption (a background job, post-1.0).
- **Nothing derives a key from a password on the client without Argon2id** (`opslimit=4`,
  `memlimit=256 MiB` locally, where we can afford it).

**If E2EE is adopted (PQ-2):** a workspace key wrapped per-member with X25519 sealed boxes,
rooted in a user key derived from a recovery phrase. The server then never sees plaintext —
and loses the ability to search, thumbnail, convert or run RAG. This is why it is a product
decision, not a security toggle.

## 6. Encryption in transit

- TLS 1.3 only. TLS 1.2 permitted only if analytics shows a material device tail; if so,
  restricted to AEAD suites with forward secrecy.
- Certificate pinning: SPKI pins, **two** pins (current + next), with a hard-coded expiry after
  which pinning falls back to system trust rather than bricking the app. A pinning outage that
  requires an app-store release to fix is a self-inflicted denial of service.
- HSTS with preload on all web origins.
- No cleartext HTTP anywhere: `usesCleartextTraffic=false`, ATS with no exceptions.

## 7. Pin rotation procedure (written down now, because it is always improvised later)

1. Generate the next key pair ≥ 90 days before rotation; publish its SPKI pin in an app release.
2. Wait until ≥ 95 % of active installs are on a version carrying both pins.
3. Rotate the server certificate to the new key.
4. Only then remove the old pin, in a subsequent release.

Never rotate to a key whose pin is not already deployed.

## 8. Authorisation model

Subject × Resource × Action, evaluated in `core/permissions` and again server-side.

- **Roles:** viewer(0) < commenter(1) < editor(2) < admin(3) < owner(4).
- **Inheritance:** workspace → document tree → child documents. An explicit grant on a child
  overrides inheritance. Denials are not expressible in v1.0 (they make evaluation
  non-monotonic and are a common source of bugs); if needed, add them with an explicit
  precedence rule.
- **Evaluation is deny-by-default.** Absence of a grant is a denial.
- **Share links** are a distinct subject kind with their own role, optional password
  (Argon2id-verified server-side), and optional expiry. A share link never confers write access
  to permissions themselves.
- **Client-side checks are UX, not security.** The server re-evaluates every time.

## 9. Redaction (called out because it is uniquely easy to get wrong)

Drawing a black rectangle over text is **not** redaction, and shipping it as such is a
foreseeable harm to users who rely on it for legal documents.

Correct implementation:
1. Tokenise the page content stream.
2. Remove text-showing operators (`Tj`, `TJ`, `'`, `"`) whose glyph boxes intersect the region
   — including partial matches, which requires splitting `TJ` arrays.
3. Clip or remove image XObjects intersecting the region; for partial intersection, re-encode
   the image with the region painted out.
4. Remove or clip vector paths intersecting the region.
5. Drop annotations, form fields, and their values intersecting the region.
6. Scrub the redacted strings from `/Info`, XMP, structure tree (`/StructTreeRoot`), embedded
   files, and any `/Metadata` streams.
7. Rewrite the file with QPDF **without** incremental update (which would retain the original
   objects in the file).
8. **Verify:** re-open the output, extract all text and metadata, and assert the redacted
   strings are absent. Fail the operation if they are not.

Limitations to state in-product: text baked into a raster image is only removed if that region
is repainted; content inside an unparseable embedded object may survive; redaction cannot undo
a copy the user already shared.

## 10. Logging, telemetry, observability

- **Never logged:** document content, OCR text, chunk text, AI prompts/completions, file names,
  passwords, tokens, keys, precise location.
- **Logged:** error codes, durations, byte counts, format identifiers, anonymous device class,
  app version, request ids.
- The logging API is typed so that a content-bearing type cannot be passed to it. A lint rule
  backs this up. "Remember not to log it" is not a control.
- Crash reports: symbolicated stacks only; heap and file paths stripped; opt-in, with a clear
  explanation.
- The audit log (`audit_log`, hash-chained) is separate from telemetry and never leaves the
  device unless the user exports it.

## 11. Secure deletion

- Soft delete → trash (30 days) → hard delete.
- Hard delete: drop the per-file key (crypto-erasure, the only reliable method on flash),
  delete the blob, remove rows, and **remove the corresponding FTS and vector rows in the same
  transaction** — otherwise the content is still searchable, which users will discover.
- Server: hard delete propagates to object storage and to backups within the published SLA.
  Backups are the part everyone forgets; the retention window must be stated publicly.
- `PRAGMA secure_delete=ON` for the DB. We state plainly that on wear-levelled flash, physical
  erasure is not guaranteed.

## 12. Dependency and supply-chain security

- Every third-party source pinned by commit hash **and** SHA-256; CI verifies both.
- Automated CVE monitoring against the pinned set; a Critical CVE in a parser is a same-week
  patch release.
- SBOM (CycloneDX) generated per release build from the actual link graph.
- Build artefacts are reproducible and signed; release keys live in an HSM.
- No dependency is added without a licence file, an owner, and a `THIRD_PARTY_NOTICES` row —
  enforced by CI (NFR-8).
- **Model weights are treated as dependencies**: pinned, hash-verified, signature-checked
  before load (T-109).

## 13. Security testing requirements

| Activity | Cadence | Gate |
|---|---|---|
| SAST (clang-tidy, CodeQL) | every PR | blocking |
| ASan + UBSan full suite | every PR | blocking |
| TSan suite | nightly | blocking on regression |
| Fuzzing (all parsers) | continuous; ≥ 72 h clean before release | release gate |
| Dependency CVE scan | daily | blocking on Critical |
| Secret scanning | every PR | blocking |
| Authorisation matrix test (every endpoint × every role × cross-tenant id) | every PR | blocking |
| Crypto unit tests against RFC/NIST vectors | every PR | blocking |
| External penetration test | before v1.0, then annually | release gate |
| Threat-model review | each phase boundary | — |

## 14. Accepted risks (explicit)

These are not solved. They are accepted, and the user-facing documentation must not imply
otherwise.

1. **Parsers share the app address space on mobile.** Process isolation is unavailable on iOS.
   (R-02)
2. **Rooted/jailbroken devices are not defended against.** (T-202)
3. **Without E2EE, a server breach exposes content.** Pending PQ-2. (R-03)
4. **Prompt injection has no complete mitigation.** We reduce blast radius; we do not eliminate
   it. (R-05)
5. **Cloud AI providers are trusted contractually, not technically.** (T-403)
6. **Metadata leaks to the server even under E2EE** — sizes, timing, workspace structure.
7. **Availability of local-only data depends on the user's own backups.** If a user disables
   sync and loses the device, the data is gone. This must be said clearly in the UI, not buried.
