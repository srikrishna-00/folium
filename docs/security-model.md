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
| T-303 | Server breach dumps the content DB | Critical | **E2EE from v1.0 (`adr/0002` decision 1, §5A).** The server holds no key: a dump yields ciphertext plus metadata. Blobs, CRDT operations and snapshots are all sealed client-side. | **Low for content** — a database dump is no longer a content dump, which is what TB-3 was designed for. **Medium for metadata** (§5A.5). Risk R-03 downgraded. |
| T-309 | **New.** Malicious server substitutes its own public key during device enrolment and receives the workspace keys | **Critical** | Out-of-band short-code verification between the enrolling and the enrolling-from device (§5A.2, mechanism 1); enrolment notifies all devices and is audited; step-up auth required | **Medium.** The defence is a user action that can be skipped by a user in a hurry. Key transparency is the real fix and is out of scope at v1.0 (§5A.6). |
| T-310 | **New.** Revoked device retains access because revocation did not rotate the workspace key | **Critical** | Revocation **always** rotates the WSK and re-wraps to remaining devices, then writes a new snapshot (§5A.3). "De-list without re-encrypt" is explicitly prohibited. | Low, **provided** rotation is tested as part of revocation rather than assumed. Forward secrecy for already-synced content is not claimed. |
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

> **Two nonce strategies, and only one of them is implemented (2026-10-17).** The chunked-blob
> path above uses a **deterministic** nonce, `HKDF(file_key, chunk_index)`, whose uniqueness
> invariant is "the chunk index is unique within a file and the file key is unique per blob".
> That is the right design *there*, because a chunk index is derivable rather than stored and
> random access to page 300 of a 500 MB PDF needs it. **It is NOT IMPLEMENTED.**
>
> What `security/crypto` implements is the other case — sealing one block, page or chunk under
> a CEK — and it uses a **fresh random 192-bit nonce generated inside `seal()`, which the
> caller cannot supply, influence or reuse.** There is no `seal_with_nonce()` overload, not
> even a test-only one, because an API that accepts a nonce cannot prevent its misuse by
> documentation. XChaCha20-Poly1305's 192-bit nonce is what makes that stateless design sound:
> collision probability is `< q²/2^193`, i.e. `2^-129` at `2^32` encryptions under one key.
> (AES-GCM's 96-bit IV reaches `~2^-33` at the same volume, which is why NIST SP 800-38D caps
> a random-IV GCM key at `2^32` invocations. The primitive choice and the nonce strategy are
> one decision, not two.)
>
> **When the chunked path lands it must be a separately-named entry point whose caller owns
> the chunk index — never a nonce parameter bolted onto `seal()`.** The full argument,
> including why a counter is more robust to RNG failure but needs durable state this layer
> must not invent, is in `security/crypto/include/uapp/crypto/crypto.h`.
- **Key rotation:** the DEK-wrap key rotates on password change; blob keys are content-derived
  and therefore rotate only via re-encryption (a background job, post-1.0).
- **Nothing derives a key from a password on the client without Argon2id** (~~`opslimit=4`,
  `memlimit=256 MiB` locally, where we can afford it~~ — **CORRECTED 2026-10-17, see below**).

