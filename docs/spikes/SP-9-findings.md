# SP-9 Findings — Cryptographic library selection and vetting

**Status:** Investigated **and implemented** (unlike SP-1/SP-5/SP-6, which were desk research).
`security/crypto` exists, builds, and is tested — see §8.
**Date:** 2026-10-17
**Spike definition:** **NEW.** SP-9 is **not** one of the eight spikes in
`technical-requirements.md` §6. That table was written before `adr/0002`
decision 1 made E2EE a v1.0 property, and it contains no crypto-dependency
spike at all. This document adopts the SP-1/SP-6 findings format because the
*question* is the same shape as SP-1's — "can we actually ship this
dependency on mobile, and on what terms?" — and because the project already
has a precedent for how a dependency gets vetted and that precedent should be
followed rather than reinvented.
**Blocks:** Phase 12 (sync/E2EE) entirely. `adr/0002` decision 1.
**New risk raised:** **R-33** (§4). No existing risk is downgraded.

> **Headline: the licence is a non-event and the real constraint is US export
> control.** libsodium is ISC, with no static-vs-dynamic distinction, so
> static linking into a closed-source App Store binary is unambiguously
> permitted — a materially easier answer than FreeType's FTL election or
> PDFium's unresolved AGG question. What is *not* answered anywhere in this
> repo is that bundling a crypto library which provides content
> confidentiality makes the app's encryption **non-exempt** for Apple's
> purposes, which pulls in an EAR Category 5 Part 2 self-classification
> report to BIS and the NSA encryption coordinator, plus a France
> declaration. That is an operational/legal deliverable with no owner. Read
> §4.
>
> **Second headline: `security-model.md` §5's Argon2id parameters cannot run
> on `adr/0002`'s own device floor.** §5 specifies `memlimit = 256 MiB`; a
> 2 GB device cannot afford that. The implementation uses 64 MiB and the
> deviation is annotated in §5 rather than absorbed. Read §5.

---

## 1. The question

> We need authenticated encryption, X25519 key agreement, a password-based
> KDF and secure memory handling, for a C++17 core that ships on iOS and
> Android. Spec §38 and basic practice forbid implementing any of it
> ourselves. **Which library, and can we actually ship it?**

