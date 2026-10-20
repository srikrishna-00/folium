// SPDX-License-Identifier: Apache-2.0
//
// tests/keys/key_hierarchy_test.cpp — security/keys.
//
// WHAT THIS SUITE IS FOR, and how it differs from tests/crypto.
//
// tests/crypto proves the PRIMITIVES against external published vectors,
// because a round trip cannot tell XChaCha20-Poly1305 from any other pair of
// mutually-inverse functions. That argument does not transfer here: this
// module implements no primitive, so there is no published vector for it to
// disagree with. What can go wrong one layer up is different in kind, and
// these are the four failure classes the cases below are built around:
//
//   1. **A wrong EDGE.** Deriving the document key from the URK instead of
//      the workspace key, or wrapping a WSK under the wrong label. Caught by
//      asserting that every derived key is distinct from every other key
//      reachable from the same parent, not merely that round trips work.
//   2. **A MISSING BINDING.** An id that is accepted on the way in but not
//      bound into the AAD, so a record or a ciphertext can be replayed into
//      another workspace or another document. Caught by moving each
//      ciphertext to a neighbouring id and requiring CryptoAuthFailed.
//   3. **DETERMINISM LOST.** The whole scheme-(a) bet in §5A.1 is that a
//      document key is a pure function of (WSK, document_id), so a second
//      device recomputes it with no exchange. Caught by deriving the same
//      key twice, and by driving a SECOND simulated device through the real
//      enrolment-shaped path and having it read the first device's content.
//   4. **A FAILURE THAT LEAKS.** Every rejection must return
//      CryptoAuthFailed and leave the out-parameter invalid/empty. Caught by
//      asserting the out-parameter's state after every single failure, not
//      just the status code.
//
// The simulated multi-device test is the one that would catch a real design
// error rather than a typo: it builds two device keypairs, wraps the URK from
// device A to device B exactly as §5A.2 mechanism (1) would after the
// short-code check, and then has B derive the same document key and read A's
// ciphertext. If any key in the chain were per-device, per-process or
// nondeterministic, that case fails and the round-trip cases do not.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "test_framework.h"
#include "uapp/crypto/crypto.h"
#include "uapp/crypto/secure_bytes.h"
#include "uapp/keys/key_hierarchy.h"
#include "uapp/status.h"

