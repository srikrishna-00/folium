# Testing Strategy

**Status:** Phase 0 draft — awaiting human sign-off
**Version:** 0.0.0
**Date:** 2026-09-29

Implements spec §28 (testing) and §29 (fuzzing). Governing principle from §28:
**"Never assume an input file is valid."**

---

## 1. What this system's bugs actually look like

Test effort should follow where the damage is, not where the code is. For this product the
high-damage failure classes are:

| Class | Why it is dangerous | Test response |
|---|---|---|
| **Memory corruption in a parser** | Untrusted input + native code = RCE. Highest severity. | Continuous fuzzing + sanitizers, mandatory per parser |
| **Sync divergence / data loss** | Silent, delayed, unrecoverable, destroys trust | Deterministic simulation + property tests |
| **Silent conversion corruption** | "Success" that produces a broken file | Golden corpus + automated verification |
| **Crypto/key-management error** | Unrecoverable data or a false privacy promise | Known-answer vectors + an explicit key-lifecycle suite |
| **Capability lies** | The matrix claims support we do not have | A test that walks the matrix and proves every claim |
| **Permission/authorisation gaps** | Cross-tenant leakage | An exhaustive authorisation matrix test |
| **AI ungroundedness** | Fabricated citations presented as sourced | Citation-integrity eval as a hard gate |

Ordinary UI regressions are comparatively cheap. The pyramid below is weighted accordingly.

## 2. Test levels

```
         ╱ Manual exploratory ╲              — each release, scripted charters
        ╱  E2E journeys (~60)   ╲            — real devices, incl. airplane mode
       ╱  UI tests (~300)         ╲          — Compose / XCUITest
      ╱  Integration (~1500)        ╲        — engine + storage + ports, real SQLite
     ╱  Binding tests (~400)          ╲      — every JNI/Swift entry point
    ╱  Unit tests (~8000)               ╲    — core C++, fast, no I/O
   ╱─────────────────────────────────────╲
  ╱  Continuous: fuzzing, sanitizers,     ╲  — always running, not a level
 ╱   property tests, simulation, benchmarks ╲
╱───────────────────────────────────────────╲
```

Counts are order-of-magnitude targets for v1.0, not quotas to be gamed.

## 3. Unit tests (core)

- **Framework:** GoogleTest + GoogleMock.
- **Rules:** no filesystem, no network, no real clock, no real RNG. `ClockPort` and
  `RandomPort` are injected fakes — this is what makes sync and crypto tests deterministic.
- **Coverage gates:** line ≥ 80 % overall; ≥ 90 % for `core/sync`, `security/crypto`,
  `core/permissions`, `file-engine/detection`, and every parser's error paths.
  Coverage is a floor, not a goal — a 100 %-covered parser with no fuzzing is still unsafe.
- **Every error path is tested.** For parsers this means a test per error code, not just the
  happy path. In practice error-path tests outnumber happy-path tests roughly 3:1 in parser code.
- **Death tests** for the invariants that must abort rather than continue (e.g. a handle used
  after free in a debug build).

## 4. Property-based testing

Where example tests are structurally insufficient. Using RapidCheck (BSD-2) or an equivalent.

| Property | Statement |
|---|---|
| Sync convergence | For any interleaving of ops across N devices, all devices reach an identical state |
| Sync no-loss | For any interleaving, every user-intended value is reachable from version history |
| Sync idempotence | Applying the same change log twice is a no-op |
| Fractional index | For any two adjacent keys, a strictly-between key exists and is generatable |
| CRDT commutativity | `apply(a, b) == apply(b, a)` for concurrent ops |
| Conversion round-trip | For T1 pairs, `read(write(ir)) ≈ ir` under the declared equivalence |
| Crypto round-trip | `decrypt(encrypt(m, k), k) == m` for all m, k; decryption with a wrong key or tampered ciphertext always fails |
| Chunked AEAD | Reordering, truncating, or splicing chunks between files always fails authentication |
| Detection stability | Detection of the same bytes is deterministic and independent of the filename |
| Permission monotonicity | Adding a grant never removes access; removing one never adds it |
| Fidelity report completeness | For a document containing K unsupported constructs, the report contains exactly K corresponding entries |

## 5. Integration tests

Real SQLite (in a temp dir), real parsers, fake network and fake platform vtable.

- Full pipelines: ingest → detect → parse → index → search; scan → OCR → PDF → index;
  edit → version → sync-enqueue.
- Transactional correctness: kill the process (simulated) between every pair of statements in a
  multi-step operation and assert no partial state is observable.
