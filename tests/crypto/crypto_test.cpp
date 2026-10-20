// SPDX-License-Identifier: Apache-2.0
//
// tests/crypto/crypto_test.cpp — security/crypto.
//
// WHY THIS FILE LEANS ON PUBLISHED TEST VECTORS
//
// A crypto round-trip test is nearly worthless on its own. `seal()` followed
// by `open()` passes for any pair of mutually-inverse functions — including
// XOR with a constant, including a wrapper that silently ignores the AAD,
// including one that confuses XChaCha20 with the original (non-IETF)
// ChaCha20 variant, and including one that mixes up the key and nonce
// arguments. Every one of those bugs is invisible to a round trip and
// catastrophic against a real attacker, or against a second implementation
// trying to read our data.
//
// So the cases below are built in three layers, in descending strength:
//
//   1. **External published vectors.** Produced by neither libsodium nor us,
//      so they can catch a wrong-construction bug in either:
//        * `crypto.vectors.xchacha20poly1305_ietf_cfrg` — the AEAD vector
//          from draft-irtf-cfrg-xchacha-03 Appendix A.3.1, driven through
//          the real `crypto::open()`.
//        * `crypto.vectors.x25519_rfc7748` — the Alice/Bob keypair from
//          RFC 7748 §6.1, used as real device keys through the real
//          wrap/unwrap path.
//   2. **Golden regression vectors.** Argon2id outputs recorded from this
//      build, honestly labelled as such. They are NOT external KATs and the
//      case comment explains why no external KAT is possible through this
//      API. What they do catch is the failure that actually matters for a
//      KDF: a dependency roll or platform change that silently alters the
//      derivation would make every existing user's recovery phrase stop
//      working, and nothing else in this suite would notice.
//   3. **Property and failure-mode tests.** Round trips, every way decryption
//      can fail, and the "no plaintext on failure" invariant.
//
// All vectors are embedded as hex strings parsed by a local helper rather
// than as brace-initialised byte arrays, so they can be diffed against the
// published documents by eye.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "test_framework.h"
#include "uapp/crypto/crypto.h"
#include "uapp/crypto/secure_bytes.h"
#include "uapp/status.h"

namespace {

using uapp::Status;
using uapp::crypto::ByteView;
using uapp::crypto::ContentKey;
using uapp::crypto::DeviceKeyPair;
using uapp::crypto::KdfProfile;
using uapp::crypto::kContentKeyBytes;
using uapp::crypto::kNonceBytes;
using uapp::crypto::kPublicKeyBytes;
using uapp::crypto::kSecretKeyBytes;
using uapp::crypto::kTagBytes;
using uapp::crypto::Nonce;
using uapp::crypto::no_aad;
using uapp::crypto::PublicKey;
using uapp::crypto::Salt;
using uapp::crypto::SealedBlob;
using uapp::crypto::SecureBytes;
using uapp::crypto::WrappedKey;

// --- helpers ---------------------------------------------------------------

/// Hex -> bytes. Deliberately hand-written rather than sodium_hex2bin,
/// because this test must not #include <sodium.h>: security/crypto links
/// libsodium PRIVATE-ly (see security/crypto/CMakeLists.txt), and a test that
/// reached around that would stop proving the header boundary holds.
std::vector<std::uint8_t> unhex(const char* hex) {
    std::vector<std::uint8_t> out;
    int                       hi = -1;
    for (const char* p = hex; *p != '\0'; ++p) {
        const char c = *p;
        int        v;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = 10 + (c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            v = 10 + (c - 'A');
        } else {
            continue;  // tolerate whitespace and line breaks in a pasted vector
        }
        if (hi < 0) {
            hi = v;
        } else {
            out.push_back(static_cast<std::uint8_t>((hi << 4) | v));
            hi = -1;
        }
    }
    return out;
}

ByteView view(const std::vector<std::uint8_t>& v) {
    return ByteView{v.empty() ? nullptr : v.data(), v.size()};
}

ByteView view(const std::string& s) {
    return ByteView{s.empty() ? nullptr : reinterpret_cast<const std::uint8_t*>(s.data()),
                    s.size()};
}

/// Every case calls this first. crypto::init() is idempotent, so calling it
/// per-case rather than once in main() keeps each case runnable in isolation
/// under a ctest name filter — which is the whole point of splitting this
/// binary into six ctest entries.
bool ensure_init() { return uapp::is_ok(uapp::crypto::init()); }

/// Load a chosen key. Uses ContentKey::import(), the only public writer.
bool load_key(ContentKey& key, const std::vector<std::uint8_t>& bytes) {
    return uapp::is_ok(key.import(bytes.data(), bytes.size()));
}

/// Build a DeviceKeyPair from published key material.
///
/// `secret_key` is a public SecureBytes member, which is what makes this
/// possible without a test-only API. That is a deliberate property of the
/// type: a keystore adapter has to be able to load a device's existing key
/// from hardware, and a test is the same shape of caller.
bool load_keypair(DeviceKeyPair& kp, const char* public_hex, const char* secret_hex) {
    const std::vector<std::uint8_t> pub = unhex(public_hex);
    const std::vector<std::uint8_t> sec = unhex(secret_hex);
    if (pub.size() != kPublicKeyBytes || sec.size() != kSecretKeyBytes) { return false; }
    std::memcpy(kp.public_key.data(), pub.data(), kPublicKeyBytes);
    if (!uapp::is_ok(SecureBytes::allocate(kSecretKeyBytes, kp.secret_key))) { return false; }
    std::memcpy(kp.secret_key.data(), sec.data(), kSecretKeyBytes);
    return true;
}

bool bytes_equal(const SecureBytes& got, const std::string& want) {
    return got.size() == want.size() &&
           uapp::crypto::secure_equal(got.data(), want.data(), want.size());
}

// --- published vector: AEAD ------------------------------------------------
//
// draft-irtf-cfrg-xchacha-03, Appendix A.3.1 — "Example and Test Vector for
// AEAD_XCHACHA20_POLY1305". The same plaintext RFC 8439 §2.8.2 uses, so the
// key and AAD are recognisable; the 24-byte nonce is what makes it XChaCha.
constexpr const char* kCfrgKey   = "808182838485868788898a8b8c8d8e8f"
                                   "909192939495969798999a9b9c9d9e9f";
constexpr const char* kCfrgNonce = "404142434445464748494a4b4c4d4e4f5051525354555657";
constexpr const char* kCfrgAad   = "50515253c0c1c2c3c4c5c6c7";
constexpr const char* kCfrgPlaintext =
    "Ladies and Gentlemen of the class of '99: If I could offer you only one "
    "tip for the future, sunscreen would be it.";
// Ciphertext followed by the 16-byte Poly1305 tag, i.e. exactly libsodium's
// (and our) COMBINED-mode layout. The draft prints them as two fields; this
// is their concatenation, which is what SealedBlob::ciphertext holds.
constexpr const char* kCfrgCiphertextWithTag =
    "bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb"
    "731c7f1b0b4aa6440bf3a82f4eda7e39ae64c6708c54c216cb96b72e1213b452"
    "2f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b2383565d3fff9"
    "21f9664c97637da9768812f615c68b13b52e"
    "c0875924c1c7987947deafd8780acf49";

// --- published vector: X25519 ----------------------------------------------
//
// RFC 7748 §6.1, the Alice/Bob example. Used here as real device key
// material, not as a KAT of the raw Diffie-Hellman output: this module
// deliberately does not expose a raw shared secret (handing one out is an
// invitation to use it directly as a key), so the DH result is checked
// indirectly — the agreement must produce the same KEK on both sides, which
// is only true if both halves of each published pair really correspond.
constexpr const char* kAliceSecret = "77076d0a7318a57d3c16c17251b26645"
                                     "df4c2f87ebc0992ab177fba51db92c2a";
constexpr const char* kAlicePublic = "8520f0098930a754748b7ddcb43ef75a"
                                     "0dbf3a0d26381af4eba4a98eaa9b4e6a";
constexpr const char* kBobSecret   = "5dab087e624a8a4b79e17f8b83800ee6"
                                     "6f3bb1292618b6fd1c2f8b27ff88e0eb";
constexpr const char* kBobPublic   = "de9edb7d7b7dc1b4d35b61c2ece43537"
                                     "3f8343c85b78674dadfc7e146f882b4f";

// --- golden (NOT external) vectors: Argon2id -------------------------------
//
// Recorded from libsodium 1.0.22 on arm64 with the parameters
// crypto::kdf_parameters() reports for each profile.
//
// WHY THERE IS NO EXTERNAL KAT HERE, stated so the gap is not mistaken for
// an oversight: RFC 9106's published Argon2id test vectors all use
// parallelism p = 4, and libsodium's crypto_pwhash() HARDCODES p = 1 and
// exposes no way to change it. The RFC vectors are therefore unreachable
// through this API, and no amount of care in this file can make them
// reachable. Reproducing them would mean linking the Argon2 reference
// implementation as a second dependency purely to test the first, which is
// not a trade worth making for a derivation whose algorithm identity is
// already pinned by value (ALG_ARGON2ID13) and asserted below.
//
// What these golden values DO catch is the failure mode that matters most
// for this specific function: `derive_key_from_passphrase` must be
// bit-identical forever, across libsodium versions, compilers and CPU
// architectures, because `security-model.md` §5A.4 makes it the path by
// which a user recovers their data on a new device. A roll that changed the
// output by one bit would lock every existing user out, and this assertion
// is the only thing in the suite that would notice.
constexpr const char* kKdfSalt       = "000102030405060708090a0b0c0d0e0f";
constexpr const char* kKdfPassphrase = "correct horse battery staple";
constexpr const char* kKdfGoldenMobileInteractive =
    "0d1a3c6523c8f06e4e0af9c515aa5b5448cfebd6838f2d52c3d8b6ef8ddc3c2e";
constexpr const char* kKdfGoldenDesktopModerate =
    "aad608b5866cef907f47d5cae529ed01a91301c92c5d5fef46e1a65e394e5742";

// --- cross-implementation vectors: HKDF-SHA256 -----------------------------
//
// STRONGER than the Argon2id goldens above and weaker than the two external
// vectors at the top of this file, so it is worth being exact about what
// these are. The expected values were produced by an INDEPENDENT
// HKDF-SHA256 implementation (a ~10-line Python one over hashlib/hmac — not
// libsodium, not this code), and that implementation was first checked
// against RFC 5869's own Test Case 1 (salt 000102...0c, IKM 0x0b*22, info
// f0f1...f9, L=42), which it reproduces byte for byte.
//
// RFC 5869's vectors cannot be driven through crypto::derive_subkey()
// directly, for the same shape of reason RFC 9106's cannot be driven through
// derive_key_from_passphrase(): this API fixes both the IKM and the output
// at 32 bytes, while the published cases use a 22-byte IKM and a 42-byte
// output. Verifying a second implementation against the RFC and then
// ourselves against that implementation is the strongest thing available,
// and it is strictly more than a self-recorded golden: a build that swapped
// the label and the info, used SHA-512, skipped the extract step or hashed
// the label's NUL terminator would disagree with these values.
constexpr const char* kHkdfParent = "000102030405060708090a0b0c0d0e0f"
                                    "101112131415161718191a1b1c1d1e1f";
// label = "uapp/keys/v1/user-root-key", info = "" (the one-child edge)
constexpr const char* kHkdfUserRootKey =
    "b1f72154966ab04ef6418a60f2d94f3783fb87eefdaf08566d92e84a23790714";
// label = "uapp/keys/v1/document-key", info = "page_0001"
constexpr const char* kHkdfDocumentKey =
    "4192c53bf10d6bbc04858e69b32877c32d66bcdc0b2ee6cbd00fabb441ddbf38";
// label = "uapp/keys/v1/workspace-key-wrap", info = "page_0001" — the SAME
// parent and the SAME info as the line above, differing only by label. The
// two values must differ; that is the entire function of a label.
constexpr const char* kHkdfWorkspaceWrap =
    "7dbce2f7f187de9cf0a7ae1b780262f3b60fcf966ee3f1980fd03e169f3c9805";

Salt salt_from_hex(const char* hex) {
    Salt                            s{};
    const std::vector<std::uint8_t> bytes = unhex(hex);
    if (bytes.size() == s.size()) { std::memcpy(s.data(), bytes.data(), s.size()); }
    return s;
}

}  // namespace

