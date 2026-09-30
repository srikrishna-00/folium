# Changelog

All notable changes to this project are documented here.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Per spec §5, nothing is recorded here as complete unless it is implemented and tested.
Planning and architecture work is recorded as such.

---

## [Unreleased]

### Added — `pdf-engine/reader`: PDF viewing bootstrap on a vendored PDFium (spec §11)

**Status: BOOTSTRAP, not the shipping arrangement.** This is the open / page-count /
page-size / render-to-caller-buffer slice of spec §11 (viewing only — no editing, merge/split,
forms, annotation, or signatures), built against a **prebuilt** PDFium binary so the reader
wrapper's own logic could be written and tested now, while the real answer SP-1 recommended —
a hermetic from-source build producing our own static archive for every target platform — is
still owed before this ships. Nothing here should be read as "PDF support: done."

- **`third_party/pdfium/`** — PDFium 156.0.8076.0 (`chromium/8076`) from
  `bblanchon/pdfium-binaries`, pinned by tag **and** SHA-256 (upstream ships no release
  tarballs or semver tags, so the tag alone does not identify the bytes). `fetch_pdfium.sh`
  downloads and verifies it; the binary itself is `.gitignore`d, not committed, and is only
  fetched for `mac-arm64` — `android-arm64`/`ios-arm64` are deliberately absent so this cannot
  be mistaken for a mobile solution. Build args cross-checked against `args.gn` in the archive:
  `pdf_enable_v8=false`, `pdf_enable_xfa=false`, `pdf_use_partition_alloc=false`, matching
  SP-1's findings; AGG (not Skia) confirmed as the rasterizer via `licenses/agg23.txt`.
- **Deviation from SP-1, recorded rather than hidden**: this release ships `libpdfium.dylib`
  (shared), not a static archive — no `pdf_is_complete_lib=true` variant exists for macOS in
  this release. The CMake target is `IMPORTED SHARED`, not `IMPORTED STATIC`; the *interface*
  SP-1 asked for (consumed via an `IMPORTED` CMake target) is honoured, the artefact type
  changes once the hermetic build produces our own archive.
- **`CMakeLists.txt`**: `UAPP_ENABLE_PDF` tristate (`AUTO`/`ON`/`OFF`). `AUTO` builds
  `pdf-engine/` iff a vendored PDFium is found and says why not otherwise; `ON` fails configure
  outright if it's missing, rather than silently dropping the PDF tests from a "green" build.
- **`pdf-engine/reader/`**: buffer-first (not just path-first) open, matching the file-engine's
  design; page count; per-page `MediaBox` dimensions; render into a **caller-owned** RGBA8888
  buffer (zero-copy per SP-5 — the wrapper writes into memory the caller supplies, it does not
  allocate and hand back a copy). `FPDF_InitLibrary`/`FPDF_DestroyLibrary` lifecycle wrapped in
  a refcounted `Library` so it is safe across multiple `Document`s and multiple test runs in
  one process.
- **9 new `Status` codes (700-708)**, deliberately coarse: PDFium's own `FPDF_GetLastError()`
  distinguishes about six failure classes, so finer-grained codes here would be invented, not
  real. `core/capi`'s exhaustive `to_c_status` switch extended to list-and-map all 9 to
  `UAPP_STATUS_UNEXPECTED` (no new ABI constant minted — same pattern as the blocks and
  file-engine commits, for the same reason: nothing reachable through the C ABI can produce
  one of these yet).
- **Licensing**: verified against the files actually inside the fetched archive, not assumed.
  PDFium itself is BSD-3-Clause (`licenses/pdfium.txt`), matching `file-format-support.md`'s
  existing "ADOPT" row. AGG 2.3 (`licenses/agg23.txt`) is the permissive pre-2.5 line, not the
  GPL 2.5+ line — evidence, not clearance; the residual risk is carried forward in
  `THIRD_PARTY_NOTICES`, not resolved here. The distributor's own build scripts are MIT.
  11 further bundled third-party notices (abseil, freetype, icu, libjpeg_turbo, zlib, ...)
  copied into `third_party/pdfium/licenses/` and indexed in `THIRD_PARTY_NOTICES`.

**Tests (real, 22 assertions, `uapp.pdf.read`/`uapp.pdf.render`/`uapp.pdf.malformed`, ctest
12/12 including the 9 pre-existing suites unchanged)**: open a valid two-page PDF from a
buffer; correct per-page `MediaBox`; move semantics; idempotent close; `Library` refcounting
returns to zero; render writes real (non-blank) pixels into the caller's buffer; **a padded
stride is honoured and the write stays inside the buffer bounds** (the fix for a real bug
found mid-development: PDFium requires a 4-byte-aligned stride, and an unaligned caller-chosen
stride was not being rejected before this); repeated renders over the same buffer agree; a
genuinely blank page renders as background, not uninitialized memory; empty/null/truncated/
non-PDF/malformed-xref input all produce defined `Status` errors, never a crash; buffer-size
arithmetic cannot be made to overflow; a password containing an embedded NUL is rejected
outright rather than silently truncated at the NUL.

**What is still owed before this leaves bootstrap status** (see
`third_party/pdfium/README.md`): the hermetic from-source build SP-1 actually recommended, for
every real target platform including Android/iOS; resolution of the AGG-version residual risk
as a documented legal call, not just a flagged one; a fuzz target for the PDF parsing path
(spec §29 names the PDF parser first in its required-fuzz-targets list — `file-engine/
detection` has one, `pdf-engine/reader` does not yet); everything else in spec §11 beyond
viewing (edit, merge/split, forms, annotation, signatures, PDF/A, watermarks, redaction).

### Added — `fuzz_detect`: a real libFuzzer target over `file-engine/detection` (spec §29)

Closes the gate the `file-engine/detection` entry below left open (`testing-strategy.md` §6:
"a parser without a fuzz target does not merge"). It is the **first** fuzz target in the
repository, and it fuzzes the **buffer** entry point — `detect_buffer` and `describe_buffer` —
because that is the untrusted-upload shape: raw bytes, no path, no filesystem, no assumption
of well-formedness. It is what the mobile picker and the eventual HTTP upload handler call,
and the first first-party code that ever touches an attacker-supplied file.

- **`tests/fuzz/fuzz_file_detection.cpp`** — `LLVMFuzzerTestOneInput` feeds the input bytes in
  verbatim: no length prefix, no framing, no embedded control fields. That is a constraint,
  not a shortcut — it means every corpus entry is a real file, a crash reproducer is directly
  usable as a regression fixture, and nobody can later mistake a harness framing bug for a
  detector bug. The filename, the one thing file bytes cannot carry, is supplied from a fixed
  table indexed by the input's own first byte, so the advisory-hint ladder
  (`extension_of` → registry lookup → conflict resolution) is exercised deterministically
  without contaminating the corpus file format.
