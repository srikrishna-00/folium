// SPDX-License-Identifier: Apache-2.0
//
// core/conversion/markdown.h — Page/Block <-> Markdown, both directions.
//
// This is the FIRST CONCRETE INSTANCE of spec section 10's architecture:
//
//     format -> Parser -> Universal Document Model -> {PDF, HTML, MD, TXT, ...}
//
// with one correction to the Phase 0 sketch, recorded here because it changes
// what that diagram means in this repository:
//
//     docs/conversion-architecture.md section 2.1 specifies a `UDMDocument`
//     type with styles, numbering, sections, resources and a 15-alternative
//     `BlockNode` union. THAT TYPE IS NOT WHAT THIS CONVERTER USES. The
//     Universal Document Model in this codebase is `blocks::Page` +
//     `blocks::Block` (core/blocks), which ADR-0003 already made the project's
//     one document representation ("Page supersedes Note"). Building a second,
//     parallel document model for the conversion engine to translate through
//     would mean every conversion is TWO lossy hops (Markdown -> UDM -> Page)
//     and that the editor and the converter can disagree about what a document
//     is. So the IR is the live domain model, and `UDMDocument` is re-scoped to
//     the SUPERSET `blocks::Block` must grow into as formats land -- a backlog,
//     not a type. docs/conversion-architecture.md section 2.1 is annotated to
//     match; this header and that document must not drift.
//
// Markdown first, and only Markdown this run, for three reasons that are
// properties of the format rather than preferences:
//
//   1. No third-party dependency and so no spec section 35 licence review.
//      DOCX needs an OOXML/ZIP stack, EPUB needs XHTML, PDF-to-text needs a
//      PDF parser. Each is a real dependency decision; none is this run.
//   2. It round-trips naturally against the EIGHT implemented block types,
//      because CommonMark's leaf blocks are almost exactly that set.
//   3. It is useful today with nothing else built: export a page to a .md
//      file, import a .md file as a page.
//
// ---------------------------------------------------------------------------
// WHAT ROUND-TRIPS EXACTLY (Page -> Markdown -> Page)
// ---------------------------------------------------------------------------
// All eight implemented block types, with content, order and nesting:
//
//   paragraph            plain text line(s)
//   heading, level 1..3  `# ` / `## ` / `### `
//   bulleted_list_item   `- `
//   numbered_list_item   `1. `, `2. `, ... numbered from sibling position,
//                        which is where core/blocks says numbering lives
//                        ("DERIVED from sibling position at render time,
//                        never stored")
//   checklist_item       `- [ ] ` / `- [x] `
//   code                 fenced, with the language tag as the info string
//   divider              `---`
//   quote                `> `
//
// plus NESTING UNDER A LIST ITEM to any depth Markdown indentation allows: a
// list item's children are written at the item's content indent and read back
// by recursively parsing that indented region as a sub-document. A paragraph,
// heading, code block, divider or quote nested under a list item therefore
// round-trips too, not only a nested list.
//
// Multi-line text in a paragraph, quote or list item round-trips: embedded
// '\n' is written as a continuation line and consecutive content lines are
// re-joined with '\n'. A line of such text that would itself parse as a
// different block ("- not a bullet") is backslash-escaped on write and
// unescaped on read, which is CommonMark's own escape and is exactly
// invertible.
//
// ---------------------------------------------------------------------------
// WHAT IS LOSSY, AND WHY -- tied to core/blocks' own NOT IMPLEMENTED list
// ---------------------------------------------------------------------------
// Every item here is lossy because of a GAP IN THE BLOCK MODEL, not because of
// a shortcut in this converter. The authority for each is named so that this
// header and core/blocks cannot disagree about what is missing:
//
//   INLINE FORMATTING  `**bold**`, `_em_`, `` `code` ``, `[text](url)`,
//                      `![alt](src)`, inline HTML.
//                      Authority: core/blocks/document_service.h -- "a block's
//                      text is a plain UTF-8 string. Inline bold/italic/link/
//                      mention spans are NOT IMPLEMENTED; they need an inline
//                      run model plus the sequence CRDT that
//                      database-design.md reserves `block.crdt_state` for."
//                      DECISION: the raw Markdown syntax is KEPT VERBATIM in
//                      the block's text. A link stays the nine characters
//                      `[a](b)`. This is byte-lossless and round-trips
//                      exactly; what is lost is INTERPRETATION, so the text
//                      renders with literal asterisks until the span model
//                      exists. The alternative -- strip the syntax to its
//                      plain text -- destroys the href, which is the one part
//                      a future span model cannot reconstruct. Reported as
//                      `preserved_note` / "inline_markup" so a UI can warn.
//
//   TABLES             `| a | b |` + delimiter row (GFM; not CommonMark).
//                      Authority: core/blocks/block.h, `BlockType::Table = 10`
//                      is declared and NOT IMPLEMENTED -- there is no variant
//                      alternative to put a table in.
//                      DECISION: the table's lines are kept as paragraph text.
//                      Structure is lost, content is not. Round-trips as a
//                      paragraph (the lines come back out unchanged), which
//                      means a future Table block type can re-parse them.
//                      Reported as `approximated` / "table".
//
//   IMAGES             `![alt](src)` as its own block.
//                      Authority: core/blocks/block.h, `BlockType::Image = 11`
//                      declared and NOT IMPLEMENTED; it also needs the blob
//                      store core/blocks/page.h cites for Files
//                      ("database-design.md principle 6: blobs are not in the
//                      database").
//                      DECISION: kept as paragraph text, same as inline
//                      markup, so the src survives. Reported as
//                      `approximated` / "image".
//
//   HEADING LEVELS 4-6 `#### ` and deeper.
//                      Authority: core/blocks/block.h, kMaxHeadingLevel = 3.
//                      DECISION: CLAMPED to 3, not dropped and not demoted to
//                      a paragraph. Reported as `approximated` /
//                      "heading_level_clamped". A level-4 heading re-exports
//                      as `### `, so this is the one in-scope construct whose
//                      Markdown -> Page -> Markdown round-trip is not
//                      byte-identical. It is deliberate: losing a heading's
//                      heading-ness is worse than losing one level of it.
//
//   CHILDREN OF A      A paragraph cannot have a child in Markdown. Markdown
//   NON-LIST BLOCK     derives nesting from CONTAINERS (list items, block
//                      quotes) only, and `blocks::Block` lets any block have
//                      children.
//                      DECISION: such children are FLATTENED to the parent's
//                      own level, in document order, immediately after it --
//                      never dropped, never silently merged into the parent's
//                      text. Reported as `approximated` /
//                      "nesting_flattened". Page -> Markdown -> Page therefore
//                      preserves content and order but loses one level of
//                      depth for these. The honest alternative, refusing the
//                      export, would make a legal page un-exportable.
//                      (A quote's children are flattened for the same reason
//                      even though `>` IS a Markdown container: `QuoteBlock` is
//                      a LEAF with a `text` field, so a quote with both text
//                      and children has no unambiguous `>`-block form.)
//
//   LEADING / TRAILING Whitespace at the start or end of a text line.
//   WHITESPACE IN TEXT DECISION: NORMALISED AWAY on read. Four or more leading
//                      spaces is an indented code block in CommonMark, so
//                      preserving it would change the block type; and there is
//                      no CommonMark escape for a leading space. Reported as
//                      `degraded` / "text_line_whitespace" on export when a
//                      text line actually has it. In scope for exact
//                      round-trip: text whose lines have no leading or
//                      trailing whitespace.
//
//   CODE INFO STRING   A language tag containing whitespace. CommonMark takes
//                      the info string's first word as the language, so
//                      "c++ 17" reads back as "c++".
//                      DECISION: written as-is, reported as `degraded` /
//                      "code_language_truncated" on export. A tag containing a
//                      backtick switches the fence to `~~~`, which CAN carry
//                      it, so that case is lossless.
//
// NOT IMPLEMENTED on the read side, and NOT silently mis-parsed -- each is
// tested for the behaviour documented here:
//
//   setext headings    `Title` followed by `===` or `---`. `---` under a
//                      paragraph is ambiguous with a thematic break, and this
//                      converter resolves it as a THEMATIC BREAK (divider),
//                      which is what its own exporter means by it. A `===`
//                      line becomes paragraph text. Choosing CommonMark's
//                      setext rule instead would make every exported
//                      paragraph-then-divider pair re-import as a heading,
//                      i.e. it would break the round-trip this module exists
//                      for. Reported as `approximated` / "setext_heading"
//                      when a `===` run is seen, so it is visible rather than
//                      quietly a paragraph.
//   indented code      A 4-space-indented block at top level is read as
//   blocks             paragraph text, not as a code block, for the same
//                      reason: the exporter never produces one, and treating
//                      indentation as code would collide with the list-item
//                      continuation rule that nesting depends on. Reported as
//                      `approximated` / "indented_code_block".
//   link reference     `[id]: http://...`, footnotes, front matter, HTML
//   definitions,       blocks, block equations. All kept as paragraph text
//   footnotes,         (never dropped) and reported as `preserved_note` /
//   front matter       "unmodelled_block".
//
// NOT IMPLEMENTED, whole formats: DOCX, PDF, HTML, EPUB, RTF, ODT, TXT and
// every other row of docs/conversion-architecture.md section 5. Those need
// real format parsers -- and, for the Office formats, third-party libraries
// that do not exist in this repository and have not had the spec section 35
// licence review. None of them is faked here, and there is no
// `ConversionPlanner`, capability graph or job protocol yet either: this is
// ONE edge of section 4's graph, implemented, not the graph.
//
// ---------------------------------------------------------------------------
// NOTHING IS SILENTLY DROPPED
// ---------------------------------------------------------------------------
// There is no input this converter discards without either returning a
// non-Ok Status or filing a FidelityReport entry. `FidelityReport::
// lost_content()` is false for every case above, because every one of them
// keeps the bytes somewhere. That is the section 6.2 contract adapted to a
// block model with no `Unsupported` alternative.
//
// Thread affinity: free functions over caller-owned values; no module state.