namespace {

using uapp::Status;
using uapp::crypto::ByteView;
using uapp::crypto::DeviceKeyPair;
using uapp::crypto::KdfProfile;
using uapp::crypto::kContentKeyBytes;
using uapp::crypto::SealedBlob;
using uapp::crypto::secure_equal;
using uapp::crypto::SecureBytes;
using uapp::keys::DeviceWrappedUrkRecord;
using uapp::keys::DocumentKey;
using uapp::keys::KeyRole;
using uapp::keys::kRecordFormatVersion;
using uapp::keys::PassphraseWrappedUrkRecord;
using uapp::keys::RecoverySecret;
using uapp::keys::UrkWrappedWorkspaceKeyRecord;
using uapp::keys::UserRootKey;
using uapp::keys::WorkspaceKey;
using uapp::keys::WrapKind;

[[nodiscard]] ByteView view(const std::string& s) {
    return ByteView{reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

/// A domain-separation label as the module itself passes it: the literal
/// without its NUL.
template <std::size_t N>
[[nodiscard]] ByteView label(const char (&literal)[N]) {
    return ByteView{reinterpret_cast<const std::uint8_t*>(literal), N - 1};
}

/// Constant-time, because one side is always a key. Same reason tests/crypto
/// uses it rather than std::memcmp.
[[nodiscard]] bool same_key(const uapp::crypto::ContentKey& a,
                            const uapp::crypto::ContentKey& b) {
    if (!a.valid() || !b.valid()) { return false; }
    return secure_equal(a.bytes(), b.bytes(), kContentKeyBytes);
}

[[nodiscard]] std::string to_string(const SecureBytes& b) {
    if (b.empty()) { return std::string(); }
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

/// Every case calls this first. crypto::init() is documented as idempotent
/// and thread-safe; a suite that assumed an earlier case had run would pass
/// or fail depending on the filter.
[[nodiscard]] bool crypto_ready() { return uapp::is_ok(uapp::crypto::init()); }

}  // namespace

// ===========================================================================
// keys.root — the recovery secret and the URK (§5A.1 top two rows)
// ===========================================================================

UAPP_TEST("keys.root derives the URK from the recovery secret, deterministically") {
    UAPP_REQUIRE(crypto_ready());

    RecoverySecret secret;
    UAPP_REQUIRE_OK(uapp::keys::generate_recovery_secret(secret));
    UAPP_REQUIRE(secret.valid());

    UserRootKey urk_a;
    UserRootKey urk_b;
    UAPP_REQUIRE_OK(uapp::keys::derive_user_root_key(secret, urk_a));
    UAPP_REQUIRE_OK(uapp::keys::derive_user_root_key(secret, urk_b));
    UAPP_REQUIRE(urk_a.valid() && urk_b.valid());

    // This is §5A.2 mechanism (2)'s entire premise: the 24 words are enough,
    // on a device that has never seen any other device, with nothing stored.
    UAPP_CHECK_MSG(same_key(urk_a.material, urk_b.material),
                   "the URK must be a pure function of the recovery secret");

    // And the URK must not BE the recovery secret. If derive_subkey were ever
    // short-circuited to a copy, every other case here would still pass.
    UAPP_CHECK_MSG(!same_key(urk_a.material, secret.material),
                   "the URK must be derived from the recovery secret, not equal to it");
}

UAPP_TEST("keys.root gives different recovery secrets different URKs") {
    UAPP_REQUIRE(crypto_ready());

    std::set<std::string> seen;
    for (int i = 0; i < 16; ++i) {
        RecoverySecret secret;
        UAPP_REQUIRE_OK(uapp::keys::generate_recovery_secret(secret));

        UserRootKey urk;
        UAPP_REQUIRE_OK(uapp::keys::derive_user_root_key(secret, urk));

        seen.insert(std::string(reinterpret_cast<const char*>(urk.material.bytes()),
                                kContentKeyBytes));
    }
    UAPP_CHECK_MSG(seen.size() == 16u, "16 recovery secrets produced a collision in the URK");
}

UAPP_TEST("keys.root rejects an invalid recovery secret rather than deriving from nothing") {
    UAPP_REQUIRE(crypto_ready());

    const RecoverySecret empty;  // default-constructed: no storage
    UserRootKey          urk;
    UAPP_REQUIRE_STATUS(uapp::keys::derive_user_root_key(empty, urk),
                        Status::CryptoKeyInvalid);
    UAPP_CHECK(!urk.valid());
}

// ===========================================================================
// keys.device — the URK wrapped between two of the user's devices (§5A.1
// "DEVICE KEY BUNDLE", produced by §5A.2 mechanism 1)
// ===========================================================================

UAPP_TEST("keys.device round-trips the URK between two simulated devices") {
    UAPP_REQUIRE(crypto_ready());

    DeviceKeyPair laptop;
    DeviceKeyPair phone;
    UAPP_REQUIRE_OK(uapp::crypto::generate_device_keypair(laptop));
    UAPP_REQUIRE_OK(uapp::crypto::generate_device_keypair(phone));

    RecoverySecret secret;
    UserRootKey    urk;
    UAPP_REQUIRE_OK(uapp::keys::generate_recovery_secret(secret));
    UAPP_REQUIRE_OK(uapp::keys::derive_user_root_key(secret, urk));

    DeviceWrappedUrkRecord bundle;
    UAPP_REQUIRE_OK(uapp::keys::wrap_urk_for_device(laptop, phone.public_key, urk, bundle));

    // The record is self-describing for a storage layer, and says who it is
    // for without being decrypted.
    UAPP_CHECK(bundle.format_version == kRecordFormatVersion);
    UAPP_CHECK(bundle.role == KeyRole::UserRoot);
    UAPP_CHECK(bundle.wrap_kind == WrapKind::DeviceWrappingKey);
    UAPP_CHECK(bundle.wrapper_public == laptop.public_key);
    UAPP_CHECK(bundle.recipient_public == phone.public_key);

    UserRootKey recovered;
    UAPP_REQUIRE_OK(
        uapp::keys::unwrap_urk_from_device(phone, laptop.public_key, bundle, recovered));
    UAPP_CHECK_MSG(same_key(recovered.material, urk.material),
                   "the phone must recover the laptop's URK byte for byte");
}

UAPP_TEST("keys.device refuses a third device, a wrong sender and a tampered bundle") {
    UAPP_REQUIRE(crypto_ready());

    DeviceKeyPair laptop;
    DeviceKeyPair phone;
    DeviceKeyPair attacker;
    UAPP_REQUIRE_OK(uapp::crypto::generate_device_keypair(laptop));
    UAPP_REQUIRE_OK(uapp::crypto::generate_device_keypair(phone));
    UAPP_REQUIRE_OK(uapp::crypto::generate_device_keypair(attacker));

    RecoverySecret secret;
    UserRootKey    urk;
    UAPP_REQUIRE_OK(uapp::keys::generate_recovery_secret(secret));
    UAPP_REQUIRE_OK(uapp::keys::derive_user_root_key(secret, urk));

    DeviceWrappedUrkRecord bundle;
    UAPP_REQUIRE_OK(uapp::keys::wrap_urk_for_device(laptop, phone.public_key, urk, bundle));

    // (1) A device the bundle was not wrapped to. T-310's "revoked device
    //     retains access" starts here: a device not in the recipient field
    //     must not be able to open a bundle even with the whole record.
    {
        UserRootKey out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::unwrap_urk_from_device(attacker, laptop.public_key, bundle, out),
            Status::CryptoAuthFailed);
        UAPP_CHECK(!out.valid());
    }

    // (2) The right device, told the wrong sender. This is the authenticated
    //     half of the crypto_kx decision (§5A.2 RESOLVED note): the recipient
    //     learns WHICH device sealed the bundle, and a mismatch is a failure
    //     rather than a shrug.
    {
        UserRootKey out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::unwrap_urk_from_device(phone, attacker.public_key, bundle, out),
            Status::CryptoAuthFailed);
        UAPP_CHECK(!out.valid());
    }

    // (3) An attacker who re-labels the bundle as having come from itself.
    //     The KEK is derived from the CALLER's expected sender, so re-writing
    //     the record's field cannot make it unwrap — it only makes the
    //     cross-check fail first.
    {
        DeviceWrappedUrkRecord forged = bundle;
        forged.wrapper_public         = attacker.public_key;

        UserRootKey out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::unwrap_urk_from_device(phone, laptop.public_key, forged, out),
            Status::CryptoAuthFailed);
        UAPP_CHECK(!out.valid());

        // ... and told to expect the attacker, it still fails, because the
        // attacker never held the laptop's secret key.
        UserRootKey out2;
        UAPP_REQUIRE_STATUS(
            uapp::keys::unwrap_urk_from_device(phone, attacker.public_key, forged, out2),
            Status::CryptoAuthFailed);
        UAPP_CHECK(!out2.valid());
    }

