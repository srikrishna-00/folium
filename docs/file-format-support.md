# File Format Support Matrix & Dependency/Licence Analysis

**Status:** Phase 0 draft — awaiting human sign-off and **legal review**
**Version:** 0.0.0
**Date:** 2026-09-29

Implements spec §9 (support matrix), §11 (dependency checks) and §35 (licensing).

> **This document is a plan, not a status report.** Nothing here is implemented. Every row is
> a *target*, and the v1.0 release gate (`product-requirements.md` §10) requires an automated
> test to prove each claimed capability against a fixture before the claim may stand.
>
> **Per spec §9: we do not claim universal format support.** The matrix below is the complete
> intended scope. Anything absent is unsupported.

---

## 1. How to read the matrix

| Symbol | Meaning |
|---|---|
| ✔ | Planned for v1.0, native (in-app, offline-capable) |
| ◐ | Planned, **partial** — see the limitations column. Never present this as full support in UI. |
| ☁ | **Requires network** — server-side only. Not available offline. |
| ✖ | Not supported. Not planned for v1.0. |
| — | Not applicable to this format |
| P2 | Post-1.0 |

Platform columns mean *"available on that platform at v1.0"*. A format can be ✔ on Android and
✖ on Web when the capability depends on a native engine that has no WASM build.

**OFFLINE** means the capability works with the network fully unavailable.

---

## 2. Documents

| FORMAT | VIEW | EDIT | CREATE | CONVERT | OCR | ANNOTATION | OFFLINE | ANDROID | IOS | DESKTOP | WEB | LICENCE (engine) | KNOWN LIMITATIONS |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| TXT | ✔ | ✔ | ✔ | ✔ → MD/HTML/PDF/DOCX | — | ✖ | ✔ | ✔ | ✔ | ✔ | ✔ | own (ICU: Unicode) | Encoding detection is heuristic (BOM → UTF-8 validity → charset guess). Mis-detection possible on legacy 8-bit files. Files > 50 MB open in a streamed read-only viewer. |
| MD | ✔ | ✔ | ✔ | ✔ → HTML/PDF/DOCX/TXT | — | ✖ | ✔ | ✔ | ✔ | ✔ | ✔ | md4c (MIT) | CommonMark + GFM tables/strikethrough/tasklists. **No** Mermaid, no arbitrary embedded HTML rendering (sanitised to text), no footnote extensions beyond GFM. |
| RTF | ◐ | ✖ | ✖ | ◐ → UDM → PDF/HTML/MD/TXT | — | ✖ | ✔ | ✔ | ✔ | ✔ | ✖ | own parser | Read-only. Supports text, basic character/paragraph formatting, tables, embedded images. **Drops:** fields, objects (OLE), drawing objects, footnotes/endnotes. Old Word-generated RTF with deep nesting may fail → `FILE_PARSE_UNSUPPORTED_CONSTRUCT`. |
| DOC (binary, Word 97–2003) | ☁ | ✖ | ✖ | ☁ → PDF/DOCX | — | ✖ | **✖** | ☁ | ☁ | ☁ | ☁ | LibreOffice headless (MPL-2.0), server-side only | **No viable permissively-licensed C/C++ parser exists** (antiword/catdoc/wv are all GPL). Client-side: NOT SUPPORTED. Requires network. Gated on PQ-4. |
| DOCX | ◐ | P2 | ◐ | ◐ → UDM → PDF/HTML/MD/TXT; UDM → DOCX (P1) | — | ✖ | ✔ | ✔ | ✔ | ✔ | ◐ | own over libexpat (MIT) + libzip (BSD-3) | **Lossy by design.** Preserved: text, headings, styles, lists+numbering, tables (incl. merges), inline/floating images, hyperlinks, headers/footers, footnotes, section breaks, basic paragraph/character formatting. **Dropped or approximated:** SmartArt (→ placeholder), embedded charts (→ static image if a rendition exists, else placeholder), OLE objects, content controls, tracked changes (read as accepted), comments (extracted separately, not in-flow), VML/legacy drawings, text boxes with complex wrapping, WordArt, macros (never executed), equations (OMML → MathML, rendered approximately). Every conversion emits a fidelity report (C-11). |
| ODT | ◐ | ✖ | ✖ | ◐ → UDM → PDF/HTML/MD/TXT | — | ✖ | ✔ | ✔ | ✔ | ✔ | ✖ | own over libexpat + libzip | Read-only, P1. Similar loss profile to DOCX. ODF is a large spec; we implement the text subset. |
| HTML | ✔ | ◐ | ✔ | ✔ → UDM → PDF/MD/TXT/DOCX | — | ✖ | ✔ | ✔ | ✔ | ✔ | ✔ | Lexbor (Apache-2.0) | **Sanitised rendering only.** No script execution, no external resource fetch (T-106), no iframes, no web fonts from the network. CSS support is a deliberate subset (block/inline layout, tables, basic text styling) — this is not a browser engine. |
| XML | ✔ | ✔ | ✔ | ◐ | — | ✖ | ✔ | ✔ | ✔ | ✔ | ✔ | libexpat (MIT) | Viewed as structured/syntax-highlighted text. **DTDs, external entities and parameter entities are disabled unconditionally** (T-103). No XSLT. |
| JSON | ✔ | ✔ | ✔ | ◐ → CSV (flat arrays only) | — | ✖ | ✔ | ✔ | ✔ | ✔ | ✔ | own | Tree viewer + text editor. Files > 100 MB stream in read-only. JSON→CSV only for arrays of flat objects. |

## 3. Spreadsheets