// ===========================================================================
// crypto.init
// ===========================================================================

UAPP_TEST("crypto.init succeeds, is idempotent and reports a version") {
    UAPP_REQUIRE_OK(uapp::crypto::init());
    UAPP_CHECK(uapp::crypto::is_initialised());

    // Idempotence is not cosmetic: sodium_init() returns 1 (not 0) when
    // already initialised, and reading that as failure is an easy mistake
    // that would make the second caller in a process fail for no reason.
    UAPP_REQUIRE_OK(uapp::crypto::init());
    UAPP_REQUIRE_OK(uapp::crypto::init());

    const char* version = uapp::crypto::library_version();
    UAPP_REQUIRE(version != nullptr);
    UAPP_CHECK_MSG(std::strcmp(version, "uninitialised") != 0, version);

    // Assert the version CMake's OWN configure-time probe found on THIS
    // machine (UAPP_SODIUM_VERSION_EXPECTED, from UappSodium.cmake via
    // tests/CMakeLists.txt), not a hardcoded "1.0.22" — that literal is the
    // macOS Homebrew dev host's pin in third_party/libsodium/VERSION, and a
    // real, confirmed-by-building-in-a-container finding is that it is NOT
    // what every platform links: Ubuntu's packaged libsodium is 1.0.18-2.
    // Hardcoding the pin here would fail this test on every platform except
    // the one it was recorded on, for a reason that has nothing to do with
    // what this check actually exists to catch.
    //
    // What it DOES catch, portably: configure-time detection and link-time
    // reality disagreeing on the SAME machine — e.g. a stale CMake cache
    // after a library upgrade, or a build picking up a different libsodium
    // than the one it detected. That is still exactly the condition
    // security-model.md §12's CVE process needs a true answer to ("which
    // libsodium is in THIS binary", checked against what the build itself
    // believed it was linking), just checked the way that generalises across
    // every platform's own locally-installed version instead of only one.
#ifdef UAPP_SODIUM_VERSION_EXPECTED
    UAPP_CHECK_MSG(std::strcmp(version, UAPP_SODIUM_VERSION_EXPECTED) == 0,
                   std::string("linked libsodium is ") + version +
                       ", but CMake's own configure-time probe found " +
                       UAPP_SODIUM_VERSION_EXPECTED +
                       " — configure-time detection and link-time reality disagree on "
                       "this machine, which usually means a stale CMake cache after a "
                       "library upgrade");
#endif
}

// ===========================================================================
// crypto.vectors — external published vectors
// ===========================================================================

UAPP_TEST("crypto.vectors.xchacha20poly1305_ietf_cfrg decrypts the published vector") {
    UAPP_REQUIRE(ensure_init());

    // The decrypt direction is used on purpose. seal() generates its own
    // nonce and the API has no way to inject one (that absence is the
    // module's nonce-reuse control, see crypto.h), so the encrypt direction
    // cannot be driven with a fixed published nonce without adding exactly
    // the back door the design forbids.
    //
    // Driving the vector through open() is not a weaker test. For open() to
    // return this plaintext, the wrapper must have the key, the nonce, the
    // AAD, the tag position and the XChaCha20-vs-ChaCha20 variant all
    // correct — i.e. it verifies the whole construction. A wrapper that, say,
    // swapped key and nonce, used the non-IETF variant, or quietly dropped
    // the AAD would fail here.
    ContentKey key;
    UAPP_REQUIRE(load_key(key, unhex(kCfrgKey)));

    const std::vector<std::uint8_t> nonce_bytes = unhex(kCfrgNonce);
    UAPP_REQUIRE(nonce_bytes.size() == kNonceBytes);

    SealedBlob blob;
    std::memcpy(blob.nonce.data(), nonce_bytes.data(), kNonceBytes);
    blob.ciphertext = unhex(kCfrgCiphertextWithTag);

    const std::string expected(kCfrgPlaintext);
    UAPP_REQUIRE_MSG(blob.ciphertext.size() == expected.size() + kTagBytes,
                     std::string("vector length ") + std::to_string(blob.ciphertext.size()) +
                         " != plaintext " + std::to_string(expected.size()) + " + tag");

    const std::vector<std::uint8_t> aad = unhex(kCfrgAad);

    SecureBytes plaintext;
    UAPP_REQUIRE_OK(uapp::crypto::open(key, blob, view(aad), plaintext));
    UAPP_CHECK_MSG(bytes_equal(plaintext, expected),
                   "recovered plaintext does not match the CFRG vector");
}