    // (4) One bit flipped anywhere in the wrapped key or its nonce.
    for (std::size_t i = 0; i < bundle.wrapped.ciphertext.size(); ++i) {
        DeviceWrappedUrkRecord tampered = bundle;
        tampered.wrapped.ciphertext[i] ^= 0x01u;

        UserRootKey out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::unwrap_urk_from_device(phone, laptop.public_key, tampered, out),
            Status::CryptoAuthFailed);
        UAPP_REQUIRE(!out.valid());
    }
    {
        DeviceWrappedUrkRecord tampered = bundle;
        tampered.wrapped.nonce[0] ^= 0x80u;

        UserRootKey out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::unwrap_urk_from_device(phone, laptop.public_key, tampered, out),
            Status::CryptoAuthFailed);
        UAPP_CHECK(!out.valid());
    }
}

UAPP_TEST("keys.device checks the record header before touching key material") {
    UAPP_REQUIRE(crypto_ready());

    DeviceKeyPair laptop;
    DeviceKeyPair phone;
    UAPP_REQUIRE_OK(uapp::crypto::generate_device_keypair(laptop));
    UAPP_REQUIRE_OK(uapp::crypto::generate_device_keypair(phone));

    RecoverySecret secret;
    UserRootKey    urk;
    UAPP_REQUIRE_OK(uapp::keys::generate_recovery_secret(secret));
    UAPP_REQUIRE_OK(uapp::keys::derive_user_root_key(secret, urk));

    DeviceWrappedUrkRecord bundle;
    UAPP_REQUIRE_OK(uapp::keys::wrap_urk_for_device(laptop, phone.public_key, urk, bundle));

    // A version this build does not know is refused, never parsed hopefully.
    {
        DeviceWrappedUrkRecord future = bundle;
        future.format_version         = static_cast<std::uint8_t>(kRecordFormatVersion + 1);

        UserRootKey out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::unwrap_urk_from_device(phone, laptop.public_key, future, out),
            Status::KeyRecordVersionUnsupported);
        UAPP_CHECK(!out.valid());
    }

    // A record relabelled as holding a workspace key is refused rather than
    // unwrapped-and-believed. This is the role-confusion defence that makes
    // the device record's unauthenticated header safe (see key_hierarchy.h,
    // "THE RECORD FORMAT").
    {
        DeviceWrappedUrkRecord relabelled = bundle;
        relabelled.role                   = KeyRole::Workspace;

        UserRootKey out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::unwrap_urk_from_device(phone, laptop.public_key, relabelled, out),
            Status::KeyRecordRoleMismatch);
        UAPP_CHECK(!out.valid());
    }
    {
        DeviceWrappedUrkRecord relabelled = bundle;
        relabelled.wrap_kind              = WrapKind::RecoveryPassphrase;

        UserRootKey out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::unwrap_urk_from_device(phone, laptop.public_key, relabelled, out),
            Status::KeyRecordRoleMismatch);
        UAPP_CHECK(!out.valid());
    }
}

// ===========================================================================
// keys.recovery — the URK under an Argon2id-derived key (§5A.4; FLAGGED in
// key_hierarchy.h as an ADDITIONAL path, not the root of trust)
// ===========================================================================

