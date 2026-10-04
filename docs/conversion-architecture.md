# Conversion Architecture — Universal Conversion Engine

**Status:** Phase 0 draft — awaiting human sign-off
**Version:** 0.0.0
**Date:** 2026-09-29

Implements spec §10. The core idea in the spec — parse into an intermediate representation,
then write out — is correct. This document specifies the intermediate representations, says
where the IR approach *breaks down*, and defines how fidelity loss is measured and reported.

---

## 1. Why an IR, and the N×M argument

Direct format-pair converters need up to N×M implementations. With an IR it is N readers +
M writers. With ~15 formats that is 225 vs. 30.

The cost is that an IR is a lossy bottleneck: anything not expressible in the IR is lost even
when both endpoints support it. A DOCX→DOCX round-trip through a document IR will lose
features that a direct DOCX rewriter would keep.

**Our resolution — three tiers:**

| Tier | Mechanism | When |
|---|---|---|
| **T1 — Direct** | Format-pair-specific path, no IR | When both sides are the same family and fidelity matters: DOCX→DOCX edit, XLSX→XLSX edit, PDF page ops, JPEG→JPEG rotate |
| **T2 — IR** | Reader → IR → Writer | The default. Cross-family conversion. |
| **T3 — Render** | Reader → rasteriser → image/PDF | Last resort when semantics cannot be preserved: PPTX→PDF, complex SVG→PNG, DOCX→PDF for constructs we cannot lay out |

Tier is chosen per conversion and **reported to the user**, because T3 means "this is now a
picture, the text is gone" — unless we also attach a text layer, which we do wherever we can.

## 2. The three intermediate representations

The spec sketches three pipelines (document, spreadsheet, image). We implement exactly those
three IRs and deliberately do **not** try to unify them. A spreadsheet is not a document with
a big table; forcing one IR to cover both produces a model that serves neither.

### 2.1 Universal Document Model (UDM)

> **IMPLEMENTATION NOTE — added when `core/conversion` landed. This note and
> `core/conversion/include/uapp/conversion/markdown.h` are two halves of one decision and
> must not drift.**
>
> **`UDMDocument` below is NOT IMPLEMENTED, and it is not the type the shipped converter
> translates through.** The Universal Document Model in this codebase is `blocks::Page` +
> `blocks::Block` (`core/blocks`), which `adr/0003-page-supersedes-note.md` already made the
> project's one document representation. Building a second, parallel document model for the
> conversion engine would mean every conversion is **two** lossy hops (Markdown → UDM →
> Page), and that the editor and the converter could disagree about what a document *is*. So
> the IR is the live domain model, and the sketch below is re-scoped to the **superset
> `blocks::Block` must grow into as formats land — a backlog, not a type.**
>
> What that backlog costs today, with the authority for each gap:
>
> | Sketched above | Status in `blocks::Block` |
> |---|---|
> | `Paragraph`, `Heading`, `CodeBlock`, `Quote`, `Divider` | **IMPLEMENTED** as five of the eight block types — except that `Heading` is levels **1–3** (`kMaxHeadingLevel`), not 1–6 |
> | `List { ordered, items }` | **IMPLEMENTED** as bulleted / numbered / checklist *item* blocks with children, rather than as a container node. Numbering is DERIVED from sibling position at render time and never stored, so there is no `numbering` table to normalise into |
> | `InlineNode` — `Text{marks}`, `Link`, `InlineImage`, `Break`, `FieldRef`, `Anchor` | **NOT IMPLEMENTED.** A block's text is a plain UTF-8 string; spans need an inline run model plus the sequence CRDT `database-design.md` reserves `block.crdt_state` for (`core/blocks/document_service.h`) |
> | `Table`, `Image` | declared and NOT IMPLEMENTED — `BlockType::Table = 10`, `Image = 11` |
> | `Equation`, `Embed` | declared and NOT IMPLEMENTED — `BlockType::Equation = 15`, `Embed = 16` |
> | `PageBreak`, `Footnote`, `styles`, `sections`, `page_setup`, header/footer | no counterpart at all; these arrive with the first paginated format, not before |
> | `resources` | NOT IMPLEMENTED — needs the blob store (`database-design.md` principle 6, "blobs are not in the database") |
> | `Unsupported` | **no counterpart, and cannot have one**: `blocks::BlockContent` is a CLOSED variant (`core/blocks/block.h`, "WHY A VARIANT"). See the note in §6.2 for how §6.2's contract is met instead. |
> | `fidelity` | **IMPLEMENTED** as `conversion::FidelityReport` (`core/conversion/include/uapp/conversion/fidelity.h`) |
>
> Scope of what exists: **one edge of §4's graph** — `blocks::Page` ↔ **Markdown**, both
> directions, tested in both directions. There is no `ConversionPlanner`, no capability graph
> and no job protocol yet, and every other row of §5 is NOT IMPLEMENTED.