UAPP_TEST("crypto.vectors.xchacha20poly1305_ietf_cfrg rejects a one-bit mutation") {
    UAPP_REQUIRE(ensure_init());

    ContentKey key;
    UAPP_REQUIRE(load_key(key, unhex(kCfrgKey)));
    const std::vector<std::uint8_t> nonce_bytes = unhex(kCfrgNonce);
    const std::vector<std::uint8_t> aad         = unhex(kCfrgAad);

    // Walk one bit in each of several positions: the first ciphertext byte,
    // a middle one, the last ciphertext byte, and a tag byte. A wrapper that
    // got the tag offset wrong would still authenticate ciphertext mutations
    // while ignoring tag mutations, so testing both regions is what makes
    // this meaningful.
    const std::vector<std::uint8_t> original = unhex(kCfrgCiphertextWithTag);
    const std::size_t               positions[] = {0, original.size() / 2, original.size() - 17,
                                     original.size() - 1};

    for (const std::size_t pos : positions) {
        SealedBlob blob;
        std::memcpy(blob.nonce.data(), nonce_bytes.data(), kNonceBytes);
        blob.ciphertext = original;
        blob.ciphertext[pos] ^= 0x01;

        SecureBytes out;
        UAPP_CHECK_MSG(uapp::crypto::open(key, blob, view(aad), out) == Status::CryptoAuthFailed,
                       std::string("mutation at byte ") + std::to_string(pos) + " was accepted");
        UAPP_CHECK_MSG(out.empty(), "plaintext was returned for a mutated ciphertext");
    }
}

UAPP_TEST("crypto.vectors.xchacha20poly1305_ietf_cfrg binds the associated data") {
    UAPP_REQUIRE(ensure_init());

    ContentKey key;
    UAPP_REQUIRE(load_key(key, unhex(kCfrgKey)));
    const std::vector<std::uint8_t> nonce_bytes = unhex(kCfrgNonce);

    SealedBlob blob;
    std::memcpy(blob.nonce.data(), nonce_bytes.data(), kNonceBytes);
    blob.ciphertext = unhex(kCfrgCiphertextWithTag);

    // The most valuable single assertion about AAD handling: a wrapper that
    // accepted the AAD argument and then passed nullptr/0 to libsodium would
    // pass every round-trip test in this file, and would silently remove the
    // only defence against a valid ciphertext being moved to a different
    // place in the data model (security-model.md §5's chunk AAD exists for
    // exactly that). Decrypting the published vector with the WRONG AAD must
    // fail, and with NO AAD must also fail.
    std::vector<std::uint8_t> wrong_aad = unhex(kCfrgAad);
    wrong_aad[0] ^= 0xff;

    SecureBytes out;
    UAPP_CHECK(uapp::crypto::open(key, blob, view(wrong_aad), out) == Status::CryptoAuthFailed);
    UAPP_CHECK(out.empty());

    UAPP_CHECK(uapp::crypto::open(key, blob, no_aad(), out) == Status::CryptoAuthFailed);
    UAPP_CHECK(out.empty());

    // ... and the correct AAD still works, so the two assertions above are
    // not passing merely because everything fails.
    const std::vector<std::uint8_t> aad = unhex(kCfrgAad);
    UAPP_REQUIRE_OK(uapp::crypto::open(key, blob, view(aad), out));
    UAPP_CHECK(bytes_equal(out, std::string(kCfrgPlaintext)));
}

UAPP_TEST("crypto.vectors.x25519_rfc7748 wraps and unwraps between the published keypairs") {
    UAPP_REQUIRE(ensure_init());

    DeviceKeyPair alice;
    DeviceKeyPair bob;
    UAPP_REQUIRE(load_keypair(alice, kAlicePublic, kAliceSecret));
    UAPP_REQUIRE(load_keypair(bob, kBobPublic, kBobSecret));
    UAPP_REQUIRE(alice.valid());
    UAPP_REQUIRE(bob.valid());

    ContentKey cek;
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(cek));
    std::vector<std::uint8_t> cek_copy(cek.bytes(), cek.bytes() + kContentKeyBytes);

    WrappedKey wrapped;
    UAPP_REQUIRE_OK(uapp::crypto::wrap_content_key(alice, bob.public_key, cek, wrapped));

    ContentKey recovered;
    UAPP_REQUIRE_OK(
        uapp::crypto::unwrap_content_key(bob, alice.public_key, wrapped, recovered));
    UAPP_REQUIRE(recovered.valid());

    // Constant-time comparison, for the reason secure_bytes.h gives: never
    // std::memcmp on secret material, not even in a test, because tests get
    // copied into production code.
    UAPP_CHECK_MSG(
        uapp::crypto::secure_equal(recovered.bytes(), cek_copy.data(), kContentKeyBytes),
        "the CEK recovered through RFC 7748 key material is not the one that was wrapped");

    uapp::crypto::secure_zero(cek_copy.data(), cek_copy.size());

    // The agreement must be DIRECTIONAL. Bob wrapping to Alice must produce
    // a key Alice can open and Bob cannot — if crypto_kx's rx/tx split were
    // being used wrongly (both sides taking tx, say), this would still round
    // trip and the replay protection described in crypto.h would be absent.
    WrappedKey reverse;
    UAPP_REQUIRE_OK(uapp::crypto::wrap_content_key(bob, alice.public_key, cek, reverse));
    UAPP_CHECK_MSG(reverse.nonce != wrapped.nonce, "two seals reused a nonce");

    ContentKey reverse_ok;
    UAPP_CHECK(uapp::is_ok(
        uapp::crypto::unwrap_content_key(alice, bob.public_key, reverse, reverse_ok)));

    // And the same wrapped blob must NOT open in the direction it was not
    // sealed for.
    ContentKey wrong_direction;
    UAPP_CHECK(uapp::crypto::unwrap_content_key(alice, bob.public_key, wrapped, wrong_direction) ==
               Status::CryptoAuthFailed);
    UAPP_CHECK(!wrong_direction.valid());
}

UAPP_TEST("crypto.vectors.x25519_rfc7748 rejects small-order peer public keys") {
    UAPP_REQUIRE(ensure_init());

    DeviceKeyPair device;
    UAPP_REQUIRE_OK(uapp::crypto::generate_device_keypair(device));

    ContentKey cek;
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(cek));

    // The all-zero public key is the canonical attack: it drives the X25519
    // agreement to an all-zero shared point, which the attacker knows, so a
    // library that accepted it would hand out a predictable KEK. libsodium's
    // crypto_scalarmult rejects an all-zero OUTPUT, which covers the whole
    // small-order subgroup; we propagate that rejection as CryptoKeyInvalid
    // rather than proceeding.
    PublicKey zero{};
    zero.fill(0);
    WrappedKey out;
    UAPP_CHECK(uapp::crypto::wrap_content_key(device, zero, cek, out) == Status::CryptoKeyInvalid);

    PublicKey one{};
    one.fill(0);
    one[0] = 1;
    UAPP_CHECK(uapp::crypto::wrap_content_key(device, one, cek, out) == Status::CryptoKeyInvalid);

    // HONEST LIMIT, asserted so nobody later mistakes the two checks above
    // for full public-key validation: an all-0xff encoding is NOT rejected.
    // libsodium only rejects keys whose agreement output is all-zero, so a
    // non-canonical encoding with the high bit set is accepted and produces
    // an ordinary-looking shared secret. That is correct behaviour for
    // X25519 (RFC 7748 does not require canonical-form checks and the
    // high bit is masked), but it means "wrap_content_key validates the peer
    // key" is a NARROWER claim than it sounds. Authenticating WHICH device a
    // public key belongs to is `security-model.md` §5A.2's out-of-band
    // short-code check, and that is NOT IMPLEMENTED.
    PublicKey high{};
    high.fill(0xff);
    UAPP_CHECK_MSG(uapp::is_ok(uapp::crypto::wrap_content_key(device, high, cek, out)),
                   "libsodium's behaviour for non-canonical X25519 public keys changed; "
                   "re-read the limit documented in this case");
}