UAPP_TEST("keys.recovery round-trips the URK under a passphrase") {
    UAPP_REQUIRE(crypto_ready());

    RecoverySecret secret;
    UserRootKey    urk;
    UAPP_REQUIRE_OK(uapp::keys::generate_recovery_secret(secret));
    UAPP_REQUIRE_OK(uapp::keys::derive_user_root_key(secret, urk));

    const std::string passphrase = "correct horse battery staple";

    PassphraseWrappedUrkRecord record;
    UAPP_REQUIRE_OK(uapp::keys::wrap_urk_under_passphrase(
        view(passphrase), KdfProfile::MobileInteractive, urk, record));

    UAPP_CHECK(record.format_version == kRecordFormatVersion);
    UAPP_CHECK(record.role == KeyRole::UserRoot);
    UAPP_CHECK(record.wrap_kind == WrapKind::RecoveryPassphrase);
    // The profile is part of the derived key's identity and MUST be stored
    // with the salt (crypto.h). A record that did not carry it would be
    // undecryptable the moment the default changed.
    UAPP_CHECK(record.kdf_profile == KdfProfile::MobileInteractive);
    UAPP_CHECK_MSG(!std::all_of(record.salt.begin(), record.salt.end(),
                                [](std::uint8_t b) { return b == 0u; }),
                   "a stored salt of all zeroes means generate_salt() did not run");

    UserRootKey recovered;
    UAPP_REQUIRE_OK(
        uapp::keys::unwrap_urk_with_passphrase(view(passphrase), record, recovered));
    UAPP_CHECK(same_key(recovered.material, urk.material));

    // Two wraps of the same URK under the same passphrase must differ: fresh
    // salt, fresh nonce. Identical records would mean one of the two was
    // being reused.
    PassphraseWrappedUrkRecord again;
    UAPP_REQUIRE_OK(uapp::keys::wrap_urk_under_passphrase(
        view(passphrase), KdfProfile::MobileInteractive, urk, again));
    UAPP_CHECK(again.salt != record.salt);
    UAPP_CHECK(again.ciphertext != record.ciphertext);
}

UAPP_TEST("keys.recovery fails cleanly on a wrong passphrase and a tampered record") {
    UAPP_REQUIRE(crypto_ready());

    RecoverySecret secret;
    UserRootKey    urk;
    UAPP_REQUIRE_OK(uapp::keys::generate_recovery_secret(secret));
    UAPP_REQUIRE_OK(uapp::keys::derive_user_root_key(secret, urk));

    const std::string passphrase = "correct horse battery staple";

    PassphraseWrappedUrkRecord record;
    UAPP_REQUIRE_OK(uapp::keys::wrap_urk_under_passphrase(
        view(passphrase), KdfProfile::MobileInteractive, urk, record));

    // (1) A wrong passphrase — including one differing in a single character
    //     — is CryptoAuthFailed and yields no key. Not a distinct code: a
    //     "wrong passphrase" that were distinguishable from "tampered
    //     record" would tell an attacker which of the two they had.
    for (const std::string& wrong : {std::string("correct horse battery stapl"),
                                     std::string("correct horse battery staplf"),
                                     std::string("Correct horse battery staple"),
                                     std::string("completely different")}) {
        UserRootKey out;
        UAPP_REQUIRE_STATUS(uapp::keys::unwrap_urk_with_passphrase(view(wrong), record, out),
                            Status::CryptoAuthFailed);
        UAPP_REQUIRE_MSG(!out.valid(), "a failed unwrap must leave no key behind");
    }

    // (2) A passphrase below the floor is rejected by the KDF before any
    //     AEAD work, with its own code — it is an API misuse, not an attack.
    {
        UserRootKey out;
        UAPP_REQUIRE_STATUS(uapp::keys::unwrap_urk_with_passphrase(view(std::string("short")),
                                                                   record, out),
                            Status::CryptoPassphraseTooShort);
        UAPP_CHECK(!out.valid());
    }

    // (3) The salt is in the AAD, so editing it is an auth failure rather
    //     than a silently different derivation.
    {
        PassphraseWrappedUrkRecord tampered = record;
        tampered.salt[0] ^= 0x01u;

        UserRootKey out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::unwrap_urk_with_passphrase(view(passphrase), tampered, out),
            Status::CryptoAuthFailed);
        UAPP_CHECK(!out.valid());
    }

    // (4) So is the KDF profile. A downgrade attack — rewriting the record to
    //     claim a cheaper profile — must not even reach the tag check with a
    //     usable key.
    {
        PassphraseWrappedUrkRecord downgraded = record;
        downgraded.kdf_profile                = KdfProfile::DesktopModerate;

        UserRootKey out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::unwrap_urk_with_passphrase(view(passphrase), downgraded, out),
            Status::CryptoAuthFailed);
        UAPP_CHECK(!out.valid());
    }

    // (5) Every single byte of the wrapped key, and the nonce.
    for (std::size_t i = 0; i < record.ciphertext.size(); ++i) {
        PassphraseWrappedUrkRecord tampered = record;
        tampered.ciphertext[i] ^= 0x01u;

        UserRootKey out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::unwrap_urk_with_passphrase(view(passphrase), tampered, out),
            Status::CryptoAuthFailed);
        UAPP_REQUIRE(!out.valid());
    }
    {
        PassphraseWrappedUrkRecord tampered = record;
        tampered.nonce[1] ^= 0x40u;

        UserRootKey out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::unwrap_urk_with_passphrase(view(passphrase), tampered, out),
            Status::CryptoAuthFailed);
        UAPP_CHECK(!out.valid());
    }

    // (6) Header checks come first, over plaintext metadata.
    {
        PassphraseWrappedUrkRecord future = record;
        future.format_version             = 99u;

        UserRootKey out;
        UAPP_REQUIRE_STATUS(uapp::keys::unwrap_urk_with_passphrase(view(passphrase), future, out),
                            Status::KeyRecordVersionUnsupported);
        UAPP_CHECK(!out.valid());
    }
}