- **An oracle, not just a crash check.** ASan/UBSan catch memory errors; they do not catch a
  self-contradictory verdict, which for a detector is both likelier and still a security
  finding, since callers gate on `confidence` and `extension_conflicts`. Every input is
  additionally checked against the invariants `detector.h` states in prose — the filename can
  never set `format`, an identified format never carries `None` confidence, `magic_offset` is
  PDF-only and never past the input, container flags never appear on a non-ZIP result, two
  calls on identical bytes agree, `describe_buffer` never contradicts `detect_buffer`. **Only
  invariants the public header actually promises** are asserted; a stricter rule than the
  contract would manufacture fake defects.
- **`UAPP_ENABLE_FUZZING`, default OFF, a third build configuration rather than a flag on the
  normal one.** libFuzzer supplies `main`, so `-fsanitize=fuzzer` cannot go into the shared
  compiler posture without turning the CLI and all four test binaries into fuzzer drivers.
  What is applied project-wide under the switch is `-fsanitize=fuzzer-no-link,address,
  undefined` — the coverage instrumentation, with `main` left alone — and the fuzz target
  alone adds the runtime at link. Instrumenting the *library* and not just the harness is the
  point: a target built against an uninstrumented `uapp::file_detection` is a random byte
  generator with no feedback. With the switch ON, tests and tools default OFF and the SQLite
  probe is skipped, so a fuzz build produces the library under test and the harness and
  nothing else. No fuzz target is registered with ctest: it has no pass/fail, it has a budget.