```
UDMDocument
  metadata     : { title, author, subject, keywords, created, modified,
                   language, custom{} }
  resources    : ResourceId → { bytes|blob_ref, mime, intrinsic_size }
  styles       : StyleId → StyleDef        (named, inheritable)
  numbering    : ListId  → { levels[], restart_rule }
  sections     : Section[]
  fidelity     : FidelityReport            (see §6)

Section
  page_setup   : { size, margins, orientation, columns }
  header, footer : BlockNode[]   (first/even/odd variants)
  content      : BlockNode[]

BlockNode  (tagged union)
  Paragraph   { style_ref, list_ref?, list_level?, inlines[], direction }
  Heading     { level 1..6, inlines[] }
  List        { ordered, items: BlockNode[][] }        -- normalised from flat numbering
  Table       { rows: Row[], col_widths[], header_rows, borders }
  Image       { resource_ref, size, alt, wrap_mode, caption? }
  CodeBlock   { language?, text }
  Quote       { content: BlockNode[] }
  Divider     {}
  PageBreak   {}
  Equation    { mathml }
  Footnote    { id, content: BlockNode[] }
  Embed       { kind, resource_ref?, placeholder_text }   -- the honest escape hatch
  Unsupported { original_tag, placeholder_text, raw_ref? } -- see §6.2

InlineNode
  Text        { text, marks: {bold,italic,underline,strike,sub,sup,code,
                              color,highlight,font,size} }
  Link        { href, title, inlines[] }
  InlineImage { resource_ref, size }
  Break       { soft | hard | column }
  FieldRef    { kind: page_number|total_pages|date|toc_entry, cached_text }
  Anchor      { id }
```

Design decisions worth defending:

- **Styles are named and inheritable, not flattened.** Flattening at read time destroys the
  ability to write a clean DOCX back out, and bloats the model.
- **Lists are normalised into a tree.** DOCX represents lists as flat paragraphs with a
  numbering reference; Markdown and HTML need a tree. Doing this normalisation once in the
  reader, not in each writer, is worth the reader complexity.
- **`Unsupported` is a first-class node.** Silently dropping content is the failure mode we are
  designing against. An unsupported construct becomes a visible node that the writer renders as
  a placeholder and the fidelity report counts.
- **Fields carry `cached_text`.** We do not evaluate `{ PAGE }` at read time; we keep what the
  producer computed and re-evaluate only in writers that can.

### 2.2 Spreadsheet Model (SSM)

```
SSMWorkbook
  metadata, defined_names, styles
  sheets : Sheet[]

Sheet
  name, visible, dimensions
  columns   : { index, width, style_ref, hidden }[]
  rows      : { index, height, style_ref, hidden }[]
  cells     : sparse map (row,col) → Cell
  merges    : Range[]
  frozen    : { rows, cols }

Cell
  value     : Empty | Number | Text(shared_string_ref) | Bool | Error | Date
  formula   : { text, cached_value }        -- text preserved, NOT evaluated at v1.0
  style_ref : StyleId
  hyperlink?, comment?
```

- **Sparse storage is mandatory**, not an optimisation. A sheet declaring 1 048 576 rows with
  30 populated cells is common, and a dense model OOMs instantly.
- **Formulas are carried as text plus the producer's cached value.** The UI must display
  "formulas not recalculated" whenever a dependent cell has been edited
  (`file-format-support.md` §3). Pretending a stale cached value is live is a correctness lie.
- Dates are a distinct value type because XLSX stores them as serial numbers with a format
  code, and losing that distinction corrupts every date on round-trip (including the 1900
  leap-year quirk, which the reader normalises).

### 2.3 Image Model (IMM)