// ===========================================================================
// keys.workspace — the WSK wrapped under the URK (§5A.1 "URK wraps WSK";
// ONE record per workspace, not per device and not per document)
// ===========================================================================

UAPP_TEST("keys.workspace round-trips a workspace key under the URK") {
    UAPP_REQUIRE(crypto_ready());

    RecoverySecret secret;
    UserRootKey    urk;
    UAPP_REQUIRE_OK(uapp::keys::generate_recovery_secret(secret));
    UAPP_REQUIRE_OK(uapp::keys::derive_user_root_key(secret, urk));

    WorkspaceKey wsk;
    UAPP_REQUIRE_OK(uapp::keys::generate_workspace_key(wsk));
    UAPP_CHECK_MSG(!same_key(wsk.material, urk.material),
                   "the WSK is generated, not derived from the URK (§5A.3 rotates it)");

    const std::string workspace = "ws_7f3c1a";

    UrkWrappedWorkspaceKeyRecord record;
    UAPP_REQUIRE_OK(uapp::keys::wrap_workspace_key(urk, view(workspace), wsk, record));
    UAPP_CHECK(record.role == KeyRole::Workspace);
    UAPP_CHECK(record.wrap_kind == WrapKind::UserRootKey);

    WorkspaceKey recovered;
    UAPP_REQUIRE_OK(
        uapp::keys::unwrap_workspace_key(urk, view(workspace), record, recovered));
    UAPP_CHECK(same_key(recovered.material, wsk.material));

    // The record is device-independent: that is what makes §5A.2 mechanism
    // (2) work. A URK re-derived from the recovery secret on a brand-new
    // device opens it with no bundle and no other device present.
    UserRootKey rederived;
    UAPP_REQUIRE_OK(uapp::keys::derive_user_root_key(secret, rederived));
    WorkspaceKey from_phrase;
    UAPP_REQUIRE_OK(
        uapp::keys::unwrap_workspace_key(rederived, view(workspace), record, from_phrase));
    UAPP_CHECK(same_key(from_phrase.material, wsk.material));
}

UAPP_TEST("keys.workspace binds the workspace id and rejects a wrong URK") {
    UAPP_REQUIRE(crypto_ready());

    RecoverySecret secret_a;
    RecoverySecret secret_b;
    UserRootKey    urk_a;
    UserRootKey    urk_b;
    UAPP_REQUIRE_OK(uapp::keys::generate_recovery_secret(secret_a));
    UAPP_REQUIRE_OK(uapp::keys::generate_recovery_secret(secret_b));
    UAPP_REQUIRE_OK(uapp::keys::derive_user_root_key(secret_a, urk_a));
    UAPP_REQUIRE_OK(uapp::keys::derive_user_root_key(secret_b, urk_b));

    WorkspaceKey wsk;
    UAPP_REQUIRE_OK(uapp::keys::generate_workspace_key(wsk));

    const std::string workspace = "ws_7f3c1a";
    const std::string other     = "ws_7f3c1b";  // one character apart, on purpose

    UrkWrappedWorkspaceKeyRecord record;
    UAPP_REQUIRE_OK(uapp::keys::wrap_workspace_key(urk_a, view(workspace), wsk, record));

    // Another user's URK.
    {
        WorkspaceKey out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::unwrap_workspace_key(urk_b, view(workspace), record, out),
            Status::CryptoAuthFailed);
        UAPP_CHECK(!out.valid());
    }

    // The right URK, the wrong workspace: the id is in both the derivation
    // and the AAD, so a record cannot be moved between workspaces.
    {
        WorkspaceKey out;
        UAPP_REQUIRE_STATUS(uapp::keys::unwrap_workspace_key(urk_a, view(other), record, out),
                            Status::CryptoAuthFailed);
        UAPP_CHECK(!out.valid());
    }

    // Tampering, byte by byte.
    for (std::size_t i = 0; i < record.ciphertext.size(); ++i) {
        UrkWrappedWorkspaceKeyRecord tampered = record;
        tampered.ciphertext[i] ^= 0x01u;

        WorkspaceKey out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::unwrap_workspace_key(urk_a, view(workspace), tampered, out),
            Status::CryptoAuthFailed);
        UAPP_REQUIRE(!out.valid());
    }

    // An empty or absent id is refused on both sides rather than treated as a
    // default. An accepted empty id would give every workspace one key.
    {
        UrkWrappedWorkspaceKeyRecord out_record;
        UAPP_REQUIRE_STATUS(
            uapp::keys::wrap_workspace_key(urk_a, ByteView{}, wsk, out_record),
            Status::KeyScopeIdInvalid);

        WorkspaceKey out;
        UAPP_REQUIRE_STATUS(uapp::keys::unwrap_workspace_key(urk_a, ByteView{}, record, out),
                            Status::KeyScopeIdInvalid);
        UAPP_CHECK(!out.valid());
    }

    // Two workspaces of the SAME user are wrapped under different keys, so a
    // leaked wrap key is scoped to one workspace.
    {
        UrkWrappedWorkspaceKeyRecord second;
        UAPP_REQUIRE_OK(uapp::keys::wrap_workspace_key(urk_a, view(other), wsk, second));

        WorkspaceKey out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::unwrap_workspace_key(urk_a, view(workspace), second, out),
            Status::CryptoAuthFailed);
        UAPP_CHECK(!out.valid());
    }
}

