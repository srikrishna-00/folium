# third_party/tesseract — vendored dependency pin

STATUS: BOOTSTRAP (system package on one dev host). NOT the shipping arrangement.
        See docs/spikes/SP-10-findings.md for the full vetting.

Upstream project   Tesseract OCR (https://github.com/tesseract-ocr/tesseract)
Distributor        Homebrew core (this bootstrap); a self-built hermetic
                    recipe for the real mobile targets (owed, see below)
Tesseract version  5.5.3
License            Apache-2.0 (LICENSE in this directory, a byte-for-byte
                    copy of /opt/homebrew/opt/tesseract/LICENSE,
                    sha256 cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30)

Why there is no pinned release tarball here, unlike third_party/pdfium and
third_party/libsodium
--------------------------------------------------------------------------
Tesseract's observable behaviour is NOT checkable against a published test
vector the way libsodium's is (draft-irtf-cfrg-xchacha-03, RFC 7748) or the
way a byte-exact PDF render would be: an OCR result is a judgement about
image content, not a known-answer output. A surprise version change is
therefore a quiet accuracy drift, not a red test, and this file cannot
offer the same "if the pin moves, the test suite tells you" guarantee the
other two third_party/ entries give. cmake/UappTesseract.cmake's own header
comment argues this in full.

What this bootstrap actually does instead: it is found via pkg-config/
find_library against whatever Tesseract the host has installed
(`brew install tesseract` on macOS, `apt install libtesseract-dev
libleptonica-dev` on Debian/Ubuntu), the exact version is read back from the
installed `tesseract/version.h` and printed at CMake configure time, and
THAT is the accountability mechanism for this bootstrap: "which recogniser
produced these numbers" is answerable from a build log.

Dependencies (verified, not assumed)
-------------------------------------
Leptonica 1.87.0, BSD-2-Clause, a TRANSITIVE dependency resolved by the
dynamic linker from libtesseract's own load commands — no Leptonica type or
symbol appears in ocr/engine's own call graph (it uses
`TessBaseAPISetImage()` with raw bytes and never touches a `Pix`). Verified
via the Homebrew formula's license metadata; see THIRD_PARTY_NOTICES for the
full entry.

The real constraint: libcurl and libarchive
--------------------------------------------
`otool -L /opt/homebrew/lib/libtesseract.dylib` shows this build linked
against both. Tesseract 5's CMake build uses them to fetch and unpack
`.traineddata` files over HTTP when a requested language pack is missing
locally. `ocr/engine` never exercises this path (it accepts only `eng` and
rejects every other language outright — see
`ocr/engine/include/uapp/ocr/engine.h`), but the capability existing in a
library that processes decrypted document plaintext (ADR-0002 decision 1)
is not an acceptable property for a shipping build even when unused.

**Any real build of Tesseract for Android or iOS MUST be configured with:**

    -DDISABLE_CURL=ON -DDISABLE_ARCHIVE=ON

This is the single most important line in this file.

What is still owed before this leaves bootstrap status
--------------------------------------------------------
1. The hermetic from-source build for android-arm64 and ios-arm64, with
   `-DDISABLE_CURL=ON -DDISABLE_ARCHIVE=ON` as above — this also closes the
   "no pin" gap by pinning a specific upstream commit once we are building
   it ourselves.
2. Closing the exceptions-boundary residual risk recorded in
   `ocr/engine/src/engine.cpp`'s own header comment: this module crosses
   from ADR-0001's `-fno-exceptions` code into libtesseract (compiled WITH
   exceptions) through `<tesseract/capi.h>` only, on the strength of that
   being the narrowest surface available — not a documented upstream
   guarantee. Building libtesseract ourselves with `-fno-exceptions` closes
   this properly.
3. A real on-device memory/time measurement of `tessdata_fast` (this
   bootstrap measured the dev-host's full `tessdata` package) against the
   ADR-0002 2GB device floor.
4. A fuzz target over `ocr::Engine::recognise()`.
5. Automated CVE monitoring against whichever commit the hermetic build
   eventually pins.

Re-roll procedure
-----------------
1. Decide the pinned upstream commit for the hermetic build (not yet
   started — this bootstrap tracks whatever a host's package manager has).
2. Build with `-DDISABLE_CURL=ON -DDISABLE_ARCHIVE=ON`, verify via `otool -L`
   / `ldd` that neither symbol appears in the output.
3. Re-run `tests/ocr/` against the new build; the fixture image and its
   ground-truth text in `tests/ocr/ocr_fixture.h` are the regression check.
4. Update the version and checksum fields at the top of this file in the
   same commit as the CMake/build-script change.
