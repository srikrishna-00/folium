# `third_party/libsodium` — a **pin and a licence**, not a vendored tree

> **This directory contains no libsodium code.** It contains the version pin,
> the licence texts verified against the bytes installed on this machine, and
> the record of what is still owed. The library itself comes from the
> **Homebrew prefix** on the dev host — `brew install libsodium` — which is a
> LOCAL DEV BOOTSTRAP in exactly the sense `third_party/pdfium`'s prebuilt
> `.dylib` is one. Neither is what ships on Android or iOS.
> See `VERSION` and `docs/spikes/SP-9-findings.md` §6.

## What is here

| Path | Committed? | What it is |
|---|---|---|
| `VERSION` | yes | The pin: version, upstream tarball URL + SHA-256, the dev-host bottle's hashes, the licence verification, and the re-roll procedure. The audit trail. |
| `LICENSE` | yes | The ISC text, copied byte-for-byte out of the installed prefix. |
| `AUTHORS` | yes | The contributor list the ISC attribution obligation points at. |
| — | — | **No `include/`, no `lib/`, no fetch script.** See below. |

## Why this looks different from `third_party/pdfium`

The two dependencies have genuinely different shapes, and copying PDFium's
arrangement mechanically would have produced a worse result.

| | PDFium | libsodium |
|---|---|---|
| Upstream release artefact | **none** — branch heads only (SP-1 F-1) | numbered, signed release tarballs |
| Upstream signature | none | minisign (Ed25519) **and** GPG |
| Upstream mobile build | **none** — GN/gclient, Chromium's `build/` tree, 8 vendored sub-projects (SP-1 F-3) | first-party `dist-build/apple-xcframework.sh` and `dist-build/android-aar.sh` in the upstream repo |
| Transitive deps | abseil, ICU, FreeType, libjpeg-turbo, lcms, libopenjpeg, partition_alloc | **none** (plain C99 + libc) |
| How we get it today | prebuilt `.dylib` from a third-party distributor, fetched by a script, pinned by SHA-256 | a Homebrew bottle, found by `pkg-config` |
| How CMake finds it | `find_library` with `NO_DEFAULT_PATH` into our own pinned tree | `pkg_check_modules` / `find_library` on the **system** paths |

That last row is the one to argue, because it inverts PDFium's rule.
`cmake/UappPdfium.cmake` refuses to look at system paths, on the grounds that
"silently falling back to a system-wide libpdfium of unknown vintage would
defeat the whole point of pinning a SHA-256". `cmake/UappSodium.cmake`
deliberately does the opposite, and the difference is not laziness:

- There is no pinned libsodium tree here to prefer. Creating one would mean
  committing a 1.1 MB binary (which `.gitignore` excludes anyway, same as
  PDFium) or writing a build script — and the build script *is* the owed
  mobile work, not a dev-host convenience.
- libsodium's whole interface is 32-byte keys, 24-byte nonces and
  byte-identical published test vectors. An unknown-vintage libsodium cannot
  silently produce different ciphertext the way an unknown-vintage PDF parser
  can silently render a different page — and `tests/crypto` asserts the
  version string *and* three external published test vectors, so a surprise
  copy is a red test rather than a quiet behaviour change.
- The dependency is **only** reachable when `UAPP_ENABLE_CRYPTO` resolves ON,
  and the configure output prints the resolved library path and version so
  "which libsodium did I just link?" is answerable from the build log.

The honest summary: PDFium's rule buys protection we actually need there and
would buy us almost nothing here, while costing the thing we do need — a
build that configures offline against whatever the host already has.

## Why `cmake/UappSodium.cmake` never touches the network

Same rule as PDFium, for the same reason, stated in
`third_party/pdfium/README.md` and in `tests/support/test_framework.h`:
`cmake -S . -B build` must work offline, so there is no `FetchContent` and no
`file(DOWNLOAD)`. The acquisition step is one explicit command run by a human
or by CI:

```sh
brew install libsodium            # macOS dev host
# apt install libsodium-dev       # Debian/Ubuntu CI
cmake -S . -B build -DUAPP_ENABLE_CRYPTO=ON
cmake --build build -j
ctest --test-dir build -R uapp.crypto --output-on-failure
```

Without it, `cmake -S . -B build` still configures and builds everything
else; `UAPP_ENABLE_CRYPTO` comes out OFF and `security/crypto/` is skipped.
Passing `-DUAPP_ENABLE_CRYPTO=ON` with no libsodium present is a hard
configure error with the install command in the message — an explicit request
is never silently downgraded. That is the PDFium tristate contract, unchanged.

## What is still owed (do not let this directory become "crypto: done")

1. **Signature verification.** Upstream signs every release with minisign;
   we have not checked one. The SHA-256 in `VERSION` comes from Homebrew's
   formula, i.e. from a second party, not from upstream's signature. This is
   the cheapest unclaimed win in the whole dependency and it is **NOT
   IMPLEMENTED**. Note the contrast with PDFium, where the equivalent gap is
   unfixable in principle because there is no upstream release to sign.
2. **Mobile.** Nothing here has been built for `android-arm64` or
   `ios-arm64`. The good news (and it is a real difference from SP-1's
   findings on PDFium) is that upstream ships the build scripts itself:
   `dist-build/apple-xcframework.sh` produces an xcframework covering iOS,
   Catalyst, tvOS, watchOS, visionOS and their simulators, and
   `dist-build/android-aar.sh` covers all Android ABIs. So the owed work is
   "run upstream's script from a signed tarball in a hermetic container", not
   "re-express someone else's build system". Budget accordingly — this is the
   cheap end of SP-1's cost model, not the expensive end.
3. **`--enable-minimal` decision.** The minimal build strips `crypto_pwhash`
   (Argon2id) and `crypto_box_seal`. `security/crypto` needs `crypto_pwhash`
   and `crypto_kx`, so minimal is almost certainly wrong for us, but the size
   delta has not been measured against the 120 MB install budget
   (`performance-strategy.md` §2.1). **NOT MEASURED.**
4. **Static linking, measured.** The dev host links the dylib (whatever
   `pkg-config` hands us). The shipping arrangement is a static archive
   inside the app binary, and SP-1 F-5's criterion — stripped,
   dead-code-eliminated *contribution to the linked binary*, per architecture
   — is unmeasured here too.
5. **The export-control deliverable.** This is the finding that makes
   libsodium different from every dependency vetted so far, and it is **not
   an engineering task**: shipping a bundled crypto library that provides
   *content confidentiality* makes the app's encryption non-exempt for Apple's
   purposes, which pulls in an EAR Category 5 Part 2 self-classification
   report to BIS/NSA and a France declaration. See
   `docs/spikes/SP-9-findings.md` §4 and the new risk **R-33**. Nothing in
   this repo currently owns it.
6. **Hardware-backed keystore integration.** `security-model.md` §5A says
   device keys are generated in and never leave the Secure Enclave /
   StrongBox. This module generates them in **ordinary process memory**
   (guarded by `sodium_malloc`, which is not the same thing). That is the
   `security/key-management` adapter named in `architecture.md`'s layer
   diagram, it does not exist, and `security/crypto`'s header says so at the
   point where it would otherwise look like it had been handled.
7. **CVE monitoring.** `security-model.md` §12 requires automated CVE
   monitoring against the pinned set. libsodium's history is unusually clean,
   which is an argument for choosing it and *not* an argument for skipping
   the monitoring. Nothing automates it yet.