// ===========================================================================
// keys.document — derived, never stored (§5A.1, scheme (a))
// ===========================================================================

UAPP_TEST("keys.document derives deterministically and distinctly, with no record") {
    UAPP_REQUIRE(crypto_ready());

    WorkspaceKey wsk;
    UAPP_REQUIRE_OK(uapp::keys::generate_workspace_key(wsk));

    const std::string doc_a = "page_0001";
    const std::string doc_b = "page_0002";

    DocumentKey k1;
    DocumentKey k2;
    UAPP_REQUIRE_OK(uapp::keys::derive_document_key(wsk, view(doc_a), k1));
    UAPP_REQUIRE_OK(uapp::keys::derive_document_key(wsk, view(doc_a), k2));
    UAPP_CHECK_MSG(same_key(k1.material, k2.material),
                   "a document key must be a pure function of (WSK, document_id)");

    DocumentKey other;
    UAPP_REQUIRE_OK(uapp::keys::derive_document_key(wsk, view(doc_b), other));
    UAPP_CHECK_MSG(!same_key(k1.material, other.material),
                   "two documents in one workspace must not share a key");

    // The document key must not be the workspace key it came from.
    UAPP_CHECK(!same_key(k1.material, wsk.material));

    // THE EDGE-CONFUSION CHECK, and the sharpest case in this file. Two edges
    // of the hierarchy run from the same parent with the same info and differ
    // ONLY by their label: the workspace-key-wrap edge and the document-key
    // edge. Asserting here that (a) the document key IS
    // HKDF(wsk, kLabelDocumentKey, id) and (b) the wrap key under the other
    // label is a different 32 bytes pins the labels as part of the stored
    // format. If the two labels were ever unified, or one were dropped, this
    // fails and every round trip above still passes.
    {
        uapp::crypto::ContentKey doc_edge;
        uapp::crypto::ContentKey wrap_edge;
        UAPP_REQUIRE_OK(uapp::crypto::derive_subkey(
            wsk.material, label(uapp::keys::kLabelDocumentKey), view(doc_a), doc_edge));
        UAPP_REQUIRE_OK(uapp::crypto::derive_subkey(
            wsk.material, label(uapp::keys::kLabelWorkspaceKeyWrap), view(doc_a), wrap_edge));

        UAPP_CHECK_MSG(same_key(doc_edge, k1.material),
                       "derive_document_key must be HKDF(WSK, kLabelDocumentKey, document_id)");
        UAPP_CHECK_MSG(!same_key(doc_edge, wrap_edge),
                       "two edges from one parent must not collide: the label IS the separation");
    }

    // A different workspace key gives a different document key for the same
    // id — the property that makes §5A.3's rotation actually rotate.
    {
        WorkspaceKey rotated;
        UAPP_REQUIRE_OK(uapp::keys::generate_workspace_key(rotated));

        DocumentKey after;
        UAPP_REQUIRE_OK(uapp::keys::derive_document_key(rotated, view(doc_a), after));
        UAPP_CHECK_MSG(!same_key(k1.material, after.material),
                       "rotating the WSK must rotate every document key under it");
    }

    // Ids are validated, not normalised.
    {
        DocumentKey out;
        UAPP_REQUIRE_STATUS(uapp::keys::derive_document_key(wsk, ByteView{}, out),
                            Status::KeyScopeIdInvalid);
        UAPP_CHECK(!out.valid());

        const std::vector<std::uint8_t> too_long(uapp::keys::kMaxScopeIdBytes + 1u, 0x41u);
        UAPP_REQUIRE_STATUS(
            uapp::keys::derive_document_key(wsk, ByteView{too_long.data(), too_long.size()}, out),
            Status::KeyScopeIdInvalid);
        UAPP_CHECK(!out.valid());
    }

    // An invalid WSK is refused rather than dereferenced.
    {
        const WorkspaceKey empty;
        DocumentKey        out;
        UAPP_REQUIRE_STATUS(uapp::keys::derive_document_key(empty, view(doc_a), out),
                            Status::CryptoKeyInvalid);
        UAPP_CHECK(!out.valid());
    }
}

// ===========================================================================
// keys.chain — THE WHOLE HIERARCHY, end to end, on real content
// ===========================================================================

