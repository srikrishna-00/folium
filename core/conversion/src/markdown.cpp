// SPDX-License-Identifier: Apache-2.0
//
// core/conversion/src/markdown.cpp — the writer and the reader.
//
// The contract this file implements is in uapp/conversion/markdown.h, in full:
// every lossy case, every fallback and every failure mode is documented there
// and is NOT re-argued here. What this file adds is the mechanics, and three
// of those are worth stating up front because they are the reason the
// round-trip holds:
//
//  1. ONE LEXER. Every "is this line a heading / a fence / a list item?"
//     question goes through src/markdown_syntax.h, so the writer's
//     "must I escape this line?" and the reader's "does this line end the
//     paragraph?" cannot drift apart.
//  2. ONE ESCAPE. Every text byte written goes through escape_text_line and
//     every text byte read comes back through unescape_text_line.
//  3. NESTING IS INDENTATION. A list item's children are written at the
//     item's content column and read back by recursively parsing that
//     indented range as a sub-document. The writer computes the content
//     column from the marker it is about to emit; the reader computes it from
//     the marker it just parsed; parse_list_marker() is what makes those the
//     same number.

#include "uapp/conversion/markdown.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "markdown_syntax.h"
#include "uapp/blocks/block.h"
#include "uapp/object_id.h"

namespace uapp::conversion {
namespace {

using blocks::Block;
using blocks::BlockContent;
using blocks::BlockId;
using blocks::BlockType;
using blocks::BulletedListItemBlock;
using blocks::ChecklistItemBlock;
using blocks::CodeBlock;
using blocks::DividerBlock;
using blocks::HeadingBlock;
using blocks::NumberedListItemBlock;
using blocks::Page;
using blocks::PageId;
using blocks::ParagraphBlock;
using blocks::QuoteBlock;
using detail::ListKind;
using detail::ListMarker;

// --- the construct vocabulary ----------------------------------------------
// The CLOSED set of construct ids markdown.h names, as constants rather than
// scattered string literals: a typo becomes a build failure, and the tests
// assert against the same symbols the implementation reports.

constexpr std::string_view kInlineMarkup        = "inline_markup";
constexpr std::string_view kTable               = "table";
constexpr std::string_view kImage               = "image";
constexpr std::string_view kHeadingLevelClamped = "heading_level_clamped";
constexpr std::string_view kNestingFlattened    = "nesting_flattened";
constexpr std::string_view kTextLineWhitespace  = "text_line_whitespace";
constexpr std::string_view kCodeLanguageTrunc   = "code_language_truncated";
constexpr std::string_view kSetextHeading       = "setext_heading";
constexpr std::string_view kIndentedCodeBlock   = "indented_code_block";
constexpr std::string_view kUnmodelledBlock     = "unmodelled_block";

// Messages carry NO user content (spec section 34: a report may be logged, a
// document body may not be).
constexpr std::string_view kMsgInlineMarkup =
    "Inline formatting is kept as literal Markdown syntax in the block's text; "
    "core/blocks has no inline run model yet.";
constexpr std::string_view kMsgTable =
    "A table is kept as paragraph text; core/blocks has no Table block type yet.";
constexpr std::string_view kMsgImage =
    "An image is kept as paragraph text; core/blocks has no Image block type yet.";
constexpr std::string_view kMsgHeadingLevelClamped =
    "Heading levels 4-6 are clamped to 3, the deepest level core/blocks models.";
constexpr std::string_view kMsgNestingFlattened =
    "Children of a non-list block are flattened to the parent's own level; "
    "Markdown derives nesting from containers only.";
constexpr std::string_view kMsgTextLineWhitespace =
    "Leading, trailing or line-breaking whitespace in a text line is normalised; "
    "CommonMark has no escape for it.";
constexpr std::string_view kMsgCodeLanguageTrunc =
    "A code info string containing whitespace keeps only its first word as the "
    "language tag.";
constexpr std::string_view kMsgSetextHeading =
    "A setext underline is read as paragraph text, not as a heading; this "
    "converter resolves the ambiguity in favour of its own exporter's output.";
constexpr std::string_view kMsgIndentedCodeBlock =
    "An indented code block is read as paragraph text; indentation is reserved "
    "for list-item nesting.";
constexpr std::string_view kMsgUnmodelledBlock =
    "An unmodelled construct is kept verbatim as paragraph text.";

// --- small shared helpers --------------------------------------------------

constexpr bool is_space_or_tab(char c) noexcept { return c == ' ' || c == '\t'; }

/// Whitespace the TEXT layer normalises, which is a superset of the whitespace
/// the Markdown STRUCTURE layer cares about (detail::trim). '\r' is in here
/// because a line ending in one cannot survive CRLF normalisation on the way
/// back in, and losing it silently is the one thing this module may not do.
constexpr bool is_text_edge_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

[[nodiscard]] std::string_view trim_text_edges(std::string_view s) noexcept {
    std::size_t begin = 0;
    std::size_t end   = s.size();
    while (begin < end && is_text_edge_space(s[begin])) { ++begin; }
    while (end > begin && is_text_edge_space(s[end - 1])) { --end; }
    return s.substr(begin, end - begin);
}

/// Trailing '\r' only. Used for CODE lines, where every other byte of
/// whitespace is content a fenced block preserves exactly -- but where a
/// trailing carriage return could not survive the reader's CRLF
/// normalisation and so must be removed visibly rather than lost.
[[nodiscard]] std::string_view trim_trailing_cr(std::string_view s) noexcept {
    while (!s.empty() && s.back() == '\r') { s.remove_suffix(1); }
    return s;
}

/// Split on '\n'. Always yields at least one element, so that an empty string
/// is one empty line rather than no lines -- see the note on code blocks in
/// write_block(), which is the one place that distinction matters.
[[nodiscard]] std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) {
            lines.push_back(text.substr(pos));
            return lines;
        }
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
}