// ===========================================================================
// crypto.aead — round trips and every failure mode
// ===========================================================================

UAPP_TEST("crypto.aead round-trips with a generated key") {
    UAPP_REQUIRE(ensure_init());

    ContentKey key;
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(key));
    UAPP_REQUIRE(key.valid());

    const std::string plaintext = "# Heading\n\nA block of a page, which is what a CEK seals.";
    const std::string aad       = "page-id:0123456789abcdef|block-index:7";

    SealedBlob blob;
    UAPP_REQUIRE_OK(uapp::crypto::seal(key, view(plaintext), view(aad), blob));

    UAPP_CHECK_MSG(blob.ciphertext.size() == plaintext.size() + kTagBytes,
                   std::string("ciphertext is ") + std::to_string(blob.ciphertext.size()) +
                       " bytes for a " + std::to_string(plaintext.size()) + "-byte plaintext");

    // The ciphertext must not contain the plaintext. A wrapper that
    // accidentally copied instead of encrypting would pass a round trip.
    UAPP_CHECK_MSG(std::search(blob.ciphertext.begin(), blob.ciphertext.end(), plaintext.begin(),
                               plaintext.end()) == blob.ciphertext.end(),
                   "the plaintext appears verbatim inside the ciphertext");

    SecureBytes recovered;
    UAPP_REQUIRE_OK(uapp::crypto::open(key, blob, view(aad), recovered));
    UAPP_CHECK(bytes_equal(recovered, plaintext));
}

UAPP_TEST("crypto.aead round-trips an empty plaintext and no aad") {
    UAPP_REQUIRE(ensure_init());

    ContentKey key;
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(key));

    // A zero-length message is still authenticated: the output is a bare
    // 16-byte tag. Worth a case because an empty-input special case is a
    // classic place to accidentally skip the AEAD entirely.
    SealedBlob blob;
    UAPP_REQUIRE_OK(uapp::crypto::seal(key, no_aad(), no_aad(), blob));
    UAPP_CHECK(blob.ciphertext.size() == kTagBytes);

    SecureBytes recovered;
    UAPP_REQUIRE_OK(uapp::crypto::open(key, blob, no_aad(), recovered));
    UAPP_CHECK(recovered.empty());
}

UAPP_TEST("crypto.aead is semantically secure: identical plaintexts seal differently") {
    UAPP_REQUIRE(ensure_init());

    ContentKey key;
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(key));

    const std::string plaintext = "the same bytes, twice";

    SealedBlob a;
    SealedBlob b;
    UAPP_REQUIRE_OK(uapp::crypto::seal(key, view(plaintext), no_aad(), a));
    UAPP_REQUIRE_OK(uapp::crypto::seal(key, view(plaintext), no_aad(), b));

    // If this failed, seal() would be deterministic — which for a database
    // of encrypted blocks means an attacker with the ciphertext can tell
    // which blocks are identical, and can tell when a block reverts to a
    // previous value. The random nonce is what prevents it.
    UAPP_CHECK_MSG(a.nonce != b.nonce, "two seals under one key produced the same nonce");
    UAPP_CHECK_MSG(a.ciphertext != b.ciphertext,
                   "two seals of identical plaintext produced identical ciphertext");

    // Both must still open, and to the same thing.
    SecureBytes ra;
    SecureBytes rb;
    UAPP_REQUIRE_OK(uapp::crypto::open(key, a, no_aad(), ra));
    UAPP_REQUIRE_OK(uapp::crypto::open(key, b, no_aad(), rb));
    UAPP_CHECK(bytes_equal(ra, plaintext));
    UAPP_CHECK(bytes_equal(rb, plaintext));
}

UAPP_TEST("crypto.aead fails cleanly on a wrong key, leaking no plaintext") {
    UAPP_REQUIRE(ensure_init());

    ContentKey right;
    ContentKey wrong;
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(right));
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(wrong));

    const std::string plaintext = "confidential block content";

    SealedBlob blob;
    UAPP_REQUIRE_OK(uapp::crypto::seal(right, view(plaintext), no_aad(), blob));

    SecureBytes out;
    UAPP_CHECK(uapp::crypto::open(wrong, blob, no_aad(), out) == Status::CryptoAuthFailed);
    UAPP_CHECK_MSG(out.empty(), "a failed decryption returned a non-empty buffer");
    UAPP_CHECK_MSG(out.data() == nullptr, "a failed decryption returned a live buffer");
}

UAPP_TEST("crypto.aead fails cleanly on a substituted nonce") {
    UAPP_REQUIRE(ensure_init());

    ContentKey key;
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(key));

    SealedBlob a;
    SealedBlob b;
    const std::string pa = "first message";
    const std::string pb = "second message";
    UAPP_REQUIRE_OK(uapp::crypto::seal(key, view(pa), no_aad(), a));
    UAPP_REQUIRE_OK(uapp::crypto::seal(key, view(pb), no_aad(), b));

    // Swapping a valid nonce from another ciphertext under the SAME key is
    // the realistic attack (an attacker who can edit stored blobs has both),
    // and it is a stronger test than a random nonce because the nonce itself
    // is well-formed and genuine.
    SealedBlob tampered = a;
    tampered.nonce      = b.nonce;

    SecureBytes out;
    UAPP_CHECK(uapp::crypto::open(key, tampered, no_aad(), out) == Status::CryptoAuthFailed);
    UAPP_CHECK(out.empty());

    // An all-zero nonce too, since that is what an uninitialised SealedBlob
    // carries and must not accidentally validate.
    tampered.nonce.fill(0);
    UAPP_CHECK(uapp::crypto::open(key, tampered, no_aad(), out) == Status::CryptoAuthFailed);
    UAPP_CHECK(out.empty());
}

UAPP_TEST("crypto.aead fails cleanly on truncated input without reading out of bounds") {
    UAPP_REQUIRE(ensure_init());

    ContentKey key;
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(key));

    const std::string plaintext = "a block that will be truncated in storage";
    SealedBlob        blob;
    UAPP_REQUIRE_OK(uapp::crypto::seal(key, view(plaintext), no_aad(), blob));

    const std::vector<std::uint8_t> full = blob.ciphertext;

    // Every truncation length from 0 up to full-1. The lengths below
    // kTagBytes exercise the length check that stops `size() - kTagBytes`
    // wrapping around — the classic way a bounds check becomes a heap
    // overflow — and the whole loop is the reason this suite is also run
    // under ASan: a read past the end here would be a real vulnerability,
    // not a wrong answer.
    for (std::size_t len = 0; len < full.size(); ++len) {
        SealedBlob truncated = blob;
        truncated.ciphertext.assign(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(len));

        SecureBytes  out;
        const Status s = uapp::crypto::open(key, truncated, no_aad(), out);

        const Status expected =
            len < kTagBytes ? Status::CryptoSealedBlobBad : Status::CryptoAuthFailed;
        UAPP_CHECK_MSG(s == expected, std::string("length ") + std::to_string(len) + " gave " +
                                          uapp::status_code(s) + ", wanted " +
                                          uapp::status_code(expected));
        UAPP_CHECK_MSG(out.empty(),
                       std::string("length ") + std::to_string(len) + " returned plaintext");
    }
}

UAPP_TEST("crypto.aead rejects an invalid key rather than dereferencing it") {
    UAPP_REQUIRE(ensure_init());

    // A default-constructed ContentKey has no storage. Every entry point must
    // reject it with CryptoKeyInvalid; the alternative is a null dereference
    // inside libsodium, reached from a caller that merely forgot a step.
    ContentKey empty;
    UAPP_REQUIRE(!empty.valid());
    UAPP_CHECK(empty.bytes() == nullptr);

    SealedBlob        blob;
    const std::string plaintext = "x";
    UAPP_CHECK(uapp::crypto::seal(empty, view(plaintext), no_aad(), blob) ==
               Status::CryptoKeyInvalid);
    UAPP_CHECK(blob.ciphertext.empty());

    ContentKey good;
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(good));
    UAPP_REQUIRE_OK(uapp::crypto::seal(good, view(plaintext), no_aad(), blob));

    SecureBytes out;
    UAPP_CHECK(uapp::crypto::open(empty, blob, no_aad(), out) == Status::CryptoKeyInvalid);
    UAPP_CHECK(out.empty());

    // import() must reject wrong lengths rather than producing a short key.
    const std::vector<std::uint8_t> short_key(16, 0xaa);
    ContentKey                      bad;
    UAPP_CHECK(bad.import(short_key.data(), short_key.size()) == Status::CryptoKeyInvalid);
    UAPP_CHECK(!bad.valid());
    UAPP_CHECK(bad.import(nullptr, kContentKeyBytes) == Status::CryptoKeyInvalid);
}