| FORMAT | VIEW | EDIT | CREATE | CONVERT | OCR | ANNOTATION | OFFLINE | ANDROID | IOS | DESKTOP | WEB | LICENCE | KNOWN LIMITATIONS |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| CSV / TSV | ✔ | ✔ | ✔ | ✔ → XLSX/JSON/PDF | — | ✖ | ✔ | ✔ | ✔ | ✔ | ✔ | own | Delimiter and encoding auto-detected (heuristic; user-overridable). RFC 4180 quoting. Very large files open in a windowed viewer, not fully in memory. |
| XLS (binary, BIFF8) | ☁ | ✖ | ✖ | ☁ → XLSX/CSV/PDF | — | ✖ | **✖** | ☁ | ☁ | ☁ | ☁ | LibreOffice headless (MPL-2.0), server-side | Same reasoning as DOC. Gated on PQ-4. |
| XLSX | ◐ | ◐ | ◐ | ◐ → CSV/PDF/ODS | — | ✖ | ✔ | ✔ | ✔ | ✔ | ◐ | xlnt (MIT) + libexpat + libzip | **Formula values are read from the cached values in the file; formulas are NOT evaluated at v1.0.** Editing a cell that other formulas depend on leaves stale cached values — the UI must mark the sheet as "formulas not recalculated". Full evaluation is its own sub-project (`technical-requirements.md` §5.1). **Dropped:** charts, pivot tables, conditional formatting (read but not rendered), data validation, macros, external links, sparklines, slicers. Sheets > 500k cells stream read-only. |
| ODS | ◐ | ✖ | ✖ | ◐ → CSV/XLSX/PDF | — | ✖ | ✔ | ✔ | ✔ | ✔ | ✖ | own over libexpat + libzip | Read-only, P1. Same formula caveat. |

## 4. Presentations

| FORMAT | VIEW | EDIT | CREATE | CONVERT | OCR | ANNOTATION | OFFLINE | ANDROID | IOS | DESKTOP | WEB | LICENCE | KNOWN LIMITATIONS |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| PPT (binary) | ☁ | ✖ | ✖ | ☁ → PDF | — | ✖ | **✖** | ☁ | ☁ | ☁ | ☁ | LibreOffice headless (MPL-2.0) | Same as DOC/XLS. Gated on PQ-4. |
| PPTX | ◐ P2 | ✖ | ✖ | ◐ P2 → PDF/images | — | ✖ | ◐ | ◐ | ◐ | ◐ | ✖ | own over libexpat + libzip | **P2 / TBD.** Text and speaker-note extraction is straightforward; *layout* fidelity is the hardest of the three OOXML families (master/layout/slide inheritance, placeholders, transforms, theme colour/font resolution, effects). v1.0 ships **text + notes extraction for search only**, plus a thumbnail if the file contains a cached one. Full slide rendering is post-1.0. |
| ODP | ✖ | ✖ | ✖ | ✖ | — | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | — | **NOT IMPLEMENTED.** No plan for v1.0. |

## 5. PDF

| FORMAT | VIEW | EDIT | CREATE | CONVERT | OCR | ANNOTATION | OFFLINE | ANDROID | IOS | DESKTOP | WEB | LICENCE | KNOWN LIMITATIONS |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| PDF (1.0–2.0) | ✔ (partly real — §5.0) | ✔ | ✔ | ✔ → images/TXT/HTML; ◐ → DOCX | ✔ | ✔ | ✔ | ✔ desktop only today | ✔ desktop only today | ✔ macOS-arm64 today | ◐ | PDFium (BSD-3) + QPDF (Apache-2.0) | **This row is the v1.0 TARGET.** §5.0 says what exists today; limitations are in §5.1. |
| PDF/A (1b/2b/3b) | ✔ (as PDF) | ✔ | ◐ TBD | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ◐ | as above | **We will not claim PDF/A output conformance until validated by an independent checker in CI** (e.g. veraPDF). Reading a PDF/A file works — it is just a PDF. Producing a *conformant* one requires font embedding, colour-profile and metadata guarantees we have not yet built. Currently: **TBD / not claimed.** |
| PDF, encrypted | ✔ w/ password | ✔ | — | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ◐ | QPDF | Supports standard security handler R2–R6 (RC4-40/128, AESV2-128, AESV3-256). **Custom/proprietary security handlers and DRM (e.g. Adobe LiveCycle rights management) are NOT supported** and never will be. |
| PDF with XFA forms | ◐ | ✖ | — | ◐ | ◐ | ◐ | ✔ | ◐ | ◐ | ◐ | ✖ | PDFium (non-XFA build) | **NOT IMPLEMENTED.** XFA is rendered only via the document's fallback static page content, which for XFA-only documents is usually a "please open in Acrobat" notice. We display that honestly rather than pretending. |

### 5.0 PDF: what is actually implemented today (2026-09-30)

The table above is the v1.0 target. This subsection is the current state, and the two must
not be confused — spec §5 forbids claiming a feature is complete unless it is implemented
and tested.

**Implemented and tested** (`pdf-engine/reader`, 22 unit tests, green under ASan+UBSan on
macOS-arm64):

| Capability | Status |
|---|---|
| Open a PDF **from an in-memory buffer** (no temp file, no path API) | ✔ real |
| Page count | ✔ real |
| Per-page size in points, from the page's own MediaBox | ✔ real |
| Render a page into a **caller-owned RGBA8888 buffer**, arbitrary pixel size, padded strides supported, zero copies (SP-5 F-2) | ✔ real |
| Defined, non-crashing `uapp::Status` for empty / truncated / corrupt / non-PDF input | ✔ real |
| Password parameter plumbed through | ◐ accepted and validated; **no encrypted-PDF test fixture exists yet**, so "opens an encrypted PDF" is unverified |

**NOT IMPLEMENTED** — absent, not stubbed: page navigation state, search, zoom, thumbnails,
merge, split, extract/reorder/delete/rotate/crop pages, compress, metadata editing,
watermarks, password protection and encryption (writing), annotation of every kind,
forms and form filling, redaction, digital signatures, PDF creation, image-to-PDF,
OCR-to-PDF, and PDF-to-image as a file-producing operation. QPDF is not vendored or linked.
Annotations are not even *rendered* (`FPDF_ANNOT` is deliberately unset).

**The dependency is a bootstrap, and this is the most important line in this subsection.**
PDFium is a **prebuilt binary** (bblanchon/pdfium-binaries `chromium/8076`, pinned by
SHA-256 — `third_party/pdfium/`), for **macOS-arm64 only**, linked **dynamically**.
`spikes/SP-1-findings.md` §5 explicitly rejects the prebuilt-vendor route for production in
favour of a hermetic from-source build in our own CI. That build does not exist. Therefore:

- **Android and iOS PDF support is NOT IMPLEMENTED** — no mobile artefact is vendored, and
  SP-1's real open question (revised criterion 2: does `ios_deployment_target = "16.0"`
  build and render?) is untouched.
- The §7.8 licence inventory is still owed, and the AGG residual risk stays open
  (`THIRD_PARTY_NOTICES` section 0).
- **There is no PDF fuzz target yet**, which §29 requires and `testing-strategy.md` §6 makes
  a merge gate for parsers. The malformed-input cases in the unit suite are a floor, not a
  substitute.