/// How many COLUMNS `s` occupies when it starts at column `start_col`. Only
/// differs from s.size() when `s` contains a tab, which is exactly when the
/// writer's byte arithmetic and the reader's column arithmetic would disagree
/// about where a list item's content starts.
[[nodiscard]] std::size_t column_width(std::string_view s, std::size_t start_col) noexcept {
    std::size_t col = start_col;
    for (const char c : s) {
        if (c == '\t') {
            col += detail::kTabWidth - (col % detail::kTabWidth);
        } else {
            ++col;
        }
    }
    return col - start_col;
}

[[nodiscard]] bool is_list_family(BlockType type) noexcept {
    return type == BlockType::BulletedListItem || type == BlockType::NumberedListItem ||
           type == BlockType::ChecklistItem;
}

// --- construct detection, shared by both directions ------------------------
// These run over the FINAL block text in both directions, so an import and a
// re-export of the same construct report the same construct id.

[[nodiscard]] std::size_t count_char(std::string_view s, char c) noexcept {
    std::size_t n = 0;
    for (const char ch : s) {
        if (ch == c) { ++n; }
    }
    return n;
}

/// A GFM delimiter row: `|---|---|`, `---|---`, `| :--- | ---: |`.
[[nodiscard]] bool is_table_delimiter_row(std::string_view t) noexcept {
    if (t.size() < 3) { return false; }
    bool dash = false;
    bool pipe = false;
    for (const char c : t) {
        if (c == '-') {
            dash = true;
        } else if (c == '|') {
            pipe = true;
        } else if (c != ':' && !is_space_or_tab(c)) {
            return false;
        }
    }
    return dash && pipe;
}

/// `![alt](src)` and nothing else on the line -- markdown.h's IMAGES case, as
/// distinct from an image appearing inside running text (INLINE FORMATTING).
[[nodiscard]] bool is_image_only(std::string_view t) noexcept {
    if (t.size() < 5 || t[0] != '!' || t[1] != '[' || t.back() != ')') { return false; }
    return t.find("](", 2) != std::string_view::npos;
}

[[nodiscard]] bool has_link_or_image(std::string_view t) noexcept {
    for (std::size_t i = 0; i + 3 < t.size(); ++i) {
        if (t[i] != '[') { continue; }
        const std::size_t rb = t.find(']', i + 1);
        if (rb == std::string_view::npos || rb + 1 >= t.size() || t[rb + 1] != '(') { continue; }
        if (t.find(')', rb + 2) != std::string_view::npos) { return true; }
    }
    return false;
}

