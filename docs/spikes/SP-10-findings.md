# SP-10 Findings — OCR library selection and vetting (Tesseract)

**Status:** Investigated **and implemented**, matching SP-9's precedent. `ocr/engine` exists,
builds, and is tested against a real recognised image — see §5.
**Date:** 2026-10-20
**Spike definition:** **NEW**, same reason SP-9 was new: `technical-requirements.md` §6's spike
table predates OCR being scoped, so there is no pre-existing OCR-dependency spike to fill in.
This document follows the SP-1/SP-9 findings format.
**Blocks:** Phase 7 (OCR) per the roadmap; spec section 13.
**New risk:** none raised at register-entry severity. One residual risk is flagged and carried
in code comments (the exceptions boundary, §3) rather than in `risk-register.md`, because it is
scoped to one translation unit and has a concrete closing condition (the hermetic build).

> **Headline: the licence is clean and the real constraint is the same one SP-9 found — this
> Homebrew build links network code onto the one path that handles decrypted document
> plaintext.** Tesseract is Apache-2.0 and its transitive dependency Leptonica is
> BSD-2-Clause — both verified against the actual installed license text, not `brew info`
> alone. But `otool -L` on the Homebrew `libtesseract.dylib` shows it linked against
> `libcurl` and `libarchive`: Tesseract 5's build system uses them to fetch and unpack
> `.traineddata` files from a URL when a language pack is missing locally. Nothing in
> `ocr/engine` calls that code path — this module never asks Tesseract to fetch anything — but
> "we never call it" and "it is not there" are different properties, and under ADR-0002
> decision 1 (document images are decrypted client-side and must not leave the device) only the
> second one is acceptable for a shipping build. A production build **must** configure
> Tesseract with `-DDISABLE_CURL=ON -DDISABLE_ARCHIVE=ON`. Read §2.

---

## 1. The question

We need printed-text OCR with positioned output (bounding boxes, confidence, per spec section
13's `TextBlock` schema) for a C++17 core shipping on iOS and Android, under the same
constraints every prior dependency has had to clear: spec section 35's license review, the
ADR-0002 decision-3 device floor (2 GB RAM), and decision-1's E2EE boundary (OCR runs on
plaintext that was decrypted client-side and must never leave the device or touch the network).

**Tesseract** is the obvious candidate: mature, widely deployed, LSTM-based since v4, Apache-2.0,
with official Android/iOS build recipes upstream. The question this spike actually had to answer
was not "is there an alternative" (there wasn't a serious one worth spiking — Apple's Vision
framework and Android's ML Kit are the two real competitors, and both are platform-native,
closed, per-OS implementations that `architecture.md` section 4 already names as *future*
adapters behind the same `OCRProviderPort` Tesseract sits behind today, not alternatives to
vet now) but "what does actually shipping Tesseract cost, and on what terms."

## 2. Licence and distribution

Verified against the files actually installed on this host, not assumed from `brew info`:

- **Tesseract — Apache-2.0.** `/opt/homebrew/opt/tesseract/LICENSE` is the real Apache-2.0 text.
  No static/dynamic distinction, so static linking into a closed-source App Store binary is
  unambiguously permitted — the same answer SP-9 found for libsodium's ISC license, and an
  easier one than SP-1's unresolved AGG question for PDFium.
- **Leptonica — BSD-2-Clause.** Verified via the Homebrew formula's own license metadata
  (`brew info leptonica`); permissive, no share-alike terms, no patent grant needed to check
  (there is none to worry about).
- **`tessdata` language packs — Apache-2.0**, per Google's `tesseract-ocr/tessdata` /
  `tessdata_fast` repositories. The `eng.traineddata` vendored by the Homebrew formula (and the
  only language this build accepts — see `ocr/engine/include/uapp/ocr/engine.h`) carries the
  same license.
- **The real constraint is not licensing.** `otool -L /opt/homebrew/lib/libtesseract.dylib`
  shows:

  ```
  /opt/homebrew/opt/libarchive/lib/libarchive.13.dylib
  /usr/lib/libcurl.4.dylib
  ```

  Tesseract 5's CMake build links these to support fetching and unpacking `.traineddata` over
  HTTP when a requested language pack is not present locally (`TessBaseAPIInit` falls back to a
  download path). This is a real network client compiled into the one library that processes
  decrypted document images under ADR-0002 decision 1. `ocr/engine` never exercises this path —
  it rejects any language but `eng` outright (`Status::OcrLanguageNotImplemented`) and never
  passes Tesseract a URL — but the Homebrew build's *capability* to do so is independent of
  whether this module *chooses* to use it. **A shipping build must be configured
  `-DDISABLE_CURL=ON -DDISABLE_ARCHIVE=ON`** when Tesseract is built from source for the real
  mobile targets, removing the capability rather than just not exercising it. Recorded in
  `third_party/tesseract/README.md` as the load-bearing item in "what is still owed."