```
IMMImage
  frames      : Frame[]           -- >1 for GIF/TIFF/animated WebP
  color_space : sRGB | Gray | CMYK | ICC(profile_bytes)
  metadata    : { exif{}, xmp{}, iptc{} }
  orientation : EXIF 1..8         -- applied on decode, preserved for re-write

Frame
  width, height, bit_depth, has_alpha
  pixels : PixelBuffer            -- may be lazily/tile-decoded for huge images
  delay_ms?, dispose_op?
```

- **EXIF orientation is applied on decode and recorded**, so downstream code never has to
  remember to rotate. Forgetting this is the single most common image bug.
- **Huge images are tile-decoded.** A 20 000 × 20 000 TIFF is 1.6 GB at RGBA8; the model holds
  a tile provider, not a buffer.
- **Colour space is preserved, not assumed sRGB.** CMYK JPEGs and ICC-tagged images convert
  approximately, and that approximation is reported.

## 3. Pipeline

```
 bytes
   │
   ├─ 1. DETECT   file-engine/detection (magic → container → metadata → MIME → ext)
   │              yields format id + confidence
   │
   ├─ 2. PLAN     ConversionPlanner
   │              · look up (src, dst) in the capability graph
   │              · choose tier T1/T2/T3
   │              · if no direct edge, find a path (≤ 2 hops, e.g. DOC→DOCX→PDF)
   │              · estimate cost + required resources
   │              · return UNSUPPORTED_CONVERSION early rather than half-trying
   │
   ├─ 3. READ     Reader → IR, inside an arena with byte/time caps
   │              collects FidelityReport entries as it goes
   │
   ├─ 4. TRANSFORM  optional IR→IR passes:
   │                · normalise (list flattening, style resolution)
   │                · downgrade (drop constructs the target cannot express, logging each)
   │                · user options (page size, image quality, OCR, page ranges)
   │
   ├─ 5. WRITE    Writer → bytes, streamed to the output blob
   │              adds its own FidelityReport entries
   │
   ├─ 6. VERIFY   cheap post-conditions (see §7)
   │
   └─ 7. REPORT   fidelity report attached to the output file record
```

**Every stage is cancellable and reports progress.** A conversion is always a job
(`api-design.md` §3.4).

## 4. Capability graph

The planner is driven by a declarative registry, not a switch statement. This is what makes the
format registry "continuously extensible" (spec §9) rather than a place where everyone adds an
`if`.

```c
typedef struct {
    uapp_format_t  src;
    uapp_format_t  dst;
    uint8_t        tier;            /* 1 direct, 2 IR, 3 render */
    uint8_t        fidelity_class;  /* 0 lossless, 1 high, 2 lossy, 3 rasterised */
    uint32_t       flags;           /* NEEDS_NETWORK | NEEDS_OCR | NEEDS_FONTS | ... */
    uint32_t       est_ms_per_mb;
    uapp_convert_fn fn;
} uapp_conversion_edge_t;
```

- `GET /v1/capabilities` and the local `uapp_conversion_list()` both project this table, so the
  UI never hard-codes what is convertible and the answer is always truthful.
- Multi-hop paths are allowed to depth 2 and are shown to the user as such ("via DOCX"),
  because each hop compounds loss.

## 5. Conversion matrix (spec §40 item 20)

Fidelity class: **L** lossless · **H** high (semantics preserved, layout may differ) ·
**Y** lossy (documented loss) · **R** rasterised (text becomes pixels unless a text layer is
added) · **–** not supported · **☁** server-only.

### 5.1 Documents → X

| From ↓ / To → | PDF | DOCX | HTML | MD | TXT | EPUB | Images |
|---|---|---|---|---|---|---|---|
| TXT | H | H | H | H | L | H | R |
| MD | H | H | H | L | H | H | R |
| HTML | Y | Y | L | Y | H | Y | R |
| RTF | Y | Y | Y | Y | H | Y | R |
| DOCX | Y | L (T1) | Y | Y | H | Y | R |
| ODT | Y | Y | Y | Y | H | Y | R |
| DOC | ☁ Y | ☁ Y | ☁ Y | ☁ Y | ☁ H | – | ☁ R |
| EPUB | Y | Y | Y | Y | H | L (T1) | R |
| PDF | L (T1) | **Y (reconstruction)** | Y | Y | H | Y | R |