#ifndef UAPP_CORE_CONVERSION_MARKDOWN_H
#define UAPP_CORE_CONVERSION_MARKDOWN_H

#include <cstddef>
#include <string>
#include <string_view>

#include "uapp/blocks/page.h"
#include "uapp/conversion/fidelity.h"
#include "uapp/ports.h"
#include "uapp/status.h"

namespace uapp::conversion {

// --- limits ----------------------------------------------------------------
// Both directions are bounded, because both are driven by untrusted input: a
// .md file on import, and a synced or restored page on export (spec section
// 38, "Validate external input"; same reasoning as blocks::kMaxBlockDepth).

/// Maximum bytes of Markdown accepted by `markdown_to_page`. Above this the
/// call fails with Status::ConversionInputTooLarge rather than attempting it.
///
/// WHY 16 MiB: it is ~10x the largest plausible hand-written document and
/// comfortably below the 256 MB mobile arena cap
/// docs/conversion-architecture.md section 8 sets, with the ~4x blow-up from
/// text into a block tree accounted for. It is NOT derived from
/// blocks::kMaxBlocksPerPage * blocks::kMaxBlockTextBytes (50 GB), which is a
/// bound on a page, not a sane bound on one file.
inline constexpr std::size_t kMaxMarkdownInputBytes = 16u * 1024u * 1024u;

/// Maximum bytes `page_to_markdown` will emit. A page at the block-model's own
/// limits would serialise to ~50 GB, so the export side needs its own cap and
/// cannot borrow the page's. Exceeding it is Status::ConversionOutputTooLarge
/// and `out` is left empty rather than half-written.
inline constexpr std::size_t kMaxMarkdownOutputBytes = 16u * 1024u * 1024u;

// --- export ----------------------------------------------------------------

struct MarkdownWriteOptions final {
    /// Write `page.title` as a level-1 heading before the blocks.
    ///
    /// Default true, and `MarkdownReadOptions::leading_heading_as_title`
    /// defaults true to match, so the DEFAULTS ROUND-TRIP: a .md file with no
    /// title line has lost the title, and a title line re-imported as an H1
    /// block has gained a block. Both options exist so that either half can
    /// be turned off, and both halves are tested off as well as on.
    ///
    /// An empty title writes no heading at all (an untitled page is a real
    /// state -- core/blocks/page.h) and re-imports as an empty title.
    bool title_as_heading = true;
};

/// Walk `page`'s block tree and emit CommonMark.
///
/// `out` is CLEARED first and is left empty on failure, so a caller cannot
/// write a truncated file by ignoring the Status. `report` is appended to, not
/// cleared, so one report can span a multi-document job.
///
/// Fails with Status::BlockDepthExceeded / Status::PageBlockLimitExceeded /
/// Status::BlockMalformed (etc.) if the page does not satisfy
/// blocks::validate_block_tree -- the export path validates its input for the
/// same reason the store's read path does: a page may have arrived from a
/// database file or a sync peer.
[[nodiscard]] Status page_to_markdown(const blocks::Page& page,
                                      const MarkdownWriteOptions& options,
                                      std::string& out,
                                      FidelityReport& report);

// --- import ----------------------------------------------------------------

struct MarkdownReadOptions final {
    /// If the document's first block is a level-1 heading, use its text as the
    /// page title instead of emitting a Heading block. See
    /// MarkdownWriteOptions::title_as_heading.
    bool leading_heading_as_title = true;