### 5.1 PDF known limitations (detail)

- **Text extraction quality varies with the producing tool.** PDFs without a `ToUnicode` CMap,
  or with subsetted symbolic fonts, extract as garbage. We detect low-confidence extraction and
  offer OCR as an alternative rather than returning nonsense silently.
- **PDF → DOCX is a reconstruction, not a conversion.** A PDF has no paragraphs, lists, or
  tables — only positioned glyphs. We infer structure heuristically. Marked ◐ permanently.
- **Very large pages / complex vector art** (maps, CAD exports) can exceed the render time
  budget. Rendering is tiled and cancellable; a page that exceeds the budget shows progressive
  tiles rather than blocking.
- **JBIG2 and JPXDecode (JPEG 2000)** images: supported via PDFium's bundled decoders. JBIG2
  has a notable CVE history — fuzzed specifically.
- **Linearisation:** QPDF can produce linearised output; we do so for files intended for
  sharing.
- **Incremental update is disabled for redaction output** (see `security-model.md` §9),
  otherwise the removed content would remain in the file.
- **Digital signature validation is P2/TBD.** We will not display a "signature valid" badge
  until validation (chain, revocation, byte-range coverage) is implemented and tested. A
  half-implemented signature check is worse than none.

## 6. Images

| FORMAT | VIEW | EDIT | CREATE | CONVERT | OCR | ANNOTATION | OFFLINE | ANDROID | IOS | DESKTOP | WEB | LICENCE | KNOWN LIMITATIONS |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| JPEG | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | libjpeg-turbo (IJG/BSD-3/zlib) | Lossless rotation only for multiples of 90° on MCU-aligned images; otherwise re-encode (quality loss, disclosed). Progressive JPEG supported. CMYK JPEGs converted to sRGB approximately. |
| PNG | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | libpng (libpng licence) + zlib | APNG animation: **view only**, no editing. Decompression-bomb limits enforced (T-102). 16-bit channels preserved on read, may be reduced to 8-bit on some edit paths (disclosed). |
| WEBP | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | libwebp (BSD-3) | Animated WebP: view only. |
| GIF | ✔ | ◐ | ✖ | ✔ → PNG/WEBP (first frame or animated) | ◐ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | giflib (MIT) | Animation view only. Editing operates on a single frame. 256-colour palette limits round-trip quality. |
| TIFF | ✔ | ◐ | ✔ | ✔ → PDF/PNG/JPEG | ✔ | ◐ | ✔ | ✔ | ✔ | ✔ | ✖ | libtiff (libtiff/BSD-like) | Multi-page supported (important for scanning). Supported compressions: none, LZW, Deflate, PackBits, CCITT G3/G4, JPEG. **Not supported:** JPEG2000-in-TIFF, Old-JPEG (compression 6), exotic photometric interpretations, most proprietary camera-raw TIFF variants. libtiff has a long CVE history → primary fuzz target. |
| BMP | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | own | Uncompressed and RLE4/RLE8. OS/2 variants: best-effort. |
| SVG | ◐ | ✖ | ✖ | ◐ → PNG/PDF | ✖ | ✖ | ✔ | ◐ | ◐ | ◐ | ✔ (native) | NanoSVG (zlib) **or** resvg (MPL-2.0) — **undecided** | **Static subset only.** No scripts, no animation (SMIL/CSS), no external references or web fonts (T-106), no filters/masks beyond basic clip paths with NanoSVG. Engine choice pending (see §7.5). Web uses the browser's own renderer. |
| HEIC / HEIF | ◐ | ✖ | ✖ | ◐ → JPEG/PNG (device only) | ✔ (after conversion) | ✖ | ✔ (mobile) | ✔ (platform) | ✔ (platform) | **✖** | ✖ | **platform decoders only** | **Decode via OS APIs only** (iOS ImageIO, Android `ImageDecoder`). We do **not** bundle libheif: it is LGPL-3.0 and its HEVC decoders are LGPL-3.0/GPL-2.0, and HEVC carries active patent-pool exposure (§7.4). **Desktop and Web: NOT SUPPORTED.** No HEIC encoding on any platform. Multi-image HEIF collections: primary image only. |
| AVIF | ◐ P2 | ✖ | ✖ | ◐ → PNG/JPEG | ✔ | ✖ | ✔ | ◐ | ◐ | ◐ | ✔ (native) | libavif (BSD-2) + dav1d (BSD-2) | Decode only. No encode at v1.0. AV1 is royalty-free (AOMedia), unlike HEVC — this is why AVIF is viable where HEIC is not. |
| RAW (CR2/NEF/ARW/DNG…) | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | — | **NOT IMPLEMENTED.** LibRaw is LGPL-2.1/CDDL dual; dcraw is effectively unmaintained. Dozens of vendor variants. Out of scope. |

## 7. E-books

| FORMAT | VIEW | EDIT | CREATE | CONVERT | OCR | ANNOTATION | OFFLINE | ANDROID | IOS | DESKTOP | WEB | LICENCE | KNOWN LIMITATIONS |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| EPUB 2/3 | ◐ | ✖ | ◐ P2 | ◐ → UDM → PDF/HTML/MD/TXT | ✖ | ◐ P2 | ✔ | ✔ | ✔ | ✔ | ◐ | own (libzip + Lexbor) | Reflowable content only. **Fixed-layout EPUB: view is approximate.** No embedded JavaScript, no media overlays, no external resources. **DRM-protected EPUB (Adobe ADEPT, LCP): NOT SUPPORTED and never will be.** Embedded fonts honoured only if licensing metadata permits. |
| MOBI / PRC | ◐ P2 | ✖ | ✖ | ◐ P2 → TXT/HTML/EPUB | ✖ | ✖ | ✔ | ◐ | ◐ | ◐ | ✖ | own minimal reader | **P2.** PalmDOC + HUFF/CDIC decompression. `libmobi` is LGPL-3.0 and therefore unusable for iOS (§7.4), so this needs our own reader. **AZW3/KF8 with DRM: NOT SUPPORTED.** |
| FB2 | ◐ P2 | ✖ | ✖ | ◐ P2 → UDM | ✖ | ✖ | ✔ | ◐ | ◐ | ◐ | ✖ | own (libexpat) | **P2.** FB2 is plain XML; low effort. `.fb2.zip` supported. |
| AZW / KFX | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | — | **NOT IMPLEMENTED.** DRM'd proprietary format. Legal non-starter. |
| DJVU | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ | — | **NOT IMPLEMENTED.** DjVuLibre is GPL-2.0. Out of scope. |