- Migration tests: every schema version N−1 fixture DB migrates to N, with post-migration
  integrity assertions.
- Storage-full, permission-denied, and corrupted-file paths are integration tests, not unit tests.

## 6. Fuzzing (spec §29)

**Mandatory. A parser without a fuzz target does not merge.** This is enforced by a CI check
that every file under `*/parsers/` has a corresponding target in `tests/fuzz/`.

### 6.1 Targets

| Target | Input | Priority | Note |
|---|---|---|---|
| `fuzz_pdf_parse` | PDF bytes | **P0** | Largest attack surface. **BUILT** — `tests/fuzz/fuzz_pdf_reader.cpp`; see §6.4 |
| `fuzz_pdf_render` | PDF + page index | **P0** | Rendering is a separate code path from parsing. **BUILT** — same harness, same iteration; see §6.4 |
| `fuzz_pdf_content_stream` | content stream bytes | **P0** | Redaction and text extraction depend on this. Not built: there is no first-party content-stream entry point to call |
| `fuzz_zip` | ZIP bytes | **P0** | Gateway to all OOXML; bombs and zip-slip |
| `fuzz_docx` | DOCX bytes | **P0** | |
| `fuzz_xlsx` | XLSX bytes | **P0** | |
| `fuzz_xml` | XML bytes | **P0** | XXE, entity expansion, deep nesting |
| `fuzz_tiff` | TIFF bytes | **P0** | Historically the most CVE-prone image format we handle |
| `fuzz_jpeg`, `fuzz_png`, `fuzz_webp`, `fuzz_gif`, `fuzz_bmp` | image bytes | P0 | |
| `fuzz_detect` | arbitrary bytes | **P0** | Must never crash on anything, by definition. **BUILT** — `tests/fuzz/fuzz_file_detection.cpp`; see §6.3 |
| `fuzz_metadata` | EXIF/XMP/ID3 bytes | P1 | |
| `fuzz_rtf`, `fuzz_html`, `fuzz_markdown`, `fuzz_epub`, `fuzz_svg` | format bytes | P1 | |
| `fuzz_ocr_input` | image bytes → Tesseract | P1 | Third-party surface |
| `fuzz_sync_delta` | CBOR delta | **P0** | Hostile-peer input |
| `fuzz_api_json` | request bodies | P1 | Server-side |
| `fuzz_conversion` | (format, bytes, target) | P1 | Exercises reader+writer together |
| `fuzz_crdt_ops` | operation sequences | P1 | Structure-aware; convergence oracle |

### 6.2 Method

- **libFuzzer + ASan + UBSan** as the default configuration; MSan runs separately (it needs an
  instrumented libc++ and cannot be combined with ASan).
- **Structure-aware fuzzing** (`libprotobuf-mutator` or a custom mutator) for formats where
  random byte flipping rarely gets past the header — OOXML, CRDT ops, sync deltas. Naive
  byte-flip fuzzing of a ZIP-based format spends 99 % of its cycles failing CRC checks.
- **Seed corpora** checked into `tests/fixtures/corpus/<target>/`, minimised with
  `-merge=1`. Seeded from our own fixture files plus public corpora.
- **Cadence:** short runs (60 s per target) on every PR as a smoke check; long runs (≥ 4 h per
  P0 target) nightly; a continuous fleet between releases.
- **Release gate:** ≥ 72 h clean across all P0 targets, zero open medium-or-higher findings.
- **Every crash becomes a regression test.** The reproducer is checked into
  `tests/fixtures/regressions/` with the issue id, and runs in the normal unit suite forever.
- **Timeout and OOM are findings**, not noise. A PDF that takes 30 s to parse is a DoS.

### 6.3 What is actually built today

**Two harnesses exist, covering three of the eighteen rows in §6.1: `fuzz_detect`
(executable `fuzz_file_detection`) and `fuzz_pdf_reader`, which covers both `fuzz_pdf_parse`
and `fuzz_pdf_render`.** The remaining rows name parsers that are still empty Phase 0
directories; a stub harness over a module that does not exist would turn this gate into a
checklist of targets that fuzz nothing. §6.3 and §6.4 say exactly what is real, one per
harness, so the gap stays visible rather than implied.

| | |
|---|---|
| Harness | `tests/fuzz/fuzz_file_detection.cpp` |
| Under test | `uapp::files::detect_buffer` and `describe_buffer` — the **buffer** entry points of `file-engine/detection` |
| Seed corpus | `tests/fixtures/corpus/fuzz_file_detection/` (24 files) |
| Build switch | `UAPP_ENABLE_FUZZING` (**default OFF**) |

