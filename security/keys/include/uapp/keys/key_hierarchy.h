// SPDX-License-Identifier: Apache-2.0
//
// security/keys/key_hierarchy.h — the key HIERARCHY layer.
//
// This module owns the RELATIONSHIPS between keys, and nothing else. Every
// cryptographic operation below is a call into `security/crypto`; there is no
// libsodium on this module's link line and no <sodium.h> anywhere under
// security/keys/ (verifiable with `nm -u`, see security/keys/CMakeLists.txt).
// `security/crypto` can generate a 32-byte key, derive one from another, seal
// bytes and wrap a key between two device keypairs, and it deliberately "has
// no opinion about which of those a given key is" (crypto.h). THIS is the
// module with the opinion.
//
// =========================================================================
// WHAT IT IMPLEMENTS — security-model.md §5A.1, edge by edge
// =========================================================================
//
//   RECOVERY SECRET (32 B, CSPRNG, client-side, never transmitted)
//     │  §5A.4 presents it to the user as a 24-word phrase, shown ONCE.
//     │  The ENCODING of that phrase is NOT in this module (see NOT
//     │  IMPLEMENTED 1); this module's input is the 32 bytes behind it.
//     │
//     └─ derive_subkey(label = kLabelUserRootKey) ──► USER ROOT KEY (URK)
//          │
//          ├─ wrap_urk_for_device()      ──► DeviceWrappedUrkRecord
//          │     one per enrolled device, the §5A.1 "DEVICE KEY BUNDLE";
//          │     authenticated crypto_kx wrap to that device's X25519
//          │     wrapping key. Stored server-side as opaque ciphertext.
//          │
//          ├─ wrap_urk_under_passphrase() ──► PassphraseWrappedUrkRecord
//          │     Argon2id over a passphrase, then AEAD. See "THE RECOVERY
//          │     DIVERGENCE" below — this is an ADDITIONAL path, not the
//          │     root of trust.
//          │
//          └─ wrap_workspace_key()       ──► UrkWrappedWorkspaceKeyRecord
//                one per workspace, NOT one per device
//                  │
//                  └─ WORKSPACE KEY (WSK, 32 B, CSPRNG)
//                       │
//                       └─ derive_document_key(WSK, document_id)
//                            ──► DOCUMENT KEY, DERIVED AND NEVER STORED
//                                 └─ seal_document_content() / open_...()
//
// =========================================================================
// THE PER-DOCUMENT KEY SCHEME: DERIVED, NOT WRAPPED. (Not a judgment call —
// §5A.1 settles it, in a sentence written to pre-empt exactly this question.)
// =========================================================================
// > "**Per-document keys are derived, not stored.** `HKDF(WSK, doc_id)` means
// > one wrapped key per workspace, not one per document. Selective sharing of
// > a single document requires wrapping that derived key separately — which
// > is why document-level sharing to a non-workspace-member is **out of scope
// > at v1.0** (§5A.6)."
//
// So this module implements derivation (scheme (a)) and does NOT generate or
// store a per-document key. The consequences are real and are the design's,
// not this module's, but they belong at the point of use:
//
//   * ONE wrapped-key record per workspace. A million documents add a
//     million derivations and zero rows. This is the property that makes
//     offline-first sync of a large workspace tractable.
//   * A document key CANNOT be rotated independently, and a document CANNOT
//     be revoked individually. Rotation is a WSK-level event (§5A.3) and
//     re-encrypts the workspace. Per-document revocation would need scheme
//     (b) — a stored wrapped key per document — and §5A.6 puts the feature
//     that would need it (cross-account document sharing) out of scope at
//     v1.0 for precisely this reason.
//   * Losing the WSK loses the whole workspace. There is no per-document
//     escape hatch, by construction.
//   * A document key is a pure function of (WSK, document_id). Two devices
//     holding the WSK compute byte-identical document keys with no exchange,
//     which is what lets a revoked-then-re-enrolled device catch up.
//
// IF THAT TRADE IS EVER REVISITED, it is an ADR and a record-format version
// bump, not a quiet change: a stored per-document wrapped key is a new record
// type, and `kRecordFormatVersion` exists so that old records remain
// readable when it appears.
//
// =========================================================================
// THE RECOVERY DIVERGENCE — flagged for review, matching the §5A.2
// crypto_box_seal/crypto_kx precedent
// =========================================================================
// §5A.1 roots the URK in a GENERATED 32-byte recovery secret (~256 bits,
// shown once as 24 words) and reaches the URK by HKDF. §5A.4 is emphatic that
// recovery does NOT rest on a user-chosen passphrase, and §5's KDF correction
// leans on that: "§5A.4 deliberately does not rest recovery on a user-chosen
// passphrase: the root of trust is a generated 24-word phrase carrying ~256
// bits, so Argon2's work factor is defence in depth here rather than the only
// defence."
//
// This module therefore ships BOTH, with a deliberate asymmetry:
//
//   1. derive_user_root_key(recovery_secret) is THE RECOVERY PATH, and it
//      stores NOTHING. A new device with the 24 words re-derives the URK and
//      then unwraps the workspace keys from the server-stored
//      UrkWrappedWorkspaceKeyRecord — which is §5A.2 mechanism (2) exactly:
//      "re-derives the URK and unwraps the workspace keys directly from the
//      server-stored ciphertext. No other device needs to be present."
//   2. wrap_urk_under_passphrase() is an ADDITIONAL, OPTIONAL wrap. It is
//      what §5A.4's open design item — "whether the platform-keychain copy
//      stores the URK or a wrapped URK" — needs in order to store a WRAPPED
//      URK, and it is the shape a PIN/passphrase escrow record (§5A.4 option
//      B, "evaluate for v1.1") would have to have from day one to avoid a
//      migration.
//
// **FLAGGED FOR REVIEW.** A passphrase-wrapped URK is strictly weaker than
// the phrase: Argon2id at MobileInteractive (ops=3, 64 MiB) over whatever a
// human typed is not ~256 bits, and §5's own correction notes that the
// memlimit cut was accepted *because* the passphrase is not the only defence.
// It must not be presented to a user as an equivalent recovery method, and
// product sign-off is needed on whether it ships at all. It is implemented
// here because the record format must exist before the data model freezes,
// not because the policy question is settled. If the answer is "no", delete
// two functions and one record type — the hierarchy above is unaffected.
//
// =========================================================================
// THE RECORD FORMAT, and the one place it is not fully authenticated
// =========================================================================
// Three record types, one per edge that needs storage, each FIXED SIZE with
// no heap allocation. Ids are NOT stored in a record: a record's workspace or
// document id is its identity in whatever table eventually holds it, and the
// id is bound into the AAD so a record cannot be moved to another workspace.
//
// Every record carries `format_version` and `role` as PLAINTEXT metadata, and
// every unwrap function takes the role the CALLER expects as a parameter and
// checks it. The record's own fields are a cross-check, never an authority:
// nothing in this module decides what to do with a key by reading a field an
// attacker can edit.
//
//   * PassphraseWrappedUrkRecord and UrkWrappedWorkspaceKeyRecord bind their
//     whole header — version, role, wrap kind, salt, KDF profile, scope id —
//     into the AEAD's AAD. Editing any byte of either record's header makes
//     the unwrap fail with CryptoAuthFailed.
//
//   * DeviceWrappedUrkRecord CANNOT do that, and this is the one honest gap
//     in the format: crypto::wrap_content_key() takes NO AAD parameter. Its
//     AAD is fixed at `wrapper_pub || recipient_pub` inside the primitive, so
//     a device record's `format_version` and `role` fields are not covered by
//     its tag. What saves it is that they are not trusted either — the role
//     is checked against the caller's expectation, the version against this
//     build's, and the KEK is derived from the caller-supplied wrapper public
//     key rather than from the record's copy of it, so a record forged by
//     anyone who does not hold an enrolled device's secret key fails to
//     unwrap regardless of what its header claims.
//     **RECOMMENDED FOLLOW-UP, deliberately not done in this run:** add an
//     AAD-taking overload to the primitive —
//     `wrap_content_key(wrapper, recipient_public, cek, ByteView aad, ...)`,
//     appending caller AAD after the two public keys, which is
//     byte-compatible with today's format when the AAD is empty. That changes
//     a committed, externally-vectored primitive, so it wants a crypto review
//     of its own rather than a drive-by edit from the layer above.
//
// =========================================================================
// NOT IMPLEMENTED — and why each is somebody else's run
// =========================================================================
//   1. The 24-word phrase ENCODING. §5A.4 lists "the exact phrase encoding
//      and wordlist" as a design item that must be settled before Phase 12,
//      and it is not settled. This module takes and returns the 32 bytes; a
//      wordlist chosen here would be a wordlist chosen by accident.
//   2. URK-pub, the published X25519 half of the URK (§5A.1). Its only
//      consumer is server-mediated enrolment and cross-account sharing, both
//      out of scope (§5A.2 is a protocol, §5A.6 defers sharing). Deriving an
//      X25519 keypair deterministically from the URK also needs a primitive
//      security/crypto does not expose (scalarmult_base), and adding it for
//      an unused API is the trap crypto.h's item 1 described.
//   3. Device ENROLMENT (§5A.2): the QR exchange, the out-of-band short-code
//      comparison, the notify-every-device requirement and the audit entry.
//      wrap_urk_for_device() is the primitive those steps call. **The
//      short-code check is what binds a public key to a device identity, and
//      without it this module's device wrap authenticates a KEY, not a
//      DEVICE.** §5A.2 calls it "the single most important user-facing step
//      in the whole E2EE design"; nothing here substitutes for it.
//   4. WSK ROTATION and REVOCATION (§5A.3). "Revocation without rotation is
//      not revocation" — and rotation is a stateful, multi-record,
//      re-encrypt-and-snapshot operation, which is a protocol over storage,
//      not a function over keys.
//   5. PERSISTENCE. No SQLite, no tables, no serialisation. Deliberate, and
//      consistent with how core/conversion, core/blocks and core/search each
//      landed their logic before their storage: the records here are plain
//      fixed-size structs, and whether they live in SQLCipher rows, a
//      keychain item or an opaque server blob is a decision with its own
//      constraints (§5A.2 "the server stores and forwards opaque bytes").
//      A byte-exact on-the-wire encoding is the first half of that follow-up
//      and is also absent: `kRecordFormatVersion` is the hook it will need.
//   6. Per-blob keys, `HKDF(DocKey, blob_id)` (§5A.1). The chunked-blob path
//      that consumes them is NOT IMPLEMENTED in security/crypto (crypto.h,
//      item 2), so shipping the derivation now would be an unused API with a
//      security-critical label — the same argument that kept derive_subkey()
//      out of the primitives layer until this module needed it. It is one
//      function and one label when the chunked path lands.
//   7. KEY LIFETIME policy: when a URK or WSK is dropped from memory, what
//      happens on app background/suspend, hardware-backed storage. See
//      secure_bytes.h limits 1 and 2. `security/key-management` still does
//      not exist.
//   8. A C ABI. Unchanged from crypto.h item 6, and this module is the reason
//      it was deferred: the open question was "whether a caller is handed a
//      CEK, a document id, or an opaque key handle" (capi_common.cpp), and
//      the answer this module suggests — a document id plus a workspace key
//      handle — should survive a round of use before it is frozen.