## 8. Archives

| FORMAT | VIEW (browse) | EDIT | CREATE | CONVERT | OCR | ANNOTATION | OFFLINE | ANDROID | IOS | DESKTOP | WEB | LICENCE | KNOWN LIMITATIONS |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ZIP | ✔ | ◐ | ✔ | — | — | — | ✔ | ✔ | ✔ | ✔ | ◐ | libzip (BSD-3) / minizip-ng (zlib) | Browse without extracting. Zip64 supported. **AES-encrypted ZIP (WinZip AE-x): read only.** Legacy ZipCrypto: read only, flagged as insecure. Zip-slip defended (T-104). Expansion-ratio cap (T-102). |
| TAR | ✔ | ✖ | ✔ | — | — | — | ✔ | ✔ | ✔ | ✔ | ◐ | libarchive (BSD-2) | GNU and POSIX/pax variants. Symlinks and device nodes are **not** materialised on extract. |
| GZ / BZ2 / XZ / ZSTD | ✔ | — | ✔ (gz, zstd) | — | — | — | ✔ | ✔ | ✔ | ✔ | ◐ | zlib / bzip2 (BSD-like) / xz (0BSD) / zstd (BSD-3) | Single-stream. `.tar.gz` handled as a composite. |
| 7Z | ◐ | ✖ | ✖ | — | — | — | ✔ | ◐ | ◐ | ◐ | ✖ | LZMA SDK (public domain) | **Read/extract only, P1.** LZMA/LZMA2/BCJ filters. **No creation.** Encrypted 7z: read with password. |
| RAR | ✖ | ✖ | ✖ | — | — | — | ✖ | ✖ | ✖ | ✖ | ✖ | — | **NOT IMPLEMENTED — LICENCE BLOCKED.** The `unRAR` source licence forbids using it to create a RAR-compatible archiver and is not OSI-approved. No permissive alternative exists. This will not change. |

## 9. Audio & Video

v1.0 scope is deliberately minimal, pending PQ-5.

| FORMAT | VIEW/PLAY | EDIT | CREATE | CONVERT | OCR | ANNOTATION | OFFLINE | ANDROID | IOS | DESKTOP | WEB | LICENCE | KNOWN LIMITATIONS |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| MP3, AAC, WAV, FLAC, OGG/Opus | ✔ (platform player) | ✖ | ✖ | ✖ TBD | — | ✖ | ✔ | ✔ | ✔ | ✔ | ✔ | platform codecs | **Playback via the OS media stack**, not a bundled decoder. Metadata (ID3/Vorbis comments) read by our own parser. **Transcode: TBD (PQ-5)** — blocked on the FFmpeg decision (§7.6). |
| MP4, MOV, WebM | ✔ (platform player) | ✖ | ✖ | ✖ TBD | — | ✖ | ✔ | ✔ | ✔ | ✔ | ✔ | platform codecs | Playback + thumbnail via the OS. Container metadata parsed by us. |
| MKV, AVI | ◐ | ✖ | ✖ | ✖ | — | ✖ | ◐ | ◐ | ✖ | ◐ | ✖ | platform | **iOS has no native MKV/AVI support.** Marked ✖ on iOS rather than pretending. Android/desktop depend on the OS codec set, which varies by device — capability is probed at runtime, not assumed. |

**Transcoding is the decision point.** Doing it ourselves means FFmpeg (LGPL-2.1+, with GPL
components if x264/x265 are enabled) plus H.264/HEVC/AAC patent exposure. See §7.6.

## 10. Code & plain-text formats

| FORMAT | VIEW | EDIT | CREATE | CONVERT | OCR | ANNOTATION | OFFLINE | ANDROID | IOS | DESKTOP | WEB | LICENCE | KNOWN LIMITATIONS |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| C, C++, Python, Java, JS, TS, Rust, Go, CSS, SQL, Shell, YAML, TOML, INI | ✔ | ✔ | ✔ | ✔ → HTML/PDF (highlighted) | — | ✖ | ✔ | ✔ | ✔ | ✔ | ✔ | own lexers (or Tree-sitter, MIT — P2) | Syntax highlighting only. **No language intelligence** (no completion, no diagnostics, no formatting). Very large files (> 10 MB) fall back to plain text without highlighting. Tabs/indentation preserved byte-exactly. |

---

# 7. Dependency & Licence Analysis

**This section is the one that can sink the project if it is wrong.** It was compiled from
upstream licence files and public documentation; sources are cited. **It is not legal advice
and must be reviewed by counsel before Phase 5 (PDF) and Phase 7 (OCR) begin.**

Distribution model assumed throughout: **closed-source commercial app, statically linked
native core, distributed through the Apple App Store and Google Play.**

## 7.1 Verdict table