/// `_em_`, but NOT `snake_case_name`. CommonMark forbids intraword emphasis
/// for '_', and honouring that here is the difference between a useful note
/// and one that fires on every identifier in a technical document.
[[nodiscard]] bool has_underscore_emphasis(std::string_view t) noexcept {
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (t[i] != '_') { continue; }
        if (i != 0 && !is_space_or_tab(t[i - 1])) { continue; }
        for (std::size_t j = i + 1; j < t.size(); ++j) {
            if (t[j] != '_') { continue; }
            if (j + 1 == t.size() || is_space_or_tab(t[j + 1]) ||
                std::string_view(".,;:!?)]}'\"").find(t[j + 1]) != std::string_view::npos) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] bool has_inline_html(std::string_view t) noexcept {
    for (std::size_t i = 0; i + 2 < t.size(); ++i) {
        if (t[i] != '<') { continue; }
        const char n = t[i + 1];
        const bool tagish = (n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z') || n == '/' ||
                            n == '!';
        if (tagish && t.find('>', i + 2) != std::string_view::npos) { return true; }
    }
    return false;
}

[[nodiscard]] bool has_inline_markup(std::string_view t) noexcept {
    return count_char(t, '`') >= 2 || count_char(t, '*') >= 2 || has_underscore_emphasis(t) ||
           has_link_or_image(t) || has_inline_html(t);
}

[[nodiscard]] bool is_link_reference_definition(std::string_view t) noexcept {
    if (t.size() < 3 || t[0] != '[') { return false; }
    const std::size_t rb = t.find("]:", 1);
    return rb != std::string_view::npos;
}

[[nodiscard]] bool is_html_block_start(std::string_view t) noexcept {
    if (t.size() < 2 || t[0] != '<') { return false; }
    const char n = t[1];
    return (n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z') || n == '/' || n == '!' || n == '?';
}

[[nodiscard]] bool is_block_equation(std::string_view t) noexcept {
    return t.size() >= 2 && t[0] == '$' && t[1] == '$';
}

[[nodiscard]] bool is_setext_underline(std::string_view t) noexcept {
    if (t.empty()) { return false; }
    for (const char c : t) {
        if (c != '=') { return false; }
    }
    return true;
}

/// Note every construct in `text` that survives only as plain text. Called by
/// the writer on a block it is about to emit and by the reader on a block it
/// has just built, which is what makes the two directions report the same ids.
void note_text_constructs(std::string_view text, std::size_t line, FidelityReport& report) {
    const std::vector<std::string_view> lines = split_lines(text);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view t = detail::trim(lines[i]);
        if (t.empty()) { continue; }
        if (is_image_only(t)) {
            report.note(Fidelity::Approximated, kImage, line, kMsgImage);
        } else if (has_inline_markup(t)) {
            report.note(Fidelity::PreservedNote, kInlineMarkup, line, kMsgInlineMarkup);
        }
        if (i > 0 && is_table_delimiter_row(t) &&
            lines[i - 1].find('|') != std::string_view::npos) {
            report.note(Fidelity::Approximated, kTable, line, kMsgTable);
        }
    }
}

// ===========================================================================
// WRITER
// ===========================================================================

class Writer final {
public:
    Writer(std::string& out, FidelityReport& report) noexcept : out_(out), report_(report) {}

    [[nodiscard]] bool overflowed() const noexcept { return overflow_; }

    void raw(std::string_view s) {
        if (overflow_) { return; }
        if (s.size() > kMaxMarkdownOutputBytes - out_.size()) {
            overflow_ = true;
            return;
        }
        out_.append(s);
    }

    void newline() { raw("\n"); }

    void indent(std::size_t cols) {
        if (overflow_ || cols == 0) { return; }
        raw(std::string(cols, ' '));
    }

    /// One text line: whitespace-normalised (reported), then escaped.
    void text_line(std::string_view line) {
        const std::string_view t = trim_text_edges(line);
        if (t.size() != line.size()) {
            report_.note(Fidelity::Degraded, kTextLineWhitespace, 0, kMsgTextLineWhitespace);
        }
        raw(detail::escape_text_line(t));
    }

    /// A single-line field (an ATX heading, the page title). An ATX heading
    /// has no continuation syntax, so embedded newlines are FOLDED to spaces
    /// rather than dropped or split off into a second block.
    void folded_text_line(std::string_view text) {
        if (text.find('\n') == std::string_view::npos) {
            text_line(text);
            return;
        }
        report_.note(Fidelity::Degraded, kTextLineWhitespace, 0, kMsgTextLineWhitespace);
        std::string folded;
        for (const std::string_view line : split_lines(text)) {
            const std::string_view t = detail::trim(line);
            if (t.empty()) { continue; }
            if (!folded.empty()) { folded.push_back(' '); }
            folded.append(t);
        }
        text_line(folded);
    }

    void write_level(const std::vector<Block>& level, std::size_t indent_cols);

private:
    void write_block(const Block& b, std::size_t indent_cols, std::size_t ordinal);

    std::string&    out_;
    FidelityReport& report_;
    bool            overflow_ = false;
};

/// Longest run of `fence` at the start of any line of `code`, ignoring up to
/// three leading spaces -- the same three CommonMark lets a closing fence be
/// indented by, so a fence sized from this cannot be closed by its content.
[[nodiscard]] std::size_t longest_fence_run(std::string_view code, char fence) noexcept {
    std::size_t longest = 0;
    for (std::string_view line : split_lines(code)) {
        std::size_t skipped = 0;
        while (skipped < 3 && skipped < line.size() && line[skipped] == ' ') { ++skipped; }
        const std::size_t run = detail::longest_leading_run(line.substr(skipped), fence);
        if (run > longest) { longest = run; }
    }
    return longest;
}

void Writer::write_block(const Block& b, std::size_t indent_cols, std::size_t ordinal) {
    const BlockContent& content = b.content;
    const BlockType     type    = blocks::block_type_of(content);

    // Columns a list item's children and continuation lines sit at. Zero for
    // everything else, because a non-container block's children are flattened.
    std::size_t content_cols = indent_cols;

    if (const auto* p = std::get_if<ParagraphBlock>(&content)) {
        note_text_constructs(p->text, 0, report_);
        for (const std::string_view line : split_lines(p->text)) {
            indent(indent_cols);
            text_line(line);
            newline();
        }
    } else if (const auto* h = std::get_if<HeadingBlock>(&content)) {
        note_text_constructs(h->text, 0, report_);
        indent(indent_cols);
        // level is 1..3; validate_block_content guaranteed it before we got here.
        raw(std::string(static_cast<std::size_t>(h->level), '#'));
        raw(" ");
        folded_text_line(h->text);
        newline();
    } else if (const auto* bl = std::get_if<BulletedListItemBlock>(&content)) {
        note_text_constructs(bl->text, 0, report_);
        content_cols = indent_cols + 2;
        const std::vector<std::string_view> lines = split_lines(bl->text);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            indent(i == 0 ? indent_cols : content_cols);
            if (i == 0) { raw("- "); }
            text_line(lines[i]);
            newline();
        }
    } else if (const auto* nl = std::get_if<NumberedListItemBlock>(&content)) {
        note_text_constructs(nl->text, 0, report_);
        // Numbering is DERIVED from sibling position (core/blocks/block.h), so
        // it comes from the caller's counter, never from the payload.
        const std::string marker = std::to_string(ordinal) + ". ";
        content_cols             = indent_cols + marker.size();
        const std::vector<std::string_view> lines = split_lines(nl->text);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            indent(i == 0 ? indent_cols : content_cols);
            if (i == 0) { raw(marker); }
            text_line(lines[i]);
            newline();
        }
    } else if (const auto* cl = std::get_if<ChecklistItemBlock>(&content)) {
        note_text_constructs(cl->text, 0, report_);
        // The checkbox is CONTENT of a two-column bullet marker, not part of
        // the marker, so children indent by 2 and not by 6. That is what
        // parse_list_marker() will measure on the way back in.
        content_cols                              = indent_cols + 2;
        const std::vector<std::string_view> lines = split_lines(cl->text);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            indent(i == 0 ? indent_cols : content_cols);
            if (i == 0) { raw(cl->checked ? "- [x] " : "- [ ] "); }
            text_line(lines[i]);
            newline();
        }
    } else if (const auto* cb = std::get_if<CodeBlock>(&content)) {
        // A backtick in the info string cannot be carried by a backtick fence,
        // so the fence switches to '~', which can. That makes the case
        // lossless rather than truncated (markdown.h, CODE INFO STRING).
        const char fence_char =
            cb->language.find('`') != std::string::npos ? '~' : '`';
        std::size_t fence_len = longest_fence_run(cb->code, fence_char) + 1;
        if (fence_len < 3) { fence_len = 3; }

        if (detail::has_whitespace(cb->language)) {
            report_.note(Fidelity::Degraded, kCodeLanguageTrunc, 0, kMsgCodeLanguageTrunc);
        }

        indent(indent_cols);
        raw(std::string(fence_len, fence_char));
        raw(cb->language);
        newline();

        // Empty code is ZERO content lines, not one empty line: the reader
        // joins the lines it finds with '\n', so one empty line would read
        // back as "" either way -- but zero lines is what the symmetry
        // argument needs for the non-empty cases to line up.
        if (!cb->code.empty()) {
            for (const std::string_view line : split_lines(cb->code)) {
                // Spaces and tabs inside a fence are CONTENT and are written
                // verbatim; a trailing '\r' is the one byte that cannot
                // survive the round trip, so it is removed and reported.
                const std::string_view emit = trim_trailing_cr(line);
                if (emit.size() != line.size()) {
                    report_.note(Fidelity::Degraded, kTextLineWhitespace, 0,
                                 kMsgTextLineWhitespace);
                }
                if (!emit.empty()) { indent(indent_cols); }
                raw(emit);
                newline();
            }
        }

        indent(indent_cols);
        raw(std::string(fence_len, fence_char));
        newline();
    } else if (std::get_if<DividerBlock>(&content) != nullptr) {
        indent(indent_cols);
        raw("---");
        newline();
    } else if (const auto* q = std::get_if<QuoteBlock>(&content)) {
        note_text_constructs(q->text, 0, report_);
        for (const std::string_view line : split_lines(q->text)) {
            indent(indent_cols);
            raw("> ");
            text_line(line);
            newline();
        }
    }

    if (b.children.empty()) { return; }

    if (is_list_family(type)) {
        // A paragraph child's first line is plain text, which the reader would
        // otherwise absorb as a continuation of the item's own text. Every
        // other block type starts with a line looks_like_block_start() stops
        // on, so no separator is needed and the list stays tight.
        if (blocks::block_type_of(b.children.front().content) == BlockType::Paragraph) {
            newline();
        }
        write_level(b.children, content_cols);
    } else {
        report_.note(Fidelity::Approximated, kNestingFlattened, 0, kMsgNestingFlattened);
        newline();
        write_level(b.children, indent_cols);
    }
}