UAPP_TEST("crypto.aead enforces its own size caps") {
    UAPP_REQUIRE(ensure_init());

    ContentKey key;
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(key));

    // A view claiming more bytes than exist must be refused on the cap, not
    // read. Allocating 8 MiB+1 of real memory to test this would be slower
    // and would prove less: what is being checked is that the cap is
    // consulted BEFORE the data pointer is used.
    std::uint8_t  one_byte = 0;
    const ByteView oversized{&one_byte, uapp::crypto::kMaxPlaintextBytes + 1};

    SealedBlob blob;
    UAPP_CHECK(uapp::crypto::seal(key, oversized, no_aad(), blob) ==
               Status::CryptoMessageTooLarge);
    UAPP_CHECK(blob.ciphertext.empty());

    const ByteView oversized_aad{&one_byte, uapp::crypto::kMaxAadBytes + 1};
    const std::string plaintext = "small";
    UAPP_CHECK(uapp::crypto::seal(key, view(plaintext), oversized_aad, blob) ==
               Status::CryptoMessageTooLarge);

    // A null pointer with a nonzero length is a caller bug, not a size
    // problem, and gets InvalidArgument rather than being dereferenced.
    const ByteView lying{nullptr, 32};
    UAPP_CHECK(uapp::crypto::seal(key, lying, no_aad(), blob) == Status::InvalidArgument);
}

// ===========================================================================
// crypto.wrap — key wrapping between simulated devices
// ===========================================================================

UAPP_TEST("crypto.wrap round-trips a CEK between two generated device keypairs") {
    UAPP_REQUIRE(ensure_init());

    DeviceKeyPair phone;
    DeviceKeyPair laptop;
    UAPP_REQUIRE_OK(uapp::crypto::generate_device_keypair(phone));
    UAPP_REQUIRE_OK(uapp::crypto::generate_device_keypair(laptop));

    // Two independently generated keypairs must differ. If generation were
    // broken (a constant seed, an uninitialised buffer) every device in the
    // fleet would share a key and everything else here would still pass.
    UAPP_REQUIRE_MSG(phone.public_key != laptop.public_key,
                     "two generated device keypairs have the same public key");
    UAPP_CHECK(!uapp::crypto::secure_equal(phone.secret_key.data(), laptop.secret_key.data(),
                                           kSecretKeyBytes));

    ContentKey document_key;
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(document_key));
    std::vector<std::uint8_t> expected(document_key.bytes(),
                                       document_key.bytes() + kContentKeyBytes);

    WrappedKey wrapped;
    UAPP_REQUIRE_OK(
        uapp::crypto::wrap_content_key(phone, laptop.public_key, document_key, wrapped));

    // The wrapped key must not be the CEK in the clear. Cheap to check and
    // it catches the worst possible implementation of this function.
    UAPP_CHECK_MSG(std::memcmp(wrapped.ciphertext.data(), expected.data(), kContentKeyBytes) != 0,
                   "the wrapped key contains the CEK verbatim");

    ContentKey unwrapped;
    UAPP_REQUIRE_OK(
        uapp::crypto::unwrap_content_key(laptop, phone.public_key, wrapped, unwrapped));
    UAPP_REQUIRE(unwrapped.valid());
    UAPP_CHECK(uapp::crypto::secure_equal(unwrapped.bytes(), expected.data(), kContentKeyBytes));

    // The unwrapped CEK must actually work on content sealed with the
    // original. This is the property the enrolment flow depends on, and it is
    // stronger than byte equality because it exercises the key end to end.
    const std::string content = "a page sealed on the phone, opened on the laptop";
    SealedBlob        blob;
    UAPP_REQUIRE_OK(uapp::crypto::seal(document_key, view(content), no_aad(), blob));
    SecureBytes recovered;
    UAPP_REQUIRE_OK(uapp::crypto::open(unwrapped, blob, no_aad(), recovered));
    UAPP_CHECK(bytes_equal(recovered, content));

    uapp::crypto::secure_zero(expected.data(), expected.size());
}

UAPP_TEST("crypto.wrap refuses a third device, a wrong sender and a tampered blob") {
    UAPP_REQUIRE(ensure_init());

    DeviceKeyPair phone;
    DeviceKeyPair laptop;
    DeviceKeyPair attacker;
    UAPP_REQUIRE_OK(uapp::crypto::generate_device_keypair(phone));
    UAPP_REQUIRE_OK(uapp::crypto::generate_device_keypair(laptop));
    UAPP_REQUIRE_OK(uapp::crypto::generate_device_keypair(attacker));

    ContentKey cek;
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(cek));

    WrappedKey wrapped;
    UAPP_REQUIRE_OK(uapp::crypto::wrap_content_key(phone, laptop.public_key, cek, wrapped));

    // A device the key was not sealed to cannot open it, even knowing both
    // public keys. This is the confidentiality property the whole wrap
    // exists for.
    ContentKey stolen;
    UAPP_CHECK(uapp::crypto::unwrap_content_key(attacker, phone.public_key, wrapped, stolen) ==
               Status::CryptoAuthFailed);
    UAPP_CHECK(!stolen.valid());

    // The intended recipient cannot open it while claiming a different
    // sender. This is the AUTHENTICATION property, and it is the one that
    // crypto_box_seal would NOT give — see the divergence note in crypto.h.
    // If this assertion failed, the recipient could be fooled about which
    // device shared the key (threat T-305).
    ContentKey misattributed;
    UAPP_CHECK(uapp::crypto::unwrap_content_key(laptop, attacker.public_key, wrapped,
                                                misattributed) == Status::CryptoAuthFailed);
    UAPP_CHECK(!misattributed.valid());

    // Tampering with any byte of the wrapped blob, including the nonce.
    for (std::size_t pos = 0; pos < wrapped.ciphertext.size(); pos += 7) {
        WrappedKey mutated = wrapped;
        mutated.ciphertext[pos] ^= 0x80;
        ContentKey out;
        UAPP_CHECK_MSG(uapp::crypto::unwrap_content_key(laptop, phone.public_key, mutated, out) ==
                           Status::CryptoAuthFailed,
                       std::string("mutation at wrapped byte ") + std::to_string(pos) +
                           " was accepted");
        UAPP_CHECK(!out.valid());
    }

    WrappedKey renonced = wrapped;
    renonced.nonce[0] ^= 0x01;
    ContentKey out;
    UAPP_CHECK(uapp::crypto::unwrap_content_key(laptop, phone.public_key, renonced, out) ==
               Status::CryptoAuthFailed);
    UAPP_CHECK(!out.valid());
}

UAPP_TEST("crypto.wrap rejects an invalid device keypair and an invalid CEK") {
    UAPP_REQUIRE(ensure_init());

    DeviceKeyPair valid_device;
    UAPP_REQUIRE_OK(uapp::crypto::generate_device_keypair(valid_device));

    ContentKey cek;
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(cek));

    DeviceKeyPair no_secret;  // public_key is all-zero, secret_key is empty
    UAPP_REQUIRE(!no_secret.valid());

    WrappedKey out;
    UAPP_CHECK(uapp::crypto::wrap_content_key(no_secret, valid_device.public_key, cek, out) ==
               Status::CryptoKeyInvalid);

    ContentKey empty_cek;
    UAPP_CHECK(uapp::crypto::wrap_content_key(valid_device, valid_device.public_key, empty_cek,
                                              out) == Status::CryptoKeyInvalid);

    ContentKey unwrap_out;
    UAPP_CHECK(uapp::crypto::unwrap_content_key(no_secret, valid_device.public_key, out,
                                                unwrap_out) == Status::CryptoKeyInvalid);
}

// ===========================================================================
// crypto.kdf — Argon2id
// ===========================================================================