> **IMPLEMENTATION NOTE (`core/conversion`).** Exactly **one** cell of this entire section is
> implemented: **MD → MD**, i.e. `blocks::Page` → Markdown and Markdown → `blocks::Page`,
> with the loss boundary documented construct-by-construct in
> `core/conversion/include/uapp/conversion/markdown.h`. Every other cell in §5.1–§5.4 is
> **NOT IMPLEMENTED**: each needs a real format parser, and the Office and EPUB rows need
> third-party libraries that are not in this repository and have not had the spec §35 licence
> review. The table stays as written because it is the *plan*; nothing here is faked.

### 5.2 Spreadsheets → X

| From ↓ / To → | XLSX | CSV | ODS | PDF | HTML |
|---|---|---|---|---|---|
| CSV | H | L | H | R | H |
| XLSX | L (T1) | Y (one sheet, values only) | Y | R | Y |
| ODS | Y | Y | L (T1) | R | Y |
| XLS | ☁ Y | ☁ Y | ☁ Y | ☁ R | ☁ Y |

CSV output from a multi-sheet workbook exports one sheet; the user picks which. Formulas export
as their cached values. Both facts are surfaced, not buried.

### 5.3 Images → X

| From ↓ / To → | PNG | JPEG | WEBP | TIFF | PDF | BMP |
|---|---|---|---|---|---|---|
| PNG | L | Y | Y/L | L | L (embed) | L |
| JPEG | L | L (T1 rotate) / Y (re-encode) | Y | L | L (embed) | L |
| WEBP | L | Y | L | L | L (embed) | L |
| TIFF | L | Y | Y | L | L (embed) | L |
| GIF | L (frame) | Y | L | L | L | L |
| BMP | L | Y | Y | L | L | L |
| SVG | R | R | R | R | H (vector, if the writer supports the constructs) | R |
| HEIC | R→L (device only) | Y | Y | Y | Y | Y |

"L (embed)" = the image bytes are embedded in the PDF without re-encoding, which is both
lossless and fast. JPEG→PDF must not re-encode.

### 5.4 PDF-specific

| Operation | Tier | Fidelity | Note |
|---|---|---|---|
| Merge / split / extract / reorder / rotate | T1 (QPDF) | L | Object-level, lossless |
| Compress | T1 | Y | Image downsampling and re-encoding is the main lever; also object streams and font subsetting. Quality target is explicit. |
| PDF → images | T3 (PDFium) | R | dpi selectable |
| Images → PDF | T1 | L | Bytes embedded, not re-encoded |
| OCR → searchable PDF | T2 | L for the image, additive text layer | The original raster is untouched; an invisible text layer (render mode 3) is added |
| PDF → DOCX | T2 | **Y, reconstruction** | Structure is inferred, not read. Permanently marked lossy. |

## 6. Fidelity reporting (spec §10: "Clearly report conversion-fidelity limitations")

This is not a nice-to-have. It is the mechanism by which we keep the promise not to pretend
things work.

### 6.1 Report shape

```json
{
  "source_format": "docx", "target_format": "pdf",
  "tier": 2, "fidelity_class": "lossy",
  "entries": [
    { "severity": "dropped",       "construct": "smartart",
      "count": 2, "location": "page 4, page 11",
      "message": "SmartArt diagrams replaced with placeholder images." },
    { "severity": "approximated",  "construct": "text_box_wrap",
      "count": 5, "message": "Complex text wrapping approximated as inline placement." },
    { "severity": "preserved_note","construct": "tracked_changes",
      "count": 12, "message": "Tracked changes were applied (shown as accepted)." }
  ],
  "summary": { "dropped": 2, "approximated": 5, "notes": 1 },
  "confidence": "medium"
}
```

Severities: `dropped` · `approximated` · `degraded` (quality reduced, e.g. image re-encode) ·
`preserved_note` (kept, but the user should know how).

### 6.2 The `Unsupported` node contract

When a reader meets a construct it cannot model:

1. Emit an `Unsupported` IR node with the original tag name and, when available, a
   producer-generated fallback rendition (OOXML often embeds one).
2. Add a `dropped` or `approximated` fidelity entry with a location.
3. Optionally retain the raw bytes under `raw_ref` so a future version can do better, and so a
   T1 round-trip can restore it.

**A reader that swallows an unknown element without emitting an `Unsupported` node is a bug**,
and a reviewable one — the parser tests assert that unknown elements produce entries.