## 3. The exceptions boundary — a flagged residual risk, not a closed one

ADR-0001 compiles this project's C++ without exceptions (`-fno-exceptions`); Tesseract itself is
compiled *with* them. `ocr/engine.cpp` crosses that boundary through `<tesseract/capi.h>` — the
C API, not `tesseract::TessBaseAPI` directly — specifically because `extern "C"` functions are
the narrowest surface available and the one upstream's own non-C++ bindings (Python, Java, .NET)
already depend on being exception-safe in practice. This is **not** a documented guarantee from
upstream, and is recorded as a flagged residual risk rather than a closed question: the properly
closed version of this is building libtesseract ourselves with `-fno-exceptions`, which is part
of the same hermetic build third_party/tesseract/README.md already owes for the mobile targets.
Not escalated to a new risk-register entry at this severity because the surface is one
translation unit with a stated closing condition, not an open-ended exposure.

## 4. Mobile build story

Tesseract ships official Android (NDK) and iOS (Xcode) build recipes upstream, unlike PDFium's
"no releases, only branch heads" problem (SP-1) — this is closer to libsodium's situation (SP-9)
than PDFium's. What this machine's Homebrew install represents is a **local dev bootstrap
only** — `/opt/homebrew/lib/libtesseract.dylib`, dynamically linked, with the curl/archive
capability described in §2 — not a build this project performed or pinned, and not what ships
on a real device. `third_party/tesseract/README.md` records this distinction explicitly, same
as `third_party/pdfium/README.md` and `third_party/libsodium/README.md` do for their own
dependencies, including why `cmake/UappTesseract.cmake` follows `UappSodium.cmake`'s
pkg-config/`find_library` shape rather than `UappPdfium.cmake`'s SHA-256-pinned-vendor-tree
shape (no upstream prebuilt binary distribution exists for Tesseract to pin in the first
place — see that file's own header comment for the full argument).

## 5. What was built and verified

`ocr/engine` (printed text only; tables/forms/receipts/handwriting/multi-language are all
NOT IMPLEMENTED with reasons — see `engine.h`'s own scoping section) wraps the Tesseract C API
behind a buffer-first `ImageView` type deliberately shaped to match `pdf::RenderTarget` field
for field, so the "render a PDF page, OCR the pixels" pipeline needs no conversion step. Verified
independently, not just agent self-report: a real 300x80 grayscale test image containing "HELLO
WORLD" (generated from source with Pillow, embedded as a literal byte array in
`tests/ocr/ocr_fixture.h` — the same "fixture built from source, not a downloaded binary"
principle `tests/pdf_engine/pdf_fixture.h` established — and independently verified against the
real `tesseract(1)` CLI before being embedded) is correctly recognised end to end through
`ocr::Engine`, with a real bounding box and confidence above 50/100. A full project rebuild
(`ctest`) went from 58/58 to 61/61 with the three new OCR suites added, a from-scratch
Debug+ASan/UBSan rebuild of the entire project is also 61/61 clean, `-DUAPP_ENABLE_OCR=OFF`
rebuilds the rest of the project at 58/58 unaffected, and `nm -u` on the built
`libuapp_ocr_engine.a` confirms zero SQLite/libsodium/crypto_ references — this module reaches
`core/common` and `uapp::tesseract` only.

## 6. What is still owed before this leaves bootstrap status

Everything SP-1/SP-9's "still owed" sections name the equivalent of: the hermetic from-source
build for Android/iOS with `-DDISABLE_CURL=ON -DDISABLE_ARCHIVE=ON` (§2, the load-bearing item);
a documented re-roll procedure once such a build exists; closing the exceptions-boundary residual
risk (§3) by building libtesseract without exceptions ourselves; a real on-device
memory/time measurement of `tessdata_fast` against the ADR-0002 device floor (this spike
measured against the dev-host `tessdata` package, not `tessdata_fast`, and not on Tier D
hardware); a fuzz target over `ocr::Engine::recognise()`, named but not written, matching the
judgement call `ai/`'s own CMakeLists.txt already recorded for its own untrusted-input surface.