UAPP_TEST("crypto.kdf resolves each profile to the documented parameters") {
    UAPP_REQUIRE(ensure_init());

    unsigned long long ops = 0;
    std::size_t        mem = 0;

    // These exact numbers are the argument of crypto.h's "KDF PARAMETERS"
    // section and of docs/spikes/SP-9-findings.md §5. Asserting them here
    // means a change to the cost of a user's recovery key is a failing test
    // with a name, rather than an edit to a switch statement that nobody
    // reviews.
    uapp::crypto::kdf_parameters(KdfProfile::MobileInteractive, ops, mem);
    UAPP_CHECK_MSG(ops == 3, std::string("MobileInteractive opslimit is ") + std::to_string(ops));
    UAPP_CHECK_MSG(mem == 64u * 1024u * 1024u,
                   std::string("MobileInteractive memlimit is ") + std::to_string(mem));

    uapp::crypto::kdf_parameters(KdfProfile::DesktopModerate, ops, mem);
    UAPP_CHECK(ops == 3);
    UAPP_CHECK(mem == 256u * 1024u * 1024u);

    uapp::crypto::kdf_parameters(KdfProfile::ArchivalSensitive, ops, mem);
    UAPP_CHECK(ops == 4);
    UAPP_CHECK(mem == 1024u * 1024u * 1024u);

    // The floor-device constraint, as an assertion rather than a comment:
    // ADR-0002 decision 3 fixes the floor at 2 GB RAM, and the only profile
    // that may run there is the one whose memlimit fits in a mobile app's
    // share of it. If someone raises MobileInteractive's memory, this fails.
    uapp::crypto::kdf_parameters(KdfProfile::MobileInteractive, ops, mem);
    UAPP_CHECK_MSG(mem <= 64u * 1024u * 1024u,
                   "MobileInteractive exceeds the 64 MiB budget the 2 GB device floor allows");
}

UAPP_TEST("crypto.kdf is deterministic and matches the golden vector") {
    UAPP_REQUIRE(ensure_init());

    const Salt        salt       = salt_from_hex(kKdfSalt);
    const std::string passphrase = kKdfPassphrase;

    ContentKey first;
    UAPP_REQUIRE_OK(uapp::crypto::derive_key_from_passphrase(
        view(passphrase), salt, KdfProfile::MobileInteractive, first));
    UAPP_REQUIRE(first.valid());

    const std::vector<std::uint8_t> golden = unhex(kKdfGoldenMobileInteractive);
    UAPP_REQUIRE(golden.size() == kContentKeyBytes);
    UAPP_CHECK_MSG(
        uapp::crypto::secure_equal(first.bytes(), golden.data(), kContentKeyBytes),
        "MobileInteractive derivation changed — every existing user's recovery phrase "
        "would now derive a different key. See the golden-vector note at the top of this file.");

    // Determinism across calls. Cheap, and it is the property the recovery
    // flow depends on most directly.
    ContentKey second;
    UAPP_REQUIRE_OK(uapp::crypto::derive_key_from_passphrase(
        view(passphrase), salt, KdfProfile::MobileInteractive, second));
    UAPP_CHECK(uapp::crypto::secure_equal(first.bytes(), second.bytes(), kContentKeyBytes));

    // A derived key must be a usable AEAD key, not merely 32 bytes.
    const std::string content = "recovery-wrapped material";
    SealedBlob        blob;
    UAPP_REQUIRE_OK(uapp::crypto::seal(first, view(content), no_aad(), blob));
    SecureBytes recovered;
    UAPP_REQUIRE_OK(uapp::crypto::open(second, blob, no_aad(), recovered));
    UAPP_CHECK(bytes_equal(recovered, content));
}

UAPP_TEST("crypto.kdf separates passphrase, salt and profile") {
    UAPP_REQUIRE(ensure_init());

    const Salt        salt       = salt_from_hex(kKdfSalt);
    const std::string passphrase = kKdfPassphrase;

    ContentKey base;
    UAPP_REQUIRE_OK(uapp::crypto::derive_key_from_passphrase(
        view(passphrase), salt, KdfProfile::MobileInteractive, base));

    // A different passphrase, differing in ONE character, must give an
    // unrelated key.
    const std::string nearby = "correct horse battery stapla";
    ContentKey        from_nearby;
    UAPP_REQUIRE_OK(uapp::crypto::derive_key_from_passphrase(
        view(nearby), salt, KdfProfile::MobileInteractive, from_nearby));
    UAPP_CHECK_MSG(
        !uapp::crypto::secure_equal(base.bytes(), from_nearby.bytes(), kContentKeyBytes),
        "two different passphrases derived the same key");

    // A different salt must give a different key for the SAME passphrase.
    // This is what makes a salt worth storing: without it, two users with
    // the same passphrase would share a key.
    Salt other = salt;
    other[0] ^= 0xff;
    ContentKey from_other_salt;
    UAPP_REQUIRE_OK(uapp::crypto::derive_key_from_passphrase(
        view(passphrase), other, KdfProfile::MobileInteractive, from_other_salt));
    UAPP_CHECK_MSG(
        !uapp::crypto::secure_equal(base.bytes(), from_other_salt.bytes(), kContentKeyBytes),
        "two different salts derived the same key");

    // A different cost profile must give a different key — which is why
    // crypto.h says the profile is part of a key's identity and has to be
    // stored next to the salt. A caller who changes the profile to "make
    // login faster" has silently changed the key.
    ContentKey from_other_profile;
    UAPP_REQUIRE_OK(uapp::crypto::derive_key_from_passphrase(
        view(passphrase), salt, KdfProfile::DesktopModerate, from_other_profile));
    UAPP_CHECK(
        !uapp::crypto::secure_equal(base.bytes(), from_other_profile.bytes(), kContentKeyBytes));

    const std::vector<std::uint8_t> golden_moderate = unhex(kKdfGoldenDesktopModerate);
    UAPP_CHECK_MSG(uapp::crypto::secure_equal(from_other_profile.bytes(), golden_moderate.data(),
                                              kContentKeyBytes),
                   "DesktopModerate derivation changed");
}

UAPP_TEST("crypto.kdf rejects a short passphrase and a null one") {
    UAPP_REQUIRE(ensure_init());

    const Salt salt = salt_from_hex(kKdfSalt);
    ContentKey out;

    const std::string too_short = "1234567";  // kMinPassphraseBytes is 8
    UAPP_CHECK(uapp::crypto::derive_key_from_passphrase(view(too_short), salt,
                                                        KdfProfile::MobileInteractive, out) ==
               Status::CryptoPassphraseTooShort);
    UAPP_CHECK(!out.valid());

    const std::string empty;
    UAPP_CHECK(uapp::crypto::derive_key_from_passphrase(view(empty), salt,
                                                        KdfProfile::MobileInteractive, out) ==
               Status::InvalidArgument);
    UAPP_CHECK(!out.valid());

    // Exactly at the floor must be accepted — an off-by-one here would be a
    // usability bug that looks like a security feature.
    const std::string at_floor = "12345678";
    UAPP_CHECK(uapp::is_ok(uapp::crypto::derive_key_from_passphrase(
        view(at_floor), salt, KdfProfile::MobileInteractive, out)));
}

UAPP_TEST("crypto.kdf generates distinct salts") {
    UAPP_REQUIRE(ensure_init());

    // A salt generator that returned a constant would make every user's
    // derived key comparable, and nothing else in this file would catch it.
    std::set<std::string> seen;
    for (int i = 0; i < 256; ++i) {
        Salt s{};
        UAPP_REQUIRE_OK(uapp::crypto::generate_salt(s));
        seen.insert(std::string(reinterpret_cast<const char*>(s.data()), s.size()));
    }
    UAPP_CHECK_MSG(seen.size() == 256,
                   std::string("256 generated salts produced only ") +
                       std::to_string(seen.size()) + " distinct values");
}

// ===========================================================================
// crypto.nonce — the uniqueness property the whole AEAD rests on
// ===========================================================================