void Writer::write_level(const std::vector<Block>& level, std::size_t indent_cols) {
    std::size_t ordinal       = 0;
    bool        first         = true;
    BlockType   previous      = BlockType::Paragraph;
    bool        prev_children = false;

    for (const Block& b : level) {
        if (overflow_) { return; }
        const BlockType type = blocks::block_type_of(b.content);

        if (!first) {
            // A blank line separates every pair EXCEPT two list items with
            // nothing nested between them: that pair is one tight list, and a
            // blank line there would also be read back correctly but would
            // make every exported list loose.
            const bool tight = is_list_family(previous) && is_list_family(type) && !prev_children;
            if (!tight) { newline(); }
        }

        ordinal = (type == BlockType::NumberedListItem) ? ordinal + 1 : 0;

        write_block(b, indent_cols, ordinal);

        previous      = type;
        prev_children = !b.children.empty();
        first         = false;
    }
}

// ===========================================================================
// READER
// ===========================================================================

struct Line final {
    std::string_view raw;
    std::size_t      number = 0;  ///< 1-based, for the fidelity report
};

/// Strip `base` COLUMNS of leading whitespace. `pad_out` is the overshoot a
/// tab straddling the boundary leaves behind, which the code-block reader
/// re-materialises as spaces so that indentation inside a tab-indented fenced
/// block is not silently lost.
[[nodiscard]] std::string_view line_body(std::string_view raw, std::size_t base,
                                         std::size_t& pad_out) noexcept {
    std::size_t cols  = 0;
    std::size_t bytes = 0;
    while (bytes < raw.size() && cols < base) {
        if (raw[bytes] == ' ') {
            cols += 1;
        } else if (raw[bytes] == '\t') {
            cols += detail::kTabWidth - (cols % detail::kTabWidth);
        } else {
            break;
        }
        ++bytes;
    }
    pad_out = (cols > base) ? cols - base : 0;
    return raw.substr(bytes);
}