- **A 24-file seed corpus** in `tests/fixtures/corpus/fuzz_file_detection/` (§6.2's location),
  holding a minimal but genuinely valid example of **each of the ten formats detection
  implements** plus the malformed shapes a mutator would take a long time to build — no
  central directory, a truncated one, an EOCD pointing past EOF, a Zip64 sentinel, a
  65534-entry claim, a bare `PK\x03\x04`, an unrecognised OOXML family, a PDF behind a
  300-byte preamble, an empty file. The ZIP-family seeds are byte-identical to what
  `tests/file_engine/zip_fixture.h` already builds, so fixture and corpus cannot drift apart.
- **A configure-time guard rail for macOS.** Apple's Command Line Tools clang accepts
  `-fsanitize=fuzzer` but ships no `libclang_rt.fuzzer_osx.a`. The configure step link-probes
  and fails with the `brew install llvm` remedy rather than letting the build reach the linker
  several hundred files later. This was not hypothetical — it is what this machine does.

**The run, actually performed** (LLVM 23.1.2, Debug, ASan+UBSan; full record in
`testing-strategy.md` §6.3): three bounded runs of 121 s each — 2 928 476, 2 166 152 and
2 247 966 executions at 24 202 / 17 902 / 18 578 exec/s — plus a 61 s re-run of the documented
smoke command verbatim against a clean rebuild (2 006 441 executions at 32 892 exec/s).
**9 349 035 executions over 424 s: zero crashes, zero hangs, zero sanitizer reports, zero
oracle violations.** Coverage settled at
907 edges / 1 877 features with the corpus plateaued at 154 units, so this is a *saturated
smoke run for this corpus and length cap* — not a claim that the module is clean, which needs
the ≥ 4 h nightly cadence §6.2 specifies. The third run raised libFuzzer's default 4096-byte
cap to 128 KiB, because the default never reaches the 8 KiB text-probe boundary or the 64 KiB
EOCD backward scan; it added 7 features and no defects.

**No product defect was found, and none is claimed.** That is a plausible result rather than a
surprising one: `ByteSpan` was built so that no accessor *can* read out of range and the ZIP
walk is capped and overflow-guarded, which is the property a first fuzz run is supposed to
confirm. The one bug the work did surface was in the **corpus generator**, not the product —
its central-directory record omitted the mod-date field, so the DOCX/XLSX/PPTX seeds were
silently degrading to plain `zip` and the fuzzer was being seeded with three fewer real
formats than intended. Caught by asserting each seed's detected format before the first run;
fixed in the generator, not in the detector. Recorded because a silently-wrong corpus is
exactly the failure mode that makes a fuzz run look fine while testing much less than it says.

**Verification.** The fuzz build configures and builds clean at `-Werror` under LLVM 23. The
normal build is untouched: a from-scratch `cmake -S . -B build_check && cmake --build
build_check && ctest` with `UAPP_ENABLE_FUZZING` at its default OFF gives **ctest 9/9** and the
same suite counts as before — notes 42, blocks 36, file_engine 65, capi 17 — with no
`-fsanitize` flag anywhere in `compile_commands.json`.

**NOT IMPLEMENTED**: the other seventeen targets in §6.1 (`fuzz_pdf_parse`, `fuzz_zip`,
`fuzz_xml`, `fuzz_sync_delta`, …). Each names a parser that is still an empty Phase 0
directory; a stub harness over a module that does not exist would turn the merge gate into a
checklist of targets that fuzz nothing. Also not done: the CI wiring for the §6.2 cadence
(60 s per PR, ≥ 4 h nightly, 72 h release gate) — there is no CI pipeline in this repo yet;
structure-aware mutation for the OOXML family; and MSan, which §6.2 requires to run separately
and which needs an instrumented libc++ this repo does not build.

### Added — `file-engine/detection`: real file-type detection (spec §8/§9)

The foundational answer to "what is this file, and what may we do with it?", which the PDF
engine, scanner, OCR and conversion will each call **before** trusting a filename. Detection
only: this module does not open, parse, render or convert anything, and must not — it is what
decides whether a parser should be invoked at all (`architecture.md` §5).

**Spec §8's "never trust the file extension alone" is enforced structurally, not by
convention.** `DetectionResult::format` is only ever assigned from bytes; there is no code
path by which a filename can set it. What the name claimed goes to `extension_hint`, and a
disagreement raises `extension_conflicts` — the security-interesting output, since an
`invoice.pdf` whose bytes are a ZIP is worth showing a human. The deliberate consequence: a
`.mp3` whose sniffer does not exist detects as `Unknown`, not as MP3. That is the honest
answer.

- **Detection implemented for ten formats**: PDF, PNG, JPEG, GIF, ZIP, DOCX, XLSX, PPTX, ODT,
  and UTF-8/ASCII plain text. Signatures are compared in full — all eight PNG bytes, not four,
  because the CRLF/EOF guard bytes exist precisely to catch the corruption a short check
  misses. PDF is scanned across the first 1 KiB rather than at offset 0 (real files carry
  preambles; every other viewer accepts them) and the **offset is reported** rather than
  silently normalised away.
- **The ambiguous case is the substance of the module.** DOCX, XLSX, PPTX, ODT, EPUB and an
  archive of holiday photos share the same four magic bytes. Two strategies, and which one was
  used is **reported** so a security decision is never taken on a guess: a **central-directory
  walk** (authoritative → `MagicAndContainer` confidence) and a **local-header walk** (best
  effort, defeatable by a crafted archive → capped at `Magic`, with `container_truncated` set).
  ODF is read from its `mimetype` member, which the ODF spec mandates be first and STORED for
  exactly this purpose. An OOXML package we cannot name stays `Zip` and sets
  `unrecognised_ooxml` — never a guess between the three families.
- **The API works on a path *and* on a buffer**, because a mobile picker returns a
  security-scoped URI or bytes, never a POSIX path. One engine (`detect_sample`, pure and
  `noexcept`) under three entry points; `detect_path` reads a bounded **head and tail** window
  and never the whole file — the tail exists solely because a ZIP's member list lives at the
  end, so a 10 MB DOCX is classified authoritatively from ~192 KB of reads.
- **The spec §8 `File` value** with all eleven fields. `location` is a tagged `FileLocation`,
  not a string: on mobile there is often no path, and a fake path is a path something will
  eventually try to open. `id` and timestamps are **passed in**, not generated — a detector
  that reaches for an ambient clock or RNG is untestable, and both are ports
  (`core/common/ports.h`).
- **Checksum: FNV-1a 64 with an avalanche finaliser**, tagged `ChecksumAlgorithm::Fnv1a64`.
  Chosen over SHA-256 because this field is an *integrity digest*, not a signature: libsodium
  is not vendored yet and §7.10 requires seven steps before any dependency lands, hand-rolling
  SHA-256 in the detection module is worse, and SHA-256 costs ~10× per byte over every byte of
  every file opened on Tier D hardware. Chosen over xxHash for the same §7.10 reason. The
  finaliser fixes FNV-1a's weak low-bit avalanche for three instructions *per file*.
  **`Checksum` is deliberately shaped as `{algorithm tag, length, 32-byte array}`, not a bare
  `uint64_t`** — adding SHA-256 later is then a new enumerator, not a data migration.
- **Capabilities are derived from `file-format-support.md`, not hand-written.** The derivation
  rules are now **§12 of that document**, and the notable results are enforced by tests:
  **DOCX gets no EDIT** (the matrix marks it P2), **PPTX gets EXTRACT and nothing else** (v1.0
  is text extraction for search), **nothing at all gets SIGN** (§5.1: "a half-implemented
  signature check is worse than none"), **only PDF gets ENCRYPT** (not ZIP — AES-ZIP is
  read-only), and **JPEG gets no COMPRESS** (re-encoding is lossy). An unidentified file gets
  the **empty** set, never a default of VIEW.
- **Malformed input is a first-class requirement (spec §28/§29), handled by the type system
  rather than by scattered length checks.** Every byte read goes through `ByteSpan`, which has
  no accessor that can read out of range: `equal_at` returns false rather than comparing a
  short buffer, `read_u32le` never half-writes its output, `subspan` clamps, and every
  `offset + length` is computed with an overflow guard so a `0xFFFFFFFF` length from a corrupt
  ZIP header cannot wrap. The ZIP walks are entry-capped, name-capped, must strictly advance,
  and reject a central directory whose offset lies outside the file rather than clamping it
  into live content.

**Deviation from `file-format-support.md`, recorded in its new §12.** Spec §8 names ten
capabilities; the matrix has six columns. **No existing cell was changed** — implementing
detection contradicted nothing — but EXPORT, COMPRESS, SIGN, ENCRYPT and EXTRACT had no column
and are now *defined* there (EXPORT ⇔ the CREATE column; COMPRESS ⇔ lossless repack where we
ship a writer; SIGN ⇔ nothing; ENCRYPT ⇔ PDF only; EXTRACT ⇔ the ZIP family and PDF).

**NOT IMPLEMENTED** (declared with a permanent enum value and a machine-readable reason, never
faked — the `block_type_not_implemented_reason()` precedent):

- **The other ~35 formats of spec §9.** The registry enumerates all of them, and answers three
  *different* questions: `format_detection_is_implemented(f)`, why not
  (`format_detection_not_implemented_reason(f)`), and — separately —
  `DetectionResult::format == Unknown`, meaning the sniffer ran and matched nothing. A test
  asserts the two queries can never disagree and that no unimplemented format is left without
  a reason. Summaries: legacy **DOC/XLS/PPT** share one OLE2/CFB header, so sniffing it names
  the container not the document, and §2 marks all three server-only anyway; **MD/HTML/XML/
  JSON/CSV/SVG/FB2/RTF** are text dialects with no magic number, so they detect as PlainText —
  true rather than wrong; **ODS/ODP/EPUB** need only a member-path signature added to the
  existing introspection; **HEIC/AVIF/MP4/MOV** share the ISO-BMFF `ftyp` brand space and
  deserve one sniffer rather than four half-sniffers; audio/video are deferred behind PQ-5 and
  the FFmpeg decision; **WEBP/TIFF/BMP/TAR/GZ/7Z** have trivial signatures but no engine
  behind them, and an unconsumed detection result is a claim with nothing behind it; **RAR** is
  permanently out of scope (§7.7).
- **Zip64 central-directory parsing.** The sentinels are *detected* and cause a documented
  fallback to the local-header walk, rather than following `0xFFFFFFFF` as a literal offset.
  Tested.
- **Legacy 8-bit charset detection** (Windows-1252, ISO-8859-x, Shift-JIS). A statistical
  problem needing ICU's `CharsetDetector` (§7.1 ADOPT, not integrated). Such a file fails the
  UTF-8 check and reports `Unknown` — "we cannot tell what encoding this is" beats handing
  mojibake to an editor.
- **Format-specific metadata** — EXIF/XMP, the PDF Info dictionary, OOXML `docProps`, ID3. All
  require opening the format. Putting an untrusted-input parser inside the module whose job is
  to decide whether a parser should run at all would invert `architecture.md` §5.
- **`DetectionHints::declared_mime_type` is accepted and ignored.** Declared now because the
  upload and Android-picker call sites are being written against this header, but using it
  needs a trust order between a host-asserted type and our own sniffing — a `security-model.md`
  policy question. Today the bytes win unconditionally, which is the safe default to be stuck
  with.
- **`FileLocation::Kind::PlatformUri` cannot be resolved**; that needs the platform vtable.
  Carried, not dereferenced.
- ~~**No fuzz target yet**~~ — **closed below**; `detect_buffer` now has one.

**One edit to `core/capi`, which this change was otherwise to leave alone** — the same
tripwire, and the same resolution, as the `core/blocks` change below. Adding the four 6xx
`Status` codes broke its *exhaustive* `to_c_status` switch under `-Werror=switch`. They are
listed explicitly and mapped to `UAPP_STATUS_UNEXPECTED`; **no new `UAPP_STATUS_*` constant was
minted** and no ABI surface changed, because `core/capi` still exposes only `NoteService` and
nothing reachable through that boundary can return one of them. That switch exists to force
this decision rather than allow a silent `default:`, so extending it is the mechanism working.

**Verification.** Configured and built from scratch in **Debug**, **Release** and
**Debug+ASan/UBSan**: **zero warnings under `-Werror`** in all three, with `-fno-exceptions`
and `-fno-rtti` unchanged. `ctest` **9/9 passed** in each. `uapp_file_engine_tests` reports
**65 passed, 0 failed**, including under ASan/UBSan. The new binary links **only**
`uapp::file_detection` → `uapp::core_common`, so "detection has no storage and no document
model behind it" is checked by the linker rather than by review.

**Two defects were found by these tests and fixed.** (1) The container/content exemption in
`resolve_conflict` was symmetric, so it suppressed the mismatch flag for *any* ZIP — meaning a
plain ZIP renamed `vacation.jpg` was reported without a conflict, defeating the one signal the
flag exists for. The rule is now asymmetric and evidence-aware: a `.docx` named `.zip` is not a
lie, a ZIP named `.jpg` is, and a ZIP named `.docx` is a conflict only when the directory walk
actually *proved* the `word/` part absent — a truncated container is unproven, not guilty.
(2) `central_directory_unreachable` was set whenever the directory scan produced no useful
evidence, conflating "we walked it and this is just a ZIP" with "we could not read it", which
would have lowered confidence on every ordinary archive.

### Added — `core/blocks`: the real block document model (spec §14)

The flat note is superseded. This implements the structure spec §14 actually asks for —
`Workspace -> Page -> Blocks`, with blocks nested inside blocks — as the model everything
later (backlinks, comments, mentions, block-level sync, version history) can anchor to. A
`body` string has nothing to anchor to; a block id does.

**Note vs. Page — the architectural call.** `Page` wins; `Note` is **deprecated in place, not
deleted**, and the two coexist for exactly as long as it takes to move `core/capi`. Reasoning
and the four-step retirement path are in **`docs/adr/0003-page-supersedes-note.md`**. The short
version: `core/capi` is built on `NoteService`, and the C ABI is the contract the Kotlin and
Swift bindings will be written against — churning it while the block model is one day old is
the wrong order. `Page` is also, today, *less* capable than `Note` in one respect (no
full-text search), so folding `Note` into it now would remove working functionality. `Note`
becomes a page holding one paragraph block once block FTS and the block ABI exist; no id
migration is needed, because `NoteId` was given the real 16-byte shape in the bootstrap slice.
`core/blocks` deliberately does **not** link `core/notes` — the build graph enforces that the
successor never acquires a dependency on what it replaces.

- **`core/blocks`** (`uapp::core_blocks`) — the portable domain. Links no database library, so
  PHASE0_SUMMARY.md H-3 stays enforced by the build graph rather than by convention.
  - **`Block` content is a `std::variant` over typed payload structs, not a class hierarchy.**
    Three reasons, recorded in `block.h`: a closed tagged union maps to C as
    `{int32_t type; union {...};}` mechanically, and ADR-0001 commits us to a C99 ABI; ADR-0001
    also bans RTTI, so a hierarchy would need a hand-rolled type tag anyway — a tagged union
    wearing a vtable; and the type set is closed and ours, which is exactly when `std::visit`'s
    compile-time exhaustiveness beats open polymorphism.
  - **Eight block types with real typed fields** (no opaque strings): `paragraph`, `heading`
    (level 1–3), `bulleted_list_item`, `numbered_list_item`, `checklist_item` (`checked` bool),
    `code` (`language` tag), `divider`, `quote`. Numbered-list numbering is *derived* from
    sibling position, never stored — storing it goes stale on every insert.
  - **The variant index IS the persisted type tag.** `static_assert`s pin each alternative to
    its `BlockType` value, so reordering the variant is a compile error instead of a silent
    reinterpretation of every stored block.
  - **`Page`** with id, title, timestamps and an ordered tree of blocks.
- **Nesting is real, and depth-capped at 64** (`kMaxBlockDepth`). Every read of a block tree is
  recursive, and the tree is user-controlled, so unbounded nesting is a stack-exhaustion DoS
  reachable from a restored or synced document — spec §38's "validate external input" applies
  to our own files too. The cap is enforced on **insert**, on **move** (destination depth +
  the moved *subtree's* height, not just the block) **and on read**, so a hand-edited database
  cannot smuggle a deeper tree past it. The two tree-measuring helpers are iterative with an
  explicit stack: the function that decides whether a tree is safe to recurse into must not
  recurse into it to find out.
- **`storage/sqlite/src/sqlite_block_store.cpp`** — the relational adapter. Pages and blocks in
  `page` + `block` tables with real columns and foreign keys, **not** a JSON blob: order,
  nesting and per-block identity have to survive into SQL for search, backlinks and block-level
  sync later. Same discipline as the note store — RAII `Connection`/`Statement`/`Transaction`,
  every mutation in one all-or-nothing transaction, `Status`-based propagation, no
  `<sqlite3.h>` in any public header, schema-version check that refuses a newer database.
  Deleting a subtree and re-densifying positions are one statement pair inside one transaction,
  so a half-applied move is unrepresentable.
- **Operations** (`DocumentService`): create page, open page (full tree), rename, delete page,
  add block at a position and nesting level, append, read block, update content (the type may
  change; children survive), delete block, reorder, move to a new parent. Sibling positions are
  **dense 0..n-1** after every operation, and an index past the end is `BlockPositionInvalid`
  rather than a silent clamp — a caller working from a stale tree should be told.
- **Delete cascades to children; it does not reparent them.** A nested bullet under a deleted
  bullet, or the body of a deleted toggle, is bound to its parent — promoting orphans into the
  grandparent produces a document the user did not write. It is also what
  `database-design.md` §3.3 already specifies (`ON DELETE CASCADE` on `block.parent_id`). Undo
  is the right answer to "I did not mean that", and is NOT IMPLEMENTED (below).
- **`core/common` gained `ObjectId<Tag>` and the generic `Clock`/`IdSource` ports.** The
  16-byte identifier and its hex parser now exist once instead of being re-hand-rolled per
  domain; the phantom tag keeps `PageId` and `BlockId` distinct types at zero runtime cost
  while staying a 16-byte standard-layout aggregate for the future C ABI. `uapp::Status` gained
  a generic `IdMalformed` (104) and an append-only 5xx block range.
- **`tests/`** — a **separate** `uapp_blocks_tests` binary (CTest: `uapp.blocks.unit`,
  `uapp.blocks.store`) that links `uapp::storage_sqlite` and never names `uapp::core_notes`, so
  ADR-0003's independence claim is checked by the linker. **36 cases**: create a page and fill
  it with every implemented payload; **persist and reload across a real close/reopen, asserting
  order and nesting exactly** (three top-level blocks, two children, a grandchild, parent links
  round-tripped); positional insert; update content and change a block's type with children
  intact; **delete cascading to a whole subtree** plus the sibling gap closing; delete page;
  reorder (front, back, middle-to-middle); **move to a new parent with the subtree travelling
  with it**, and back out to the top level; cycle rejection (under a descendant, and under
  itself); **the depth guard accepting exactly 64 and rejecting the 65th**, with nothing written
  by the failed attempt; a move refused because the *subtree* would not fit where a lone block
  would; and error paths — block on a nonexistent page, nil ids, parent on another page,
  position past the end, unknown block id, read-only database, entropy-source failure. Plus the
  domain-only suite: id hex round-trip and rejection, the type table, payload validation
  (heading level, embedded NUL, oversize, code-language control bytes) and tree validation.

**Two real defects were found by these tests and fixed**, both recorded because neither is
visible by inspection. (1) `Statement::bind_text` in the shared `sqlite_raii.h` bound **SQL
NULL** for any empty `std::string_view`, because a default-constructed view has
`data() == nullptr` and `sqlite3_bind_text` reads a null pointer as NULL. Against a `NOT NULL`
column that surfaced as `SQLITE_CONSTRAINT` — reported as "already exists" for a row that did
not exist. This was latent in the note store too. (2) The tree reassembler rejected a tree at
*exactly* the depth limit, because it judged depth before checking whether the deepest block
had any children at all.

**Deliberate deviations from `database-design.md` §3.3**, recorded so they are not mistaken for
oversights, both with their migration written down at the point of deviation:

- **Ordering is an integer `sort_index`, not a fractional `sort_key`.** The design doc is right
  that integer positions turn a 1-block insert into an N-row renumber, which becomes an N-block
  *conflict* once sync exists. That cost is only paid once sync exists (Phase 13); today there
  is one writer on one device, the renumber is the tail of one sibling list inside the same
  transaction, and dense positions make every ordering assertion exact instead of a comparison
  of two opaque strings. Deferred, not rejected: the migration is one `ALTER TABLE` plus a
  single backfill pass in `sort_index` order, with no change to any identifier.
- **Block content is in four typed columns, not a CBOR blob.** CBOR is the right answer for
  nineteen types with disjoint fields; with the eight implemented here the whole payload space
  is `text`/`checked`/`heading_level`/`language`. A hand-rolled codec would need its own fuzz
  target before it could be trusted with a file off the network. `schema_ver` is present from
  day one, and `block.h` names the trigger for the switch: the first type (`table`, `image`, …)
  whose payload does not fit these columns.

**NOT IMPLEMENTED** (declared, reasoned, and not faked):

- **Eleven of the nineteen §14 block types.** `toggle`, `callout`, `table`, `image`, `video`,
  `audio`, `file`, `equation`, `embed`, `database`, `bookmark` are enumerated in `BlockType`
  with permanent numeric values and each returns a machine-readable reason from
  `block_type_not_implemented_reason()` — in code, not in a comment, so the reason cannot rot
  and a UI can show it. Summaries: `table`/`database` need the §15 property and view system
  (explicitly a later run); `image`/`video`/`audio`/`file` need the blob store, since
  database-design.md principle 6 forbids putting bytes in the database; `embed`/`bookmark`
  fetch remote URLs, which breaks offline-first and needs the network policy gate and URL
  allow-listing; `equation` needs a LaTeX subset parser that deserves its own fuzz target;
  `toggle` needs per-viewer collapsed/expanded view state (its child-block half is already
  covered by generic nesting); `callout` needs the icon and colour-token vocabulary.
  Constructing one is impossible — there is no variant alternative — and reading one written by
  a newer build yields `BlockTypeUnsupported` rather than a silently-wrong empty paragraph.
- **Undo/redo**, and therefore **version history**. Out of scope for this run by instruction,
  and deliberately *not* a stub method: doing it properly means an operation log with inverses,
  which changes how every mutation is recorded. Bolting it on later is cheaper than faking it
  now.
- **Copy/paste** (needs a clipboard serialisation format), **backlinks** and **mentions** (need
  inline rich-text spans plus a link index), **comments**, **sharing**, **permissions**,
  **templates**.
- **Rich text.** A block's text is a flat UTF-8 string. Inline bold/italic/link spans need an
  inline run model plus the sequence CRDT that `database-design.md` reserves `block.crdt_state`
  for.
- **Child pages, databases, files and workspaces** — the other three arms of the §14 tree.
  `page` has no `parent_id` or `workspace_id` column: absent rather than present-and-ignored,
  so nothing can half-work. All three are additive migrations.
- **Full-text search over block text.** No counterpart to `note_fts` yet; it needs the
  denormalised `text_cache` fed into FTS5 under the same one-transaction discipline. This is
  the one respect in which `Page` is currently *less* capable than `Note`, and it is step 1 of
  ADR-0003's retirement path.
- **Cross-page block moves** — rejected with `InvalidArgument`. The whole subtree's `page_id`
  would have to be rewritten, and once backlinks and per-page FTS exist those indexes move with
  it. A half-moved subtree is a corrupt document, so refusing is the honest answer.
- **UUIDv7 layout.** Identifiers are 16 random bytes, so they lack the B-tree locality v7 buys.
  The shape is already right, so this is a one-file change with no format migration.

**One edit to `core/capi`, which this change was otherwise to leave alone.** Adding Status
codes broke its *exhaustive* `to_c_status` switch under `-Werror=switch`. The nine new codes
are listed explicitly and mapped to `UAPP_STATUS_UNEXPECTED`; no new `UAPP_STATUS_*` constant
was minted. The C ABI is separately versioned and its codes are permanent, and nothing
reachable through that boundary can return one of these yet — `core/capi` still exposes only
`NoteService`. No ABI surface was added or changed.

**Verification.** Built from scratch in Debug, Release, and Debug+ASan/UBSan: **zero warnings
under `-Werror` in all three**, and `ctest` **6/6 passed** in each. `uapp_blocks_tests` reports
**36 passed, 0 failed**; the pre-existing suites are unchanged — `uapp_notes_tests` still
**42 passed, 0 failed**, the C ABI binary still **17 passed, 0 failed**, and
`uapp.notes.cli_demo` still passes.

### Added — Phase 1 bootstrap: the note vertical slice (first production code)

Implements exactly the slice spec §40 names as the task after architecture approval:

```text
CREATE NOTE -> STORE LOCALLY -> SEARCH NOTE -> OPEN NOTE -> EDIT NOTE -> SAVE NOTE
```

Nothing else from Phase 1 is done. This is the seed, not the phase.

- **Buildable CMake project at the repo root.** C++17 per ADR-0001. Top-level
  `CMakeLists.txt` plus `cmake/UappCompilerOptions.cmake`, which encodes the ADR's
  restricted subset as toolchain flags — `-fno-exceptions -fno-rtti`, `-Wall -Wextra
  -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast -Werror`. Options:
  `UAPP_BUILD_TESTS`, `UAPP_BUILD_TOOLS`, `UAPP_ENABLE_SANITIZERS`, `UAPP_WARNINGS_AS_ERRORS`.
- **`cmake/FindSQLite3Fts5.cmake`** — locates SQLite *and compiles-and-runs a probe* proving
  FTS5 is present, failing configuration otherwise. An FTS5-less libsqlite3 would not fail at
  link time; it would fail on a user's device the first time anyone searched.
- **`core/common`** (`uapp::core_common`) — `uapp::Status`, a flat append-only error enum with
  both a stable machine-readable code (`NOTE_NOT_FOUND`) and a human-readable message, per §33.
  Every fallible call in the core returns it and is `[[nodiscard]]`.
- **`core/notes`** (`uapp::core_notes`) — the portable domain. `NoteId` (16-byte opaque id,
  validating hex parse), `Note`, validation with hard size limits, the `NoteStore` persistence
  **port**, `Clock`/`IdGenerator` ports, and `NoteService` — the public API implementing the
  six slice steps. Links no database library at all, so PHASE0_SUMMARY.md H-3 ("nothing outside
  `storage/sqlite` includes `sqlite3.h`") is enforced by the build graph, not by convention.
- **`storage/sqlite`** (`uapp::storage_sqlite`) — the SQLite adapter. RAII wrappers
  (`Connection`/`Statement`/`Transaction`, rollback-on-destruction) so no early return can leak
  a statement or double-free one; WAL + `foreign_keys=ON` + busy timeout; bind helpers that
  check every `size_t`→`int` narrowing before it reaches the C API; content and FTS5 index
  written in **one transaction** per database-design.md §3.9; schema-version check that refuses
  a database written by a newer build.
- **`storage/sqlite/fts_query`** — converts raw user input into a safe FTS5 `MATCH` expression
  via an allow-list (word bytes only, each term double-quoted, ANDed, last term prefix-matched).
  Binding user text straight to `MATCH` is not SQL injection but *is* injection into FTS5's
  query language; this is where §38's "validate external input" applies to search.
- **`tools/notes_cli`** (`uapp-notes`) — developer harness. `demo` drives the whole slice
  against a real on-disk database and verifies every step, exiting non-zero on the first
  failure; `create`/`search`/`open`/`edit`/`delete`/`count` allow manual use.
- **`tests/`** — 42 unit/integration cases plus a CLI integration test, wired into CTest as
  `uapp.notes.unit` and `uapp.notes.cli_demo`. Coverage includes create, persist-and-reload
  across a closed connection, FTS hit *and* miss, prefix match, edit-then-reload, index
  freshness after edit and delete, UTF-8 round-trip, and eleven error-path cases (unknown id,
  nil id, save-after-delete, oversized title/body/query, read-only database write rejection,
  unopenable path, empty path, entropy-source failure, duplicate id, hostile search input).

**Verification.** Debug, Release, and ASan+UBSan configurations all build with zero warnings
under `-Werror`; `ctest` is 2/2 and the unit binary reports **42 passed, 0 failed** in each.

### Added — the C ABI (`libuapp_capi`): the platform-bridge boundary

Infrastructure, not a feature. The note slice already worked and was tested; this gives it a
C-callable face so the Android (JNI) and iOS (Swift/ObjC) bindings have something to attach to.
No note behaviour changed — `core/notes` and `storage/sqlite` are untouched.

Implements PHASE0_SUMMARY.md **H-2** (split the spec §4 "platform bridge" into a versioned C
ABI plus hand-written idiomatic bindings, so a C++ ABI never crosses JNI or Swift interop) and
the ABI rules in `api-design.md` §10 / `mobile-architecture.md` §2.

- **`core/capi`** (`uapp::capi`) — two pure C99 public headers. `uapp/capi/uapp_capi.h` carries
  the ABI version, the status enum and the string-ownership rules; `uapp/capi/uapp_notes.h`
  carries the note surface. Opaque handles only (`uapp_note_service_t*`, `uapp_note_t*`,
  `uapp_search_results_t*`); no C++ type, no STL container and no C++ ABI crosses the line. The
  implementation is C++17 under the same `-fno-exceptions -fno-rtti -Werror` posture as
  everything else. It is also the composition root that binds the abstract `NoteStore` port to
  the SQLite adapter — a PRIVATE link, so nothing above it inherits a path to `sqlite3.h`.
- **Versioned per spec §30.** `UAPP_ABI_VERSION_MAJOR/MINOR/STRING` plus real exported
  accessors and `uapp_abi_is_compatible()`, so a binding can interrogate the library it
  actually loaded rather than the header it compiled against. Structs crossing the boundary
  carry a leading `uint32_t struct_size`; a caller with a smaller struct is rejected, one with
  a larger struct is served. **Major is 0 deliberately** — the machinery is complete and
  enforced, the surface is not yet frozen. The freeze and the ABI-diff CI job over the exported
  symbol list (`api-design.md` §10 rule 5) land with the first shipped release.
- **Every fallible function returns `uapp_status_t`**, a C mirror of `uapp::Status` whose
  numeric values are asserted identical to the C++ enum's — code by code, at compile time — so
  a renumbering in the core is a build failure rather than a shipped client reporting the wrong
  error a release later. Two C-ABI-only codes in a 9xx block: `OUT_OF_MEMORY` and `UNEXPECTED`.
  The only `void` functions are the destructors and the pure version/string lookups.
- **Nothing unwinds across the boundary.** Every exported body is wrapped in a guard that
  converts an escaping exception into `UAPP_STATUS_UNEXPECTED`. Under the shipping
  `-fno-exceptions` build it compiles to nothing; it exists because the flag is per translation
  unit, and a future engine linked in with exceptions on must not unwind through a JNI frame.
- **String ownership is a two-rule contract, and the function names carry it.** `uapp_*_get_*`
  borrows from the handle (freeing it is a bug; it dies with the handle). `uapp_*_copy_*`
  transfers ownership and **must** be released with `uapp_free_string()`, never `free()` — the
  host and the core need not share an allocator. Status and version strings are static storage.
- **Handle hygiene.** Every destructor takes a *pointer to* the handle and NULLs it, so
  double-release is a no-op and the ordinary use-after-close mistake degrades to
  `INVALID_ARGUMENT` instead of a native crash. Each handle type carries a distinct magic
  cookie, so a NULL, zeroed or **wrong-type** handle is rejected rather than dereferenced. This
  is defence in depth, not a memory-safety claim, and the header says so.
- **No hidden global state.** No `uapp_ctx_t` singleton, no thread-local last-error slot, no
  process-wide registry. A service handle owns its connection, clock and id generator; any
  number may coexist and they share nothing. Proved, not asserted — see the tests.
- **`tests/capi/note_capi_test.c`** — **a C file**, compiled by the C compiler at `-std=c99`
  with `-Wall -Wextra -Wpedantic -Wconversion -Werror`, including only the two public headers
  and linking only `uapp::capi`. It is simultaneously the functional test and the
  C99-conformance check `architecture.md` §8 rule 5 asks for: a C++-ism in a public header
  breaks the build. 17 cases, wired into CTest as `uapp.capi.slice` and `uapp.capi.boundary`.
  Coverage: the full slice (create/store/search/open/edit/save/delete/count), persistence
  across close-and-reopen, UTF-8 round trip, search-limit default and clamp, **two simultaneous
  independent services proving isolation in both directions**, and boundary safety — null
  handles on every entry point, null strings, null out-parameters, use-after-close,
  double-close, wrong-type handles, malformed ids, oversized input, out-of-range result
  indices, `struct_size` versioning, and an unopenable database.
- **Build.** The project now enables C as a first-class language (`CMAKE_C_STANDARD 99`,
  extensions off). `cmake/UappCompilerOptions.cmake` and `core/common/CMakeLists.txt` gate the
  C++-only flags (`-fno-exceptions`, `-fno-rtti`, `-Wold-style-cast`, …) on
  `COMPILE_LANGUAGE:CXX`, so one options target still governs both languages and the warning
  posture is identical for each.

**Deliberate deviations from the `api-design.md` §10 sketch**, recorded so they are not mistaken
for oversights. That sketch describes the eventual full ABI; this is the note slice only. No
`uapp_init`/`uapp_ctx_t` — a process-wide context is exactly the hidden global state a
multi-handle design must avoid, and nothing here needs one. No `uapp_last_error()` TLS slot —
errors are self-describing codes with static message strings, which suffices until the
`uapp_error_t` cause chain exists. No FlatBuffers payloads — a flat title+body note has no
structured payload to carry; they arrive with the block tree. No job/progress/cancellation
protocol — every operation here is a local SQLite call and none can block indefinitely, so it
would be scaffolding around nothing.

**Verification.** Built from scratch in Debug, Release, and Debug+ASan/UBSan: **zero warnings
under `-Werror` in all three**, and `ctest` **4/4 passed** in each. The C ABI binary reports
**17 passed, 0 failed**; the pre-existing C++ suite still reports **42 passed, 0 failed**,
unchanged. The public headers additionally compile clean standalone under
`-std=c99 -pedantic-errors`, `-std=c11` and `-std=c17`, and as C++11/C++17 with
`-Wold-style-cast -fno-exceptions -fno-rtti` (the posture an ObjC++ iOS bridge would use).
macOS `leaks` reports **0 leaks** for the C ABI test binary.

Two defects were found by tooling during this work and fixed; both are recorded because neither
is visible by inspection. **UBSan** caught that reading an out-of-range `uapp_status_t` is
undefined behaviour — and a C caller can pass any `int` — fixed by pinning the enum with a
`UAPP_STATUS_FORCE_INT32` sentinel, which also guarantees the type is 32 bits wide on every
toolchain instead of implementation-defined. **`leaks`** caught that nulling an out-parameter on
a failure path silently discards whatever handle the caller's variable was holding; the ABI now
matches `NoteService`'s own contract — **out-parameters are never written on failure** — and a
test asserts exactly that.

### Test framework decision

No third-party test framework. Catch2 and GoogleTest both need exceptions, which ADR-0001
disables core-wide — adopting either would mean testing a configuration we do not ship.
`FetchContent` would also make `cmake` require the network, a poor default for an offline-first
product, and §35 requires a real license/redistribution review plus a `THIRD_PARTY_NOTICES`
entry before any dependency lands. `tests/support/test_framework.h` is ~120 lines providing
named cases, a registry, fatal/non-fatal assertions, a name filter and a pass/fail count.
Recorded as a bootstrap decision to revisit (with an ADR) when the suite needs fixtures or
CI-consumable output.

The C ABI test carries its own ~40-line harness for a further reason: it is a **C** file, and
`tests/support/test_framework.h` is C++. Including it would defeat the point of the file, which
is that only the public C headers are reachable from it. The two harnesses print the same
format on purpose, so the output of the two suites reads identically.

### Known gaps — deliberately out of scope for this slice, recorded so they are not forgotten

- **NOT IMPLEMENTED: end-to-end encryption.** ADR-0002 decision 1 makes E2EE a **v1.0
  requirement**. This slice stores note titles and bodies as plaintext in an unencrypted SQLite
  file. SQLCipher integration and the key hierarchy in `security-model.md` must land before any
  build carries real user data.
- **NOT IMPLEMENTED: sync, CRDT, multi-device.** Single-device only. The Fugue CRDT is Phase 13
  and needs its own correctness-proof spike first (ADR-0002 amendment); no CRDT work was started.
- **Partially implemented: the C ABI (`libuapp_capi`).** *(Was: NOT IMPLEMENTED. Narrowed —
  `core/capi` now exists, is tested from C, and is the supported boundary for the note slice.)*
  What is **not** covered, precisely:
  - **Notes only.** `uapp_note_*` is the entire surface. There is no C ABI for documents,
    blocks, files, PDF, OCR, scanner, search-beyond-notes, sync or AI, because none of those
    engines exist. When the `document`+`block` model lands (database-design.md §3.3), its ABI
    is new surface, not a reshaping of this one.
  - **NOT IMPLEMENTED: the `uapp_error_t` cause chain** from architecture.md §7. The boundary
    carries a code plus a static, content-free message — no `domain`, no `detail`, no causes.
  - **NOT IMPLEMENTED: the job protocol** (`uapp_job_submit`/`cancel`, progress and done
    callbacks) and the `uapp_platform_vtable_t` from mobile-architecture.md §6. Nothing in this
    slice is long-running or needs a platform service, so neither would have been exercised.
  - **NOT IMPLEMENTED: FlatBuffers payloads** (`uapp_buf_t`) for structured data, per
    api-design.md §10 rule 4. Search results cross as an opaque handle with typed accessors,
    which is adequate for four scalar fields and would not be for a block tree.
  - **Deferred: shared-library packaging and the ABI freeze.** `libuapp_capi` is a static
    archive; the `.so`/`.framework` build with a closed export set (the `UAPP_CAPI_API`
    visibility annotations are already in place for it) is not done, and neither is the
    ABI-diff CI job over the exported symbol list required by api-design.md §10 rule 5. The
    version is **0.1** precisely because the surface is not frozen yet.
  - **Deferred: the bindings themselves.** No `uapp-kotlin`, no `UAPPKit`, no JNI or Swift
    code, no Gradle or Xcode project. This run built the boundary those will attach to and
    stopped there.
  - **Not proven: concurrent use from multiple threads.** Distinct handles share no mutable
    state and are designed to be independent, and the tests prove independence — but they
    exercise it sequentially on one thread. "Safe to use two handles from two threads at once"
    is a design property here, not a tested one.
- **Deferred: vendoring SQLite.** The build links the platform libsqlite3. Reproducible mobile
  builds need the vendored amalgamation (+ SQLCipher).
- **Deferred: external-content FTS5 and an ICU tokenizer.** `note_fts` stores its own copy of
  the text, and `unicode61` does not segment CJK — both are Phase 9 items already named in
  database-design.md §3.9.
- **Deferred: the real document/block schema.** `note` is a flat title+body table, not the
  `document`+`block` tree of database-design.md §3.3. The 16-byte BLOB id matches the real
  design so growing into it needs no id migration.
- **Deferred: allocation-failure handling in C++ containers.** *(Narrowed — the C ABI boundary
  now exists and honours ADR-0001's contract for its own allocations: handles and owned strings
  use `new (std::nothrow)`, check, and return `UAPP_STATUS_OUT_OF_MEMORY` having changed
  nothing.)* What remains: `std::string`/`std::vector` growth **inside** the C++ core and
  inside the handle structs still terminates on OOM under `-fno-exceptions`. So the boundary
  reports allocation failure it can see and aborts on allocation failure it cannot. Making that
  contract complete means a nothrow-aware container policy through `core/`, which is its own
  piece of work.
- **Deferred: fuzz targets.** §29 and architecture.md §5 require a `LLVMFuzzerTestOneInput`
  entry point per parser. `build_fts5_match_expression` is the natural first target and has
  unit tests but no fuzz harness.

---

## [0.0.0] — 2026-09-29 — Phase 0: architecture and feasibility study

**No production code.** This release is documentation only, per spec §40
(*"Do not write production code until this analysis is complete"*).

### Added

- `docs/product-requirements.md` — PRD: personas, prioritised feature inventory (P0–P3),
  user stories with acceptance criteria, measurable quality attributes, explicit non-goals,
  v1.0 release gates, and 7 open product questions requiring sign-off.
- `docs/architecture.md` — system architecture. Five whole-system alternatives evaluated
  (cross-platform UI framework, Rust core, thin client, microkernel/process isolation,
  the spec's own layering). The spec §4 diagram is **accepted with three revisions**: split the
  platform bridge into a C ABI plus idiomatic bindings; collapse the duplicated persistence
  layers into one port; add an explicit Policy & Audit layer and a diagnostics spine.
- `docs/technical-requirements.md` — module architecture, port contracts, a technology
  selection matrix with rejected alternatives and confidence levels, six known-hard
  sub-problems, and 8 time-boxed Phase 0 spikes (~32 engineer-days).
- `docs/database-design.md` — SQLite local schema across 13 namespaces with sync columns,
  UUIDv7 identifiers, fractional indexing, EAV for user-defined databases (with the rationale
  and a documented escape hatch), plus the PostgreSQL cloud schema and the local↔cloud
  mapping. Capacity assumptions stated so the design is falsifiable.
- `docs/api-design.md` — REST surface refining spec §31 (deviations marked and justified),
  path-major versioning with a 24-month support window, idempotency, job protocol, cursor
  pagination, error envelope, rate limits, auth model, and the separately-versioned core C ABI.
- `docs/security-model.md` — assets, four trust boundaries, a STRIDE threat model with ~35
  enumerated threats and mitigations, key hierarchy with chunked AEAD, authorisation model,
  a correct redaction procedure, and **seven explicitly accepted risks**.
- `docs/ai-architecture.md` — provider abstraction (primitives vs. task services), a
  non-bypassable policy gate with an unforgeable policy token, router, local inference with
  honest limits, an 8-stage RAG pipeline including **citation verification**, agent framework
  with mechanically-enforced §20 prohibitions, model licensing rules, and an eval harness.
- `docs/file-format-support.md` — the full support matrix over all spec §9 formats with the
  required columns, plus a **dependency and licence analysis**. Key findings in §7 below.
- `docs/conversion-architecture.md` — three intermediate representations (UDM, SSM, IMM),
  a three-tier conversion strategy (direct / IR / render), a declarative capability graph,
  the full conversion matrix with fidelity classes, the `Unsupported` node contract, and
  automated post-conversion verification.
- `docs/sync-architecture.md` — per-datatype merge strategies rather than one algorithm,
  version vectors for exact conflict detection, Lamport clocks (**wall clocks never used for
  ordering**), outbox pattern, push/pull protocol, and a mechanical statement of the
  "never silently overwrite" invariant.
- `docs/offline-architecture.md` — the offline capability matrix including what does *not*
  work, the three-state connectivity model, on-demand asset handling, storage eviction with
  unsynced data never evicted, checkpointing instead of background-time assumptions, and the
  required UI states.
- `docs/mobile-architecture.md` — Android (Kotlin + Compose + JNI) and iOS (Swift + ObjC++
  bridge + SwiftUI/UIKit) architectures, JNI lifetime and threading rules, copy-free bitmap
  paths on both platforms, and the platform-service vtable.
- `docs/testing-strategy.md` — test levels weighted by damage class, property-based testing,
  mandatory fuzzing per parser with 18 named targets, the full fixture inventory from spec §28,
  the CI pipeline, and the **format-matrix verification test** that makes the support matrix
  unable to drift from reality.
- `docs/performance-strategy.md` — reference device tiers and ~60 measurable budgets,
  techniques mapped to where they are load-bearing, thermal/power policy, the benchmark
  regression gate, and 8 known performance risks.
- `docs/deployment.md` — monorepo build pipeline for six artefact families, four independent
  version axes, release trains, staged rollout with automatic halt criteria, kill switches
  (the only rollback mobile has), and backend deploy/migration practice.
- `docs/contributing.md` — development workflow, the non-negotiable rules from spec §37/§38,
  code standards, the nine-item dependency checklist, and the Definition of Done.
- `docs/roadmap.md` — the 20 phases from spec §36 expanded with entry criteria, deliverables,
  and phase-specific "done when" lists tied to the spec §41 Definition of Done; plus sequencing
  constraints and the phases most likely to slip.
- `docs/adr/0001-core-language-c-vs-cpp.md` — ADR: **C++17 internals behind a C99 ABI**, with
  four alternatives evaluated and a disciplined C++ subset defined.
- `docs/risk-register.md` — 28 risks with probability, impact, owner role, mitigation, and
  trigger conditions.
- `docs/engineering-complexity-estimate.md` — phase-by-phase effort estimate with complexity
  drivers, confidence levels, and the historical basis for the estimate.
- `THIRD_PARTY_NOTICES` (repo root) — initialised from the licence findings.
- `docs/PHASE0_SUMMARY.md` — decisions made with high confidence, decisions needing human
  sign-off before Phase 1, and spec ambiguities resolved by judgment call.

### Key licence findings (§7 of `file-format-support.md`)

- **PDFium (BSD-3-Clause) + QPDF (Apache-2.0)** selected for PDF. Both are safe for closed
  commercial distribution and static linking.
- **MuPDF (AGPL-3.0), Poppler (GPL-2.0+), Xpdf (GPL)** — **BLOCKED** for closed distribution.
  MuPDF remains available under a paid Artifex commercial licence if the business chooses.
- **LGPL-3.0 is a hard block for the iOS client.** Its anti-tivoization/relinking requirement
  cannot be satisfied under App Store code signing. This blocks **libheif, libde265, libmobi**.
- **HEIC/HEIF** is additionally blocked by active HEVC patent-pool exposure. Decision: use
  platform decoders on mobile only; **HEIC is unsupported on desktop and web**. AVIF
  (royalty-free) is the strategic alternative.
- **RAR is permanently out of scope** — the `unRAR` licence forbids the use we would need.
- **Legacy binary DOC/XLS/PPT** have no viable permissively-licensed C/C++ parser (antiword,
  catdoc, wv are all GPL). Marked server-only via headless LibreOffice (MPL-2.0, separate
  process), and therefore **not available offline**.
- **FFmpeg deferred.** LGPL static-linking friction plus H.264/HEVC/AAC patent exposure.
  v1.0 ships no transcoding; playback uses the platform media stack.
- **PDFium's vendored AGG copy requires explicit verification** at the pinned commit — AGG 2.4
  and earlier are permissive, AGG 2.5 is GPL.
- **FreeType must be elected under the FTL**, not its GPLv2 alternative.
- **Redis ≥ 7.4 (RSALv2/SSPL)** avoided server-side in favour of Valkey (BSD-3).

### Notes

- No source code exists in `core/`, `file-engine/`, `pdf-engine/`, `scanner/`, `ocr/`,
  `media/`, `ai/`, `knowledge/`, `storage/`, `network/`, `security/`, `api/`, `mobile/`,
  `desktop/`, or `web/`. This is intentional and required by spec §40.
- Every capability in every matrix in these documents is a **plan**, not a status. The v1.0
  release gate requires automated proof of each claim before it may stand.
- Licence findings were compiled from upstream licence files and public documentation and
  **must be re-verified against the pinned upstream revision at integration time, and reviewed
  by counsel** before Phase 5 and Phase 7 begin.

[Unreleased]: #unreleased
[0.0.0]: #000--2026-09-29--phase-0-architecture-and-feasibility-study