UAPP_TEST("keys.chain carries real content from a recovery phrase to plaintext and back") {
    UAPP_REQUIRE(crypto_ready());

    // --- device A: first install -------------------------------------------
    DeviceKeyPair laptop;
    UAPP_REQUIRE_OK(uapp::crypto::generate_device_keypair(laptop));

    RecoverySecret secret;
    UAPP_REQUIRE_OK(uapp::keys::generate_recovery_secret(secret));

    UserRootKey urk;
    UAPP_REQUIRE_OK(uapp::keys::derive_user_root_key(secret, urk));

    WorkspaceKey wsk;
    UAPP_REQUIRE_OK(uapp::keys::generate_workspace_key(wsk));

    const std::string workspace   = "ws_7f3c1a";
    const std::string document    = "page_0001";
    const std::string content     = "# Quarterly plan\n\nThis block is asset A1.";

    // The two records that get stored (server-side, as opaque bytes).
    UrkWrappedWorkspaceKeyRecord workspace_record;
    UAPP_REQUIRE_OK(uapp::keys::wrap_workspace_key(urk, view(workspace), wsk, workspace_record));

    DocumentKey doc_key;
    UAPP_REQUIRE_OK(uapp::keys::derive_document_key(wsk, view(document), doc_key));

    SealedBlob sealed;
    UAPP_REQUIRE_OK(
        uapp::keys::seal_document_content(doc_key, view(document), view(content), sealed));
    UAPP_REQUIRE(sealed.ciphertext.size() == content.size() + uapp::crypto::kTagBytes);

    // The ciphertext must not contain the plaintext. A wrapper that forgot to
    // encrypt would pass every round-trip assertion in this file.
    {
        const std::string blob_bytes(reinterpret_cast<const char*>(sealed.ciphertext.data()),
                                     sealed.ciphertext.size());
        UAPP_CHECK_MSG(blob_bytes.find("Quarterly plan") == std::string::npos,
                       "plaintext is visible in the sealed blob");
    }

    // --- device A reads it back --------------------------------------------
    {
        SecureBytes plaintext;
        UAPP_REQUIRE_OK(
            uapp::keys::open_document_content(doc_key, view(document), sealed, plaintext));
        UAPP_CHECK(to_string(plaintext) == content);
    }

    // --- device B: enrolled by device A (§5A.2 mechanism 1) ----------------
    // B holds only its own keypair, the bundle A wrapped to it, and the
    // workspace record. It has never seen the WSK or the document key.
    {
        DeviceKeyPair phone;
        UAPP_REQUIRE_OK(uapp::crypto::generate_device_keypair(phone));

        DeviceWrappedUrkRecord bundle;
        UAPP_REQUIRE_OK(uapp::keys::wrap_urk_for_device(laptop, phone.public_key, urk, bundle));

        UserRootKey urk_on_phone;
        UAPP_REQUIRE_OK(
            uapp::keys::unwrap_urk_from_device(phone, laptop.public_key, bundle, urk_on_phone));

        WorkspaceKey wsk_on_phone;
        UAPP_REQUIRE_OK(uapp::keys::unwrap_workspace_key(urk_on_phone, view(workspace),
                                                          workspace_record, wsk_on_phone));

        DocumentKey doc_key_on_phone;
        UAPP_REQUIRE_OK(
            uapp::keys::derive_document_key(wsk_on_phone, view(document), doc_key_on_phone));
        UAPP_CHECK_MSG(same_key(doc_key_on_phone.material, doc_key.material),
                       "both devices must derive the same document key with no exchange");

        SecureBytes plaintext;
        UAPP_REQUIRE_OK(uapp::keys::open_document_content(doc_key_on_phone, view(document),
                                                          sealed, plaintext));
        UAPP_CHECK_MSG(to_string(plaintext) == content,
                       "device B must read device A's content through the full chain");
    }

    // --- device C: recovery phrase only (§5A.2 mechanism 2) ----------------
    // No enrolled device present, nothing but the 32 bytes behind the 24
    // words and the server-stored workspace record.
    {
        UserRootKey urk_from_phrase;
        UAPP_REQUIRE_OK(uapp::keys::derive_user_root_key(secret, urk_from_phrase));

        WorkspaceKey wsk_recovered;
        UAPP_REQUIRE_OK(uapp::keys::unwrap_workspace_key(urk_from_phrase, view(workspace),
                                                          workspace_record, wsk_recovered));

        DocumentKey doc_key_recovered;
        UAPP_REQUIRE_OK(
            uapp::keys::derive_document_key(wsk_recovered, view(document), doc_key_recovered));

        SecureBytes plaintext;
        UAPP_REQUIRE_OK(uapp::keys::open_document_content(doc_key_recovered, view(document),
                                                          sealed, plaintext));
        UAPP_CHECK_MSG(to_string(plaintext) == content,
                       "the recovery phrase alone must restore access to content");
    }

    // --- device D: the passphrase-wrapped URK path (flagged, additional) ---
    {
        const std::string          passphrase = "a recovery passphrase";
        PassphraseWrappedUrkRecord urk_record;
        UAPP_REQUIRE_OK(uapp::keys::wrap_urk_under_passphrase(
            view(passphrase), KdfProfile::MobileInteractive, urk, urk_record));

        UserRootKey urk_from_passphrase;
        UAPP_REQUIRE_OK(uapp::keys::unwrap_urk_with_passphrase(view(passphrase), urk_record,
                                                                urk_from_passphrase));

        WorkspaceKey wsk_recovered;
        UAPP_REQUIRE_OK(uapp::keys::unwrap_workspace_key(urk_from_passphrase, view(workspace),
                                                          workspace_record, wsk_recovered));

        DocumentKey doc_key_recovered;
        UAPP_REQUIRE_OK(
            uapp::keys::derive_document_key(wsk_recovered, view(document), doc_key_recovered));

        SecureBytes plaintext;
        UAPP_REQUIRE_OK(uapp::keys::open_document_content(doc_key_recovered, view(document),
                                                          sealed, plaintext));
        UAPP_CHECK(to_string(plaintext) == content);
    }
}