| Library | Purpose | Licence | Static link into closed app? | iOS App Store? | Verdict |
|---|---|---|---|---|---|
| **PDFium** | PDF parse + render | BSD-3-Clause | Yes | Yes | ✅ **ADOPT** |
| **QPDF** ≥ 11 | PDF structure, encryption | Apache-2.0 | Yes | Yes | ✅ **ADOPT** |
| MuPDF | PDF parse + render | **AGPL-3.0** or commercial | **No** | **No** | ❌ **BLOCKED** (unless a commercial licence is purchased from Artifex) |
| Poppler | PDF render | **GPL-2.0-or-later** | **No** | **No** | ❌ **BLOCKED** |
| Xpdf | PDF render | **GPL** (or commercial) | **No** | **No** | ❌ **BLOCKED** |
| PoDoFo | PDF manipulation | **LGPL-2.0+** (lib) | Conditional | Risky | ⚠️ **AVOID** — no renderer anyway; QPDF covers the need with a clean licence |
| **Tesseract 5** | OCR | Apache-2.0 | Yes | Yes | ✅ **ADOPT** |
| **Leptonica** | Image preprocessing for OCR | BSD-2-Clause | Yes | Yes | ✅ **ADOPT** |
| Tesseract `tessdata_fast` / `_best` | OCR language data | Apache-2.0 | Yes | Yes | ✅ **ADOPT** |
| **OpenCV** (core/imgproc/photo) | Scanner CV | Apache-2.0 (since 4.5.0) | Yes | Yes | ✅ **ADOPT** — **must not link `opencv_contrib` nonfree** |
| **SQLite** | Local DB | Public domain | Yes | Yes | ✅ **ADOPT** |
| **SQLCipher** (community) | DB encryption | BSD-3-Clause (Zetetic) | Yes | Yes | ✅ **ADOPT** — attribution required |
| **libsodium** | Crypto | ISC | Yes | Yes | ✅ **ADOPT** |
| **libjpeg-turbo** | JPEG | IJG / BSD-3 / zlib (triple) | Yes | Yes | ✅ **ADOPT** |
| **libpng** | PNG | libpng licence (permissive) | Yes | Yes | ✅ **ADOPT** |
| **zlib** | Deflate | zlib | Yes | Yes | ✅ **ADOPT** |
| **libwebp** | WebP | BSD-3 | Yes | Yes | ✅ **ADOPT** |
| **libtiff** | TIFF | libtiff (BSD-like) | Yes | Yes | ✅ **ADOPT** |
| **giflib** | GIF | MIT | Yes | Yes | ✅ **ADOPT** |
| **libavif + dav1d** | AVIF | BSD-2 / BSD-2 | Yes | Yes | ✅ **ADOPT** (P2) |
| **libheif** | HEIF | **LGPL-3.0** | **No** | **No** | ❌ **BLOCKED** — see §7.4 |
| **libde265** | HEVC decode | **LGPL-3.0** | **No** | **No** | ❌ **BLOCKED** |
| **x265** | HEVC encode | **GPL-2.0** or commercial | **No** | **No** | ❌ **BLOCKED** |
| **FreeType** | Font rasterisation | **FTL** (BSD-style) *or* GPLv2 — dual | Yes, **under FTL** | Yes | ✅ **ADOPT under FTL only.** Must document the election and reproduce the FTL credit. |
| **HarfBuzz** | Text shaping | MIT (Old MIT) | Yes | Yes | ✅ **ADOPT** |
| **ICU** | Unicode, collation, segmentation | Unicode-3.0 (permissive) | Yes | Yes | ✅ **ADOPT** — use a trimmed data set (~10 MB otherwise) |
| **libexpat** | XML pull parsing | MIT | Yes | Yes | ✅ **ADOPT** |
| **pugixml** | XML DOM | MIT | Yes | Yes | ✅ **ADOPT** |
| **Lexbor** | HTML5 parsing | Apache-2.0 | Yes | Yes | ✅ **ADOPT** |
| **md4c** | Markdown | MIT | Yes | Yes | ✅ **ADOPT** |
| **xlnt** | XLSX read/write | MIT | Yes | Yes | ✅ **ADOPT** |
| **libzip** | ZIP | BSD-3 | Yes | Yes | ✅ **ADOPT** |
| **minizip-ng** | ZIP (alt) | zlib | Yes | Yes | ✅ ADOPT (either) |
| **libarchive** | TAR and friends | BSD-2 | Yes | Yes | ✅ **ADOPT** |
| **LZMA SDK** | 7z | Public domain | Yes | Yes | ✅ **ADOPT** |
| **zstd** | Compression | BSD-3 (+ GPLv2 dual) | Yes, under BSD-3 | Yes | ✅ **ADOPT** |
| **unRAR** | RAR | Proprietary, restrictive | **No** | **No** | ❌ **BLOCKED** — see §7.7 |
| **llama.cpp / ggml** | Local LLM | MIT | Yes | Yes | ✅ **ADOPT** (weights licensed separately) |
| **ONNX Runtime** | On-device inference | MIT | Yes | Yes | ✅ **ADOPT** |
| **sqlite-vec** | Vector index | Apache-2.0 / MIT | Yes | Yes | ✅ **ADOPT** |
| **ZXing-C++** | Barcode/QR | Apache-2.0 | Yes | Yes | ✅ ADOPT |
| ZBar | Barcode (alt) | **LGPL-2.1** | Conditional | Risky | ⚠️ AVOID — ZXing-C++ is cleaner |
| **GoogleTest / Benchmark** | Test/bench | BSD-3 / Apache-2.0 | Test only | n/a | ✅ ADOPT |
| **FlatBuffers** | Serialisation | Apache-2.0 | Yes | Yes | ✅ ADOPT |
| **NanoSVG** | SVG (minimal) | zlib | Yes | Yes | ✅ ADOPT (candidate) |
| **resvg** | SVG (full) | MPL-2.0 (Rust) | Yes (file-level copyleft) | Yes | ⚠️ ACCEPTABLE — must publish modifications to MPL-covered files; adds a Rust toolchain |
| librsvg | SVG (alt) | **LGPL-2.1** (Rust) | Conditional | Risky | ⚠️ AVOID |
| **LibreOffice** | Legacy Office conversion | MPL-2.0 (+ LGPLv3 parts) | **Server-side separate process only** | n/a | ⚠️ **CONDITIONAL** — see §7.3 |
| Apache POI | Office formats | Apache-2.0 | JVM only | n/a | ⚠️ Not usable in a C++ core; possible Android-only or server-side |
| antiword / catdoc / wv | Legacy .doc | **GPL** | **No** | **No** | ❌ **BLOCKED** |
| unrtf | RTF | **GPL** | **No** | **No** | ❌ **BLOCKED** |
| libmobi | MOBI | **LGPL-3.0** | **No** | **No** | ❌ **BLOCKED** — write our own |
| DjVuLibre | DjVu | **GPL-2.0** | **No** | **No** | ❌ BLOCKED (format out of scope anyway) |
| Xapian | Search | **GPL-2.0+** | **No** | **No** | ❌ BLOCKED (FTS5 covers it) |
| **FFmpeg** | A/V transcode | LGPL-2.1+ (GPL if `--enable-gpl`) | ⚠️ conditional | ⚠️ conditional | ⚠️ **DEFERRED** — see §7.6 |
| Qt | Desktop UI | GPL/LGPLv3 or commercial | ⚠️ | n/a | ⚠️ Commercial licence required for closed static linking; Compose Multiplatform preferred |
| Redis ≥ 7.4 | Server cache | RSALv2 / SSPL | n/a (server) | n/a | ⚠️ **Use Valkey (BSD-3) or Redis ≤ 7.2.4** |

## 7.2 The GPL-family problem, stated precisely

This is the single most consequential licensing fact for this project, so it is spelled out
rather than summarised.