**Why this entry point.** `detect_buffer` takes raw bytes with no path and no filesystem: it is
the untrusted-upload shape, it is what the mobile picker and the eventual HTTP upload handler
call, and it is the first first-party code that ever touches an attacker-supplied file — before
any parser is allowed to look at it. `detect_path` is deliberately **not** a target: it is a
bounded-read wrapper isolated in one translation unit that fills a `FileSample` and hands it to
this same engine, so fuzzing it would spend every cycle in `open(2)` re-testing this code
through a slower door.

#### Building and running it

Fuzzing is a **third build configuration**, not a flag on the normal one, because libFuzzer
supplies `main` — linking `-fsanitize=fuzzer` into the shared compiler posture would turn the
CLI and all four test binaries into fuzzer drivers. What *is* applied project-wide under the
switch is `-fsanitize=fuzzer-no-link,address,undefined`, which adds the coverage
instrumentation libFuzzer steers by and leaves `main` alone; the fuzz target alone adds the
runtime at link time. Instrumenting the library and not merely the harness is the point: a
target built against an uninstrumented `uapp::file_detection` is a random byte generator with
no feedback.

```sh
# configure — see the toolchain note below for why the compiler is named
cmake -S . -B build-fuzz -DUAPP_ENABLE_FUZZING=ON \
      -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang \
      -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++
cmake --build build-fuzz -j

# 60 s smoke run from the seed corpus (the per-PR cadence in §6.2).
# The FIRST directory is written to — libFuzzer saves every input that reaches
# new coverage into it — so it is scratch, and the checked-in seed set that
# follows it is read-only. Pointing the smoke run straight at the repo corpus
# would add a few hundred files to the working tree on every invocation.
mkdir -p /tmp/uapp-fuzz-work
./build-fuzz/bin/fuzz_file_detection -max_total_time=60 -print_final_stats=1 \
    -artifact_prefix=/tmp/uapp-fuzz-artifacts/ \
    /tmp/uapp-fuzz-work tests/fixtures/corpus/fuzz_file_detection/

# longer run: same shape, bigger budget, and a length cap past the 64 KiB
# EOCD scan (libFuzzer's default is 4096, which never reaches it)
./build-fuzz/bin/fuzz_file_detection -max_total_time=3600 -max_len=131072 \
    -artifact_prefix=/tmp/uapp-fuzz-artifacts/ \
    /tmp/uapp-fuzz-work tests/fixtures/corpus/fuzz_file_detection/

# replay a reproducer
./build-fuzz/bin/fuzz_file_detection /tmp/uapp-fuzz-artifacts/crash-<sha1>
```

With the switch OFF nothing under `tests/fuzz/` is configured, parsed or compiled, and no
sanitizer flag appears in `compile_commands.json`. With it ON, `UAPP_BUILD_TESTS` and
`UAPP_BUILD_TOOLS` default OFF and the SQLite probe is skipped: a fuzz build produces the
library under test and the harness, nothing else. No fuzz target is registered with ctest — a
fuzz target has no pass/fail, it has a time budget.

**Toolchain note (macOS).** Apple's Command Line Tools clang *accepts* `-fsanitize=fuzzer` but
ships no `libclang_rt.fuzzer_osx.a`, so the flag compiles and the link fails. Upstream LLVM
(`brew install llvm`) is required. The configure step link-probes for this and fails with that
instruction rather than letting the build get several hundred files in before the linker
complains.

#### The oracle: crashes are not the only bug

ASan and UBSan catch memory errors and UB. They do not catch a *self-contradictory verdict*,
which for a detector is the likelier defect and is still a security finding, because callers
gate on `confidence` and `extension_conflicts`. So every input is also checked against the
invariants `detector.h` states in prose — among them: the filename can never set `format`
(`ExtensionHintOnly`/`ExtensionOnly` must leave it `Unknown`); an identified format never
carries `None` confidence; `magic_offset` is non-zero only for PDF and never points past the
input; container flags never appear on a non-ZIP-family result; two calls on identical bytes
produce identical results; and `describe_buffer` never disagrees with `detect_buffer`. Only
invariants the public header actually promises are asserted — inventing a stricter rule than
the contract would manufacture fake defects.

#### Seed corpus

