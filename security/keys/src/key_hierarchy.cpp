// SPDX-License-Identifier: Apache-2.0
//
// security/keys/key_hierarchy.cpp — the hierarchy, and nothing but the
// hierarchy.
//
// RULE 0 INHERITED FROM security/crypto: no cryptographic primitives are
// implemented here. There is no <sodium.h> in this file — this module is not
// even linked against libsodium (see CMakeLists.txt) — so the compiler
// enforces it rather than a review. What this file contains is: role and
// version checks over plaintext metadata, AAD construction, and calls into
// uapp::crypto in the order security-model.md §5A.1 draws them.
//
// Every function follows the same shape, which is the shape the primitives
// layer established and is worth stating once:
//   1. clear the out-parameter FIRST, so no path can return a partially
//      populated key or record alongside an error;
//   2. check the cheap, non-secret things (version, role, id length) before
//      touching any key material;
//   3. do exactly one key operation, and propagate its Status unchanged —
//      never translate a CryptoAuthFailed into something more specific.

#include "uapp/keys/key_hierarchy.h"

#include <cstring>

namespace uapp::keys {
namespace {

/// A label is a string literal; its AAD/HKDF form excludes the NUL.
template <std::size_t N>
[[nodiscard]] crypto::ByteView label_view(const char (&literal)[N]) noexcept {
    static_assert(N >= 2, "a domain-separation label must not be empty");
    static_assert(N - 1 <= crypto::kMaxKdfLabelBytes, "label is longer than crypto's cap");
    return crypto::ByteView{reinterpret_cast<const std::uint8_t*>(literal), N - 1};
}

/// Enough for the longest label, the longest scope id, and the handful of
/// header bytes in between, with room to spare. AAD is framing, never
/// content (crypto.h), so this is a cap on metadata and not on data.
inline constexpr std::size_t kAadBufferBytes =
    crypto::kMaxKdfLabelBytes + 64u + kMaxScopeIdBytes;

/// Builds the canonical AAD for a record. Fixed buffer, no heap, and every
/// append is checked: an AAD that silently truncated would be an AAD that
/// stopped binding the field at the end of it.
///
/// Not a SecureBytes, deliberately: an AAD here is a label, a version, a role
/// and an id — all public metadata that travels with the ciphertext.
class Aad final {
  public:
    [[nodiscard]] bool append(crypto::ByteView v) noexcept {
        if (v.size > buf_.size() - size_) { return false; }
        if (v.size != 0) {
            if (v.data == nullptr) { return false; }
            std::memcpy(buf_.data() + size_, v.data, v.size);
        }
        size_ += v.size;
        return true;
    }

    [[nodiscard]] bool append_byte(std::uint8_t b) noexcept {
        if (size_ == buf_.size()) { return false; }
        buf_[size_] = b;
        ++size_;
        return true;
    }

    [[nodiscard]] crypto::ByteView view() const noexcept {
        return crypto::ByteView{buf_.data(), size_};
    }