#ifndef UAPP_SECURITY_KEYS_KEY_HIERARCHY_H
#define UAPP_SECURITY_KEYS_KEY_HIERARCHY_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "uapp/crypto/crypto.h"
#include "uapp/crypto/secure_bytes.h"
#include "uapp/status.h"

namespace uapp::keys {

// --- sizes and limits ------------------------------------------------------

/// The recovery secret is 32 bytes because §5A.1 says 32 bytes, and because
/// HKDF's IKM for a 256-bit output should not be narrower than the output.
inline constexpr std::size_t kRecoverySecretBytes = crypto::kContentKeyBytes;

/// Wrapped-key ciphertext: a 32-byte key plus its Poly1305 tag. A fixed
/// width, so no record in this module allocates.
inline constexpr std::size_t kWrappedKeyBytes = crypto::kContentKeyBytes + crypto::kTagBytes;

/// Longest workspace / document id this layer will bind into an AAD or feed
/// to HKDF as info. Matches crypto::kMaxKdfInfoBytes, which is the real
/// constraint; stated here so a caller reads one number.
inline constexpr std::size_t kMaxScopeIdBytes = crypto::kMaxKdfInfoBytes;

/// The only record format version this build writes or reads.
///
/// Bumped when the LAYOUT or the AAD CONSTRUCTION changes — the two are the
/// same thing, because the AAD is the header. A version this build does not
/// know is Status::KeyRecordVersionUnsupported and never a best-effort parse.
inline constexpr std::uint8_t kRecordFormatVersion = 1;

// --- domain-separation labels ---------------------------------------------
//
// One per edge of the hierarchy, each carrying a `/v1/` segment. These are
// the HKDF salt (see crypto.h, "SUBKEY DERIVATION") and the AAD prefix, and
// they are the reason two different edges cannot produce the same key from
// the same parent.
//
// **CHANGING A LABEL CHANGES EVERY KEY BELOW IT.** A URK derived under
// "uapp/keys/v1/user-root-key" is not the URK derived under any other string,
// so an edited label does not weaken a key — it silently makes every existing
// workspace undecryptable. Treat these as part of the stored format: a new
// label is a new version segment plus a migration, never an edit.

inline constexpr char kLabelUserRootKey[]       = "uapp/keys/v1/user-root-key";
inline constexpr char kLabelWorkspaceKeyWrap[]  = "uapp/keys/v1/workspace-key-wrap";
inline constexpr char kLabelDocumentKey[]       = "uapp/keys/v1/document-key";
inline constexpr char kLabelDocumentContent[]   = "uapp/keys/v1/document-content";
inline constexpr char kLabelPassphraseUrkWrap[] = "uapp/keys/v1/passphrase-urk-wrap";

// --- what a key IS in the hierarchy ---------------------------------------
//
// Four distinct types over one carrier. Every one of them is 32 bytes in a
// crypto::ContentKey, and the C++ type system is the only thing that stops a
// WSK being passed where a URK is expected — which is a mistake that would
// "work" (both are 32-byte AEAD keys) and would silently produce the wrong
// document keys for an entire workspace. Making it a compile error costs four
// one-line structs.
//
// All four are MOVE-ONLY, inherited from ContentKey: a key cannot be
// duplicated into a second unmanaged lifetime by an implicit copy.

/// The root of the whole hierarchy: 32 CSPRNG bytes that §5A.4 shows the user
/// once, as 24 words, and never transmits. Everything else is derivable from
/// this; lose it with no enrolled device left and the data is gone (§5A.4
/// option A, accepted risk 8).
struct RecoverySecret final {
    crypto::ContentKey material;
    [[nodiscard]] bool valid() const noexcept { return material.valid(); }
};

/// USER ROOT KEY. Symmetric, workspace-INDEPENDENT, one per user. Wraps
/// workspace keys; is itself wrapped per device and (optionally) under a
/// passphrase. Never leaves a device except wrapped.
struct UserRootKey final {
    crypto::ContentKey material;
    [[nodiscard]] bool valid() const noexcept { return material.valid(); }
};

/// WORKSPACE KEY. One per workspace, generated (not derived) so that §5A.3's
/// rotation can replace it without touching the URK.
struct WorkspaceKey final {
    crypto::ContentKey material;
    [[nodiscard]] bool valid() const noexcept { return material.valid(); }
};

/// DOCUMENT KEY. Derived from (WSK, document_id) on demand and never stored —
/// see "THE PER-DOCUMENT KEY SCHEME" above. Holding one of these is holding a
/// cached derivation, not a stored key.
struct DocumentKey final {
    crypto::ContentKey material;
    [[nodiscard]] bool valid() const noexcept { return material.valid(); }
};

// --- record metadata -------------------------------------------------------

/// Which key a record holds. Plaintext metadata, checked against the
/// caller's expectation on every unwrap and trusted for nothing.
enum class KeyRole : std::uint8_t {
    UserRoot  = 1,
    Workspace = 2,
    // No `Document = 3`. A document key is derived and never wrapped, so a
    // record role for it would name a record type that must not exist. If
    // scheme (b) ever arrives, this is where it announces itself.
};

/// What a record is wrapped UNDER. One value per record type, so it is
/// redundant with the C++ type — and that is the point: it is the field a
/// storage layer reads to know which struct to parse, and it is bound into
/// the AAD wherever the AAD is ours to choose.
enum class WrapKind : std::uint8_t {
    DeviceWrappingKey  = 1,  ///< authenticated crypto_kx wrap to a device's X25519 key
    RecoveryPassphrase = 2,  ///< Argon2id-derived key
    UserRootKey        = 3,  ///< a key derived from the URK
};

/// A URK wrapped to ONE device's X25519 wrapping key — §5A.1's "DEVICE KEY
/// BUNDLE", stored server-side as opaque ciphertext. One per enrolled device.
///
/// Both public keys are stored because both are public by design and because
/// a storage layer needs to know which device a bundle is for without
/// decrypting it. Neither is TRUSTED: see the record-format note above.
struct DeviceWrappedUrkRecord final {
    std::uint8_t      format_version = kRecordFormatVersion;
    KeyRole           role           = KeyRole::UserRoot;
    WrapKind          wrap_kind      = WrapKind::DeviceWrappingKey;
    crypto::PublicKey wrapper_public{};    ///< the enrolled device that sealed it
    crypto::PublicKey recipient_public{};  ///< the device that can open it
    crypto::WrappedKey wrapped{};          ///< nonce + 48 bytes
};

/// A URK wrapped under an Argon2id-derived key. See "THE RECOVERY
/// DIVERGENCE" — this is the optional path, flagged for product sign-off.
///
/// The salt AND the profile are both stored, because both are part of the
/// derived key's identity: "a key derived at MobileInteractive cannot be
/// re-derived at DesktopModerate — the profile is part of the key's identity
/// and must be stored with the salt" (crypto.h).
struct PassphraseWrappedUrkRecord final {
    std::uint8_t                              format_version = kRecordFormatVersion;
    KeyRole                                   role           = KeyRole::UserRoot;
    WrapKind                                  wrap_kind      = WrapKind::RecoveryPassphrase;
    crypto::Salt                              salt{};
    crypto::KdfProfile                        kdf_profile = crypto::KdfProfile::MobileInteractive;
    crypto::Nonce                             nonce{};
    std::array<std::uint8_t, kWrappedKeyBytes> ciphertext{};
};

/// A workspace key wrapped under a key derived from the URK. ONE per
/// workspace — not one per device, and not one per document.
///
/// The workspace id is NOT a field: it is this record's identity in storage,
/// and it is bound into both the derivation and the AAD, so the record cannot
/// be replayed as another workspace's.
struct UrkWrappedWorkspaceKeyRecord final {
    std::uint8_t                              format_version = kRecordFormatVersion;
    KeyRole                                   role           = KeyRole::Workspace;
    WrapKind                                  wrap_kind      = WrapKind::UserRootKey;
    crypto::Nonce                             nonce{};
    std::array<std::uint8_t, kWrappedKeyBytes> ciphertext{};
};

// --- generation ------------------------------------------------------------
//
// crypto::init() must have succeeded first. Every function here propagates
// Status::CryptoNotInitialised rather than producing unseeded output.

/// 32 fresh CSPRNG bytes. The only thing in this module a user must back up.
[[nodiscard]] Status generate_recovery_secret(RecoverySecret& out);

/// Derive the URK from the recovery secret. DETERMINISTIC: this is §5A.2
/// mechanism (2), the recovery path that needs no other device present.
[[nodiscard]] Status derive_user_root_key(const RecoverySecret& secret, UserRootKey& out);

/// A fresh workspace key. GENERATED, not derived: §5A.3 rotates the WSK on
/// device revocation, which a key derived from the URK could not do without
/// rotating the URK and therefore the recovery phrase.
[[nodiscard]] Status generate_workspace_key(WorkspaceKey& out);

// --- URK <-> device bundles (§5A.1, §5A.2 mechanism 1) ---------------------

/// Wrap `urk` so that only `recipient_public`'s holder can unwrap it,
/// authenticated as coming from `wrapper`.
///
/// This is the primitive §5A.2's proximity enrolment calls AFTER the
/// out-of-band short-code comparison has established that `recipient_public`
/// really belongs to the device in front of the user. Calling it without that
/// step wraps the user's root key to whatever public key the server supplied.
[[nodiscard]] Status wrap_urk_for_device(const crypto::DeviceKeyPair& wrapper,
                                         const crypto::PublicKey&     recipient_public,
                                         const UserRootKey&           urk,
                                         DeviceWrappedUrkRecord&      out);

/// Unwrap a URK from a device bundle.
///
/// `expected_wrapper_public` is MANDATORY and comes from the caller's trusted
/// device list, never from the record: the key-encryption key is derived from
/// THIS value, so a bundle forged by anyone without an enrolled device's
/// secret key fails to unwrap no matter what its header claims. A record
/// whose own `wrapper_public` disagrees is rejected before any key material
/// is touched, with the same CryptoAuthFailed the AEAD would have produced.
///
/// Errors: KeyRecordVersionUnsupported, KeyRecordRoleMismatch (both checked
/// first, over plaintext metadata), CryptoKeyInvalid, CryptoAuthFailed for
/// every form of "this did not authenticate".
[[nodiscard]] Status unwrap_urk_from_device(const crypto::DeviceKeyPair&  recipient,
                                            const crypto::PublicKey&      expected_wrapper_public,
                                            const DeviceWrappedUrkRecord& record,
                                            UserRootKey&                  out);

// --- URK under a recovery passphrase (§5A.4, flagged above) ---------------

/// Wrap `urk` under an Argon2id-derived key, generating a fresh salt.
///
/// `profile` is explicit, with no default, for the reason crypto.h gives: the
/// cost is part of the key's identity. MobileInteractive is the only profile
/// legal on a path that must also run on a phone.
[[nodiscard]] Status wrap_urk_under_passphrase(crypto::ByteView            passphrase,
                                               crypto::KdfProfile          profile,
                                               const UserRootKey&          urk,
                                               PassphraseWrappedUrkRecord& out);

/// Unwrap a URK with the passphrase that wrapped it. The salt and profile
/// come from the record and are bound into the AAD, so editing either makes
/// this fail rather than silently derive a different key.
///
/// A wrong passphrase is CryptoAuthFailed — the same code as a tampered
/// record, deliberately, and `out` is left invalid in both cases.
[[nodiscard]] Status unwrap_urk_with_passphrase(crypto::ByteView                  passphrase,
                                                const PassphraseWrappedUrkRecord& record,
                                                UserRootKey&                      out);

// --- workspace keys under the URK -----------------------------------------

/// Wrap `wsk` for `workspace_id` under a key derived from `urk`.
///
/// `workspace_id` is bound into the derivation AND the AAD. It is not stored
/// in the record; the caller owns the mapping from id to record.
[[nodiscard]] Status wrap_workspace_key(const UserRootKey& urk, crypto::ByteView workspace_id,
                                        const WorkspaceKey&            wsk,
                                        UrkWrappedWorkspaceKeyRecord& out);

/// Unwrap `workspace_id`'s key. Passing a different id than the wrap used is
/// CryptoAuthFailed, not a wrong key: the id is in the AAD.
[[nodiscard]] Status unwrap_workspace_key(const UserRootKey& urk, crypto::ByteView workspace_id,
                                          const UrkWrappedWorkspaceKeyRecord& record,
                                          WorkspaceKey&                       out);

// --- document keys: derived, never stored ---------------------------------

/// Derive `document_id`'s key from `wsk`. DETERMINISTIC and storage-free.
///
/// Errors: KeyScopeIdInvalid (empty, null or over kMaxScopeIdBytes — an
/// empty id would give every document in a workspace the SAME key, so it is
/// rejected rather than normalised), CryptoKeyInvalid, CryptoKdfFailed.
[[nodiscard]] Status derive_document_key(const WorkspaceKey& wsk, crypto::ByteView document_id,
                                         DocumentKey& out);

// --- content, at the bottom of the chain ----------------------------------
//
// Two thin functions, here rather than at the call site, because the AAD
// POLICY for document content is a hierarchy decision: content sealed for one
// document must not verify as another document's. Leaving that to each caller
// is how one caller forgets.

/// Seal `plaintext` under `key`, binding `document_id` into the AAD.
///
/// The nonce is generated inside crypto::seal() and comes back in `out`; this
/// layer adds no nonce discipline of its own and must not.
[[nodiscard]] Status seal_document_content(const DocumentKey& key, crypto::ByteView document_id,
                                           crypto::ByteView plaintext, crypto::SealedBlob& out);

/// Open a sealed blob, requiring the same `document_id`.
///
/// `out` is a SecureBytes because recovered document content is asset A1
/// (§1). On ANY failure it is left empty: no unauthenticated plaintext is
/// ever handed back, which the test suite asserts rather than assumes.
[[nodiscard]] Status open_document_content(const DocumentKey&        key,
                                           crypto::ByteView          document_id,
                                           const crypto::SealedBlob& blob,
                                           crypto::SecureBytes&      out);

}  // namespace uapp::keys

#endif  // UAPP_SECURITY_KEYS_KEY_HIERARCHY_H
