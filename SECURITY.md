# Security Policy

## Project status

Folium is an early-stage native core. Read this before filing anything: **encryption
primitives and the key hierarchy exist and are tested (`security/crypto`, `security/keys`),
but are not yet wired into document storage.** Nothing in this repository currently provides
confidentiality for real user data — there is no shipped application, and the E2EE design in
[`docs/security-model.md`](docs/security-model.md) is implemented at the primitives layer only.

That said, real security issues are still worth reporting, particularly in:

- `security/crypto` and `security/keys` (cryptographic correctness)
- `file-engine/detection`, `pdf-engine/reader`, `ocr/engine` (untrusted-input parsing — these
  already have fuzz targets or fuzz coverage planned; see `docs/testing-strategy.md`)
- `core/capi` (the C ABI boundary)

## Reporting a vulnerability

**Please do not open a public GitHub issue for a security vulnerability.**

Use GitHub's private vulnerability reporting instead: go to this repository's **Security** tab →
**Report a vulnerability**. That opens a private advisory only the maintainer can see, with:

- A description of the issue and its potential impact
- Steps to reproduce (a minimal repro is extremely helpful)
- The commit hash or version you tested against

We'll acknowledge within a reasonable timeframe and work with you on a fix and coordinated
disclosure before any public write-up. This is a small, early-stage open-source project without
a dedicated security team yet, so please be patient — but every report is read.

## Scope notes

- Third-party dependencies (PDFium, libsodium, Tesseract, SQLite) have their own upstream
  security processes. See [`THIRD_PARTY_NOTICES`](THIRD_PARTY_NOTICES) for what's vendored and
  how. A vulnerability in an upstream dependency itself should generally go to that project
  directly, unless it's specifically about how Folium uses it (e.g. a missing bounds check in
  how a buffer is handed to the library).
- `docs/risk-register.md` and `docs/security-model.md` document known, accepted risks and open
  items. If you're about to report something already listed there, it's still useful to hear
  from you, but check first — it may already be tracked.