  private:
    std::array<std::uint8_t, kAadBufferBytes> buf_{};
    std::size_t                               size_ = 0;
};

[[nodiscard]] std::uint8_t role_byte(KeyRole r) noexcept { return static_cast<std::uint8_t>(r); }
[[nodiscard]] std::uint8_t kind_byte(WrapKind k) noexcept { return static_cast<std::uint8_t>(k); }

[[nodiscard]] std::uint8_t profile_byte(crypto::KdfProfile p) noexcept {
    return static_cast<std::uint8_t>(static_cast<int>(p));
}

/// A workspace or document id must be present and bounded.
///
/// An EMPTY id is rejected rather than treated as a default, because an empty
/// id would make every document in a workspace derive the SAME key — a
/// catastrophic outcome from a caller's forgotten field, and exactly the kind
/// of mistake this layer exists to make impossible.
[[nodiscard]] Status check_scope_id(crypto::ByteView id) noexcept {
    if (id.data == nullptr || id.size == 0) { return Status::KeyScopeIdInvalid; }
    if (id.size > kMaxScopeIdBytes) { return Status::KeyScopeIdInvalid; }
    return Status::Ok;
}

/// Version and role/kind, in that order, over plaintext fields only.
///
/// This is a cross-check, NOT an authority: the caller states the role it
/// expects, and a record claiming a different one is refused instead of being
/// believed. See the record-format note in key_hierarchy.h.
[[nodiscard]] Status check_header(std::uint8_t version, KeyRole role, WrapKind kind,
                                  KeyRole expected_role, WrapKind expected_kind) noexcept {
    if (version != kRecordFormatVersion) { return Status::KeyRecordVersionUnsupported; }
    if (role != expected_role || kind != expected_kind) { return Status::KeyRecordRoleMismatch; }
    return Status::Ok;
}

/// Public keys are public; a timing-safe comparison would be theatre. The
/// secret-bearing comparisons in this module are all inside libsodium's AEAD
/// tag check, which is constant-time by construction.
[[nodiscard]] bool same_public_key(const crypto::PublicKey& a,
                                   const crypto::PublicKey& b) noexcept {
    return a == b;
}

/// Move 32+16 sealed bytes out of a crypto::SealedBlob into a record's fixed
/// array, rejecting any length that is not exactly right.
[[nodiscard]] Status copy_wrapped(const crypto::SealedBlob&                  sealed,
                                  std::array<std::uint8_t, kWrappedKeyBytes>& out) noexcept {
    if (sealed.ciphertext.size() != out.size()) { return Status::Internal; }
    std::memcpy(out.data(), sealed.ciphertext.data(), out.size());
    return Status::Ok;
}

/// Rebuild a crypto::SealedBlob from a record's fixed fields.
[[nodiscard]] crypto::SealedBlob to_blob(const crypto::Nonce&                              nonce,
                                         const std::array<std::uint8_t, kWrappedKeyBytes>& ct) {
    crypto::SealedBlob blob;
    blob.nonce = nonce;
    blob.ciphertext.assign(ct.begin(), ct.end());
    return blob;
}

/// AAD for a passphrase-wrapped URK record: the whole header.
[[nodiscard]] bool build_passphrase_aad(const PassphraseWrappedUrkRecord& record, Aad& aad) {
    return aad.append(label_view(kLabelPassphraseUrkWrap)) &&
           aad.append_byte(record.format_version) && aad.append_byte(role_byte(record.role)) &&
           aad.append_byte(kind_byte(record.wrap_kind)) &&
           aad.append(crypto::ByteView{record.salt.data(), record.salt.size()}) &&
           aad.append_byte(profile_byte(record.kdf_profile));
}

/// AAD for a URK-wrapped workspace-key record: the header plus the workspace
/// id, which is NOT a field of the record (it is its identity in storage) and
/// is therefore the one part the caller must supply identically on both
/// sides.
[[nodiscard]] bool build_workspace_aad(const UrkWrappedWorkspaceKeyRecord& record,
                                       crypto::ByteView workspace_id, Aad& aad) {
    return aad.append(label_view(kLabelWorkspaceKeyWrap)) &&
           aad.append_byte(record.format_version) && aad.append_byte(role_byte(record.role)) &&
           aad.append_byte(kind_byte(record.wrap_kind)) && aad.append(workspace_id);
}

}  // namespace

// --- generation ------------------------------------------------------------

Status generate_recovery_secret(RecoverySecret& out) {
    out.material.reset();
    static_assert(kRecoverySecretBytes == crypto::kContentKeyBytes,
                  "the recovery secret and a ContentKey must be the same width for "
                  "generate_content_key() to be the right generator");
    return crypto::generate_content_key(out.material);
}

Status derive_user_root_key(const RecoverySecret& secret, UserRootKey& out) {
    out.material.reset();
    if (!secret.valid()) { return Status::CryptoKeyInvalid; }

    // No info: the recovery-secret-to-URK edge has exactly one child. The
    // label alone is the domain separation, which is why an unlabelled
    // derivation is not reachable through crypto::derive_subkey().
    return crypto::derive_subkey(secret.material, label_view(kLabelUserRootKey), crypto::ByteView{},
                                 out.material);
}

Status generate_workspace_key(WorkspaceKey& out) {
    out.material.reset();
    return crypto::generate_content_key(out.material);
}

// --- URK <-> device bundles ------------------------------------------------

Status wrap_urk_for_device(const crypto::DeviceKeyPair& wrapper,
                           const crypto::PublicKey& recipient_public, const UserRootKey& urk,
                           DeviceWrappedUrkRecord& out) {
    out = DeviceWrappedUrkRecord{};

    if (!urk.valid()) { return Status::CryptoKeyInvalid; }
    if (!wrapper.valid()) { return Status::CryptoKeyInvalid; }

    const Status s = crypto::wrap_content_key(wrapper, recipient_public, urk.material, out.wrapped);
    if (!is_ok(s)) {
        out = DeviceWrappedUrkRecord{};
        return s;
    }

    out.wrapper_public   = wrapper.public_key;
    out.recipient_public = recipient_public;
    return Status::Ok;
}

Status unwrap_urk_from_device(const crypto::DeviceKeyPair&  recipient,
                              const crypto::PublicKey&      expected_wrapper_public,
                              const DeviceWrappedUrkRecord& record, UserRootKey& out) {
    out.material.reset();

    const Status header = check_header(record.format_version, record.role, record.wrap_kind,
                                       KeyRole::UserRoot, WrapKind::DeviceWrappingKey);
    if (!is_ok(header)) { return header; }

    if (!recipient.valid()) { return Status::CryptoKeyInvalid; }

    // Both of these are cross-checks that save work rather than provide
    // security: the KEK below is derived from `expected_wrapper_public` and
    // `recipient`'s own keypair, and the primitive binds both public keys
    // into its AAD, so a record that disagrees with either would fail to
    // authenticate anyway. Reporting the SAME CryptoAuthFailed keeps that
    // true from the caller's side as well — a record for another device must
    // not be distinguishable from a forged one.
    if (!same_public_key(record.wrapper_public, expected_wrapper_public)) {
        return Status::CryptoAuthFailed;
    }
    if (!same_public_key(record.recipient_public, recipient.public_key)) {
        return Status::CryptoAuthFailed;
    }

    return crypto::unwrap_content_key(recipient, expected_wrapper_public, record.wrapped,
                                      out.material);
}

// --- URK under a recovery passphrase --------------------------------------

Status wrap_urk_under_passphrase(crypto::ByteView passphrase, crypto::KdfProfile profile,
                                 const UserRootKey& urk, PassphraseWrappedUrkRecord& out) {
    out = PassphraseWrappedUrkRecord{};

    if (!urk.valid()) { return Status::CryptoKeyInvalid; }

    // A FRESH salt per record, always. Reusing one across two users would
    // make their derived keys comparable (crypto.h), and reusing one across
    // two records for the same user would make an attacker's Argon2id work
    // pay twice.
    const Status salted = crypto::generate_salt(out.salt);
    if (!is_ok(salted)) {
        out = PassphraseWrappedUrkRecord{};
        return salted;
    }
    out.kdf_profile = profile;

    crypto::ContentKey wrap_key;
    const Status       derived =
        crypto::derive_key_from_passphrase(passphrase, out.salt, profile, wrap_key);
    if (!is_ok(derived)) {
        out = PassphraseWrappedUrkRecord{};
        return derived;
    }

    Aad aad;
    if (!build_passphrase_aad(out, aad)) {
        out = PassphraseWrappedUrkRecord{};
        return Status::Internal;
    }

    crypto::SealedBlob sealed;
    const Status       s = crypto::seal(
        wrap_key, crypto::ByteView{urk.material.bytes(), crypto::kContentKeyBytes}, aad.view(),
        sealed);
    if (!is_ok(s)) {
        out = PassphraseWrappedUrkRecord{};
        return s;
    }

    out.nonce            = sealed.nonce;
    const Status copied  = copy_wrapped(sealed, out.ciphertext);
    if (!is_ok(copied)) { out = PassphraseWrappedUrkRecord{}; }
    return copied;
}

Status unwrap_urk_with_passphrase(crypto::ByteView                  passphrase,
                                  const PassphraseWrappedUrkRecord& record, UserRootKey& out) {
    out.material.reset();

    const Status header = check_header(record.format_version, record.role, record.wrap_kind,
                                       KeyRole::UserRoot, WrapKind::RecoveryPassphrase);
    if (!is_ok(header)) { return header; }

    // The salt and the profile come from the record and are both in the AAD
    // below, so an edited salt or a downgraded profile is a CryptoAuthFailed
    // rather than a silently different derivation. Note the ORDER: Argon2id
    // runs before the tag is checked, so a wrong passphrase costs the full
    // KDF. That is inherent to this construction and is also what makes the
    // guess expensive for an attacker.
    crypto::ContentKey wrap_key;
    const Status       derived = crypto::derive_key_from_passphrase(passphrase, record.salt,
                                                                    record.kdf_profile, wrap_key);
    if (!is_ok(derived)) { return derived; }

    Aad aad;
    if (!build_passphrase_aad(record, aad)) { return Status::Internal; }

    const crypto::SealedBlob blob = to_blob(record.nonce, record.ciphertext);

    crypto::SecureBytes plaintext;
    const Status        s = crypto::open(wrap_key, blob, aad.view(), plaintext);
    if (!is_ok(s)) { return s; }
    if (plaintext.size() != crypto::kContentKeyBytes) { return Status::Internal; }

    const Status imported = out.material.import(plaintext.data(), plaintext.size());
    if (!is_ok(imported)) { out.material.reset(); }
    return imported;
}

// --- workspace keys under the URK -----------------------------------------

Status wrap_workspace_key(const UserRootKey& urk, crypto::ByteView workspace_id,
                          const WorkspaceKey& wsk, UrkWrappedWorkspaceKeyRecord& out) {
    out = UrkWrappedWorkspaceKeyRecord{};

    const Status id = check_scope_id(workspace_id);
    if (!is_ok(id)) { return id; }
    if (!urk.valid() || !wsk.valid()) { return Status::CryptoKeyInvalid; }

    // The wrapping key is derived per workspace, not shared across them: two
    // workspaces of the same user are wrapped under different keys even
    // though both come from one URK.
    crypto::ContentKey wrap_key;
    const Status       derived = crypto::derive_subkey(
        urk.material, label_view(kLabelWorkspaceKeyWrap), workspace_id, wrap_key);
    if (!is_ok(derived)) { return derived; }

    Aad aad;
    if (!build_workspace_aad(out, workspace_id, aad)) { return Status::Internal; }

    crypto::SealedBlob sealed;
    const Status       s =
        crypto::seal(wrap_key, crypto::ByteView{wsk.material.bytes(), crypto::kContentKeyBytes},
                     aad.view(), sealed);
    if (!is_ok(s)) {
        out = UrkWrappedWorkspaceKeyRecord{};
        return s;
    }

    out.nonce           = sealed.nonce;
    const Status copied = copy_wrapped(sealed, out.ciphertext);
    if (!is_ok(copied)) { out = UrkWrappedWorkspaceKeyRecord{}; }
    return copied;
}

Status unwrap_workspace_key(const UserRootKey& urk, crypto::ByteView workspace_id,
                            const UrkWrappedWorkspaceKeyRecord& record, WorkspaceKey& out) {
    out.material.reset();

    const Status header = check_header(record.format_version, record.role, record.wrap_kind,
                                       KeyRole::Workspace, WrapKind::UserRootKey);
    if (!is_ok(header)) { return header; }

    const Status id = check_scope_id(workspace_id);
    if (!is_ok(id)) { return id; }
    if (!urk.valid()) { return Status::CryptoKeyInvalid; }

    crypto::ContentKey wrap_key;
    const Status       derived = crypto::derive_subkey(
        urk.material, label_view(kLabelWorkspaceKeyWrap), workspace_id, wrap_key);
    if (!is_ok(derived)) { return derived; }

    Aad aad;
    if (!build_workspace_aad(record, workspace_id, aad)) { return Status::Internal; }

    const crypto::SealedBlob blob = to_blob(record.nonce, record.ciphertext);

    crypto::SecureBytes plaintext;
    const Status        s = crypto::open(wrap_key, blob, aad.view(), plaintext);
    if (!is_ok(s)) { return s; }
    if (plaintext.size() != crypto::kContentKeyBytes) { return Status::Internal; }

    const Status imported = out.material.import(plaintext.data(), plaintext.size());
    if (!is_ok(imported)) { out.material.reset(); }
    return imported;
}

// --- document keys ---------------------------------------------------------

Status derive_document_key(const WorkspaceKey& wsk, crypto::ByteView document_id,
                           DocumentKey& out) {
    out.material.reset();

    const Status id = check_scope_id(document_id);
    if (!is_ok(id)) { return id; }
    if (!wsk.valid()) { return Status::CryptoKeyInvalid; }

    // This is the whole of "per-document keys are derived, not stored"
    // (§5A.1). There is no record, no row and no wrap — which is why there is
    // no KeyRole::Document.
    return crypto::derive_subkey(wsk.material, label_view(kLabelDocumentKey), document_id,
                                 out.material);
}

// --- content ---------------------------------------------------------------
namespace {

[[nodiscard]] bool build_content_aad(crypto::ByteView document_id, Aad& aad) {
    return aad.append(label_view(kLabelDocumentContent)) && aad.append(document_id);
}

}  // namespace

Status seal_document_content(const DocumentKey& key, crypto::ByteView document_id,
                             crypto::ByteView plaintext, crypto::SealedBlob& out) {
    out.nonce.fill(0);
    out.ciphertext.clear();

    const Status id = check_scope_id(document_id);
    if (!is_ok(id)) { return id; }
    if (!key.valid()) { return Status::CryptoKeyInvalid; }

    Aad aad;
    if (!build_content_aad(document_id, aad)) { return Status::Internal; }

    return crypto::seal(key.material, plaintext, aad.view(), out);
}

Status open_document_content(const DocumentKey& key, crypto::ByteView document_id,
                             const crypto::SealedBlob& blob, crypto::SecureBytes& out) {
    out.reset();

    const Status id = check_scope_id(document_id);
    if (!is_ok(id)) { return id; }
    if (!key.valid()) { return Status::CryptoKeyInvalid; }

    Aad aad;
    if (!build_content_aad(document_id, aad)) { return Status::Internal; }

    // crypto::open() resets `out` on every failure path; this layer adds no
    // second opinion about that, and the test suite asserts out.empty() for
    // each failure mode here as well as there.
    return crypto::open(key.material, blob, aad.view(), out);
}

}  // namespace uapp::keys