class Reader final {
public:
    Reader(const std::vector<Line>& lines, IdSource& ids, FidelityReport& report,
           const PageId& page_id, std::int64_t now_ms) noexcept
        : lines_(lines), ids_(ids), report_(report), page_id_(page_id), now_ms_(now_ms) {}

    [[nodiscard]] Status parse_region(std::size_t begin, std::size_t end, std::size_t base_cols,
                                      std::size_t depth, const BlockId& parent,
                                      std::vector<Block>& out);

private:
    [[nodiscard]] Status make_block(const BlockId& parent, BlockContent&& content, Block& out);

    [[nodiscard]] bool blank(std::size_t i) const noexcept {
        return detail::trim(lines_[i].raw).empty();
    }

    [[nodiscard]] std::size_t absolute_indent(std::size_t i) const noexcept {
        std::size_t bytes = 0;
        return detail::measure_indent(lines_[i].raw, bytes);
    }

    const std::vector<Line>& lines_;
    IdSource&                ids_;
    FidelityReport&          report_;
    PageId                   page_id_;
    std::int64_t             now_ms_     = 0;
    std::size_t              block_count_ = 0;
};

Status Reader::make_block(const BlockId& parent, BlockContent&& content, Block& out) {
    ++block_count_;
    if (block_count_ > blocks::kMaxBlocksPerPage) { return Status::PageBlockLimitExceeded; }
    if (const Status s = blocks::validate_block_content(content); !is_ok(s)) { return s; }

    ObjectIdBytes raw{};
    if (const Status s = ids_.generate(raw); !is_ok(s)) { return s; }
    const BlockId id(raw);
    if (id.is_nil()) { return Status::Internal; }

    out.id             = id;
    out.parent_id      = parent;
    out.page_id        = page_id_;
    out.content        = std::move(content);
    out.created_at_ms  = now_ms_;
    out.modified_at_ms = now_ms_;
    return Status::Ok;
}

