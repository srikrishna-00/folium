// SPDX-License-Identifier: Apache-2.0
//
// security/crypto/crypto.h — the cryptographic PRIMITIVES layer.
//
// This is the foundation ADR-0002 decision 1 ("E2EE from v1.0") stands on,
// and it is deliberately ONLY the foundation. What is here: key generation,
// authenticated encryption, key wrapping between two device keypairs, and a
// passphrase KDF. What is NOT here is listed in "NOT IMPLEMENTED" below, and
// that list is longer than the implemented one.
//
// `architecture.md`'s layer diagram names this module exactly:
// "security/crypto (libsodium)", in the ADAPTERS row. That placement is why
// it lives under security/ rather than core/ — core/ is the portable domain
// layer that "knows nothing about SQLite, HTTP, platform APIs or UI"
// (core/CMakeLists.txt), and a wrapper around a specific third-party C
// library is an adapter by the same rule that puts storage/sqlite outside
// core/.
//
// =========================================================================
// RULE 0: WE DO NOT IMPLEMENT CRYPTOGRAPHIC PRIMITIVES.
// =========================================================================
// Spec §38. Every operation below delegates to libsodium. There is no
// arithmetic, no S-box, no bignum and no hand-written constant-time loop in
// the .cpp — if a future change adds one, that change is wrong. The *only*
// things this module does itself are (a) choosing between libsodium's
// documented options, (b) deciding and enforcing a nonce strategy, (c)
// mapping libsodium's int returns onto uapp::Status, and (d) making the
// dangerous mistakes unrepresentable in the API. (d) is the part that earns
// the module's existence; a thin pass-through would not.
//
// =========================================================================
// ALGORITHM CHOICES, and where each is already specified
// =========================================================================
//   AEAD        XChaCha20-Poly1305-IETF   (crypto_aead_xchacha20poly1305_ietf_*)
//               Named by security-model.md §5 and §5A.1. 256-bit key,
//               192-bit nonce, 128-bit tag. The 192-bit nonce is the whole
//               reason to prefer it over ChaCha20-Poly1305-IETF or AES-GCM —
//               see "NONCE STRATEGY" below, which is not a detail.
//   Key agree   X25519, via crypto_kx (X25519 + BLAKE2b)
//               security-model.md §5A.1 specifies X25519 for the device
//               wrapping key, and §5A.6 pins "X25519 only. Revisit when
//               libsodium ships a stable hybrid KEM." No post-quantum here.
//   Passphrase  Argon2id, via crypto_pwhash, ALG pinned explicitly
//               security-model.md §5 ("Nothing derives a key from a password
//               on the client without Argon2id"). See "KDF PARAMETERS" for
//               the one place this module could not honour the written
//               design, and why.
//
// crypto_pwhash_ALG_ARGON2ID13 is passed EXPLICITLY rather than taking
// crypto_pwhash_ALG_DEFAULT. The default has changed meaning once already
// (Argon2i -> Argon2id at libsodium 1.0.15), and a key-derivation function
// whose identity depends on which libsodium happened to be linked is a key
// that silently stops deriving after a dependency roll. Everything a derived
// key depends on is pinned in this header, by value.
//
// =========================================================================
// NONCE STRATEGY — the decision this module most needs to get right
// =========================================================================
// Reusing a nonce with the same key under any ChaCha20-Poly1305 variant is
// catastrophic, not merely weak: the keystream repeats, so XOR of the two
// ciphertexts is the XOR of the two plaintexts, AND the Poly1305 one-time
// key is reused, which leaks the authentication key and lets an attacker
// forge arbitrary messages. It is the single most common way real AEAD
// deployments are broken.
//
// **DECISION: a fresh random 192-bit nonce is generated INSIDE seal(), from
// libsodium's CSPRNG, for every single encryption. The API gives the caller
// no way to supply, influence or reuse a nonce.** The nonce comes back inside
// the SealedBlob and must be stored with the ciphertext.
//
// There is no `seal_with_nonce()` overload, not even a private or
// test-only one. That absence is the control. An API that accepts a nonce
// cannot prevent its misuse by documentation, and a test-only back door is
// a production back door with a comment on it.
//
// WHY RANDOM IS SAFE HERE — the birthday-bound arithmetic, not a vibe:
//   The nonce is 192 bits. For q encryptions under one key, the probability
//   that any two nonces collide is at most q(q-1)/2 / 2^192 < q^2 / 2^193.
//     q = 2^32  (4.3e9 encryptions under ONE key):  p < 2^-129
//     q = 2^40  (1.1e12):                           p < 2^-113
//     q = 2^64  (1.8e19, absurdly beyond reality):  p < 2^-65
//   Realistic volume is many orders of magnitude below even the first row: a
//   content-encryption key in this design covers one document, a page holds
//   at most blocks::kMaxBlocksPerPage = 50 000 blocks, and each block is
//   sealed once per change. 2^-129 is far below the probability of an
//   undetected hardware fault, so random nonces are not the weakest link by
//   a wide margin.
//
//   COMPARE AES-GCM, which is why the primitive choice matters: with a
//   96-bit random IV, collision probability reaches ~2^-33 at only q = 2^32,
//   which is why NIST SP 800-38D caps a random-IV AES-GCM key at 2^32
//   invocations. XChaCha20-Poly1305's 192-bit nonce removes that cap, and
//   removing it is precisely what makes the stateless random-nonce design
//   above correct rather than merely convenient.
//
// WHY NOT A COUNTER — stated because it is the better answer in one place:
//   A monotonic counter is immune to RNG failure, and random nonces are not:
//   if the platform CSPRNG is broken or unseeded, this design breaks
//   silently. A counter, however, requires durable per-key state that
//   survives process death, crash, restore-from-backup and clock changes,
//   and a counter that resets on restore is strictly worse than a random
//   nonce (guaranteed reuse, not improbable reuse). This layer has no
//   durable state and must not invent any.
//
//   `security-model.md` §5 already specifies a DETERMINISTIC nonce for the
//   chunked-blob path: `nonce = HKDF(file_key, chunk_index)`, whose
//   uniqueness invariant is "the chunk index is unique within a file and the
//   file key is unique per blob". That is the right design there, because a
//   chunk index is derivable rather than stored and random access to page
//   300 of a 500 MB PDF needs it. **That path is NOT IMPLEMENTED in this
//   module** (see NOT IMPLEMENTED, item 2), and when it lands it must be a
//   separate, clearly-named entry point whose caller owns the chunk index —
//   not a nonce parameter bolted onto seal().
//
// WHERE THE CSPRNG COMES FROM: libsodium's randombytes_buf, which uses
// arc4random on Apple platforms and getrandom(2)//dev/urandom on Linux and
// Android — i.e. the kernel CSPRNG. There is no userspace PRNG, no seeding
// for us to get wrong, and no RandomPort indirection: `ports.h` offers a
// RandomPort seam for testability, and this module deliberately does NOT
// use it, because an injectable RNG in a crypto module is a way to inject a
// predictable one.
//
// =========================================================================
// KDF PARAMETERS — and a CONFLICT with security-model.md §5 that must be
// resolved by a human, not by this file
// =========================================================================
// `security-model.md` §5 currently says: "Nothing derives a key from a
// password on the client without Argon2id (opslimit=4, memlimit=256 MiB
// locally, where we can afford it)."
//
// **On ADR-0002 decision 3's device floor — 2 GB RAM, iOS 15+, Android 8+ —
// we cannot afford it.** A 256 MiB transient allocation on a 2 GB device,
// on top of the app's own working set, is a foreground jetsam / low-memory
// kill on both platforms. The parenthetical "where we can afford it" was
// written before the floor was fixed, and on the floor the answer is no.
//
// What this module does instead, following libsodium's OWN documented
// procedure for choosing parameters rather than inventing one: "Set memlimit
// to the amount of memory you want to reserve for password hashing. Then set
// opslimit to 3 and measure the time it takes to hash a password." Our
// memory budget on the floor device is the INTERACTIVE preset's 64 MiB —
// that is the largest of libsodium's three presets that fits — so memlimit
// is 64 MiB and opslimit is 3. See KdfProfile below for all three profiles
// and the measured timings.
//
// This is a DEVIATION FROM A WRITTEN SECURITY DESIGN, so it is flagged
// rather than absorbed: docs/security-model.md §5 has been annotated in
// place, and docs/spikes/SP-9-findings.md §5 carries the argument. The
// security cost is real and is stated there (a 4x memory reduction is a 4x
// reduction in an attacker's memory-hardness cost); it is accepted because
// the alternative is a passphrase path that cannot run on the devices we
// committed to supporting.
//
// =========================================================================
// NOT IMPLEMENTED — the E2EE architecture this layer is the floor of
// =========================================================================
// Explicitly out of scope here, each with the document that owns it:
//   1. The key HIERARCHY — **NO LONGER HERE, and still not in this module.**
//      Recovery Secret -> URK -> Workspace Key -> document key
//      (security-model.md §5A.1) landed on 2026-10-20 in `security/keys`,
//      which links this module and contains no cryptography of its own.
//      THIS MODULE STILL HAS NO OPINION about which of those a given
//      ContentKey is: it can generate 32 bytes, derive 32 bytes from 32
//      bytes, seal and wrap, and that is all. The one thing that changed
//      here is that derive_subkey() now exists (see "SUBKEY DERIVATION"
//      below), because the hierarchy that needed it arrived — the condition
//      this item previously set for wrapping it.
//      NOT in security/keys either, and listed again under its own
//      NOT IMPLEMENTED heading there: URK-pub (the published X25519 half of
//      the URK), enrolment, rotation and revocation.
//   2. Chunked blob encryption (security-model.md §5: 1 MiB chunks,
//      deterministic nonce, AAD = blob_id || chunk_index || total_chunks).
//      seal() takes an AAD argument and binding that triple into it is
//      exactly the intended use, but the chunking, the index derivation and
//      the truncation defence are the caller's and do not exist.
//   3. Device enrolment, the out-of-band short-code check, WSK rotation and
//      revocation (security-model.md §5A.2, §5A.3). wrap_content_key() is
//      the primitive those protocols call; the protocols are Phase 12.
//   4. Ed25519 device IDENTITY keys and mutation signing (security-model.md
//      §4, §5). This module does key AGREEMENT only. Signing is a separate
//      keypair with a separate lifetime and separate storage, and conflating
//      the two in one type would be the beginning of using one key for two
//      purposes.
//   5. Hardware-backed key storage. See secure_bytes.h's limits.
//   6. A C ABI. core/capi does not expose any of this, on purpose: an ABI is
//      a promise, and the shape of the key hierarchy above will change what
//      these functions should look like.
//   7. Post-quantum anything (security-model.md §5A.6).