24 hand-authored files, each a real file of its claimed format rather than a byte blob: a
minimal but complete example of **every one of the ten formats detection currently
implements** (PDF, PNG, JPEG, GIF87a, GIF89a, ZIP, DOCX, XLSX, PPTX, ODT, and ASCII/UTF-8/
UTF-8-BOM/UTF-16-BOM text), plus the malformed shapes a byte mutator would take a very long
time to construct: an OOXML package of an unrecognised family, an archive with no central
directory, a bare `PK\x03\x04`, a truncated central directory, an EOCD pointing past EOF, a
Zip64 sentinel, a 65534-entry count claim, a PDF behind a 300-byte preamble, a buffer with an
embedded NUL, and an empty file.
The ZIP-family seeds are byte-identical to what `tests/file_engine/zip_fixture.h` builds, so a
fixture and its corpus entry cannot drift into describing different files.

Per §15 only this minimised seed set lives in the repo; the evolved corpus from a long run
belongs in object storage, and a run's survivors come back via `-merge=1`.

#### Run record

| Date | Build | Runs | Duration | exec/s | Coverage | Findings |
|---|---|---|---|---|---|---|
| 2026-09-30 | LLVM 23.1.2, Debug, ASan+UBSan | 2 928 476 | 121 s | 24 202 | — | none |
| 2026-09-30 | continued, from evolved corpus | 2 166 152 | 121 s | 17 902 | 907 edges / 1 870 features | none |
| 2026-09-30 | `-max_len=131072` | 2 247 966 | 121 s | 18 578 | 907 edges / 1 877 features | none |
| 2026-09-30 | clean rebuild, seed corpus only (the documented 60 s smoke command, run verbatim) | 2 006 441 | 61 s | 32 892 | 906 edges / 1 740 features | none |

**9 349 035 executions over 424 s: zero crashes, zero hangs (slowest unit < 1 s), zero
sanitizer reports, zero oracle violations.** Corpus plateaued at 154 units and coverage stopped
growing, so this is a saturated smoke run for this corpus and length cap — not a claim that the
module is clean, which needs the ≥ 4 h nightly cadence in §6.2. The third run exists because
libFuzzer's default 4096-byte cap never reaches the 8 KiB text-probe boundary or the 64 KiB
EOCD backward scan; raising the cap added 7 features and no defects.

The one bug this work did find was in the **corpus generator, not the product**: its
central-directory record omitted the mod-date field, so the DOCX/XLSX/PPTX seeds were silently
degrading to plain `zip` and seeding the fuzzer with three fewer real formats than intended.
Caught by checking each seed's detected format before the first run, and fixed in the
generator. Worth recording because a silently-wrong seed corpus is the failure mode that makes
a fuzz run look fine while testing much less than it claims.

### 6.4 `fuzz_pdf_reader` — the PDF target

| | |
|---|---|
| Harness | `tests/fuzz/fuzz_pdf_reader.cpp` |
| Under test | `uapp::pdf::Document::open_buffer`, `page_count`, `page_size`, `render_page` — the whole of `pdf-engine/reader` |
| §6.1 rows covered | `fuzz_pdf_parse` **and** `fuzz_pdf_render` |
| Seed corpus | `tests/fixtures/corpus/fuzz_pdf_reader/` (10 files) |
| Build switch | `UAPP_ENABLE_FUZZING` (**default OFF**), plus a vendored PDFium |

**One harness for two rows.** §6.1 lists parsing and rendering separately because "rendering is
a separate code path from parsing", and that is true — but `pdf-engine/reader` is one library
behind one door, so two binaries would mean two processes re-parsing the same bytes to reach
the second half of the same call chain. The separation §6.1 wants is honoured by exercising
both in *every iteration* instead: open, count the pages, measure them, then rasterise page 0
into a caller-owned buffer. `fuzz_pdf_content_stream` stays unbuilt — there is no first-party
content-stream entry point to call.

**Why this entry point.** Same reasoning as §6.3: `open_buffer` takes raw bytes with no path
and no filesystem. Here it is also the *only* door — `document.h` design rule 1 is "buffer
first, path never", so there is no second entry point left unfuzzed. The input byte string is
the PDF file verbatim; the two things a PDF's bytes cannot carry, the **password** and the
**render geometry**, are derived deterministically from the input's own first byte, exactly as
§6.3's harness derives a filename.

#### The honest limit: PDFium is not instrumented