UAPP_TEST("keys.chain leaks no plaintext when any link in the chain is wrong") {
    UAPP_REQUIRE(crypto_ready());

    RecoverySecret secret;
    UserRootKey    urk;
    WorkspaceKey   wsk;
    UAPP_REQUIRE_OK(uapp::keys::generate_recovery_secret(secret));
    UAPP_REQUIRE_OK(uapp::keys::derive_user_root_key(secret, urk));
    UAPP_REQUIRE_OK(uapp::keys::generate_workspace_key(wsk));

    const std::string document = "page_0001";
    const std::string other    = "page_0002";
    const std::string content  = "the quick brown fox jumps over the lazy dog";

    DocumentKey doc_key;
    UAPP_REQUIRE_OK(uapp::keys::derive_document_key(wsk, view(document), doc_key));

    SealedBlob sealed;
    UAPP_REQUIRE_OK(
        uapp::keys::seal_document_content(doc_key, view(document), view(content), sealed));

    // (1) The right key, the wrong document id. The id is in the AAD, so a
    //     blob cannot be moved from one document to another even inside the
    //     same workspace — this is the defence against a server shuffling
    //     ciphertexts between rows.
    {
        SecureBytes out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::open_document_content(doc_key, view(other), sealed, out),
            Status::CryptoAuthFailed);
        UAPP_CHECK(out.empty());
    }

    // (2) The other document's key on this document's blob.
    {
        DocumentKey other_key;
        UAPP_REQUIRE_OK(uapp::keys::derive_document_key(wsk, view(other), other_key));

        SecureBytes out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::open_document_content(other_key, view(document), sealed, out),
            Status::CryptoAuthFailed);
        UAPP_CHECK(out.empty());
    }

    // (3) A document key derived from a DIFFERENT workspace key: the
    //     revoked-device case after §5A.3 rotation, which must fail rather
    //     than partially succeed.
    {
        WorkspaceKey rotated;
        UAPP_REQUIRE_OK(uapp::keys::generate_workspace_key(rotated));

        DocumentKey stale;
        UAPP_REQUIRE_OK(uapp::keys::derive_document_key(rotated, view(document), stale));

        SecureBytes out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::open_document_content(stale, view(document), sealed, out),
            Status::CryptoAuthFailed);
        UAPP_CHECK(out.empty());
    }

    // (4) Every byte of the ciphertext and the tag, one bit each. The
    //     out-parameter is checked after every one: "no partial plaintext on
    //     failure" is a security property, so it is asserted rather than
    //     trusted.
    for (std::size_t i = 0; i < sealed.ciphertext.size(); ++i) {
        SealedBlob tampered = sealed;
        tampered.ciphertext[i] ^= 0x01u;

        SecureBytes out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::open_document_content(doc_key, view(document), tampered, out),
            Status::CryptoAuthFailed);
        UAPP_REQUIRE(out.empty());
    }

    // (5) A truncated blob is structurally bad, not a crash. A synced blob is
    //     untrusted input (spec §28) even though it used to be ours.
    {
        SealedBlob truncated = sealed;
        truncated.ciphertext.resize(uapp::crypto::kTagBytes - 1u);

        SecureBytes out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::open_document_content(doc_key, view(document), truncated, out),
            Status::CryptoSealedBlobBad);
        UAPP_CHECK(out.empty());
    }

    // (6) An empty document id on the way out, and an invalid key.
    {
        SecureBytes out;
        UAPP_REQUIRE_STATUS(
            uapp::keys::open_document_content(doc_key, ByteView{}, sealed, out),
            Status::KeyScopeIdInvalid);
        UAPP_CHECK(out.empty());

        const DocumentKey empty_key;
        UAPP_REQUIRE_STATUS(
            uapp::keys::open_document_content(empty_key, view(document), sealed, out),
            Status::CryptoKeyInvalid);
        UAPP_CHECK(out.empty());

        SealedBlob blob;
        UAPP_REQUIRE_STATUS(
            uapp::keys::seal_document_content(empty_key, view(document), view(content), blob),
            Status::CryptoKeyInvalid);
        UAPP_CHECK(blob.ciphertext.empty());
    }
}