#ifndef UAPP_SECURITY_CRYPTO_CRYPTO_H
#define UAPP_SECURITY_CRYPTO_CRYPTO_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "uapp/crypto/secure_bytes.h"
#include "uapp/status.h"

namespace uapp::crypto {

// --- sizes -----------------------------------------------------------------
// Every one of these is asserted against the corresponding libsodium macro
// in crypto.cpp with a static_assert, so a dependency roll that changed one
// is a compile error here rather than a wrong-length buffer at runtime. They
// are duplicated into this header rather than included from <sodium.h>
// because <sodium.h> is PRIVATE to this module: nothing outside
// security/crypto should be able to reach a raw libsodium call, and the only
// mechanical way to enforce that is for the public header not to expose it.

inline constexpr std::size_t kPublicKeyBytes  = 32;  ///< crypto_kx_PUBLICKEYBYTES
inline constexpr std::size_t kSecretKeyBytes  = 32;  ///< crypto_kx_SECRETKEYBYTES
inline constexpr std::size_t kContentKeyBytes = 32;  ///< ..._xchacha20poly1305_ietf_KEYBYTES
inline constexpr std::size_t kNonceBytes      = 24;  ///< ..._ietf_NPUBBYTES (192 bits)
inline constexpr std::size_t kTagBytes        = 16;  ///< ..._ietf_ABYTES (Poly1305)
inline constexpr std::size_t kSaltBytes       = 16;  ///< crypto_pwhash_SALTBYTES

/// Largest plaintext this layer will seal in one call.
///
/// Not libsodium's limit — libsodium's is ~256 GiB — but ours, and it is a
/// statement about what this API is FOR. One call seals one block, one page
/// or one 1 MiB blob chunk. 8 MiB is 8x blocks::kMaxBlockTextBytes, which
/// leaves room for a page's worth of blocks plus framing while still being
/// far too small to tempt anyone into passing a whole 500 MB PDF. Whole-file
/// encryption is the chunked path (NOT IMPLEMENTED, item 2), and a cap that
/// made it accidentally work here would ensure it never got written.
///
/// It is also a secure-allocator limit: open() puts recovered plaintext in a
/// SecureBytes, i.e. in guarded, mlock()ed pages, and guarded pages are a
/// far scarcer resource than heap (see secure_bytes.h, limit 3).
inline constexpr std::size_t kMaxPlaintextBytes = 8u * 1024u * 1024u;

/// Largest associated data. AAD is framing — ids, indices, counts (§5's
/// `blob_id || chunk_index || total_chunks`) — never content, so a small cap
/// is a correct statement about its purpose rather than an arbitrary number.
inline constexpr std::size_t kMaxAadBytes = 64u * 1024u;

/// Shortest passphrase accepted by derive_key_from_passphrase().
///
/// libsodium's own floor (crypto_pwhash_PASSWD_MIN) is 0, which is not a
/// usable product rule. 8 bytes is a floor against obvious API misuse — an
/// empty string, a single digit — and is explicitly NOT a password policy.
/// The real entropy guarantee in this design does not come from a policy at
/// all: `security-model.md` §5A.4 roots recovery in a 24-word generated
/// phrase carrying ~256 bits, precisely so that a user-chosen passphrase is
/// never the only thing between an attacker and the data. Argon2id's work
/// factor is what protects a weak passphrase, and §4's breached-password
/// check is what rejects a common one; neither lives here.
inline constexpr std::size_t kMinPassphraseBytes = 8;

// --- types -----------------------------------------------------------------

/// A device's X25519 public key. A plain array, not a SecureBytes: it is
/// public BY DESIGN — `security-model.md` §5A.1 publishes URK-pub to the
/// server — and wrapping it in guarded memory would imply a secrecy
/// requirement that does not exist and would waste three pages saying so.
using PublicKey = std::array<std::uint8_t, kPublicKeyBytes>;

/// A 192-bit AEAD nonce. Public: it travels with the ciphertext.
using Nonce = std::array<std::uint8_t, kNonceBytes>;

/// An Argon2id salt. Public: it is stored alongside the thing it protects.
/// It must be UNIQUE per derivation, which is why generate_salt() exists and
/// why there is no default value for this type.
using Salt = std::array<std::uint8_t, kSaltBytes>;

/// A read-only view over bytes. C++17, so no std::span.
struct ByteView final {
    const std::uint8_t* data = nullptr;
    std::size_t         size = 0;
};

/// An empty view, for "no associated data". Spelled as a named value so that
/// a call site reads `seal(k, pt, no_aad(), out)` rather than `{nullptr, 0}`,
/// which is easy to write by accident when AAD was meant.
[[nodiscard]] inline ByteView no_aad() noexcept { return ByteView{}; }

/// A 256-bit symmetric content-encryption key (CEK), in guarded memory.
///
/// Move-only, like the SecureBytes it holds, so a key cannot be duplicated
/// into a second unmanaged lifetime by an implicit copy.
///
/// Default-constructed instances are INVALID (no storage). Every producer in
/// this header (generate_content_key, derive_key_from_passphrase,
/// unwrap_content_key) allocates into its out-parameter, so a caller never
/// has to call allocate() itself; import() is for a keystore adapter loading
/// an existing key.
class ContentKey final {
  public:
    ContentKey() noexcept = default;