**GPL-2.0 / GPL-3.0 / AGPL-3.0 (MuPDF, Poppler, Xpdf, antiword, unrtf, DjVuLibre, Xapian):**
linking these into our application — statically **or** dynamically — makes the combined work a
derivative under the FSF's interpretation, requiring the entire application be released under
the same terms. That is incompatible with closed commercial distribution. AGPL additionally
triggers on *network* use, so it is not even usable server-side without publishing our source.
**These are hard blocks, not risks to manage.**

MuPDF and Xpdf offer commercial licences. Purchasing an Artifex MuPDF licence is a legitimate
option — it is a real cost line (typically negotiated per-product/per-platform), and if the
business wants MuPDF's rendering, that is a budget decision. Our recommendation is PDFium
precisely because it removes this negotiation from the critical path.

**LGPL-2.1 (PoDoFo, ZBar, librsvg, LibRaw):** permits use in a closed application *if* the user
can replace the LGPL component. Dynamic linking satisfies this straightforwardly. **Static
linking** requires distributing our object files (or a relinkable `.o`) so a user can relink
against a modified library. That is technically possible on iOS but operationally unpleasant,
and it obliges us to publish enough of our build to relink — which many organisations consider
unacceptable. We treat LGPL-2.1 as "avoid unless there is no alternative".

**LGPL-3.0 (libheif, libde265, libmobi): effectively unusable on iOS.** LGPL-3.0 inherits
GPLv3's anti-tivoization terms (the "Installation Information" requirement). Apple's code
signing means a user cannot install a relinked binary on a non-jailbroken device, so the
relinking right cannot be exercised. This is a well-documented conflict and is why the FSF's
own position is that GPLv3-family code cannot be distributed through the App Store.
**We treat LGPL-3.0 as a hard block for the iOS client.**

**MPL-2.0 (resvg, LibreOffice):** file-level copyleft. We may link it into a closed application
provided we publish modifications to the MPL-covered *files*. This is compatible with our
distribution model. It is the one weak-copyleft licence we are comfortable with.

## 7.3 LibreOffice: acceptable server-side, unacceptable client-side

- Client-side: LibreOffice is ~400 MB, is not designed for embedding on mobile, and
  LibreOfficeKit's mobile story is not production-viable for our use case. **Rejected.**
- Server-side: running `soffice --headless --convert-to` as a **separate process** invoked over
  a command line is mere aggregation, not linking. MPL-2.0 imposes no obligation on our
  separate program. We must still reproduce its notices for the distributed server image if
  that image is distributed; for a hosted service it is not distributed at all.
- This is why legacy DOC/XLS/PPT is marked ☁ **server-only** in the matrix, and why PQ-4
  matters: saying yes to legacy Office formats means saying yes to a network dependency.

## 7.4 HEIC/HEIF: licence **and** patent

Two independent blockers, either of which is sufficient:

1. **Licence.** libheif is LGPL-3.0; its decoders libde265 (LGPL-3.0) and encoder x265
   (GPL-2.0) are worse. Unusable for the iOS client per §7.2.
2. **Patents.** HEVC is covered by multiple active patent pools (and unpooled holders).
   Distributing an HEVC decoder can require a patent licence depending on jurisdiction and
   volume — independently of the software licence. Several major open-source projects ship
   HEVC support while explicitly accepting this risk; **we choose not to.**

**Decision:** use the platform's own HEIF decoder on iOS and Android, where the OS vendor has
already paid for the patent licence and the decoder is part of the OS. Accept that desktop and
web have no HEIC support, and say so in the matrix. AVIF (AV1, royalty-free via AOMedia) is the
strategically correct format to support instead.

## 7.5 SVG: undecided

- **NanoSVG (zlib):** tiny, zero new toolchain, handles basic SVG. Fails on filters, masks,
  advanced gradients, and text-as-text.
- **resvg (MPL-2.0):** near-complete static SVG support, but Rust.

**Recommendation:** start with NanoSVG; if the fixture corpus shows unacceptable failure rates,
adopt resvg *at the same time* as any other Rust decision (`yrs`, tokenizers), so the toolchain
cost is paid once. Do not add Rust for SVG alone.

## 7.6 FFmpeg: deferred, and why the deferral is the right call

FFmpeg's default build is LGPL-2.1+. Statically linking LGPL-2.1 into an iOS app requires the
relinking accommodation (§7.2). Dynamic linking on iOS is possible via an embedded framework,
which is the usual route. **But:**

- Enabling x264 or x265 flips the whole build to **GPL** — a hard block.
- Even LGPL FFmpeg decoding H.264/HEVC/AAC carries **codec patent** exposure separate from the
  software licence.
- Bundling FFmpeg adds 10–30 MB and a large, CVE-active attack surface.

**Recommendation:** ship v1.0 with **no transcoding**. Use the platform media stack for playback
and thumbnails, where patent licensing is the OS vendor's problem. If PQ-5 resolves to "we need
transcode", do it **server-side** in a container (where dynamic linking and notice obligations
are trivially satisfiable and patent exposure is a single, assessable enterprise question)
rather than on-device.

## 7.7 unRAR: permanently blocked

The `unRAR` licence states the source may not be used to develop a RAR-compatible archiver.
It is not OSI-approved and is incompatible with both GPL-family and permissive expectations.
There is no permissively-licensed RAR implementation of usable quality. **RAR is out of scope,
permanently.** Do not revisit.

## 7.8 PDFium's bundled third-party code

PDFium itself is BSD-3-Clause, but it vendors third-party code whose notices we must
reproduce. At integration time (SP-1) the build must be audited and each of the following
confirmed and recorded in `THIRD_PARTY_NOTICES`:

| Component | Licence (expected) | Note |
|---|---|---|
| FreeType | FTL / GPLv2 dual | **Elect FTL.** Verify the vendored copy's `docs/FTL.TXT`. |
| libjpeg-turbo | IJG / BSD-3 / zlib | |
| libopenjpeg (OpenJPEG) | BSD-2 | JPEG 2000 in PDF |
| lcms2 (Little CMS) | MIT | Colour management |
| libpng, zlib | libpng / zlib | |
| AGG 2.3 (`third_party/agg23`) | Anti-Grain Geometry — the **2.3/2.4-era permissive** terms | ⚠️ **Verify explicitly.** AGG **2.5 is GPL**; only the 2.4-and-earlier line is permissive (modified BSD / the AGG Public Licence). PDFium vendors the older permissive line, but this must be confirmed against the actual vendored `LICENSE` at the pinned commit, not assumed. |
| ICU (if enabled) | Unicode | Disable if not needed |
| V8 | BSD-3 | **Build with `pdf_enable_v8 = false`** — we do not want PDF JavaScript (T-105) and it removes a large dependency |
| Skia | BSD-3 | **Build with the AGG rasteriser** unless SP-1 shows Skia is needed; it is a large dependency |
| XFA support | mixed | **Build with `pdf_enable_xfa = false`** |