> **Correction (2026-10-17): the parameters above cannot run on our own device floor.**
> `adr/0002` decision 3 fixes the floor at **2 GB RAM / iOS 15+ / Android 8+**. A 256 MiB
> transient Argon2 allocation on top of the app's working set is a foreground low-memory kill
> on both platforms. The clause "where we can afford it" was written *before* the floor was
> fixed; once it was, the answer became no, and this paragraph was not revisited.
>
> **Implemented parameters** (`security/crypto`, `KdfProfile::MobileInteractive`):
> **`opslimit = 3`, `memlimit = 64 MiB`**, algorithm pinned to
> `crypto_pwhash_ALG_ARGON2ID13` explicitly rather than `ALG_DEFAULT` (which changed meaning
> from Argon2i to Argon2id at libsodium 1.0.15 — a derived key whose algorithm depends on the
> linked version is a key that silently stops deriving after a dependency roll).
>
> This is libsodium's **own documented procedure** applied to our constraint, not an invented
> pair: "set `memlimit` to the amount of memory you want to reserve… then set `opslimit` to
> `3`". 64 MiB is libsodium's INTERACTIVE preset and the largest of its three that fits the
> floor. Measured 61 ms on an Apple M4; estimated 0.4-0.6 s on a floor-tier device, which is
> inside the 1 s online budget libsodium's own guidance names.
>
> **The cost, stated plainly:** a 4x cut in `memlimit` is a 4x cut in an offline attacker's
> memory cost per guess — the exact dimension Argon2 exists to raise. It is accepted because
> (a) the alternative is a passphrase path that cannot run on the devices we committed to, and
> (b) §5A.4 deliberately does not rest recovery on a user-chosen passphrase: the root of trust
> is a generated 24-word phrase carrying ~256 bits, so Argon2's work factor is defence in
> depth here rather than the only defence.
>
> **Still owed:** measure on real floor-tier hardware rather than extrapolating from a desktop
> multiplier (same class of gap as SP-3/SP-4's Tier D re-runs). `docs/spikes/SP-9-findings.md`
> §5 carries the full argument and the measurements.
>
> A `DesktopModerate` profile (ops=3, 256 MiB) exists in the API for desktop-only keys. It
> **must not** be used for any key that also has to be derivable on mobile: the cost profile is
> part of a derived key's identity and must be stored alongside the salt.

---

## 5A. E2EE key management (`adr/0002` decision 1)

**E2EE is a v1.0 property. The server holds no key that can decrypt user content.**

The cryptography here is the easy part — libsodium does it. **The hard part is key
distribution across devices and what happens when a device is lost**, and that is where this
section spends its length. Anything written here that sounds like hand-waving should be
treated as an unresolved design item, not as a decision.

> **Dependency status (2026-10-17).** "libsodium does it" was, until this date, the entire
> extent of the library decision: no licence review, no pin, no `THIRD_PARTY_NOTICES` row and
> no mobile-distribution analysis existed, all four of which §12 requires before a dependency
> lands. `docs/spikes/SP-9-findings.md` supplies them and the library is now vetted,
> pinned (`third_party/libsodium/`) and integrated (`THIRD_PARTY_NOTICES` SECTION 0).
>
> Two results from that vetting belong here rather than only in the spike:
>
> 1. **The licence is a non-event.** ISC, verified against the installed bytes. No
>    static-vs-dynamic distinction, so static linking into a closed-source App Store binary is
>    unambiguously permitted; attribution is the only obligation.
> 2. **The real mobile constraint is US export control, and it has no owner.** Bundling a
>    crypto library that provides *content confidentiality* makes the app's encryption
>    **non-exempt** for Apple's purposes (`ITSAppUsesNonExemptEncryption = YES`), which pulls
>    the app into EAR Category 5 Part 2 and requires an annual self-classification report to
>    BIS and the NSA encryption coordinator, plus a France declaration. **This follows from
>    `adr/0002` decision 1, not from the choice of libsodium** — any library encrypting user
>    content lands in the same place. §12 below covers supply chain and says nothing about
>    export control; this is new risk **R-33** and it needs counsel, not engineering.
>
> **What exists in code today** (updated 2026-10-20) is **two layers**:
>
> * `security/crypto` — the PRIMITIVES: key generation, AEAD, authenticated key wrapping
>   between two device keypairs, HKDF-SHA256 subkey derivation, the passphrase KDF, and
>   secure memory.
> * `security/keys` — **the hierarchy in §5A.1**, i.e. Recovery Secret → URK → Workspace Key
>   → derived per-document key, plus the wrapped-key record format. It links
>   `security/crypto` and **not libsodium** (verified: zero libsodium symbols imported), so
>   "all cryptography goes through `security/crypto`" is a property of the build graph one
>   layer up as well as at the bottom.
>
> **The enrolment PROTOCOL in §5A.2 and the rotation/revocation in §5A.3 remain NOT
> IMPLEMENTED**, as do the 24-word phrase encoding, URK-pub, per-blob keys, and any
> persistence of the records. `security/keys/include/uapp/keys/key_hierarchy.h` and
> `security/crypto/include/uapp/crypto/crypto.h` each list their gaps item by item, at the
> point where a reader would otherwise assume they had been handled.

### 5A.1 Extended key hierarchy

```
RECOVERY SECRET  (32 B, generated client-side, never transmitted)
  ├─ presented to the user as a 24-word BIP-39-style phrase, shown ONCE
  └─ HKDF ──► USER ROOT KEY (URK)
                │
                ├── URK-pub (X25519)  ── published to the server (public, by design)
                │
                ├── wraps ──► WORKSPACE KEY (WSK, one per workspace, symmetric)
                │               └── HKDF(WSK, doc_id) ──► DOCUMENT KEY
                │                     ├─ seals CRDT ops + snapshots (sync-architecture §10)
                │                     └─ HKDF(DocKey, blob_id) ──► per-file key
                │                           └─ chunked XChaCha20-Poly1305 (§5)
                │
                └── wrapped per device ──► DEVICE KEY BUNDLE
                                            stored server-side as OPAQUE CIPHERTEXT
Per device, as before (§5):
  Device Identity Key (Ed25519)   — signs mutations; non-exportable; Keystore/Enclave
  Device Wrapping Key  (X25519)   — receives sealed key material; non-exportable
  Database Key                    — SQLCipher; local only; never leaves the device
```

**Key properties, and the reason each exists:**

- **The URK never touches the server.** Only `URK-pub` does. The server can therefore help
  devices find each other without ever being able to decrypt anything they exchange.
- **Per-document keys are derived, not stored.** `HKDF(WSK, doc_id)` means one wrapped key
  per workspace, not one per document. Selective sharing of a single document requires
  wrapping that derived key separately — which is why document-level sharing to a
  non-workspace-member is **out of scope at v1.0** (§5A.6).
- **The database key is local-only and unrelated to the URK.** Restoring a device backup
  therefore cannot resurrect a decryptable database (T-203 holds unchanged).

> **IMPLEMENTED 2026-10-20 — `security/keys`, and three things this section needed to be
> more specific about.** The hierarchy above is now code: `generate_recovery_secret()`,
> `derive_user_root_key()`, `generate_workspace_key()`, `wrap_urk_for_device()`,
> `unwrap_urk_from_device()`, `wrap_workspace_key()`, `unwrap_workspace_key()`,
> `derive_document_key()` and the two content functions, with 13 cases over 6 ctest entries.
> The module performs no cryptography of its own and is not linked against libsodium.
>
> **1. "Per-document keys are derived, not stored" was unambiguous and is implemented as
> written.** `derive_document_key()` is a pure function of `(WSK, document_id)` — no record,
> no row, no per-document wrapped key, and no `KeyRole::Document` in the record format,
> because a role for a key that is never wrapped would name a record type that must not
> exist. The price is restated here because it is permanent until an ADR changes it: there is
> **no per-document rotation and no per-document revocation**; rotation is a WSK-level event
> (§5A.3) that re-encrypts the workspace; and losing the WSK loses the workspace with no
> per-document escape hatch. In exchange, a device that holds the WSK derives every document
> key with no exchange and no storage, which is what makes §5A.2 mechanism (2) and
> post-rotation catch-up work at all.
>
> **2. The HKDF is `crypto_kdf_hkdf_sha256`, not `crypto_kdf_derive_from_key`** — a change
> from what `crypto.h` previously predicted, recorded rather than absorbed.
> `crypto_kdf_derive_from_key` takes a fixed **8-byte** context and a **uint64** subkey id,
> and a `doc_id` here is a string; hashing a string id into a u64 to fit that API would create
> a collision domain — two documents sharing a key — that this section does not have, to save
> nothing. This section says "HKDF", and RFC 5869 HKDF is what landed (libsodium ≥ 1.0.19).
> The construction, which is now part of the stored format: `PRK = HKDF-Extract(salt = edge
> label, IKM = parent key)`, then `HKDF-Expand(PRK, info = the child's id, 32)`. The label is a
> versioned compile-time constant, one per edge (`uapp/keys/v1/user-root-key`,
> `.../workspace-key-wrap`, `.../document-key`, `.../document-content`,
> `.../passphrase-urk-wrap`). **Changing a label does not weaken a key; it silently makes
> every existing workspace undecryptable.** Treat labels as stored format: a new label is a
> new version segment plus a migration.
>
> **3. The record format, and one place it is not fully authenticated.** Three fixed-size
> record types — `DeviceWrappedUrkRecord` (the "DEVICE KEY BUNDLE" above, one per enrolled
> device), `PassphraseWrappedUrkRecord` (see §5A.4) and `UrkWrappedWorkspaceKeyRecord` (one per
> workspace). Ids are not record fields: an id is a record's identity in storage and is bound
> into the AAD, so a record cannot be replayed into another workspace or document. Every
> record's `format_version` and `role` are plaintext metadata, and **every unwrap takes the
> expected role as a parameter and checks it** — no code path decides what to do with a key by
> believing a field an attacker can edit. The passphrase and workspace records bind their whole
> header into the AEAD's AAD, so a tampered salt or a **downgraded KDF profile** is an
> authentication failure (asserted in the suite). The device record **cannot**, because
> `wrap_content_key()` takes no AAD parameter — its AAD is fixed at `wrapper_pub ||
> recipient_pub` inside the primitive. It is safe because nothing trusts those fields: the KEK
> is derived from the **caller-supplied** wrapper public key, taken from the caller's trusted
> device list, so a forged bundle fails regardless of its header. **FOR REVIEW:** the clean fix
> is an AAD-taking overload of `wrap_content_key()`, byte-compatible with today's format for an
> empty AAD; it was deliberately not done in the same run, because it changes a committed
> primitive that is pinned by external published vectors and deserves its own crypto review.
>
> **Persistence is NOT IMPLEMENTED and is deliberately not sketched.** These records are plain
> structs; whether they live in a SQLCipher row, a keychain item or an opaque server blob is a
> decision with constraints (§5A.2's "the server stores and forwards opaque bytes") that the
> run which has them in front of it should make. `kRecordFormatVersion` is the hook a wire
> encoding will need.

### 5A.2 Adding a device — the key exchange

The server mediates but cannot read. Three mechanisms, in preference order:

**(1) Proximity enrolment (preferred).** The new device generates its X25519 wrapping
keypair in the Keystore/Enclave and displays its public key as a QR code plus a short
numeric code. An **already-enrolled device** scans it, verifies the short code out-of-band
(the user reads it aloud / compares on screen — this is the step that defeats a
server-substituted key), wraps the workspace keys to the new device's public key using an
**authenticated** key-wrap (see the resolution below), and uploads the wrapped bundle. The
server stores and forwards opaque bytes.

> The out-of-band short-code comparison is **not optional**. Without it, a malicious server
> substitutes its own public key for the new device's and receives the workspace keys. This
> is the single most important user-facing step in the whole E2EE design, and it is the one
> most likely to be cut for onboarding-funnel reasons. **It must not be.**

> **RESOLVED 2026-10-20 (raised 2026-10-17, `spikes/SP-9-findings.md` §7 F-5): the wrap
> authenticates the sender. `crypto_box_seal` is NOT used here.**
>
> `crypto_box_seal` uses an **ephemeral** sender keypair, so it authenticates **nobody** — the
> new device would only learn that *someone* holding its public key sealed the bundle. That is
> a real property (sender anonymity) but not the one this step needs: T-305 is "a malicious/
> compromised device pushes poisoned sync data," and an unauthenticated key-wrap channel is a
> way to do exactly that during enrolment.
>
> **Decision (product-owner sign-off 2026-10-20): `security/crypto`'s `wrap_content_key()`
> implements the authenticated `crypto_kx` variant** (X25519 + BLAKE2b over *both* static
> public keys, deriving a directional KEK, then XChaCha20-Poly1305 over the key being wrapped
> with both public keys bound into the AAD). The recipient can tell which device sealed it, and
> the rx/tx split means a wrapped key cannot be replayed back at its sender. This gives up
> sender anonymity, which enrolment does not need — every device belongs to the same user, and
> knowing *which* of the user's own devices sealed a bundle is a feature (it is exactly what an
> audit log of enrolment events wants), not a leak. If an anonymous-sender primitive is ever
> needed elsewhere in this design, `crypto_box_seal` is an **additional** function for that
> specific case, never a silent substitute for this one — the two have different security
> properties and picking one by accident is how protocols break.
>
> Separately, and asserted in `tests/crypto` so it cannot be forgotten: libsodium rejects a
> peer X25519 public key only when the agreement output is all-zero (the small-order
> subgroup). A non-canonical all-`0xff` encoding is accepted. So "the library validates the
> peer key" is narrower than it sounds, and binding a public key to a *device identity*
> remains entirely the job of the short-code check above.

**(2) Recovery-phrase enrolment.** The user enters the 24-word phrase on the new device,
which re-derives the URK and unwraps the workspace keys directly from the server-stored
ciphertext. No other device needs to be present or online. This is the *only* mechanism that
works when the user has exactly one device and loses it.

**(3) Provider-assisted enrolment — deliberately NOT built at v1.0.** See §5A.4.

**Enrolment is visible.** Every device addition notifies every other enrolled device, names
the device, and is written to the audit log. A user who did not add a device must find out
immediately. Enrolment requires step-up re-authentication (§4).

### 5A.3 Key rotation with multiple devices

Rotation is where multi-device E2EE designs typically break, because a device that is
offline across a rotation must not be silently locked out, and a *revoked* device must not
silently retain access.

**Rotation events and what each does:**

| Event | Action |
|---|---|
| Device revoked / lost | **Rotate the WSK.** Re-wrap to all *remaining* devices. Write a new snapshot under the new document keys (`sync-architecture.md` §10.3), then prune superseded updates. |
| User password change | Rotates the **DEK-wrap key only**. The URK is unaffected — it is rooted in the recovery secret, not the password. |
| Recovery phrase regenerated | Re-derive the URK; re-wrap every WSK; re-wrap every device bundle. Expensive; user-initiated only. |
| Routine/scheduled rotation | **Not done at v1.0.** Rotation without a triggering event adds a failure mode and buys little. Stated rather than silently omitted. |

**The forward-secrecy limit, stated plainly.** A revoked device still holds every byte it
already synced, and it holds the *old* WSK. Rotation prevents it decrypting **future**
content. It does **not** retroactively protect past content. Presenting "remove device" as
if it erased that device's copy would be dishonest; the UI must say "this device will not
receive new changes" and not imply more.

**The offline-device problem.** A device offline across a rotation cannot decrypt anything
written after it. On reconnect it fetches its new sealed bundle (wrapped to its unchanged
X25519 public key) and catches up. This works because rotation re-wraps to *public* keys and
needs no interaction with the offline device. **But if that device was revoked while offline,
it never receives a new bundle — which is exactly correct, and is why revocation must rotate
rather than merely de-list.**

**The lazy-rotation trap, called out because it is the tempting shortcut:** "mark the device
revoked server-side and skip the re-encryption" leaves every future document encrypted under
a key the revoked device still holds. If it retains any read access — a leaked token, a
sync endpoint that fails open, a backup — it reads everything written afterwards. Revocation
without rotation is not revocation.

### 5A.4 Recovery when a device is lost — the genuinely hard problem

**There is no solution that is simultaneously (a) zero-knowledge, (b) survivable when the
user forgets everything, and (c) usable by a non-technical person. Pick two.** Anyone
claiming otherwise is either escrowing a key or losing users' data. What follows is which
two we pick and what we give up.

**The options, with what each actually costs:**

| Option | Zero-knowledge | Survives total user amnesia | Usability | Verdict |
|---|---|---|---|---|
| **A. Recovery phrase only** | ✅ | ❌ — lose the phrase, lose the data, permanently | Poor: users lose phrases at scale | **Ship as the baseline.** |
| **B. PIN + rate-limited HSM/enclave escrow** (Signal SVR / iCloud Keychain model) | ~ Reduces to trusting the enclave and the attempt counter | ✅ if the PIN is remembered | Good | **Evaluate for v1.1.** See below. |
| **C. Server escrow of the URK** | ❌ | ✅ | Excellent | **Blocked.** It is not E2EE. Never ship. |
| **D. Social / guardian recovery (M-of-N shares)** | ✅ | ✅ if guardians persist | Poor for consumers; good for teams | **Out of scope at v1.0.** |
| **E. Platform keychain sync** (iCloud Keychain / Google Block Store) | Delegated to the platform | Partly | Excellent, but single-ecosystem | **Ship as a convenience layer, never as the only copy.** |

**What the real-world designs actually do**, because this is worth learning from rather than
re-deriving:

- **Signal's SVR** stores a client secret server-side under a low-entropy PIN, and makes that
  safe by **hard-limiting guess attempts inside an enclave** with strongly-consistent
  replicated state for the guess counter (SVR2 uses in-memory Raft). SVR3 goes further and
  splits trust across **three enclave types from three vendors on three clouds** (Intel SGX /
  Azure, AMD SEV-SNP / GCP, Nitro / AWS), so compromising two is not enough. And it still
  concedes the core limit: **a forgotten PIN is unrecoverable**, mitigated only by
  periodically re-prompting the user so the PIN stays in memory.
- **Apple's iCloud Keychain escrow** guards records in **HSM clusters**, proves PIN knowledge
  via **SRP** so the code never reaches Apple, allows **10 attempts**, and then **destroys
  the escrow record** — deliberately sacrificing the data rather than allowing brute force.

Both are multi-year infrastructure projects. Neither is a v1.0 feature for this product, and
pretending otherwise would be the kind of estimate this project has tried to avoid.

**v1.0 position:**

1. **Recovery phrase (option A) is the root of trust.** Generated client-side, shown once,
   with a mandatory verification step (re-enter N random words) before the workspace is
   usable. No "remind me later" — the funnel cost is real and is accepted.
2. **Platform keychain (option E) as a convenience copy**, opt-in, clearly labelled as
   "stored by Apple/Google, protected by your device passcode", never presented as
   equivalent to the phrase.
3. **Multiple enrolled devices are the practical recovery mechanism.** Two devices means
   losing one is an inconvenience, not a catastrophe. The product should actively push users
   toward a second device, and warn — persistently and unmissably — while only one exists.
4. **The single-device, no-phrase, no-keychain user is unrecoverable, and we say so before
   it happens, not after.** This is accepted risk 8 (§14). It is the direct price of E2EE and
   it must appear in onboarding, in settings, and in the privacy policy.
5. **Support cannot help.** There is no back door, no "verify your identity and we'll
   restore it". Support processes, training and canned responses must be written on that
   basis **before launch** — this is an operational deliverable, not just an engineering one.

**Design items that must be settled before Phase 12 coding** (see SPIKE_SUMMARY open
questions): the exact phrase encoding and wordlist; whether the platform-keychain copy stores
the URK or a wrapped URK; and whether option B is committed for v1.1 (which would change the
v1.0 data model, because the escrow record's shape must exist from the start to avoid a
migration).

> **PARTIALLY ADDRESSED 2026-10-20; the sign-off below was given 2026-10-20.**
>
> **What this section's position means in code.** `security/keys` implements position 1 as
> **the** recovery path and it stores nothing: `derive_user_root_key(recovery_secret)` is
> deterministic, so a new device holding only the 32 bytes behind the 24 words re-derives the
> URK and unwraps the workspace key from the server-stored record with no other device present
> — §5A.2 mechanism (2), verified end to end on real content in `uapp.keys.chain`. The phrase
> **encoding and wordlist remain unsettled and unimplemented**; the module's input is the 32
> bytes, deliberately, so that a wordlist is not chosen by accident by the layer below the
> decision.
>
> **What is new, and what needs a decision.** The second design item above — "whether the
> platform-keychain copy stores the URK or a wrapped URK" — needs a wrapped-URK record to be
> answerable either way, so one exists: `PassphraseWrappedUrkRecord`, the URK sealed under an
> **Argon2id-derived key** (profile and salt stored with it, both bound into the AAD, so a
> downgrade rewrite is an authentication failure). It is also the shape **option B's** escrow
> record would need from day one to avoid the migration this section warns about.
>
> **This is an ADDITIONAL path, not a second root of trust, and it does not ship as one.**
> Argon2id at `MobileInteractive` over a user-chosen passphrase is not the ~256 bits a
> generated phrase carries, and §5's own `memlimit` correction was accepted *because* the
> passphrase is not the only defence — using this record as a primary recovery method would
> retroactively invalidate that argument.
>
> **Decision (product-owner sign-off 2026-10-20): keep this as record-format infrastructure
> only; do not commit to shipping it as a recovery path yet.** The policy question — whether
> option B (or any platform-keychain-under-passphrase escrow) is pursued for v1.1 — remains
> genuinely open and is **not** decided by this code existing. What this resolves is narrower:
> the record format exists now, before the data model freezes, so that *if* the answer is
> later yes, it is not a migration across every user's keys (the exact failure mode §5A.4's
> closing paragraph warns about). If the eventual answer is no, `PassphraseWrappedUrkRecord`
> and its two functions delete cleanly with no effect on the rest of the hierarchy — nothing
> else in `security/keys` depends on it. Revisit when v1.1 scope is actually planned.