    /// Title for the resulting page when no title is taken from the document.
    /// Empty by default: inventing "Untitled" here would put a word of UI copy
    /// in the domain layer, and an empty title is legal.
    std::string_view default_title{};
};

/// Parse `markdown` into a page with a full block tree.
///
/// `out` is overwritten on success and left untouched on failure. `clock` and
/// `ids` are the ambient-authority ports (core/common/ports.h) -- the same two
/// blocks::DocumentService takes -- so the ids and timestamps of an imported
/// tree are deterministic under test and come from the host's approved
/// sources in production. The returned page satisfies
/// blocks::validate_page_metadata and blocks::validate_block_tree, so it can
/// be handed straight to a BlockStore.
///
/// Empty or whitespace-only input is Status::Ok with ZERO blocks, not an
/// error: an empty document is a legal empty page, and spec section 33's
/// "errors are never silently ignored" cuts the other way too -- reporting a
/// non-error as an error is its own lie. Callers who need "the user picked a
/// blank file" can check `out.blocks.empty()`.
///
/// Failure modes, all of them bounded rather than crashing:
///   ConversionInputTooLarge   > kMaxMarkdownInputBytes
///   ConversionInputMalformed  an embedded NUL byte (it would truncate at the
///                             first C ABI boundary, exactly as
///                             blocks::validate_block_content rejects one)
///   BlockDepthExceeded        list nesting deeper than blocks::kMaxBlockDepth
///   PageBlockLimitExceeded    more blocks than blocks::kMaxBlocksPerPage
///   BlockTextTooLarge         a single block's text over
///                             blocks::kMaxBlockTextBytes
///   Internal                  `ids` failed to produce an identifier
[[nodiscard]] Status markdown_to_page(std::string_view markdown,
                                      const MarkdownReadOptions& options,
                                      Clock& clock,
                                      IdSource& ids,
                                      blocks::Page& out,
                                      FidelityReport& report);

}  // namespace uapp::conversion

#endif  // UAPP_CORE_CONVERSION_MARKDOWN_H