`security-model.md` §5A already answers the first half, in passing: "The
cryptography here is the easy part — libsodium does it." That sentence is
correct and it is also the whole of the vetting that had been done. §5A.1,
§5A.2 and §5 further name XChaCha20-Poly1305, X25519, Argon2id and
`crypto_box_seal` — i.e. the *libsodium* vocabulary — so the library choice
was effectively made during Phase 0 without a licence review, a pin, a
`THIRD_PARTY_NOTICES` row or a mobile-distribution analysis. All four are
what §12 ("no dependency is added without a licence file, an owner, and a
`THIRD_PARTY_NOTICES` row — enforced by CI (NFR-8)") requires, and none
existed. This spike supplies them.

So the question is deliberately split, SP-1-style, because conflating the
parts is what makes dependency decisions look binary:

| Sub-question | Answerable independently? |
|---|---|
| Q-A: Is libsodium the right library on technical grounds? | Yes — §2, §3 |
| Q-B: Does its licence permit closed-source commercial static linking on both app stores? | Yes — §4.1 |
| Q-C: Are there **other** mobile-distribution constraints? | **Yes, and this is the finding** — §4.2 |
| Q-D: Can its parameters meet `adr/0002`'s device floor? | **Partly. Argon2id cannot, as written** — §5 |
| Q-E: Can it be vendored to the same bar as PDFium? | Yes, and more cheaply — §6 |

## 2. Method

1. Installed libsodium on the dev host (`brew install libsodium`) and read
   the **actual** LICENSE, AUTHORS and `sbom.spdx.json` in the installed
   prefix, rather than upstream's website. Hashed the licence file.
2. Read the installed headers for the real API surface and the real constant
   values — `crypto_aead_xchacha20poly1305.h`, `crypto_kx.h`,
   `crypto_pwhash_argon2id.h`, `utils.h` — rather than relying on
   documentation for parameter values that end up compiled into key
   derivations.
3. Ran the published **draft-irtf-cfrg-xchacha-03 §A.3.1** AEAD vector and
   the **RFC 7748 §6.1** X25519 vector against the installed library before
   writing any wrapper, to confirm the library does what the standards say.
4. **Measured** Argon2id at each of libsodium's three presets plus the chosen
   pair, on real hardware, instead of quoting the documentation's "0.7 s on a
   2.8 GHz Core i7".
5. Checked the licence against the same criteria SP-1 applied to PDFium:
   commercial redistribution, static vs. dynamic linking, App Store
   distribution, attribution obligations, and the "did the licence change
   between versions?" failure mode (`THIRD_PARTY_NOTICES` rule 3).
6. Investigated the mobile *distribution* story specifically — upstream build
   support for iOS/Android, and the App Store encryption declaration.
7. Compared the whole dependency shape against `third_party/pdfium`, to
   decide which parts of that precedent to copy and which to deliberately
   deviate from.

## 3. Findings — technical (Q-A)

### F-1 — libsodium covers every primitive the design already names, and nothing it names is missing.

Checked against `security-model.md` §5 and §5A.1 item by item, from the
installed headers rather than the docs:

| Design requirement | libsodium API | Present in 1.0.22? |
|---|---|---|
| XChaCha20-Poly1305 AEAD (§5, §5A.1) | `crypto_aead_xchacha20poly1305_ietf_*` | yes — 32 B key, **24 B nonce**, 16 B tag |
| X25519 device wrapping key (§5A.1) | `crypto_kx_*` (X25519+BLAKE2b), `crypto_scalarmult` | yes |
| Sealing to a public key (§5A.2) | `crypto_box_seal` | yes |
| Argon2id passphrase KDF (§5) | `crypto_pwhash`, `ALG_ARGON2ID13` | yes |
| HKDF-style subkey derivation (§5, §5A.1) | `crypto_kdf_derive_from_key` (BLAKE2b), `crypto_kdf_hkdf_sha256/512` | yes — **not wrapped yet**, see §7 |
| Ed25519 identity/signing key (§4, §5) | `crypto_sign_*` | yes — **not wrapped yet**, see §7 |
| Secure memory (not previously specified) | `sodium_malloc`, `sodium_memzero`, `sodium_mlock`, `sodium_memcmp` | yes |

There is no gap between what the design asks for and what the library has.
That is a stronger position than SP-1 found for PDFium, where the design's
"no XFA, no V8, no Skia" had to be reconciled with a build system we do not
control.

### F-2 — The 192-bit nonce is the reason to pick this library, not a detail.

`security-model.md` §5 already specifies XChaCha20-Poly1305 and §5's chunked
blob design already relies on a nonce wide enough to derive deterministically
per chunk. What had not been written down is *why* that width is the
load-bearing property, and it is worth stating because it determines whether
a stateless random-nonce design is correct:

- XChaCha20-Poly1305 takes a **192-bit** nonce. Random nonces collide with
  probability `< q²/2^193` for `q` encryptions under one key: `2^-129` at
  `q = 2^32`, `2^-65` at an absurd `q = 2^64`.
- AES-GCM takes a **96-bit** IV. At `q = 2^32` the collision probability is
  already ~`2^-33`, which is why NIST SP 800-38D caps a random-IV AES-GCM key
  at `2^32` invocations.
- ChaCha20-Poly1305-IETF also takes 96 bits and inherits the same cap.

So XChaCha20-Poly1305 is the only one of the three for which "generate a
fresh random nonce inside the encrypt call and never let the caller supply
one" is sound without a counter, and a counter would need durable per-key
state this layer must not invent. **The primitive choice and the nonce
strategy are one decision, not two.** `security/crypto`'s `seal()` has no
nonce parameter at all, and the full argument including the
counter-vs-random trade is in
`security/crypto/include/uapp/crypto/crypto.h`.

### F-3 — Audit and CVE history support the choice, but should not be used to skip monitoring.

libsodium has been externally audited (the 2017 Private Internet Access /
Cure53 review of 1.0.12) and has an unusually clean CVE record for a
cryptographic library of its age and deployment footprint. It is also the
library behind Signal-adjacent tooling, WireGuard userspace implementations,
and a very large number of language bindings — i.e. there is real adversarial
attention on it.

This is a reason to prefer it over, say, writing a ChaCha20 ourselves or
pulling in a less-exercised alternative. It is **not** a reason to skip
`security-model.md` §12's automated CVE monitoring, and the temptation to
treat "libsodium is safe" as a standing conclusion is exactly how a pinned
dependency goes stale. Nothing automates that monitoring yet
(`third_party/libsodium/README.md`, owed item 7).

### F-4 — Alternatives, and why each loses.

Recorded so the decision is reviewable rather than asserted:

| Candidate | Licence | Why not |
|---|---|---|
| **libsodium** | ISC | **CHOSEN.** See above. |
| OpenSSL / BoringSSL | Apache-2.0 / mixed | Vastly larger surface for the four primitives we need; BoringSSL has no stable API or releases and explicitly tells third parties not to depend on it — the same "branch heads, not releases" problem SP-1 F-1 found fatal for PDFium's build. Would be the answer if we needed TLS, and we do not: TLS comes from the platform stack. |
| Monocypher | CC0 / BSD-2 | Attractively tiny and genuinely good, but no Argon2id (Argon2i only), no secure allocator, and a much smaller audit/deployment footprint. The missing Argon2id is disqualifying given §5. |
| Apple CryptoKit + Android Keystore/Tink | platform | **No portable implementation**, which collides directly with ADR-0001's C/C++-core mandate: we would need two implementations plus a desktop third, and the same key would have to be derivable identically on all of them (§5A.4's recovery path). Worth revisiting **only** for the hardware-backed key storage layer, which is where the platform genuinely wins and where `security/key-management` will have to live anyway. |
| libhydrogen | ISC | Designed for microcontrollers; no Argon2id, no X25519 `crypto_kx` equivalent at the same maturity. |
| Hand-rolled | — | Prohibited by spec §38 and by sense. |