    ContentKey(ContentKey&&) noexcept            = default;
    ContentKey& operator=(ContentKey&&) noexcept = default;
    ContentKey(const ContentKey&)                = delete;
    ContentKey& operator=(const ContentKey&)     = delete;

    /// True iff this holds kContentKeyBytes of storage. Every function here
    /// that consumes a ContentKey rejects an invalid one with
    /// Status::CryptoKeyInvalid rather than dereferencing null.
    [[nodiscard]] bool valid() const noexcept { return bytes_.size() == kContentKeyBytes; }

    /// Read access for the implementation and for a keystore adapter
    /// exporting a key to hardware. Null if !valid().
    [[nodiscard]] const std::uint8_t* bytes() const noexcept { return bytes_.data(); }

    /// Allocate guarded, zeroed storage. Mostly internal; a caller needs it
    /// only before import().
    [[nodiscard]] Status allocate();

    /// Copy exactly kContentKeyBytes in from `src`, allocating if needed.
    ///
    /// THE ONLY WAY TO GET A CHOSEN KEY IN. It exists for two legitimate
    /// callers: a future keystore adapter unwrapping a key from
    /// Keychain/Keystore, and the test suite loading a published test vector.
    /// It is NOT a general mutable accessor — there is deliberately no
    /// `mutable_bytes()`, because a raw writable pointer into key storage is
    /// how half-initialised keys get used.
    [[nodiscard]] Status import(const std::uint8_t* src, std::size_t n);