**Action item (blocking Phase 5):** SP-1 must produce a machine-generated inventory of every
licence in the pinned PDFium tree, checked into the repo, and that inventory becomes the basis
of the PDF section of `THIRD_PARTY_NOTICES`.

**Status 2026-09-30 — partially, and provisionally, evidenced.** `pdf-engine/reader` now
links a *prebuilt* PDFium (`third_party/pdfium/`, bootstrap — see §5.0). That archive ships
its own `licenses/` directory, which has been preserved at `third_party/pdfium/licenses/`
and covers every component in the table above except Skia, V8 and XFA — all three absent,
as the recorded `args.gn` confirms (`pdf_enable_v8=false`, `pdf_enable_xfa=false`,
`pdf_use_skia` unset). `agg23.txt` in that set declares **AGG 2.3 with the permissive
grant**, which is consistent with what this table expected.

That is evidence, **not the action item.** It is the distributor's account of a tree we did
not build, so it is not a `gn desc`-derived inventory of the code we ship, and it does not
answer whether AGG 2.3's terse grant is acceptable for static linking into a closed-source
App Store binary — a counsel question, not an SPDX lookup. **AGG stays open**, and the
action item above stays blocking. See `THIRD_PARTY_NOTICES` section 0.

**SP-1 outcome (`spikes/SP-1-findings.md`):** PDFium is built **from pinned source with
upstream's own GN/depot_tools toolchain, in a hermetic container, in our CI**, and consumed
from our CMake as an `IMPORTED` target — *not* via a hand-written CMake port and *not* from a
third-party binary vendor. This matters for this section specifically: the abandoned CMake
port substitutes system packages for PDFium's vendored tree, which would make the inventory
above describe a build we do not ship. **GN gives a more accurate licence inventory than a
CMake port would**, because `DEPS` at a pinned commit plus `gn desc` describes exactly the
code compiled. Confirmed build args: `pdf_enable_v8=false`, `pdf_enable_xfa=false`,
`pdf_use_partition_alloc=false`, `is_component_build=false`, `pdf_is_complete_lib=true`,
`use_custom_libcxx=false`; `pdf_use_skia` left unset, i.e. **AGG is the shipped rasteriser** —
so the AGG 2.3 licence row above is load-bearing and must be verified, not skipped.

## 7.9 Attribution obligations

Even permissive licences impose obligations. The following require reproducing the copyright
notice and licence text in the distributed app (an in-app "Open Source Licences" screen plus
the `THIRD_PARTY_NOTICES` file):

BSD-2 / BSD-3 (PDFium, Leptonica, SQLCipher, libwebp, zstd, libarchive, dav1d, libavif, …),
Apache-2.0 (QPDF, Tesseract, OpenCV, Lexbor, sqlite-vec, ZXing-C++ — **plus the NOTICE file, if
present, must be reproduced**), MIT (llama.cpp, ONNX Runtime, md4c, xlnt, expat, pugixml,
HarfBuzz, giflib), ISC (libsodium), FTL (FreeType — requires a specific credit line),
libpng/zlib/libtiff (permissive, notice required), MPL-2.0 (resvg — plus publication of
modified files), Unicode-3.0 (ICU).

**Apache-2.0 also carries an express patent grant with a termination clause** (initiating
patent litigation over the work terminates your grant). Harmless for us; worth knowing.

## 7.10 Process (spec §35)

No dependency is added without all of:

1. A licence identified by SPDX id, with the actual licence text vendored at
   `third_party/<name>/LICENSE`.
2. A verdict in the table above.
3. A pinned commit hash **and** SHA-256, verified in CI.
4. A row in `THIRD_PARTY_NOTICES`, **generated by CI from the link graph**, not hand-written.
5. Confirmation of static-linking and iOS-distribution compatibility.
6. A named owner responsible for tracking its CVEs.
7. For anything weak-copyleft or dual-licensed: an explicit note of which licence we elect and
   why.

CI fails the build if a `third_party/` directory lacks a `LICENSE` or a
`THIRD_PARTY_NOTICES` entry (NFR-8).

## 11. Low-end device constraints on format support (`adr/0002` decision 3)

`adr/0002` decision 3 makes **Tier D** (2–3 GB RAM, entry SoC, eMMC — see
`performance-strategy.md` §1.1) a hard constraint. Format *support* does not change by tier —
a format we claim is supported is supported everywhere we ship — but **what the engine does
inside that support does change**, and per J-9 the limitations column is where that belongs.

**Principle: the matrix stays tier-independent; the limitations are tier-annotated.** We do
not publish a second matrix. A capability that cannot be delivered on Tier D at all is either
removed from the matrix entirely or marked ◐ with the constraint named — never left as ✔ with
a footnote nobody reads.

### 11.1 Engine-level constraints on Tier D

| Engine | Constraint |
|---|---|
| **PDF (PDFium + QPDF)** | Render scale capped at 1×; 2-page decoded bitmap LRU; ≤ 16 MB tile cache; `RGB_565` for thumbnails. Documents > 200 pages / > 50 MB open read-only-fast, with whole-document-model editing disabled and a reason shown. |
| **OCR (Tesseract + platform)** | **Ship `tessdata_fast` (≈ 4–5 MB full set; ~2 MB for `eng`), never `tessdata_best` (≈ 15–23 MB).** Serial page processing only; 200 DPI rather than 300 DPI; relaxed per-page budget (`performance-strategy.md` §1.2). ML Kit **unbundled** (≈ 260 KB/script) rather than bundled (≈ 4 MB/script) where Play Services is present — which makes S-15/MQ-4 a device-footprint question, not only a market-coverage one. |
| **Conversion** | 96 MB arena rather than 256 MB. **A conversion that does not fit fails with a specific, actionable error — never an OOM kill.** This is the difference between "this file is too large for this device" and the app disappearing. |
| **Scanner (OpenCV)** | Capture resolution capped (~8 MP) so the perspective-correction working set fits the arena. |
| **Images** | Decompression-bomb caps (T-102) unchanged in *ratio* but with a lower absolute output cap on Tier D. Large TIFF/PNG decode is streamed page-by-page, never whole-image where the format allows otherwise. |
| **Archives** | Browse-without-extract (already the design) matters more here, not less. Extraction is streamed with a lower absolute cap. |