`third_party/pdfium` is a **prebuilt** `libpdfium.dylib` (see its README, "What is still
owed"). It carries no SanitizerCoverage and no ASan/UBSan instrumentation, and nothing the
harness does can add any. Two consequences, stated here rather than discovered later:

1. **libFuzzer's coverage feedback comes from the wrapper, not the parser.** 316 edges is the
   wrapper's size, not PDFium's; the mutator is close to blind past the first branch that
   matters. That is why the seed corpus carries hand-built adversarial structures instead of
   trusting mutation to invent them.
2. **ASan sees PDFium's heap through the malloc interceptor** — a double-free, a use-after-free
   of a heap block, or an overflow caught by an intercepted libc call *is* reported — but a
   plain out-of-bounds read inside uninstrumented PDFium code is **not**, because there is no
   instrumented load to check. UBSan sees nothing inside PDFium at all.

What this target therefore genuinely covers is *our* layer: the `Status` mapping, the `Library`
refcount, the bounds arithmetic, and — because the render destination is an **exact-fit** heap
allocation flanked by ASan redzones — any write by PDFium outside the geometry `RenderTarget`
authorised. That last one is the memory-safety property this module actually promises, and it
is checkable today. The rest waits on the hermetic from-source PDFium build SP-1 asks for.

#### The oracle

Every input is held to the `Status` sets `document.h` states in prose, **and only to those**:
`open_buffer` returns one of Ok / InvalidArgument / FileEmpty / PdfMalformed /
PdfPasswordRequired / PdfSecurityUnsupported; `page_size` one of Ok / PdfNotOpen /
PdfPageNotFound / PdfPageLoadFailed; `render_page` one of those plus PdfRenderSizeInvalid /
PdfBufferTooSmall / PdfRenderFailed. Also asserted: a failed open leaves no half-open
`Document`, `page_count()` is never negative, an index of `-1` or `page_count` is always
`PdfPageNotFound` on both page-level entry points, a geometry `required_buffer_bytes()`
accepted is never then rejected by `render_page`, and a successful render never touches the
caller's **row padding**. Deliberately *not* asserted: that a page has positive dimensions,
that a recovered document has any particular page count, or that a broken content stream fails
— `document.h` promises none of those and PDFium's xref-rebuilding recovery makes all three
legitimately variable. Inventing a stricter rule than the header would manufacture a fake
defect, which is the trap §6.3 already calls out.

Page walking is capped at 48 pages per input. A hostile PDF can claim a very large page count
cheaply, and walking all of it would turn every such input into a libFuzzer timeout — a
*harness*-amplified hang, not a product one, and §6.2's "timeout and OOM are findings" only
means anything if the findings belong to the product. The last index is probed separately, so
the count/index boundary is still covered on a large document.

#### Building and running it

Same switch and same toolchain requirement as §6.3, plus a vendored PDFium
(`third_party/pdfium/fetch_pdfium.sh`). Unlike `storage/` and `core/capi`, `pdf-engine/` is
**not** skipped in a fuzz build — those two are off the untrusted-input path and only cost
build time, whereas the PDF reader *is* the untrusted-input path. If no PDFium is vendored the
target simply does not exist and the fuzz build still produces `fuzz_file_detection`; a missing
third-party binary must not become a configure error for someone who did not ask for PDF.

```sh
cmake -S . -B build-fuzz -DUAPP_ENABLE_FUZZING=ON -DUAPP_ENABLE_PDF=ON \
      -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang \
      -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++
cmake --build build-fuzz -j

mkdir -p /tmp/uapp-fuzz-pdf-work
./build-fuzz/bin/fuzz_pdf_reader -max_total_time=120 -print_final_stats=1 \
    -timeout=25 -rss_limit_mb=2048 \
    -artifact_prefix=/tmp/uapp-fuzz-artifacts/ \
    /tmp/uapp-fuzz-pdf-work tests/fixtures/corpus/fuzz_pdf_reader/
```

`-timeout` and `-rss_limit_mb` are not decoration the way they would be for the detector. §6.2
says "a PDF that takes 30 s to parse is a DoS", and a PDF parser is the one component in this
repo where an input genuinely can ask for unbounded work or unbounded memory. At the defaults a
decompression bomb reads as a slow iteration instead of as the finding it is.

#### Seed corpus

Ten files. Eight are the **byte-identical output of `tests/pdf_engine/pdf_fixture.h`**, so a
fixture and its seed cannot drift into describing different files (the same guarantee §6.3's
ZIP seeds get from `zip_fixture.h`): the valid two-page document, a bare `%PDF-` header with no
body, a `startxref` pointing past EOF, four truncations (4, 40, 300 and *n*−40 bytes), and the
regression fixture below. The ninth is a PDF whose trailer claims an `/Encrypt` dictionary
(standard security handler, V1/R2) — the only seed that reaches PDFium's security handler and
the only one that makes the harness exercise the password overload of `open_buffer`. The tenth
is the raw libFuzzer reproducer for the finding below, kept verbatim.

Each seed's behaviour was checked against the reader *before* the first run, which is the
control §6.3's corpus-generator bug argued for: valid → Ok/2 pages/200×100/render Ok,
`startxref`-past-EOF → Ok (PDFium rebuilds the table), `/Encrypt` → PdfPasswordRequired, all
four truncations and the bare header → PdfMalformed.

#### Run record

| Date | Build | Runs | Duration | exec/s | Coverage | Findings |
|---|---|---|---|---|---|---|
| 2026-09-30 | LLVM 23.1.2, Debug, ASan+UBSan, **pre-fix** | 139 | aborted on the finding | — | 286 edges / 344 features | **1 — oracle violation, real product bug** |
| 2026-09-30 | post-fix, clean, seed corpus only (the documented command, run verbatim) | 1 687 510 | 121 s | 13 946 | 316 edges / 472 features | none |
| 2026-09-30 | continued, from evolved corpus | 1 924 058 | 121 s | 15 901 | 316 edges / 481 features | none |
| 2026-09-30 | `-max_len=131072` | 1 821 616 | 121 s | 15 054 | 316 edges / 481 features | none |

**5 433 184 executions over 363 s after the fix: zero crashes, zero hangs (slowest unit < 1 s),
zero sanitizer reports, zero oracle violations.** Coverage and the working corpus (21–22 units)
both plateaued, so this is a saturated smoke run *for an uninstrumented PDFium* — see the
honest-limit note above; it is emphatically not a claim that the PDF parser is clean, which
needs both the ≥ 4 h nightly cadence in §6.2 and an instrumented PDFium. Raising the length cap
to 128 KiB added nothing, which is consistent with feedback coming from the wrapper.

#### The finding: a real product bug, fixed

At **139 executions** the fuzzer mutated the valid seed's page tree from `/Kids [3 0 R 4 0 R]`
to `/Kids [3 0 R 44 0 R]`, leaving `/Count 2`. Object 44 does not exist, so `page_count()`
returned 2 and index 1 was simultaneously *in range* and *unresolvable* — a combination no
well-formed file can produce and no hand-written case had covered.

`page_size()` and `render_page()` both answered it by calling the document-load error mapper,
which reads PDFium's process-global `FPDF_GetLastError()`. Neither `FPDF_GetPageSizeByIndexF`
nor `FPDF_LoadPage` writes that global, so it still held `FPDF_ERR_SUCCESS` from the document
load that had just succeeded, and the mapper turned "no error" into **`Status::Internal`**.

Two defects in one, and neither is cosmetic. `Internal` is outside both functions' documented
return sets, so a caller switching on the documented statuses fell through to its default
branch; and `status.h` defines `Internal` as *"a bug in us, not in the caller's input"* for a
condition caused entirely by the caller's input — meaning attacker-supplied bytes could make
the product report an internal defect, lying to whoever reads the log and poisoning any
error-rate alert built on it.

**Fix:** both page-level call sites now return `Status::PdfPageLoadFailed` (704, *"index in
range, but the page object is unusable"*) unconditionally instead of consulting a global that
nothing wrote. No information is lost, because there was none. The mapper is now
document-load-only and says so. **Regression test:** `pdf.malformed an in-range page that will
not load is PDF_PAGE_LOAD_FAILED`, over a new `dangling_page_reference_pdf()` fixture rebuilt
from source per `pdf_fixture.h`'s no-opaque-blobs rule; the raw reproducer is also a corpus
seed. Confirmed failing (`Internal`) against the pre-fix build and passing after.

*Deviation from §6.2, recorded rather than hidden:* §6.2 says the reproducer is checked into
`tests/fixtures/regressions/` with an issue id. There is no such directory and no issue tracker
in this repo yet, so the reproducer went to the seed corpus and the regression lives as a
source-built fixture in the normal unit suite — which is the part of §6.2 that actually
matters ("runs in the normal unit suite forever").

## 7. Sanitizers and static analysis

| Tool | Cadence | Gate |
|---|---|---|
| ASan + UBSan (full suite) | every PR | blocking |
| TSan (concurrency-relevant suites) | nightly | blocking on new findings |
| MSan | weekly | triage |
| Valgrind (Linux CI) | weekly | triage |
| clang-tidy (`bugprone-*`, `cert-*`, `concurrency-*`, `cppcoreguidelines-*`) | every PR | blocking |
| CodeQL / Semgrep | every PR | blocking on high severity |
| `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror` | every build | blocking |
| Include-graph/layering check (`architecture.md` §8) | every PR | blocking |
| ABI-diff against the last release | every PR | blocking on a non-additive change |
| Licence/notices check (NFR-8) | every PR | blocking |

## 8. Fixtures (spec §28)

Fixtures are a deliverable in their own right, owned and versioned. Stored in
`tests/fixtures/`, with files > 1 MB in Git LFS.

### 8.1 Required categories

| Category | Contents | Min count |
|---|---|---|
| **Small valid** | One minimal valid file per supported format | 1 per format |
| **Large** | 500-page PDF; 100 MB PDF; 50 MB DOCX; 1 M-row CSV; 500k-cell XLSX; 20 000 × 20 000 TIFF | 10 |
| **Corrupted** | Truncated at 10/50/90 %; flipped bytes in the header, in the middle, in the trailer; wrong length fields; bad CRCs | ≥ 5 per P0 format |
| **Malformed but "plausible"** | Valid magic, invalid structure; conflicting internal lengths; cyclic references; deeply nested structures (1000 levels) | ≥ 5 per P0 format |
| **Adversarial** | ZIP bombs; XXE/billion-laughs; zip-slip paths; PDFs with JS/OpenAction/Launch; polyglots (PDF+ZIP, PDF+HTML); PDFs with cyclic object references; a font designed to stress FreeType | ≥ 20 |
| **Password-protected** | PDF R2/R4/R6; AES-encrypted ZIP; ZipCrypto ZIP; password-protected DOCX/XLSX | 8 |
| **Unicode** | Emoji + ZWJ sequences; combining marks; RTL (Arabic, Hebrew); bidi override characters; CJK; Devanagari; surrogate pairs; NFC vs NFD pairs; zero-width characters in filenames | 15 |
| **Multi-language OCR** | Printed pages in eng/spa/fra/deu/hin/ara/zho/jpn, at 150/300/600 DPI, clean and degraded | 24 |
| **Scanned documents** | Skewed; shadowed; creased; low contrast; handwritten; receipts (thermal); business cards; forms; multi-column; tables with and without ruling lines | 25 |
| **Real-world DOCX/XLSX/PPTX** | Produced by several different generators, since each emits materially different OOXML | 30 |
| **Complex PDFs** | Scanned; vector-heavy (a map); CJK with embedded subset fonts; AcroForms; XFA; tagged/accessible; PDF/A; linearised; incrementally-updated; no `ToUnicode` CMap | 20 |
| **Database fixtures** | A DB at every schema version, for migration tests | 1 per version |
| **Sync scenarios** | Recorded operation logs for known conflict patterns | 20 |
| **Golden outputs** | Expected IR and expected output for each conversion pair | 1 per matrix cell |

### 8.2 Provenance rules

- **No copyrighted third-party documents.** Fixtures are generated by us, taken from
  permissively-licensed public corpora, or synthesised. Each fixture directory carries a
  `PROVENANCE.md` naming the source and licence.
- **No real personal data**, ever, including in "realistic" scanned samples. Synthetic names,
  addresses, and numbers only.
- Adversarial fixtures are clearly quarantined and never opened by tooling that scans the repo.

## 9. Performance testing

Google Benchmark micro-benchmarks plus device-level macro-benchmarks. Budgets and the
regression gate are defined in `performance-strategy.md`; the testing obligations are:

- Every budget in that document has an automated benchmark.
- Benchmarks run nightly on fixed, reserved physical devices (not emulators, not shared CI
  runners — the variance makes results meaningless).
- A > 10 % regression against the rolling 7-day median on a P0 benchmark fails the build.
- Memory-ceiling tests assert peak RSS, not just "it completed".

## 10. Security testing

Detailed in `security-model.md` §13. The obligations that land on this document:

- **Authorisation matrix test:** generated, covering every endpoint × every role × an object
  belonging to another tenant. This is generated from the OpenAPI spec so a new endpoint cannot
  be added without being covered.
- **Crypto known-answer tests** against RFC/NIST vectors for every primitive.
- **Key-lifecycle suite:** generate, wrap, unwrap, rotate, destroy; assert that a destroyed key
  makes its ciphertext permanently unrecoverable.
- **Redaction verification** is a hard gate: for every redaction fixture, extract text and
  metadata from the output and assert the redacted strings are absent (`security-model.md` §9).
- **Secret-in-logs test:** run the full suite with a logging sink that fails the test if it ever
  sees a marker string planted in fixture content.

## 11. AI evaluation

Covered in `ai-architecture.md` §11. Treated as tests, with gates:
retrieval recall@10 ≥ 0.85; citation integrity ≥ 0.95 post-verification; **zero unverified
citations reaching the UI (P0 bug)**; zero unauthorised tool invocations on the injection
red-team corpus.

## 12. Mobile and E2E

- **Binding tests** on both platforms exercising every entry point including error and
  cancellation paths, run under ASan, asserting zero leaked handles at teardown.
- **E2E journeys** (~60) covering every P0 user story, each with an airplane-mode variant where
  applicable.
- **Chaos on device:** kill the app mid-OCR, mid-conversion, mid-sync-apply, mid-upload; fill
  the disk; revoke the device; change the clock. Assert resume, not restart, and no loss.
- **Device matrix:** low-end, mid, flagship, and one tablet per platform, on real hardware.
- **Accessibility:** automated scans (Accessibility Scanner / XCUITest audits) plus a manual
  screen-reader pass on every P0 flow each release.

## 13. CI pipeline

```
PR  ──► format check · lint · layering check · licence check
    ──► build (Linux, macOS, Android arm64, iOS arm64)
    ──► unit + integration (ASan+UBSan)
    ──► property tests (fixed seed set)
    ──► binding tests (Android emulator, iOS simulator)
    ──► fuzz smoke (60 s per P0 target)
    ──► ABI diff · coverage gate · benchmark smoke
    ──► CodeQL
                                        [~25 min target for the PR path]

Nightly ──► full fuzz (4 h per P0 target) · TSan · device benchmarks
        ──► E2E on the physical device matrix · AI evals
        ──► dependency CVE scan · long sync simulation (10⁶ ops)

Weekly  ──► MSan · Valgrind · 7-day sync soak (rolling) · full E2E matrix

Release ──► everything, plus: 72 h fuzz clean · external pentest findings closed
        ──► the format-matrix verification test (§14)
```

**The PR path must stay under ~25 minutes.** A slow PR pipeline gets bypassed, and a bypassed
pipeline provides no safety.

## 14. The format-matrix verification test

A specific requirement from `product-requirements.md` §10.2, called out because it is unusual
and important:

A test reads `docs/file-format-support.md`, parses the matrix, and for **every cell claiming a
capability** locates a fixture and asserts the capability actually works. A ✔ with no passing
assertion fails the build.

This is the mechanism that makes "do not claim universal support" (spec §9) and "do not pretend
unsupported functionality works" (spec §43) enforceable rather than aspirational. Documentation
that can drift from reality will drift; this test removes the possibility.

## 15. Test data management

- Fixtures in Git LFS; a manifest with SHA-256 per file; CI verifies integrity.
- Fuzz corpora stored separately (object storage) and synced by CI, with the minimised seed set
  in-repo.
- Golden outputs regenerated only by an explicit, reviewed command — never silently updated by a
  `--update-snapshots` flag that reviewers rubber-stamp.

## 16. Definition of Done for a test suite

A phase's tests are complete when:

- [ ] Every public function of the phase's modules has unit tests covering success and every
      error code
- [ ] Every parser added in the phase has a fuzz target and a seed corpus
- [ ] Integration tests cover the phase's end-to-end pipeline
- [ ] Property tests exist for any invariant that can be stated as one
- [ ] Benchmarks exist for anything with a budget in `performance-strategy.md`
- [ ] Security tests cover the phase's threat-model entries
- [ ] Fixtures for the phase's formats exist in all required categories
- [ ] The suite runs clean under ASan+UBSan
- [ ] Coverage gates met
- [ ] No test is skipped or disabled without a linked issue and an owner

## 17. Anti-patterns we are explicitly banning

Written down because each of these has sunk a native codebase before:

1. **Deleting or disabling a test to make a build pass** (spec §37). Quarantine with an issue
   and an owner, or fix it.
2. **Asserting on error *messages*** instead of codes. Messages are not API.
3. **Tests that depend on wall-clock time or real randomness.** Inject both.
4. **Snapshot tests over PDF bytes.** Object ordering and timestamps make them useless; compare
   structurally.
5. **Mocking the thing under test.** Parser tests use real parsers and real bytes.
6. **Coverage as the goal.** A fuzz target finds more than 200 lines of coverage-chasing tests.
7. **"Flaky, just rerun it."** A flaky test in sync or concurrency is usually a real bug
   reporting itself. Investigate, do not retry.