UAPP_TEST("crypto.nonce generates no duplicate nonce across many seals") {
    UAPP_REQUIRE(ensure_init());

    ContentKey key;
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(key));

    // WHAT THIS CAN AND CANNOT SHOW, because overclaiming here would be
    // worse than not testing it.
    //
    // CANNOT: it cannot confirm the birthday bound in crypto.h. The expected
    // number of collisions among 20 000 random 192-bit values is about
    // 2e-52 — a test that passes because collisions are astronomically
    // unlikely is a test that would also pass if the nonce were 64 bits
    // (expected collisions ~1e-11), or 48 bits.
    //
    // CAN, and this is the realistic failure mode: it catches a nonce that
    // is CONSTANT, INCREMENTING FROM A FIXED START, derived from a
    // low-resolution clock, or drawn from a PRNG that was never seeded.
    // Those are the ways nonce generation actually breaks, and every one of
    // them produces duplicates or a visible pattern at this sample size.
    //
    // The high-bit check below is the pattern half: a nonce from a counter,
    // a timestamp or a small-range PRNG leaves most high-order bytes at zero.
    constexpr int kSamples = 20000;

    std::set<std::string> nonces;
    std::set<std::string> ciphertexts;
    const std::string     plaintext = "identical plaintext every time";

    int high_bytes_nonzero = 0;

    for (int i = 0; i < kSamples; ++i) {
        SealedBlob blob;
        UAPP_REQUIRE_OK(uapp::crypto::seal(key, view(plaintext), no_aad(), blob));

        nonces.insert(std::string(reinterpret_cast<const char*>(blob.nonce.data()), kNonceBytes));
        ciphertexts.insert(
            std::string(reinterpret_cast<const char*>(blob.ciphertext.data()), kTagBytes));

        // The last 8 bytes are where a counter or a timestamp would leave
        // zeros. Expected fraction nonzero for real random bytes: ~1.
        if (blob.nonce[kNonceBytes - 1] != 0 || blob.nonce[kNonceBytes - 2] != 0) {
            ++high_bytes_nonzero;
        }
    }

    UAPP_CHECK_MSG(nonces.size() == static_cast<std::size_t>(kSamples),
                   std::string("collision: ") + std::to_string(kSamples) +
                       " seals produced only " + std::to_string(nonces.size()) +
                       " distinct nonces");

    // Identical plaintext under one key: the tag prefix must still vary,
    // which it can only do if the nonce fed the keystream.
    UAPP_CHECK_MSG(ciphertexts.size() == static_cast<std::size_t>(kSamples),
                   std::string("only ") + std::to_string(ciphertexts.size()) +
                       " distinct tags across " + std::to_string(kSamples) +
                       " seals of identical plaintext");

    // P(both top bytes zero) = 2^-16 per sample, so ~0.3 of 20 000 samples.
    // A threshold of 99% is loose enough never to flake and tight enough to
    // catch a counter.
    UAPP_CHECK_MSG(high_bytes_nonzero > kSamples * 99 / 100,
                   std::string("only ") + std::to_string(high_bytes_nonzero) + " of " +
                       std::to_string(kSamples) +
                       " nonces had a nonzero high byte pair — this looks like a counter or a "
                       "timestamp, not a CSPRNG");
}

UAPP_TEST("crypto.nonce generates distinct device keys and content keys") {
    UAPP_REQUIRE(ensure_init());

    // Same argument as above, applied to key generation: the realistic bug
    // is a constant or unseeded generator, and 2 000 samples catch it.
    constexpr int kSamples = 2000;

    std::set<std::string> content_keys;
    std::set<std::string> device_publics;

    for (int i = 0; i < kSamples; ++i) {
        ContentKey ck;
        UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(ck));
        content_keys.insert(std::string(reinterpret_cast<const char*>(ck.bytes()),
                                        kContentKeyBytes));

        DeviceKeyPair dk;
        UAPP_REQUIRE_OK(uapp::crypto::generate_device_keypair(dk));
        device_publics.insert(std::string(reinterpret_cast<const char*>(dk.public_key.data()),
                                          kPublicKeyBytes));
    }

    UAPP_CHECK_MSG(content_keys.size() == static_cast<std::size_t>(kSamples),
                   std::string("only ") + std::to_string(content_keys.size()) +
                       " distinct content keys from " + std::to_string(kSamples) +
                       " generations");
    UAPP_CHECK_MSG(device_publics.size() == static_cast<std::size_t>(kSamples),
                   std::string("only ") + std::to_string(device_publics.size()) +
                       " distinct device public keys from " + std::to_string(kSamples) +
                       " generations");
}

// ===========================================================================
// crypto.memory — SecureBytes
// ===========================================================================

UAPP_TEST("crypto.memory allocates, zeroes and releases guarded storage") {
    UAPP_REQUIRE(ensure_init());

    SecureBytes buf;
    UAPP_CHECK(buf.empty());
    UAPP_CHECK(buf.data() == nullptr);

    UAPP_REQUIRE_OK(SecureBytes::allocate(64, buf));
    UAPP_REQUIRE(buf.size() == 64);
    UAPP_REQUIRE(buf.data() != nullptr);

    // allocate() promises ZEROED storage. sodium_malloc fills with 0xdb
    // instead, so if the explicit sodium_memzero in SecureBytes::allocate
    // were removed, this would fail — and an all-0xdb "key" would be a
    // plausible-looking wrong key rather than an obvious one.
    bool all_zero = true;
    for (std::size_t i = 0; i < buf.size(); ++i) {
        if (buf.data()[i] != 0) { all_zero = false; }
    }
    UAPP_CHECK_MSG(all_zero, "allocate() did not return zeroed storage");

    // Zero-size is legal and allocates nothing — three guard pages to hold
    // nothing would be a real cost on a 2 GB device.
    SecureBytes none;
    UAPP_REQUIRE_OK(SecureBytes::allocate(0, none));
    UAPP_CHECK(none.empty());
    UAPP_CHECK(none.data() == nullptr);

    // reset() is idempotent. Calling it twice must not double-free — which
    // would abort the process via sodium_free's canary check, so this case
    // failing looks like a crash rather than a failed assertion.
    buf.reset();
    UAPP_CHECK(buf.empty());
    buf.reset();
    UAPP_CHECK(buf.empty());

    // NOT TESTABLE FROM INSIDE THIS PROCESS, stated rather than faked:
    // "the bytes were actually zeroed before the pages were returned to the
    // OS" cannot be asserted, because reading the freed region is undefined
    // behaviour and, with sodium_malloc's guard pages, an immediate SIGSEGV.
    // What the suite does instead is run under ASan/UBSan, where a
    // use-after-free or double-free in this type is a hard failure, and rely
    // on libsodium's documented sodium_free() behaviour for the zeroing
    // itself. secure_zero() below is the part that IS directly testable.
}

UAPP_TEST("crypto.memory secure_zero and secure_equal behave") {
    UAPP_REQUIRE(ensure_init());

    // secure_zero on a live, caller-owned buffer. This IS directly
    // observable, unlike the zero-on-free above.
    std::array<std::uint8_t, 48> stack_key{};
    for (std::size_t i = 0; i < stack_key.size(); ++i) {
        stack_key[i] = static_cast<std::uint8_t>(i + 1);
    }
    uapp::crypto::secure_zero(stack_key.data(), stack_key.size());
    bool zeroed = true;
    for (const std::uint8_t b : stack_key) {
        if (b != 0) { zeroed = false; }
    }
    UAPP_CHECK_MSG(zeroed, "secure_zero left nonzero bytes");

    // Degenerate arguments must not crash.
    uapp::crypto::secure_zero(nullptr, 16);
    uapp::crypto::secure_zero(stack_key.data(), 0);

    const std::array<std::uint8_t, 4> a{1, 2, 3, 4};
    const std::array<std::uint8_t, 4> b{1, 2, 3, 4};
    const std::array<std::uint8_t, 4> c{1, 2, 3, 5};
    UAPP_CHECK(uapp::crypto::secure_equal(a.data(), b.data(), 4));
    UAPP_CHECK(!uapp::crypto::secure_equal(a.data(), c.data(), 4));
    UAPP_CHECK(uapp::crypto::secure_equal(a.data(), c.data(), 0));
    UAPP_CHECK(!uapp::crypto::secure_equal(nullptr, c.data(), 4));
    UAPP_CHECK(!uapp::crypto::secure_equal(a.data(), nullptr, 4));
}