## 4. Findings — licence and mobile distribution (Q-B, Q-C)

### 4.1 — The licence is clean, and cleaner than two dependencies already in the tree. (Q-B: PASS)

Verified against the actual file at
`/opt/homebrew/Cellar/libsodium/1.0.22/LICENSE`, copied byte-for-byte to
`third_party/libsodium/LICENSE` (sha256
`508a76d186356c0dd807a670ef510964f8724557024796a2c426c6c0e19ab683`):

> ISC License — Copyright (c) 2013-2026 Frank Denis. "Permission to use,
> copy, modify, and/or distribute this software for any purpose with or
> without fee is hereby granted, provided that the above copyright notice and
> this permission notice appear in all copies."

Against SP-1's criteria for PDFium:

| Criterion | libsodium |
|---|---|
| Closed-source commercial redistribution | **Permitted.** No source-disclosure trigger of any kind. |
| **Static linking** into a proprietary binary | **Permitted.** There is no static-vs-dynamic distinction anywhere in the text. This is the material contrast with LGPL (`THIRD_PARTY_NOTICES` SECTION 2) and with FreeType's FTL, where the licence choice has consequences. |
| Apple App Store distribution | **No licence obstacle.** The GPL-vs-App-Store incompatibility that puts libheif in SECTION 2 does not arise. |
| Attribution obligation | **Yes, and it is the only obligation**: reproduce the copyright and permission notice. Satisfied by `THIRD_PARTY_NOTICES` plus the in-app licences screen, same as every SECTION 1 entry. |
| Patent grant | **None** — ISC grants no patent licence. Same as BSD-2/BSD-3, which the project has already accepted eight times over. Not a new exposure. |
| Patent retaliation / termination | None. |
| Licence stability across versions (`THIRD_PARTY_NOTICES` rule 3) | ISC since 2013; the only movement in the text is the copyright year range. The re-roll procedure in `third_party/libsodium/VERSION` treats **any** other diff as a stop condition — this is the AGG-2.4-to-2.5 and OpenCV-BSD-to-Apache failure mode, and it is cheap to guard here. |