    /// Zero and release immediately, rather than at end of scope. Use when a
    /// key's useful life ends well before its enclosing object's.
    void reset() noexcept { bytes_.reset(); }

  private:
    // No `friend` declarations and no private writable accessor. The
    // implementation writes a key by computing it into a scratch buffer and
    // calling import(), then secure_zero()ing the scratch. That is one extra
    // 32-byte copy per key, and it buys the invariant that import() is the
    // ONLY code path that can put bytes into a ContentKey — which is what
    // makes "a ContentKey is either invalid or holds exactly 32 bytes"
    // checkable by reading one function instead of five.
    SecureBytes bytes_;
};

/// A device's long-term X25519 identity keypair for KEY AGREEMENT.
///
/// `security-model.md` §5A.1 calls this the "Device Wrapping Key (X25519)"
/// and lists it alongside, and distinct from, the "Device Identity Key
/// (Ed25519)" that signs mutations. THIS TYPE IS ONLY THE FORMER. The naming
/// here follows the task's "long-term identity keypair (X25519 for key
/// exchange)", but the separation in §5A.1 is the one that matters: one
/// keypair must not do both agreement and signing, and there is no Ed25519
/// key in this module at all (NOT IMPLEMENTED, item 4).
///
/// Move-only: the secret half is a SecureBytes.
struct DeviceKeyPair final {
    PublicKey   public_key{};  ///< publishable
    SecureBytes secret_key;    ///< guarded; never leaves the device