UAPP_TEST("crypto.memory moves transfer ownership exactly once") {
    UAPP_REQUIRE(ensure_init());

    // A move that left the source holding its pointer would double-free when
    // both destructors ran, aborting the process on sodium_free's canary
    // check. This is the case that keeps SecureBytes' move operators honest,
    // and it is the one most likely to be broken by a careless edit.
    SecureBytes first;
    UAPP_REQUIRE_OK(SecureBytes::allocate(32, first));
    first.data()[0] = 0x5a;
    const std::uint8_t* original = first.data();

    SecureBytes moved(std::move(first));
    UAPP_CHECK_MSG(first.empty(), "moved-from SecureBytes still reports a size");
    UAPP_CHECK_MSG(first.data() == nullptr, "moved-from SecureBytes still holds its pointer");
    UAPP_CHECK(moved.size() == 32);
    UAPP_CHECK(moved.data() == original);
    UAPP_CHECK(moved.data()[0] == 0x5a);

    SecureBytes assigned;
    UAPP_REQUIRE_OK(SecureBytes::allocate(16, assigned));
    assigned = std::move(moved);
    UAPP_CHECK(assigned.size() == 32);
    UAPP_CHECK(moved.empty());
    UAPP_CHECK(moved.data() == nullptr);

    // Self-move-assignment must not zero the buffer it is about to adopt.
    // Written with a reference to defeat -Wself-move, and present because
    // `a = std::move(a)` happens for real inside generic code such as
    // std::swap or a container reallocation.
    SecureBytes& alias = assigned;
    assigned           = std::move(alias);
    UAPP_CHECK_MSG(assigned.size() == 32, "self-move-assignment destroyed the buffer");
    UAPP_CHECK(assigned.data()[0] == 0x5a);

    // A ContentKey's move must carry the key, and leave the source invalid.
    ContentKey key;
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(key));
    std::vector<std::uint8_t> copy(key.bytes(), key.bytes() + kContentKeyBytes);
    ContentKey                moved_key(std::move(key));
    UAPP_CHECK(!key.valid());
    UAPP_REQUIRE(moved_key.valid());
    UAPP_CHECK(uapp::crypto::secure_equal(moved_key.bytes(), copy.data(), kContentKeyBytes));
    uapp::crypto::secure_zero(copy.data(), copy.size());
}

// ===========================================================================
// crypto.hkdf — subkey derivation (added 2026-10-20 with security/keys)
// ===========================================================================

UAPP_TEST("crypto.hkdf matches an independently-computed HKDF-SHA256") {
    UAPP_REQUIRE(ensure_init());

    ContentKey parent;
    UAPP_REQUIRE(load_key(parent, unhex(kHkdfParent)));

    struct Case final {
        const char* label;
        const char* info;
        const char* expected;
    };
    const Case cases[] = {
        {"uapp/keys/v1/user-root-key", "", kHkdfUserRootKey},
        {"uapp/keys/v1/document-key", "page_0001", kHkdfDocumentKey},
        {"uapp/keys/v1/workspace-key-wrap", "page_0001", kHkdfWorkspaceWrap},
    };

    for (const Case& c : cases) {
        const std::string label_str(c.label);
        const std::string info_str(c.info);

        ContentKey derived;
        UAPP_REQUIRE_OK(uapp::crypto::derive_subkey(parent, view(label_str), view(info_str),
                                                    derived));
        UAPP_REQUIRE(derived.valid());

        const std::vector<std::uint8_t> expected = unhex(c.expected);
        UAPP_REQUIRE(expected.size() == kContentKeyBytes);
        UAPP_CHECK_MSG(
            uapp::crypto::secure_equal(derived.bytes(), expected.data(), kContentKeyBytes),
            std::string("derive_subkey disagrees with the reference for label ") + c.label);
    }

    // The two cases sharing a parent and an info must not share a key. If
    // this ever fails, the label is not reaching the construction.
    UAPP_CHECK(std::string(kHkdfDocumentKey) != std::string(kHkdfWorkspaceWrap));
}

UAPP_TEST("crypto.hkdf separates parent, label and info") {
    UAPP_REQUIRE(ensure_init());

    ContentKey parent_a;
    ContentKey parent_b;
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(parent_a));
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(parent_b));

    const std::string label_a("uapp/keys/v1/a");
    const std::string label_b("uapp/keys/v1/b");
    const std::string info_a("id-1");
    const std::string info_b("id-2");

    std::set<std::string> seen;
    for (const ContentKey* p : {&parent_a, &parent_b}) {
        for (const std::string* l : {&label_a, &label_b}) {
            for (const std::string* i : {&info_a, &info_b}) {
                ContentKey derived;
                UAPP_REQUIRE_OK(uapp::crypto::derive_subkey(*p, view(*l), view(*i), derived));
                seen.insert(
                    std::string(reinterpret_cast<const char*>(derived.bytes()),
                                kContentKeyBytes));
            }
        }
    }
    UAPP_CHECK_MSG(seen.size() == 8u,
                   "all three of parent, label and info must affect the derived key");

    // Deterministic: the same inputs twice, in the same process, give the
    // same key. (The cross-implementation vectors above are what prove it
    // across processes, builds and machines.)
    ContentKey first;
    ContentKey second;
    UAPP_REQUIRE_OK(uapp::crypto::derive_subkey(parent_a, view(label_a), view(info_a), first));
    UAPP_REQUIRE_OK(uapp::crypto::derive_subkey(parent_a, view(label_a), view(info_a), second));
    UAPP_CHECK(uapp::crypto::secure_equal(first.bytes(), second.bytes(), kContentKeyBytes));

    // A subkey must not equal its parent — i.e. this is a derivation and not
    // a copy with extra steps.
    UAPP_CHECK(!uapp::crypto::secure_equal(first.bytes(), parent_a.bytes(), kContentKeyBytes));
}

UAPP_TEST("crypto.hkdf rejects an unlabelled derivation and an invalid parent") {
    UAPP_REQUIRE(ensure_init());

    ContentKey parent;
    UAPP_REQUIRE_OK(uapp::crypto::generate_content_key(parent));

    const std::string label("uapp/keys/v1/a");
    const std::string info("id-1");

    // An empty or absent label is refused, because an unlabelled derivation
    // is how two edges of a hierarchy end up producing the same key.
    {
        ContentKey out;
        UAPP_REQUIRE_STATUS(uapp::crypto::derive_subkey(parent, ByteView{}, view(info), out),
                            Status::InvalidArgument);
        UAPP_CHECK(!out.valid());

        const std::string empty_label;
        UAPP_REQUIRE_STATUS(
            uapp::crypto::derive_subkey(parent, view(empty_label), view(info), out),
            Status::InvalidArgument);
        UAPP_CHECK(!out.valid());
    }

    // Over-long label and info are refused rather than truncated: a
    // truncated label is a different label, silently.
    {
        ContentKey        out;
        const std::string long_label(uapp::crypto::kMaxKdfLabelBytes + 1u, 'L');
        UAPP_REQUIRE_STATUS(
            uapp::crypto::derive_subkey(parent, view(long_label), view(info), out),
            Status::InvalidArgument);
        UAPP_CHECK(!out.valid());

        const std::string long_info(uapp::crypto::kMaxKdfInfoBytes + 1u, 'I');
        UAPP_REQUIRE_STATUS(
            uapp::crypto::derive_subkey(parent, view(label), view(long_info), out),
            Status::InvalidArgument);
        UAPP_CHECK(!out.valid());
    }

    // An invalid parent is an error, never a derivation from zero bytes.
    {
        const ContentKey invalid;
        ContentKey       out;
        UAPP_REQUIRE_STATUS(uapp::crypto::derive_subkey(invalid, view(label), view(info), out),
                            Status::CryptoKeyInvalid);
        UAPP_CHECK(!out.valid());
    }

    // An empty info IS legal — it is the one-child edge — and must not be
    // confused with an empty label.
    {
        ContentKey        out;
        const std::string no_info;
        UAPP_REQUIRE_OK(uapp::crypto::derive_subkey(parent, view(label), view(no_info), out));
        UAPP_CHECK(out.valid());

        ContentKey out_null;
        UAPP_REQUIRE_OK(uapp::crypto::derive_subkey(parent, view(label), ByteView{}, out_null));
        UAPP_CHECK(out_null.valid());
        UAPP_CHECK_MSG(
            uapp::crypto::secure_equal(out.bytes(), out_null.bytes(), kContentKeyBytes),
            "an empty info and an absent info must derive the same key");
    }
}