**Corroborating, not determinative:** the Homebrew bottle ships
`sbom.spdx.json` whose `licenseConcluded` is `ISC` (its `licenseDeclared` is
`NOASSERTION` — Homebrew records the formula's field, it does not scan). The
determination above rests on the licence file.

`THIRD_PARTY_NOTICES` SECTION 1 already listed `libsodium ... ISC ... AEAD,
KDF, signatures` as approved. **That row is now confirmed against the bytes
rather than against upstream's documentation**, and the dependency has moved
to SECTION 0.

### 4.2 — THE FINDING: shipping libsodium makes the app's encryption non-exempt under US export control. (Q-C: a real, unowned obligation)

This has nothing to do with the licence, which is why reading only the
licence would have missed it, and it is the reason this spike exists rather
than being a one-line note in `THIRD_PARTY_NOTICES`.

**What Apple requires.** An app that "uses, accesses, contains, implements or
incorporates encryption" is an export of encryption software and must declare
`ITSAppUsesNonExemptEncryption` in `Info.plist`. Apple's exemptions cover,
in substance: encryption provided **by the operating system** and called
through Apple's own APIs; HTTPS/TLS to one's own backend; and encryption used
**only** for authentication, signing or integrity. Our case matches none of
them:

- the encryption is in a **bundled third-party library**, not the OS;
- it is used for **content confidentiality** (asset A1), not authentication;
- `adr/0002` decision 1 makes that the product's headline property, not an
  incidental one.

So `ITSAppUsesNonExemptEncryption = YES`, and the app falls under **EAR
Category 5 Part 2**.

**What that costs.** The mass-market path is **License Exception ENC,
15 CFR 740.17(b)(1)**, which requires a **self-classification report emailed
to BIS and to the ENC Encryption Request Coordinator (NSA) before export, and
again annually**. A CCATS classification request is needed only for the
(b)(2)/(b)(3) categories, which standard AES/ChaCha20/X25519 at these key
lengths in a mass-market app should not fall into — but "should not" is a
lawyer's call, not an engineer's. France separately requires a declaration
for distribution there, which is why App Store Connect asks about it.

**Three things are worth being precise about, because each is easy to get
wrong in the opposite direction:**

1. **This is not a libsodium problem.** It follows from `adr/0002` decision 1.
   Any library that encrypts user content lands in the same place. Choosing
   Monocypher or writing our own would not avoid it; only *not shipping
   E2EE* would, and that decision is made.
2. **Using Apple's CryptoKit instead would probably not avoid it either**, and
   the common belief that it would is a misreading. The exemption is about
   encryption *provided by the OS*; whether using OS-provided AES for
   first-party content confidentiality qualifies is precisely the sort of
   question that needs counsel. We should not design around a guess — and in
   any case F-4 already rules CryptoKit out on portability.
3. **It is not an engineering deliverable.** It is a release-process one, it
   has a recurring annual component, and **nothing in this repo owns it.**
   `security-model.md` §12 covers supply-chain security and says nothing
   about export control; `deployment.md` covers release mechanics and says
   nothing either.

**Action: new risk R-33, added to `docs/risk-register.md`.** Owner: product
lead (it needs counsel, not a compiler). Trigger: the first App Store
submission that contains `security/crypto`. **This is not a blocker for the
primitives layer** — it is a blocker for *submission*, which is why it must be
written down now rather than discovered during the first release.

### 4.3 — Upstream's mobile build support is a genuine advantage over PDFium. (Q-E, partly)

SP-1's central finding about PDFium was F-1/F-3: no release tarball, no
standalone build, eight Chromium-pinned sub-projects, so the only honest path
is a hermetic container build of someone else's build system, re-rolled
forever. **None of that applies here**, and the contrast is worth recording
because it should change the effort estimate:

| | PDFium (SP-1) | libsodium (SP-9) |
|---|---|---|
| Release artefact | none — branch heads | numbered tarballs |
| Signed | no | **minisign (Ed25519) + GPG** |
| Upstream mobile build | none | **`dist-build/apple-xcframework.sh`** (iOS, Catalyst, tvOS, watchOS, visionOS + simulators) and **`dist-build/android-aar.sh`** (all ABIs) |
| Transitive deps | 8 | **0** (plain C99 + libc) |
| OS version floor | iOS 17 in the only green recipe (SP-1 F-6) | none beyond libc — far below iOS 15 / API 26 |
| Owed mobile work | own a hermetic GN/gclient container build, ~0.5 ew/quarter standing | run upstream's script from a signed tarball in a container |

Note what this does to SP-1 F-6, the risk that mattered most there: libsodium
has **no deployment-target risk at all**. It is plain C99 with no platform
API surface, so `adr/0002`'s floor is a non-issue for the library. The floor
constrains the *parameters* (§5), not the build.

## 5. Findings — the device floor conflicts with the written KDF parameters (Q-D)

`security-model.md` §5 says:

> "Nothing derives a key from a password on the client without Argon2id
> (`opslimit=4`, `memlimit=256 MiB` locally, where we can afford it)."

**On `adr/0002` decision 3's floor — 2 GB RAM, iOS 15+, Android 8+ — we
cannot afford it.** A 256 MiB transient allocation on top of the app's own
working set is a foreground low-memory kill on both platforms. The
parenthetical "where we can afford it" was written before the floor was
fixed; once it was fixed, the answer became no, and nobody went back to §5.

**Measured** (Apple M4 / 16 GB, `-O2`, 32-byte output, best of 5-7 runs —
and note these are *desktop* numbers):

| Profile | opslimit | memlimit | measured here | floor-device estimate |
|---|---|---|---|---|
| libsodium INTERACTIVE | 2 | 64 MiB | 42 ms | ~0.3-0.4 s |
| **chosen: MobileInteractive** | **3** | **64 MiB** | **61 ms** | **~0.4-0.6 s** |
| libsodium MODERATE (= §5's memory) | 3 | 256 MiB | 277 ms | **unrunnable** |
| libsodium SENSITIVE | 4 | 1 GiB | 1563 ms | **unrunnable** |

**The chosen pair is libsodium's own documented procedure applied to our
constraint, not an invented one.** libsodium's `pwhash` documentation says:
"Set `memlimit` to the amount of memory you want to reserve for password
hashing. Then set `opslimit` to `3` and measure the time it takes to hash a
password." Our memory budget on the floor device is 64 MiB — the largest
preset that fits — so `memlimit = 64 MiB`, `opslimit = 3`. INTERACTIVE's own
`opslimit` is 2; the extra pass costs ~45% more CPU (42 → 61 ms) and buys
back part of the memory-hardness given up by not using MODERATE. Buying on
the cheap axis is the right call when the expensive axis is the hard
constraint.

**The security cost, stated plainly rather than minimised:** a 4x reduction
in `memlimit` is a 4x reduction in an offline attacker's memory cost per
guess, which is the dimension Argon2 exists to raise. This is accepted
because the alternative is a passphrase path that cannot run on the devices
we committed to supporting — and because §5A.4 deliberately does **not** rest
recovery on a user-chosen passphrase: the root of trust is a generated
24-word phrase carrying ~256 bits, for which Argon2's work factor is a
belt-and-braces measure rather than the only defence.

**Two things are owed here and are not done:**

1. **Measure on real floor-tier hardware.** The 0.4-0.6 s above is an
   estimate from a desktop multiplier. If a real 2 GB Android 8 device takes
   3 s, the parameters need revisiting — and this is the same class of gap as
   SP-3/SP-4's "must be re-run on Tier D hardware".
2. **Decide whether a *desktop* client may use a stronger profile.** It
   cannot, for any key that must also be derivable on mobile — the profile is
   part of the key's identity. `DesktopModerate` exists in the API only for
   desktop-only keys, and whether any such key exists is a §5A design
   question that is open.

`docs/security-model.md` §5 is annotated in place with this conflict.

## 6. Findings — how it is vendored, and where this deviates from the PDFium precedent (Q-E)

`third_party/libsodium/` carries the pin, the licence texts verified against
the installed bytes, and the re-roll procedure — the same four artefacts
`third_party/pdfium/` carries. **Three deliberate deviations**, each argued
in `third_party/libsodium/README.md` rather than left as a silent difference:

1. **CMake looks at SYSTEM paths.** `cmake/UappPdfium.cmake` uses
   `NO_DEFAULT_PATH` so that only our pinned copy is linked;
   `cmake/UappSodium.cmake` does the opposite, trying pkg-config first and
   plain `find_library` second. The protection PDFium's rule buys — "never
   link an unpinned parser of unknown vintage" — is worth much less here,
   because libsodium's entire observable behaviour is checked against two
   **external published** test vectors in `tests/crypto`: a surprise copy is
   a red test, not a silent behaviour change. A PDF parser has no equivalent
   check.
2. **No fetch script.** `third_party/pdfium/fetch_pdfium.sh` downloads and
   SHA-256-verifies a prebuilt binary. The equivalent for libsodium would
   either have to *build* it (which is the owed mobile work, not a dev-host
   convenience) or wrap `brew install` (which would dress a convenience up
   as supply-chain tooling). The part worth scripting first is **minisign
   verification of the release tarball**, and that is owed.
3. **No committed payload.** Same as PDFium — `.gitignore` excludes `*.a` and
   `*.dylib`, and the audit trail (version, URL, SHA-256, licence texts) is
   what gets committed.

**Unchanged from the PDFium precedent, because both rules are right:**
CMake never touches the network (`cmake -S . -B build` must configure
offline), and the gate is a tristate — `AUTO` detects, `ON` fails configure
with the install command in the message, `OFF` skips — so an explicit request
is never silently downgraded.

## 7. Findings — two places the written design is less specific than the code needed

Both are flagged for a human rather than resolved by the implementation.

### F-5 — §5A.2 does not say whether device key-wrapping must authenticate the sender.

`security-model.md` §5A.2 mechanism 1 says an enrolled device "seals the
workspace keys to the new device's public key (`crypto_box_seal`)".
`crypto_box_seal` uses an **ephemeral** sender keypair: it authenticates
**nobody**. The recipient learns only that *someone* holding its public key
sealed the bundle. That is a real property — it gives the sender anonymity —
but §5A.2 does not say it was chosen deliberately, and for "share a
document's key with another of the user's own devices" the recipient arguably
*should* be able to tell which device sent it, since T-305 is "a
malicious/compromised device pushes poisoned sync data".

`security/crypto` implements the **authenticated** variant: `crypto_kx`
(X25519 + BLAKE2b over both static public keys) deriving a directional KEK,
then our own XChaCha20-Poly1305 AEAD over the CEK, with both public keys
bound into the AAD. This gives sender authentication and replay-direction
separation, and keeps the module to one AEAD instead of two
(`crypto_box` uses XSalsa20-Poly1305). It gives up sender anonymity.

**Decision needed:** does enrolment want anonymity or authentication? If
both are wanted in different places, the anonymous form is an **additional**
function, never a replacement — they have different security properties and
picking one by accident is how protocols break. `security-model.md` §5A.2 is
annotated with this question.

### F-6 — No external Argon2id known-answer test is reachable through libsodium's API.

Worth recording because it looks like a test gap and is actually a structural
one. RFC 9106's published Argon2id vectors all use **parallelism p = 4**;
libsodium's `crypto_pwhash` **hardcodes p = 1** and exposes no way to change
it. The RFC vectors are therefore unreachable through this API, and no care
in the test file can make them reachable. Reproducing them would mean linking
the Argon2 reference implementation as a second dependency purely to test the
first.

`tests/crypto` therefore uses **golden regression vectors** for Argon2id,
explicitly labelled as such, plus assertions on the resolved parameters. What
they catch is the failure that actually matters: a derivation that silently
changes across a libsodium roll would lock every existing user out of their
own data (§5A.4), and nothing else in the suite would notice. The AEAD and
X25519 layers **do** have external published vectors (§8).

## 8. What was implemented, and what was verified

`security/crypto` — the primitives layer only. `architecture.md`'s layer
diagram already named it ("`security/crypto (libsodium)`", ADAPTERS row), so
it went where the architecture put it rather than into `core/`.

**Implemented:** X25519 device keypair generation; CEK and salt generation
(all CSPRNG-backed); XChaCha20-Poly1305-IETF seal/open with AAD and an
internally-generated random nonce the caller cannot supply; authenticated
CEK wrapping/unwrapping between two device keypairs; Argon2id passphrase
derivation with the three cost profiles; and `SecureBytes`, a
`sodium_malloc`-backed move-only buffer used for every secret **and for
recovered plaintext**.

**Verified, with real output:**

```
$ cmake -S . -B build_cryptoslice -DCMAKE_BUILD_TYPE=Debug -DUAPP_ENABLE_CRYPTO=ON
-- uapp: security/crypto     = ON  (libsodium 1.0.22, /opt/homebrew/lib/libsodium.dylib
                                   — LOCAL DEV BOOTSTRAP, see third_party/libsodium/README.md)
$ ctest --test-dir build_cryptoslice
100% tests passed out of 46          # 39 pre-existing + 7 new crypto entries
$ ./build_cryptoslice/bin/uapp_crypto_tests
27 passed, 0 failed

# Debug + ASan + UBSan, same tree
$ cmake -S . -B build_cryptoasan -DCMAKE_BUILD_TYPE=Debug \
        -DUAPP_ENABLE_CRYPTO=ON -DUAPP_ENABLE_SANITIZERS=ON
$ ASAN_OPTIONS=detect_stack_use_after_return=1:strict_string_checks=1:\
check_initialization_order=1:strict_init_order=1 \
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ctest --test-dir build_cryptoasan
100% tests passed out of 46
Total Test time (real) =  67.62 sec   # vs 23.99 s unsanitised

# the tristate, all three states
-DUAPP_ENABLE_CRYPTO=AUTO (default) -> ON, libsodium 1.0.22 found
-DUAPP_ENABLE_CRYPTO=OFF            -> OFF; 39/46 tests, everything else unaffected
-DUAPP_ENABLE_CRYPTO=ON, nothing findable -> hard configure error with the
                                             install command in the message
```

**The link-graph and header boundary, checked rather than asserted:**

```
# no first-party library outside security/crypto imports a libsodium symbol
libuapp_core_blocks.a: 0   libuapp_core_common.a: 0   libuapp_core_conversion.a: 0
libuapp_core_database.a: 0 libuapp_core_notes.a: 0    libuapp_core_search.a: 0
libuapp_storage_sqlite.a: 0

# exactly two files in the tree include the libsodium header
security/crypto/src/crypto.cpp
security/crypto/src/secure_bytes.cpp

# and security/crypto imports 15 libsodium calls and implements no primitive
crypto_aead_xchacha20poly1305_ietf_{encrypt,decrypt,keygen}
crypto_kx_{keypair,client_session_keys,server_session_keys}
crypto_pwhash  randombytes_buf
sodium_{init,malloc,free,memzero,memcmp,is_zero,version_string}
```

**External published vectors in the suite** (the layer that a round-trip test
cannot provide):

- `draft-irtf-cfrg-xchacha-03` §A.3.1 — the AEAD_XChaCha20_Poly1305 vector,
  driven through the real `crypto::open()`, plus one-bit mutations in both
  the ciphertext and the tag region, plus wrong-AAD and no-AAD rejection.
  Using the **decrypt** direction is deliberate: `seal()` has no nonce
  parameter, and adding a test-only one would be adding the back door the
  nonce strategy exists to forbid.
- `RFC 7748` §6.1 — the Alice/Bob X25519 keypairs, used as real device keys
  through the real wrap/unwrap path, in both directions, plus small-order
  (all-zero and `01||0…0`) peer public-key rejection.

**One honest limit found while testing and asserted so it cannot be
forgotten:** libsodium rejects a peer X25519 public key only when the
agreement output is all-zero, i.e. the small-order subgroup. An all-`0xff`
(non-canonical, high-bit-set) encoding is **accepted** and yields an
ordinary-looking shared secret. That is correct X25519 behaviour, but it
means "we validate the peer key" is narrower than it sounds. Binding a public
key to a *device identity* is §5A.2's out-of-band short-code check, which is
NOT IMPLEMENTED.

## 9. NOT IMPLEMENTED

Deliberate, and listed in full in
`security/crypto/include/uapp/crypto/crypto.h` so it is visible at the point
someone would otherwise assume it had been handled:

1. **The key hierarchy.** No Recovery Secret, URK, Workspace Key or per-
   document key derivation (§5A.1). `crypto_kdf_derive_from_key` is the
   primitive `HKDF(WSK, doc_id)` will use and it is not wrapped, because
   wrapping it without the hierarchy means shipping an unused API with a
   security-critical context string.
2. **Chunked blob encryption** (§5: 1 MiB chunks, deterministic nonce,
   AAD = `blob_id || chunk_index || total_chunks`). `seal()` takes AAD and
   that triple is its intended use; the chunking and index derivation are the
   caller's and do not exist. **This is the one place the design specifies a
   non-random nonce**, and it must arrive as a separately-named entry point
   whose caller owns the index — never as a nonce parameter on `seal()`.
3. **Device enrolment, the short-code check, WSK rotation, revocation**
   (§5A.2, §5A.3). Phase 12.
4. **Ed25519 device identity keys and mutation signing** (§4, §5). This
   module does key *agreement* only. Conflating agreement and signing in one
   keypair is the start of using one key for two purposes.
5. **Hardware-backed key storage.** Keys are generated in ordinary process
   memory, guarded by `sodium_malloc`, which is **not** a Secure Enclave.
   `security/key-management` does not exist.
6. **A C ABI.** Deferred on a stronger argument than the other modules': the
   shape of these functions will change once the hierarchy above exists, and
   an ABI is a promise. `core/capi` flattens all nine new `Status` codes to
   `UAPP_STATUS_UNEXPECTED` with that argument recorded in
   `capi_common.cpp`.
7. **Post-quantum anything** (§5A.6: "X25519 only").
8. **A fuzz target over `crypto::open()`.** Legitimate — a synced blob is
   untrusted input (§28) — but not written, because the attack surface is
   ~15 lines of length arithmetic in front of a library we must not fuzz on
   its behalf, and spec §29's required list names the format parsers.

## 10. Recommendation and estimate impact

**Adopt libsodium. The licence question is closed; the export-control
question is open and needs counsel, not engineering.**

- **Estimate impact on Phase 12:** the *library* risk is lower than the
  Phase 0 estimate assumed, because upstream ships mobile build scripts and
  has zero transitive dependencies (§4.3). There is no libsodium analogue of
  SP-1's iOS-deployment-target risk. The expensive parts of E2EE remain
  exactly where `security-model.md` §5A already said they were — key
  distribution across devices and recovery when a device is lost — and
  nothing in this spike makes them cheaper.
- **New standing cost, not previously budgeted:** the EAR self-classification
  report is **annual**, not one-off (§4.2). Small, recurring, and currently
  unowned.
- **Owed before Phase 12 coding:** verify a release signature with minisign;
  decide the `--enable-minimal` question and measure the size delta; measure
  Argon2id on real floor-tier hardware; resolve F-5 (authenticated vs.
  anonymous sealing); and settle §5A.4's open items, which this spike does
  not touch.

## Sources

- [libsodium documentation](https://doc.libsodium.org/) — [installation / cross-compilation](https://doc.libsodium.org/installation) (minisign key `RWQf6LRCGA9i53mlYecO4IzT51TGPpvWucNSCh1CBM0QTaLn73Y7GFO3`), [the `pwhash` API](https://doc.libsodium.org/password_hashing/default_phf) (the `memlimit`-then-`opslimit=3` procedure quoted in §5)
- [jedisct1/libsodium](https://github.com/jedisct1/libsodium) — [`dist-build/`](https://github.com/jedisct1/libsodium/tree/master/dist-build) (`apple-xcframework.sh`, `android-aar.sh`), [`crypto_pwhash` parallelism is hardcoded to 1](https://github.com/jedisct1/libsodium/issues/993) (§7 F-6)
- The installed prefix itself: `/opt/homebrew/Cellar/libsodium/1.0.22/{LICENSE,AUTHORS,sbom.spdx.json,include/sodium/*.h}`
- [draft-irtf-cfrg-xchacha-03](https://datatracker.ietf.org/doc/html/draft-irtf-cfrg-xchacha-03) §A.3.1 — the AEAD test vector
- RFC 7748 §6.1 — the X25519 test vector; RFC 8439 §2.8.2 — the plaintext/AAD the XChaCha vector reuses
- RFC 9106 — Argon2; its published vectors use `p = 4` (§7 F-6)
- NIST SP 800-38D — the `2^32` invocation limit on a random-IV AES-GCM key (§3 F-2)
- [Apple — Complying with Encryption Export Regulations](https://developer.apple.com/documentation/Security/complying-with-encryption-export-regulations) and the App Store Connect export-compliance flow (§4.2)
- 15 CFR 740.17 (License Exception ENC) — the self-classification report obligation (§4.2)
- [Homebrew core formula for libsodium](https://github.com/Homebrew/homebrew-core/blob/HEAD/Formula/lib/libsodium.rb) — the source of the tarball SHA-256 recorded in `third_party/libsodium/VERSION`