### 5A.5 What E2EE does **not** protect

Stated here so the privacy policy can be drafted from it honestly:

- **Metadata.** The server learns `doc_id`, `device_id`, per-device counters, ciphertext
  sizes and timing — i.e. **who edits what, when, and roughly how much**
  (`sync-architecture.md` §10.2). Accepted risk 6.
- **A compromised client.** E2EE protects against a breached server, not against malware on
  an unlocked device (T-202, unchanged and still High).
- **Consented cloud AI egress.** Plaintext sent under §13.2 of `ai-architecture.md` is
  outside the encryption boundary by the user's explicit decision. That is the *point* of the
  consent flow, and it must be presented as an exception, never as covered.
- **Content a user exports or shares by other means.**

### 5A.6 Explicitly out of scope at v1.0

- Cross-account document sharing with independent keys (requires per-document key wrapping
  and a membership-change protocol — this is most of what makes Phase 17 expensive).
- Server-side full-text search, thumbnailing, conversion or RAG (`sync-architecture.md` §10.4).
- Post-quantum key exchange. X25519 only. Revisit when libsodium ships a stable hybrid KEM.
- Key transparency / auditable key directories (the real defence against server key
  substitution at scale; §5A.2's out-of-band code is the v1.0 substitute).

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
| Crypto unit tests against RFC/NIST vectors | every PR | blocking — **partially live since 2026-10-17**, see below |
| External penetration test | before v1.0, then annually | release gate |
| **Export-control self-classification report to BIS/NSA (R-33, §5A)** | **before first submission, then annually** | **release gate — NOT an engineering task, see `spikes/SP-9-findings.md` §4.2** |
| Threat-model review | each phase boundary | — |

**On the "crypto unit tests against RFC/NIST vectors" row, now that it has an implementation
to be blocking on.** `tests/crypto` (7 ctest entries, 27 cases) runs on every build where
`UAPP_ENABLE_CRYPTO` resolves ON, in both Debug and Debug+ASan/UBSan. External published
vectors cover two of the three layers:

- **AEAD** — `draft-irtf-cfrg-xchacha-03` §A.3.1, driven through the real decrypt path, plus
  one-bit mutations in both ciphertext and tag, plus wrong-AAD and no-AAD rejection.
- **X25519** — RFC 7748 §6.1's Alice/Bob keypairs used as real device keys through the real
  wrap/unwrap path, both directions, plus small-order public-key rejection.
- **Argon2id — NO external vector is possible**, and this is structural rather than an
  omission: RFC 9106's published vectors all use parallelism `p = 4` and libsodium's
  `crypto_pwhash` hardcodes `p = 1`, so they are unreachable through the API. Golden
  regression vectors are used instead and labelled as such. What they catch is the failure
  that matters: a derivation that silently changed across a dependency roll would lock every
  existing user out of their own data (§5A.4). See `spikes/SP-9-findings.md` §7 F-6.

A round-trip test is *not* what satisfies this row. `seal()` followed by `open()` passes for
any pair of mutually-inverse functions, including a wrapper that silently drops the AAD or
confuses XChaCha20 with the original ChaCha20 — both invisible to a round trip and
catastrophic against a real attacker.

## 14. Accepted risks (explicit)

These are not solved. They are accepted, and the user-facing documentation must not imply
otherwise.

1. **Parsers share the app address space on mobile.** Process isolation is unavailable on iOS.
   (R-02)
2. **Rooted/jailbroken devices are not defended against.** (T-202)
3. ~~Without E2EE, a server breach exposes content.~~ **Resolved by `adr/0002` decision 1.**
   E2EE ships at v1.0; a server breach yields ciphertext and metadata. (R-03 downgraded.)
4. **Prompt injection has no complete mitigation.** We reduce blast radius; we do not eliminate
   it. (R-05)
5. **Cloud AI providers are trusted contractually, not technically.** (T-403) Under E2EE this
   is now a *user-consented exception* to the encryption boundary rather than an ambient
   condition (`ai-architecture.md` §13.2) — which is better, but not zero.
6. **Metadata leaks to the server even under E2EE** — `doc_id`, `device_id`, per-device
   counters, sizes, timing. Who edits what and when is visible. (§5A.5)
7. **Availability of local-only data depends on the user's own backups.** If a user disables
   sync and loses the device, the data is gone. This must be said clearly in the UI, not buried.
8. **Key loss is unrecoverable, by design.** A single-device user who loses the device and has
   neither the recovery phrase nor a platform-keychain copy has lost the data permanently.
   Support cannot help. There is no back door. **This is the direct price of decision 3 in the
   options table (§5A.4), it is not a bug, and it must be stated before it happens** — in
   onboarding, in settings, and in the privacy policy. (New)
9. **Revocation is not retroactive.** A revoked device keeps everything it already synced.
   Rotation protects future content only. (§5A.3)
10. **No key transparency at v1.0.** A malicious server can attempt key substitution at
    enrolment; the only defence is the user performing the out-of-band code check. (T-309)