Status Reader::parse_region(std::size_t begin, std::size_t end, std::size_t base_cols,
                            std::size_t depth, const BlockId& parent,
                            std::vector<Block>& out) {
    // Checked on ENTRY, before any recursion, so a file with ten thousand
    // levels of indentation costs 64 frames and one Status rather than a
    // stack overflow (blocks::kMaxBlockDepth's own rationale).
    if (depth > blocks::kMaxBlockDepth) { return Status::BlockDepthExceeded; }

    std::size_t i = begin;
    while (i < end) {
        if (blank(i)) {
            ++i;
            continue;
        }

        std::size_t            pad  = 0;
        const std::string_view body = line_body(lines_[i].raw, base_cols, pad);
        std::size_t            nb   = 0;
        const std::size_t      rel  = pad + detail::measure_indent(body, nb);
        const std::string_view t    = detail::trim(body);
        const std::size_t      lineno = lines_[i].number;

        char              fence_char = '\0';
        const std::size_t fence_len  = (rel < 4) ? detail::fence_run(t, fence_char) : 0;
        const std::size_t heading_run = (rel < 4) ? detail::atx_heading_run(t) : 0;
        ListMarker        marker;
        const bool        is_item =
            (rel < 4) && !detail::is_thematic_break(t) && detail::parse_list_marker(t, marker);

        if (rel >= 4) {
            // markdown.h, "indented code blocks": paragraph text, reported,
            // never a code block.
            report_.note(Fidelity::Approximated, kIndentedCodeBlock, lineno,
                         kMsgIndentedCodeBlock);
            std::string text;
            std::size_t j = i;
            while (j < end && !blank(j)) {
                std::size_t            p2 = 0;
                const std::string_view b2 = line_body(lines_[j].raw, base_cols, p2);
                if (j != i) { text.push_back('\n'); }
                text.append(detail::unescape_text_line(detail::trim(b2)));
                ++j;
            }
            note_text_constructs(text, lineno, report_);
            Block block;
            if (const Status s = make_block(parent, ParagraphBlock{std::move(text)}, block);
                !is_ok(s)) {
                return s;
            }
            out.push_back(std::move(block));
            i = j;
        } else if (fence_len > 0) {
            const std::string_view info = detail::trim(t.substr(fence_len));
            std::string            language;
            if (!info.empty()) {
                std::size_t word = 0;
                while (word < info.size() && !is_space_or_tab(info[word])) { ++word; }
                if (word < info.size()) {
                    report_.note(Fidelity::Degraded, kCodeLanguageTrunc, lineno,
                                 kMsgCodeLanguageTrunc);
                }
                language.assign(info.substr(0, word));
            }

            std::string code;
            bool        first_code_line = true;
            std::size_t j               = i + 1;
            while (j < end) {
                std::size_t            p2  = 0;
                const std::string_view b2  = line_body(lines_[j].raw, base_cols, p2);
                std::size_t            nb2 = 0;
                const std::size_t      rel2 = p2 + detail::measure_indent(b2, nb2);
                const std::string_view t2   = detail::trim(b2);
                char                   fc2  = '\0';
                const std::size_t      run2 = detail::fence_run(t2, fc2);
                if (rel2 < 4 && run2 >= fence_len && fc2 == fence_char &&
                    detail::trim(t2.substr(run2)).empty()) {
                    ++j;
                    break;
                }
                if (!first_code_line) { code.push_back('\n'); }
                first_code_line = false;
                code.append(p2, ' ');
                code.append(b2);
                ++j;
            }

            Block block;
            if (const Status s = make_block(
                    parent, CodeBlock{std::move(code), std::move(language)}, block);
                !is_ok(s)) {
                return s;
            }
            out.push_back(std::move(block));
            i = j;
        } else if (detail::is_thematic_break(t)) {
            // markdown.h, "setext headings": `---` under a paragraph is a
            // THEMATIC BREAK here, which is what this module's own exporter
            // means by it. Reached only after the paragraph branch has already
            // stopped on this line.
            Block block;
            if (const Status s = make_block(parent, DividerBlock{}, block); !is_ok(s)) {
                return s;
            }
            out.push_back(std::move(block));
            ++i;
        } else if (heading_run > 0) {
            int level = static_cast<int>(heading_run);
            if (level > blocks::kMaxHeadingLevel) {
                report_.note(Fidelity::Approximated, kHeadingLevelClamped, lineno,
                             kMsgHeadingLevelClamped);
                level = blocks::kMaxHeadingLevel;
            }
            std::string text =
                detail::unescape_text_line(detail::trim(t.substr(heading_run)));
            note_text_constructs(text, lineno, report_);
            Block block;
            if (const Status s = make_block(parent, HeadingBlock{std::move(text), level}, block);
                !is_ok(s)) {
                return s;
            }
            out.push_back(std::move(block));
            ++i;
        } else if (t[0] == '>') {
            std::string text;
            std::size_t j = i;
            while (j < end && !blank(j)) {
                std::size_t            p2   = 0;
                const std::string_view b2   = line_body(lines_[j].raw, base_cols, p2);
                std::size_t            nb2  = 0;
                const std::size_t      rel2 = p2 + detail::measure_indent(b2, nb2);
                std::string_view       t2   = detail::trim(b2);
                if (rel2 >= 4 || t2.empty() || t2[0] != '>') { break; }
                t2.remove_prefix(1);
                if (!t2.empty() && is_space_or_tab(t2[0])) { t2.remove_prefix(1); }
                if (j != i) { text.push_back('\n'); }
                text.append(detail::unescape_text_line(detail::trim(t2)));
                ++j;
            }
            note_text_constructs(text, lineno, report_);
            Block block;
            if (const Status s = make_block(parent, QuoteBlock{std::move(text)}, block);
                !is_ok(s)) {
                return s;
            }
            out.push_back(std::move(block));
            i = j;
        } else if (is_item) {
            const std::string_view after = t.substr(marker.width);
            const std::size_t marker_col = base_cols + rel;
            const std::size_t content_cols =
                marker_col + column_width(t.substr(0, marker.width), marker_col);

            bool             checked        = false;
            std::size_t      checkbox_width = 0;
            const bool       is_check       = (marker.kind == ListKind::Bullet) &&
                                  detail::parse_checkbox(after, checked, checkbox_width);
            const std::string_view head = is_check ? after.substr(checkbox_width) : after;

            std::string text = detail::unescape_text_line(detail::trim(head));

            // Lazy continuation of the item's OWN text: indented to the
            // content column, non-blank, and not the start of another block.
            std::size_t j = i + 1;
            while (j < end && !blank(j) && absolute_indent(j) >= content_cols) {
                std::size_t            p2 = 0;
                const std::string_view b2 = line_body(lines_[j].raw, content_cols, p2);
                const std::string_view t2 = detail::trim(b2);
                if (detail::looks_like_block_start(t2)) { break; }
                text.push_back('\n');
                text.append(detail::unescape_text_line(t2));
                ++j;
            }

            // Everything else still inside the item's indented region is the
            // item's CHILDREN, parsed as a sub-document.
            std::size_t k = j;
            while (k < end && (blank(k) || absolute_indent(k) >= content_cols)) { ++k; }

            // Trailing blank lines are part of the region to SKIP but not part
            // of the child sub-document. Recursing into a range of nothing but
            // blank lines would cost a depth level for no block and would fail
            // a file nested exactly kMaxBlockDepth deep at its last item.
            std::size_t child_end = k;
            while (child_end > j && blank(child_end - 1)) { --child_end; }

            note_text_constructs(text, lineno, report_);

            BlockContent content;
            if (is_check) {
                content = ChecklistItemBlock{std::move(text), checked};
            } else if (marker.kind == ListKind::Ordered) {
                content = NumberedListItemBlock{std::move(text)};
            } else {
                content = BulletedListItemBlock{std::move(text)};
            }

            Block item;
            if (const Status s = make_block(parent, std::move(content), item); !is_ok(s)) {
                return s;
            }
            if (child_end > j) {
                if (const Status s = parse_region(j, child_end, content_cols, depth + 1,
                                                  item.id, item.children);
                    !is_ok(s)) {
                    return s;
                }
            }
            out.push_back(std::move(item));
            i = k;
        } else {
            std::string text;
            std::size_t j = i;
            while (j < end && !blank(j)) {
                std::size_t            p2 = 0;
                const std::string_view b2 = line_body(lines_[j].raw, base_cols, p2);
                const std::string_view t2 = detail::trim(b2);
                if (j != i && detail::looks_like_block_start(t2)) { break; }

                if (j != i && is_setext_underline(t2)) {
                    report_.note(Fidelity::Approximated, kSetextHeading, lines_[j].number,
                                 kMsgSetextHeading);
                }
                if (is_link_reference_definition(t2) || is_html_block_start(t2) ||
                    is_block_equation(t2)) {
                    report_.note(Fidelity::PreservedNote, kUnmodelledBlock, lines_[j].number,
                                 kMsgUnmodelledBlock);
                }

                if (j != i) { text.push_back('\n'); }
                text.append(detail::unescape_text_line(t2));
                ++j;
            }
            note_text_constructs(text, lineno, report_);
            Block block;
            if (const Status s = make_block(parent, ParagraphBlock{std::move(text)}, block);
                !is_ok(s)) {
                return s;
            }
            out.push_back(std::move(block));
            i = j;
        }
    }

    return Status::Ok;
}

}  // namespace