    [[nodiscard]] bool valid() const noexcept { return secret_key.size() == kSecretKeyBytes; }
};

/// Ciphertext produced by seal(), with the nonce that produced it.
///
/// The nonce is a MEMBER rather than a separate out-parameter so that it is
/// structurally impossible to store a ciphertext and lose its nonce — which
/// would render the data permanently undecryptable, and is a data-loss bug
/// rather than a security one.
///
/// `ciphertext` is an ordinary std::vector, NOT a SecureBytes, and that is
/// correct: ciphertext is not secret. Putting it in guarded memory would
/// consume a scarce resource to protect bytes we intend to hand to a server.
struct SealedBlob final {
    Nonce                     nonce{};
    std::vector<std::uint8_t> ciphertext;  ///< plaintext_len + kTagBytes
};

/// A CEK sealed to a peer device. Fixed size, so no heap at all.
struct WrappedKey final {
    Nonce                                              nonce{};
    std::array<std::uint8_t, kContentKeyBytes + kTagBytes> ciphertext{};
};

/// Argon2id cost profiles.
///
/// The three values libsodium documents, plus the one we actually default to.
///
/// Measured 2026-10-17 on an Apple M4 / 16 GB dev host, -O2, 32-byte output,
/// best of 5-7 runs. A floor-tier device (ADR-0002 decision 3: 2 GB RAM,
/// Android 8 / iOS 15) is roughly 6-10x slower, and **that** is the number
/// that matters — it is an ESTIMATE, not a measurement, because this is a
/// desktop environment with no floor-tier hardware attached. Confirming it
/// on a real 2 GB device is owed (docs/spikes/SP-9-findings.md §5):
///
///   MobileInteractive  ops=3, mem= 64 MiB     61 ms here -> est. 0.4-0.6 s
///   DesktopModerate    ops=3, mem=256 MiB    277 ms here -> UNRUNNABLE on floor
///   ArchivalSensitive  ops=4, mem=  1 GiB   1563 ms here -> UNRUNNABLE on floor
///
/// For reference, libsodium's own INTERACTIVE preset (ops=2, same 64 MiB)
/// measures 42 ms here — so the extra pass costs ~45% more CPU and buys back
/// part of the memory-hardness lost by not using MODERATE's 256 MiB.
///
/// **MobileInteractive is the default and is the only profile that may be
/// used on a path that runs on a mobile device.** The other two are present
/// because the same code runs on desktop (spec's desktop target) and because
/// naming them makes the choice explicit at each call site instead of
/// hiding it behind a single constant. A caller that picks
/// DesktopModerate for a mobile code path has written a bug, and the enum
/// name is the warning.
///
/// opslimit=3 with memlimit at the INTERACTIVE preset's 64 MiB is libsodium's
/// own documented procedure applied to our memory budget, not an invented
/// pair: "Set memlimit to the amount of memory you want to reserve... Then
/// set opslimit to 3." INTERACTIVE's own opslimit is 2; we spend the extra
/// pass because CPU time is affordable on the floor device and memory is not,
/// so the cheap axis is the one to buy on.
enum class KdfProfile : int {
    MobileInteractive = 0,
    DesktopModerate   = 1,
    ArchivalSensitive = 2,
};

// --- lifecycle -------------------------------------------------------------

/// Initialise libsodium. MUST succeed before any other function here; they
/// all return Status::CryptoNotInitialised otherwise rather than producing
/// unseeded output.
///
/// Idempotent and safe to call from multiple threads (libsodium's
/// sodium_init() documents both). Call it once at process start anyway —
/// sodium_init() is only thread-safe in the sense that concurrent calls do
/// not corrupt state, and relying on that is worse than calling it early.
[[nodiscard]] Status init();

/// True iff init() has completed successfully.
[[nodiscard]] bool is_initialised() noexcept;

/// The linked libsodium's version string, e.g. "1.0.22". Never null; returns
/// "uninitialised" before init(). Exposed so a build can RECORD which
/// libsodium it linked (security-model.md §12's CVE requirement needs an
/// answer to that at runtime, not only at configure time) and so the test
/// suite can assert it rather than trusting third_party/libsodium/VERSION.
[[nodiscard]] const char* library_version() noexcept;

// --- key generation --------------------------------------------------------

/// Generate a fresh X25519 keypair from the platform CSPRNG.
/// Replaces whatever `out` held.
[[nodiscard]] Status generate_device_keypair(DeviceKeyPair& out);

/// Generate a fresh 256-bit CEK from the platform CSPRNG.
/// Replaces whatever `out` held.
[[nodiscard]] Status generate_content_key(ContentKey& out);

/// Generate a fresh Argon2id salt from the platform CSPRNG.
///
/// A separate function rather than something derive_key_from_passphrase()
/// does internally, because the salt must be STORED and reused on every
/// later derivation for the same passphrase — if derivation invented its own
/// salt, no derived key could ever be reproduced. Making the caller hold the
/// salt is what makes that obligation visible.
[[nodiscard]] Status generate_salt(Salt& out);

// --- authenticated encryption ---------------------------------------------

/// Seal `plaintext` under `key`, binding `aad`, with a fresh random nonce.
///
/// `aad` is authenticated but NOT encrypted, and it is where framing that
/// must not be swappable belongs: `security-model.md` §5's
/// `blob_id || chunk_index || total_chunks` is the canonical example. Pass
/// no_aad() if there is genuinely nothing to bind — but prefer binding
/// something, because AAD is the only defence against an attacker moving a
/// valid ciphertext to a different place in the data model.
///
/// Errors: CryptoNotInitialised, CryptoKeyInvalid (key not valid()),
/// CryptoMessageTooLarge, InvalidArgument (null data with nonzero size),
/// Internal (libsodium refused, which should not happen).
///
/// On any error `out` is left empty — never partially filled.
[[nodiscard]] Status seal(const ContentKey& key, ByteView plaintext, ByteView aad,
                          SealedBlob& out);

/// Open `blob` under `key` with `aad`, writing plaintext into `out`.
///
/// `out` is a SecureBytes because recovered plaintext IS the asset A1 that
/// `security-model.md` §1 ranks first. Returning it in a std::vector would
/// leave decrypted document content in unzeroed heap, which is the exact
/// failure secure_bytes.h exists to prevent, and callers would have no way
/// to fix it from outside.
///
/// FAILURE BEHAVIOUR, which is a security property and is tested as one:
///   * Wrong key, flipped ciphertext bit, truncated ciphertext, substituted
///     nonce and mismatched AAD all return the SAME Status::CryptoAuthFailed.
///     Distinguishing them would be a decryption oracle (see status.h).
///   * On ANY failure `out` is reset to empty. No partial, unauthenticated
///     plaintext is ever handed back, and the test suite asserts
///     out.empty() for every failure mode rather than trusting it.
///   * A blob too short to hold a tag is CryptoSealedBlobBad, not a crash —
///     this is the untrusted-input case (spec §28: a synced blob is
///     untrusted input even though it used to be ours).
[[nodiscard]] Status open(const ContentKey& key, const SealedBlob& blob, ByteView aad,
                          SecureBytes& out);

// --- key wrapping ----------------------------------------------------------
//
// WHAT THESE DO: derive a key-encryption key (KEK) from an X25519 agreement
// between the wrapping device's keypair and the recipient device's public
// key, then AEAD-seal the CEK under it. This is `security-model.md` §5A.2's
// "seals the workspace keys to the new device's public key", at the
// primitive level.
//
// DIVERGENCE FROM security-model.md §5A.2, recorded because it is a real
// design question and not a detail: §5A.2 names `crypto_box_seal`.
// crypto_box_seal uses an EPHEMERAL sender keypair, so it authenticates
// NOBODY — the recipient learns only that someone who had its public key
// sealed this. That is sometimes what you want (it gives the sender
// anonymity), but for "share a document's key with another of the user's own
// devices" the recipient should be able to tell WHICH device sent it: T-305
// is "a malicious/compromised device pushes poisoned sync data", and an
// unauthenticated key-wrap channel is a way to do that.
//
// So this module implements the AUTHENTICATED variant, using libsodium's
// crypto_kx (X25519 + BLAKE2b, with the two public keys hashed into the
// session keys) rather than crypto_box_seal. Consequences:
//   * both parties' STATIC keys contribute, so the recipient authenticates
//     the wrapper;
//   * crypto_kx's rx/tx split makes the KEK DIRECTIONAL, so a wrapped key
//     cannot be replayed back at the wrapper as though the recipient had
//     sent it;
//   * the module has ONE AEAD (XChaCha20-Poly1305) rather than two —
//     crypto_box uses XSalsa20-Poly1305 — so there is one nonce discipline
//     to reason about instead of two;
//   * it does NOT give sender anonymity, which crypto_box_seal does.
//
// **security-model.md §5A.2 does not currently say which property it wants,
// and that ambiguity is the actual gap.** It is flagged in that document and
// in docs/spikes/SP-9-findings.md §7 for a human decision. If anonymous
// sealing turns out to be required for enrolment, it is an ADDITIONAL
// function here, not a replacement — the two have different security
// properties and picking one by accident is how protocols break.
//
// The role assignment is fixed by which function you call: the wrapper is
// crypto_kx's "client" and uses its tx key; the recipient is the "server"
// and uses its rx key. That is why these are two functions with different
// signatures rather than one symmetric `derive_kek()`.

/// Wrap `cek` so that only the holder of `recipient_public` can unwrap it,
/// authenticated as coming from `wrapper`.
///
/// Errors include CryptoKeyInvalid if `recipient_public` is not a usable
/// X25519 public key — an all-zero or low-order point drives the agreement
/// to a known value, and libsodium's crypto_scalarmult rejects exactly that
/// case. We propagate the rejection instead of proceeding with a predictable
/// KEK.
[[nodiscard]] Status wrap_content_key(const DeviceKeyPair& wrapper,
                                      const PublicKey& recipient_public, const ContentKey& cek,
                                      WrappedKey& out);

/// Unwrap a CEK sealed by `wrapper_public` to `recipient`.
///
/// Returns CryptoAuthFailed — not a more specific code — when the wrapped
/// key was sealed by a different device, to a different device, or has been
/// tampered with. All three are "this did not authenticate", for the same
/// oracle reason open() gives.
[[nodiscard]] Status unwrap_content_key(const DeviceKeyPair& recipient,
                                        const PublicKey& wrapper_public,
                                        const WrappedKey& wrapped, ContentKey& out);

// --- subkey derivation (HKDF) ---------------------------------------------
//
// ADDED 2026-10-20, when the layer that needed it arrived. NOT IMPLEMENTED
// item 1 below used to say: "`crypto_kdf_derive_from_key` is the libsodium
// primitive that HKDF(WSK, doc_id) will use; it is not wrapped yet, because
// wrapping it without the hierarchy to use it would be an unused API with a
// security-critical context string." `security/keys` is that hierarchy, so
// the function is now wrapped — and with ONE deliberate change from what
// that sentence predicted, recorded here because it is a real choice:
//
// **It is crypto_kdf_hkdf_sha256, not crypto_kdf_derive_from_key.**
// crypto_kdf_derive_from_key's context is a FIXED 8 BYTES and its subkey
// selector is a uint64. `security-model.md` §5A.1 writes the edges of the
// hierarchy as `HKDF(WSK, doc_id)` and `HKDF(DocKey, blob_id)`, and a doc_id
// in this codebase is a string identifier, not a u64 (core/blocks). Hashing
// a string id down to a u64 to fit the API would introduce a collision
// domain — two documents sharing a derived key — that the written design
// does not have, to save nothing. libsodium ships HKDF (RFC 5869) since
// 1.0.19 and §5A.1 literally says HKDF, so this is the construction the
// design names rather than a substitution.
//
// Extract-then-expand, using libsodium's two-call API:
//   PRK = HKDF-Extract(salt = label, IKM = parent key)
//   out = HKDF-Expand(PRK, info, 32)
// The LABEL is the salt and the ID is the info, which is the division RFC
// 5869 §3.1/§3.2 describes: the label names the EDGE of the hierarchy (a
// compile-time constant, one per edge, versioned) and the info names WHICH
// child along that edge. Swapping the two would make every URK-to-WSK key
// collide with every WSK-to-document key that happened to share an id.

/// Longest domain-separation label. Labels are compile-time constants in
/// `security/keys` ("uapp/keys/v1/..."), never caller data.
inline constexpr std::size_t kMaxKdfLabelBytes = 64;

/// Longest derivation info (a workspace id, a document id, a blob id).
inline constexpr std::size_t kMaxKdfInfoBytes = 256;

/// Derive a 256-bit subkey from `parent` under `label` and `info`.
///
/// DETERMINISTIC and device-independent: the same (parent, label, info)
/// yields the same subkey everywhere, forever. That is what lets
/// `security-model.md` §5A.1 store ONE wrapped key per workspace instead of
/// one per document — the document key is recomputed, never stored. It also
/// means a change to a label is a change to every key below it, which is why
/// the labels carry a version segment.
///
/// `label` is MANDATORY and must be non-empty: an unlabelled derivation is
/// how two different edges of a hierarchy end up producing the same key from
/// the same parent. `info` may be empty, for an edge with exactly one child
/// (the recovery-secret-to-URK edge is the only such edge today).
///
/// This does NOT take a passphrase and is NOT a password KDF. HKDF is fast
/// by design; it assumes `parent` is already a uniformly random 256-bit key.
/// Deriving from something a human chose is derive_key_from_passphrase().
///
/// Errors: CryptoNotInitialised, CryptoKeyInvalid (parent not valid()),
/// InvalidArgument (empty/null label, null-with-size view, or either input
/// over its cap), CryptoSecureAllocFailed, Internal.
[[nodiscard]] Status derive_subkey(const ContentKey& parent, ByteView label, ByteView info,
                                   ContentKey& out);

// --- passphrase key derivation --------------------------------------------

/// Derive a 256-bit key from a passphrase and salt using Argon2id.
///
/// DETERMINISTIC: the same (passphrase, salt, profile) always yields the same
/// key, on every device and every libsodium >= 1.0.15. That is the whole
/// point — it is how `security-model.md` §5A.4's recovery path re-derives a
/// key on a new device from something the user knows — and it is why the
/// algorithm and both cost parameters are pinned by value in this module
/// rather than taken from libsodium's defaults.
///
/// The SAME salt must be reused for the same passphrase, and a DIFFERENT
/// salt must be used for a different purpose. A salt is not a secret; it is
/// a uniqueness device, and reusing one across two users makes their derived
/// keys comparable.
///
/// `profile` is explicit at every call site, with no default argument, so
/// that "which cost did this key cost?" is answerable by reading the call.
/// A key derived at MobileInteractive cannot be re-derived at
/// DesktopModerate — the profile is part of the key's identity and must be
/// stored with the salt.
///
/// Errors: CryptoPassphraseTooShort, CryptoKdfFailed (Argon2id could not
/// allocate its memory — expected and handleable on a floor-tier device, NOT
/// a reason to abort), CryptoNotInitialised.
[[nodiscard]] Status derive_key_from_passphrase(ByteView passphrase, const Salt& salt,
                                                KdfProfile profile, ContentKey& out);

/// The (opslimit, memlimit) a profile resolves to. Exposed so that the
/// values can be ASSERTED by a test and recorded in a document, rather than
/// living only inside a switch in the .cpp where a change would be invisible.
void kdf_parameters(KdfProfile profile, unsigned long long& opslimit,
                    std::size_t& memlimit_bytes) noexcept;

}  // namespace uapp::crypto

#endif  // UAPP_SECURITY_CRYPTO_CRYPTO_H