> **IMPLEMENTATION NOTE (`core/conversion`).** Step 1 has **no counterpart and cannot have
> one**: the IR is `blocks::Block`, whose `BlockContent` is a closed variant with no
> `Unsupported` alternative (see §2.1). The shipped converter therefore keeps the *spirit* of
> this contract by a different mechanism: an unmodelled construct is **kept as the plain text
> it was written in** — never discarded — and a `FidelityReport` entry is filed naming it.
> The consequence is that `lost_content()` is **false for every case the Markdown converter
> can produce**, because every one of them keeps the bytes somewhere, so `severity:
> "dropped"` is declared (the report shape is shared with converters that will need it) but
> never emitted here. Entries aggregate by `(severity, construct)` rather than appearing once
> per occurrence, which bounds the report against untrusted input (spec §38) and is also the
> shape §6.1's own example shows. The "parser tests assert that unknown elements produce
> entries" sentence is honoured literally: `tests/conversion/markdown_test.cpp` asserts the
> exact construct id, severity and resulting text for every documented lossy case.

### 6.3 Surfacing

- Fidelity reports are stored with the output file and viewable at any time.
- Before an expensive or lossy conversion, the UI shows the *predicted* fidelity class from the
  capability graph.
- After conversion, if `summary.dropped > 0`, the UI shows a non-modal notice with a link to
  the detail. It is not hidden behind an "advanced" disclosure.

## 7. Verification

Cheap post-conditions run automatically on every conversion. They catch the class of bug where
a converter "succeeds" and produces garbage:

| Check | Applies to | Assertion |
|---|---|---|
| Non-empty output | all | size > 0, and > 5 % of a reasonable expected floor |
| Format validity | all | the output re-detects as the target format |
| Text retention | text-bearing pairs | extracted output text length ≥ 90 % of input text length (excluding deliberate drops) |
| Page count | paginated pairs | within the expected range |
| Image dimensions | image pairs | match unless resizing was requested |
| Round-trip | T1 same-format ops | byte-level or semantic equality where claimed lossless |
| Redaction | redaction only | redacted strings absent from output text and metadata (hard gate) |

A failed check turns a "success" into `FILE_CONVERSION_VERIFICATION_FAILED` with the specific
check named. We would rather fail than hand the user a broken file.

## 8. Resource governance

Conversion is the most likely place for the app to be OOM-killed or to hang.

- **Arena per job** with a hard byte cap (default 256 MB mobile, 2 GB server). Exhaustion is
  `FILE_RESOURCE_LIMIT`, not a crash.
- **Wall-clock cap** per job, scaled by input size, with progress-based extension.
- **Streaming wherever the formats permit:** CSV, TXT, XML/OOXML sheet parts, TIFF pages, PDF
  pages. Anything that must be fully materialised (a DOCX's `document.xml`) has a declared
  size limit.
- **Tile-based image processing** for images above a pixel threshold.
- **Concurrency limit:** one heavy conversion at a time on mobile; the rest queue. Two parallel
  200 MB PDF compressions on a phone is a guaranteed OOM.
- **Background execution:** conversions continue under the platform's background-task API, with
  checkpointing so a suspension does not lose work.

## 9. Testing

Detail in `testing-strategy.md`. Conversion-specific:

- **A corpus, not examples.** ≥ 300 real-world files across the matrix, including files
  produced by different generators (Word vs. Google Docs vs. LibreOffice vs. Pages export)
  because each emits meaningfully different OOXML.
- **Golden-output tests** with a structural comparator (IR-level diff), not byte comparison —
  byte comparison on PDF output is useless because of timestamps and object ordering.
- **Round-trip property tests:** `read(write(ir)) ≈ ir` for every T1 pair, with a defined
  equivalence relation.
- **Fidelity-report tests:** a file with known unsupported constructs must produce exactly the
  expected report entries. This prevents silent regression into silent dropping.
- **Fuzzing** every reader (spec §29).
- **Performance regression gates** per pair (`performance-strategy.md`).

## 10. Explicit non-goals and limits

- **We do not aim for byte-identical round-trips through the IR.** T1 exists for that.
- **PDF→DOCX will always be marked lossy.** No amount of engineering changes the fact that a
  PDF has no semantic structure to recover.
- **Formula evaluation is out of scope at v1.0** (`technical-requirements.md` §5.1).
- **PPTX rendering is out of scope at v1.0** — text extraction only.
- **No conversion ever fetches a network resource** (T-106). Missing external images render as
  placeholders and generate a fidelity entry.
- **Macros are never executed**, under any circumstances, in any format.