// ===========================================================================
// PUBLIC ENTRY POINTS
// ===========================================================================

Status page_to_markdown(const blocks::Page& page, const MarkdownWriteOptions& options,
                        std::string& out, FidelityReport& report) {
    out.clear();

    // The export path validates its input for the same reason the store's
    // read path does: a page may have arrived from a database file or a sync
    // peer (markdown.h).
    if (const Status s = blocks::validate_page_metadata(page); !is_ok(s)) { return s; }
    if (const Status s = blocks::validate_block_tree(page.id, page.blocks); !is_ok(s)) {
        return s;
    }

    Writer writer(out, report);

    if (options.title_as_heading && !page.title.empty()) {
        note_text_constructs(page.title, 0, report);
        writer.raw("# ");
        writer.folded_text_line(page.title);
        writer.newline();
        if (!page.blocks.empty()) { writer.newline(); }
    }

    writer.write_level(page.blocks, 0);

    if (writer.overflowed()) {
        out.clear();
        return Status::ConversionOutputTooLarge;
    }
    return Status::Ok;
}

Status markdown_to_page(std::string_view markdown, const MarkdownReadOptions& options,
                        Clock& clock, IdSource& ids, blocks::Page& out,
                        FidelityReport& report) {
    if (markdown.size() > kMaxMarkdownInputBytes) { return Status::ConversionInputTooLarge; }
    // An embedded NUL would truncate at the first C ABI boundary, exactly as
    // blocks::validate_block_content rejects one.
    if (markdown.find('\0') != std::string_view::npos) {
        return Status::ConversionInputMalformed;
    }

    std::vector<Line> lines;
    {
        std::size_t pos    = 0;
        std::size_t number = 1;
        for (;;) {
            const std::size_t nl = markdown.find('\n', pos);
            std::string_view  raw =
                (nl == std::string_view::npos) ? markdown.substr(pos)
                                               : markdown.substr(pos, nl - pos);
            // CRLF input is normalised here and nowhere else.
            if (!raw.empty() && raw.back() == '\r') { raw.remove_suffix(1); }
            lines.push_back(Line{raw, number});
            ++number;
            if (nl == std::string_view::npos) { break; }
            pos = nl + 1;
        }
    }

    Page page;
    {
        ObjectIdBytes raw{};
        if (const Status s = ids.generate(raw); !is_ok(s)) { return s; }
        const PageId id(raw);
        if (id.is_nil()) { return Status::Internal; }
        page.id = id;
    }
    page.created_at_ms  = clock.now_unix_ms();
    page.modified_at_ms = page.created_at_ms;
    page.title.assign(options.default_title);

    Reader reader(lines, ids, report, page.id, page.created_at_ms);
    if (const Status s =
            reader.parse_region(0, lines.size(), 0, 1, BlockId{}, page.blocks);
        !is_ok(s)) {
        return s;
    }

    if (options.leading_heading_as_title && !page.blocks.empty()) {
        const auto* h = std::get_if<HeadingBlock>(&page.blocks.front().content);
        // A heading too long to BE a title stays a heading rather than failing
        // the whole import: the title is a convenience, the block is content.
        if (h != nullptr && h->level == 1 && is_ok(blocks::validate_page_title(h->text))) {
            page.title = h->text;
            page.blocks.erase(page.blocks.begin());
        }
    }

    if (const Status s = blocks::validate_page_metadata(page); !is_ok(s)) { return s; }
    if (const Status s = blocks::validate_block_tree(page.id, page.blocks); !is_ok(s)) {
        return s;
    }

    out = std::move(page);
    return Status::Ok;
}

}  // namespace uapp::conversion