### 11.2 Formats whose viability changes on Tier D

| Format | Note |
|---|---|
| Very large multi-page TIFF | Page-at-a-time only; no whole-document preview grid |
| 100 MB+ PDFs | Supported for view/search; page-reordering and merge operations that rewrite the whole file may exceed the arena and fail explicitly |
| XLSX with 1M-row sheets | Streamed read stands; the 100 ms database-view budget does not apply on Tier D and the row window is smaller |
| DOCX → PDF via the layout engine | The layout engine holds a page model, not the document; unchanged in principle, but the fidelity-report buffer needs the same arena cap |

### 11.3 Interaction with E2EE

`adr/0002` decision 1 removes the server-side conversion escape hatch. **H-16 (no LibreOffice
in the client; server-side only) means legacy DOC/XLS/PPT now requires the explicit
per-document decrypt-and-send consent flow** (`ai-architecture.md` §13.2) rather than being a
transparent ☁ capability. Combined with Tier D's constraints this makes **S-7 ("is legacy
DOC/XLS/PPT required at v1.0?") materially less attractive than it already was**, and the
existing recommendation — *no* — is strengthened rather than changed.

The same applies to every ☁ cell in the matrix: under E2EE, ☁ no longer means "we do it on
the server for you" but "we ask you, per document, whether to decrypt and send it". The
symbol's meaning has changed and §1's legend must be updated when Phase 8 is specified.

---

## 12. Mapping this matrix onto the spec §8 capability flags

Added when `file-engine/detection` landed. It is the first code to *consume* this document
rather than merely describe intent: `format_capabilities()` returns the spec §8 capability set
for a detected format, and a UI will use it to decide whether to show an "Edit" button. A
wrong cell here becomes a button that fails at the point of use.

**Five capabilities map straight onto a column:**

| §8 capability | Rule |
|---|---|
| VIEW | matrix **VIEW** is ✔ or ◐ **and not ☁**. A ☁ cell is a per-document decrypt-and-upload consent flow (§11.3), not something the client can do. |
| EDIT | matrix **EDIT** is ✔ or ◐. **P2 is not a capability.** |
| CONVERT | matrix **CONVERT** is ✔ or ◐, and not ☁ / TBD / P2. |
| OCR | matrix **OCR** is ✔ or ◐. |
| ANNOTATE | matrix **ANNOTATION** is ✔ or ◐. |

**Five have no column, so this section defines them.** *This is the deviation* — §9 of the
spec lists ten capabilities and this matrix has six columns, and the gap had to be closed by
judgment rather than transcription:

| §8 capability | Definition adopted | Consequence |
|---|---|---|
| EXPORT | "the engine can write this format out" — i.e. exactly the **CREATE** column. The alternative reading ("can be saved elsewhere") is true of every byte sequence and carries no information. | GIF and ODT lose it (CREATE ✖); DOCX keeps it (CREATE ◐). |
| COMPRESS | "can be losslessly repacked/optimised in place". Granted only where we ship a writer *and* recompression is lossless. | PDF (QPDF object streams, linearisation), PNG and ZIP (re-deflate). **Not JPEG** — §6 says re-encoding is lossy and must be disclosed, so it cannot be a silent action. |
| SIGN | "apply or validate a document signature". | **Granted to nothing, PDF included.** §5.1 already says validation is P2/TBD and that "a half-implemented signature check is worse than none"; a flag that lights up a Sign affordance *is* that half-implementation. A test asserts this holds across the whole registry. |
| ENCRYPT | "produce an encrypted copy in the format's own scheme". | **PDF only** (QPDF standard security handler R2–R6). **Not ZIP** — §8 marks AES-encrypted ZIP read-only. |
| EXTRACT | "enumerate and pull out embedded sub-objects" (archive members, OOXML parts, PDF attachments). | The ZIP family and PDF. |

**Two rows are worth calling out because the derived answer is narrower than a quick reading
of the matrix suggests, and both are enforced by tests:**

- **DOCX gets no EDIT.** §2 marks DOCX EDIT as **P2**. XLSX, by the same table, does get it —
  so this is the actual matrix, not a blanket "OOXML is read-only" rule.
- **PPTX gets EXTRACT and nothing else.** §4 marks VIEW as ◐ **P2** and says v1.0 ships "text +
  notes extraction for search only". Claiming VIEW would be the most misleading cell we could
  publish, so the derived set has exactly one member.

**No matrix cell was changed.** Implementing detection did not contradict any existing claim;
it only required the five definitions above. If a future engine change makes one of them
wrong, this section and `file-engine/detection/include/uapp/file_engine/capabilities.h` must
move together — the header cites this section by number.

---

## 8. Sources

- [PDFium LICENSE (googlesource)](https://pdfium.googlesource.com/pdfium/+/main/LICENSE)
- [Anti-Grain Geometry licence](https://agg.sourceforge.net/antigrain.com/license/index.html) and [AGG (Wikipedia) — v2.4 BSD vs v2.5 GPL](https://en.wikipedia.org/wiki/Anti-Grain_Geometry)
- [libheif issue #591 — x265 GPL and HEVC patents](https://github.com/strukturag/libheif/issues/591)
- [libde265 project page](https://www.libde265.org/)
- [x265 (Wikipedia) — GPL terms](https://en.wikipedia.org/wiki/X265)
- [PDF Association — Survey of open-source PDF solutions](https://pdfa.org/wp-content/uploads/2021/06/Survey-of-OpenSource-Solutions.pdf)
- [PyMuPDF licensing discussion — MuPDF AGPL / Artifex commercial](https://github.com/pymupdf/PyMuPDF/discussions/971)
- [The perils of LGPLv3 (nmav) — relinking vs. code signing](https://nikmav.blogspot.com/2013/03/the-perils-of-lgplv3.html)
- [LGPL static vs dynamic linking guide](https://licensecompat.com/guides/lgpl-static-vs-dynamic-linking/)

**All of the above must be re-verified against the pinned upstream revision at integration
time.** Licences change between versions — AGG 2.4 → 2.5 and OpenCV BSD → Apache-2.0 are both
examples of exactly that.
