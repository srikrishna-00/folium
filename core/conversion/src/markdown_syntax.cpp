// SPDX-License-Identifier: Apache-2.0
//
// core/conversion/src/markdown_syntax.cpp — the shared lexical layer.
//
// Every function here is a pure function of ONE line of text. The two that
// matter most are at the bottom: `escape_text_line` / `unescape_text_line`,
// which carry the invariant the whole module's round-trip rests on
//
//     unescape_text_line(escape_text_line(t)) == t
//
// and which are implemented here so that there is exactly one escaper and
// exactly one unescaper in the build. See markdown_syntax.h for why.

#include "markdown_syntax.h"

namespace uapp::conversion::detail {
namespace {

constexpr bool is_space_or_tab(char c) noexcept { return c == ' ' || c == '\t'; }

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

/// CommonMark caps an ordered-list marker at nine digits. Without the cap a
/// line of twenty digits followed by a dot would become a list item, which is
/// both wrong and a way to make the writer escape text it does not need to.
constexpr std::size_t kMaxOrderedMarkerDigits = 9;

/// Up to four spaces after a list delimiter belong to the marker
/// (markdown_syntax.h, parse_list_marker). A fifth would start an indented
/// code block inside the item, so it is content, not marker.
constexpr std::size_t kMaxMarkerPadding = 4;

[[nodiscard]] std::size_t digit_run(std::string_view t) noexcept {
    std::size_t i = 0;
    while (i < t.size() && is_digit(t[i])) { ++i; }
    return i;
}

/// Leading space/tab BYTES of `t`, at most `limit`.
[[nodiscard]] std::size_t absorb_padding(std::string_view t, std::size_t limit) noexcept {
    std::size_t i = 0;
    while (i < t.size() && i < limit && is_space_or_tab(t[i])) { ++i; }
    return i;
}

/// Leading run of `c` in `t`.
[[nodiscard]] std::size_t char_run(std::string_view t, char c) noexcept {
    std::size_t i = 0;
    while (i < t.size() && t[i] == c) { ++i; }
    return i;
}

/// The ordered-list delimiter position of `t`, or npos.
///
/// Shared by the marker parser and the escaper, because the escaper's second
/// insertion position is defined as "before this byte" and the two must not
/// disagree about where that is.
[[nodiscard]] std::size_t ordered_delimiter_pos(std::string_view t) noexcept {
    const std::size_t digits = digit_run(t);
    if (digits == 0 || digits > kMaxOrderedMarkerDigits || digits >= t.size()) {
        return std::string_view::npos;
    }
    const char delim = t[digits];
    if (delim != '.' && delim != ')') { return std::string_view::npos; }
    return digits;
}

}  // namespace

std::size_t measure_indent(std::string_view raw, std::size_t& bytes_out) noexcept {
    std::size_t cols  = 0;
    std::size_t bytes = 0;
    while (bytes < raw.size()) {
        if (raw[bytes] == ' ') {
            cols += 1;
        } else if (raw[bytes] == '\t') {
            cols += kTabWidth - (cols % kTabWidth);
        } else {
            break;
        }
        ++bytes;
    }
    bytes_out = bytes;
    return cols;
}

std::string_view trim_right(std::string_view s) noexcept {
    std::size_t end = s.size();
    while (end > 0 && is_space_or_tab(s[end - 1])) { --end; }
    return s.substr(0, end);
}

std::string_view trim(std::string_view s) noexcept {
    std::size_t begin = 0;
    while (begin < s.size() && is_space_or_tab(s[begin])) { ++begin; }
    return trim_right(s.substr(begin));
}

std::size_t atx_heading_run(std::string_view t) noexcept {
    const std::size_t run = char_run(t, '#');
    if (run == 0 || run > 6) { return 0; }
    if (run == t.size() || is_space_or_tab(t[run])) { return run; }
    return 0;
}

bool is_thematic_break(std::string_view t) noexcept {
    if (t.empty()) { return false; }
    const char c = t[0];
    if (c != '-' && c != '*' && c != '_') { return false; }
    std::size_t marks = 0;
    for (const char ch : t) {
        if (ch == c) {
            ++marks;
        } else if (!is_space_or_tab(ch)) {
            return false;
        }
    }
    return marks >= 3;
}

std::size_t fence_run(std::string_view t, char& fence_char) noexcept {
    if (t.empty()) { return 0; }
    const char c = t[0];
    if (c != '`' && c != '~') { return 0; }
    const std::size_t run = char_run(t, c);
    if (run < 3) { return 0; }
    fence_char = c;
    return run;
}

bool parse_list_marker(std::string_view t, ListMarker& out) noexcept {
    if (t.empty()) { return false; }

    // A thematic break is NOT a list item, even though "- - -" has the shape
    // of one. The reader classifies thematic breaks first; this guard keeps
    // the two answers from depending on that ordering.
    if (is_thematic_break(t)) { return false; }

    const char c = t[0];
    if (c == '-' || c == '*' || c == '+') {
        if (t.size() == 1) {
            out.kind  = ListKind::Bullet;
            out.width = 1;
            return true;
        }
        if (!is_space_or_tab(t[1])) { return false; }
        out.kind  = ListKind::Bullet;
        out.width = 1 + absorb_padding(t.substr(1), kMaxMarkerPadding);
        return true;
    }

    const std::size_t delim = ordered_delimiter_pos(t);
    if (delim == std::string_view::npos) { return false; }
    const std::size_t after = delim + 1;
    if (after != t.size() && !is_space_or_tab(t[after])) { return false; }
    out.kind  = ListKind::Ordered;
    out.width = after + absorb_padding(t.substr(after), kMaxMarkerPadding);
    return true;
}

bool parse_checkbox(std::string_view t, bool& checked, std::size_t& width) noexcept {
    if (t.size() < 3 || t[0] != '[' || t[2] != ']') { return false; }
    const char mark = t[1];
    const bool is_checked = (mark == 'x' || mark == 'X');
    if (mark != ' ' && !is_checked) { return false; }
    if (t.size() > 3 && !is_space_or_tab(t[3])) { return false; }
    checked = is_checked;
    width   = 3 + absorb_padding(t.substr(3), kMaxMarkerPadding);
    return true;
}

bool looks_like_block_start(std::string_view t) noexcept {
    if (t.empty()) { return false; }
    if (atx_heading_run(t) > 0) { return true; }
    if (is_thematic_break(t)) { return true; }
    char fence = '\0';
    if (fence_run(t, fence) > 0) { return true; }
    if (t[0] == '>') { return true; }
    ListMarker marker;
    if (parse_list_marker(t, marker)) { return true; }
    bool        checked = false;
    std::size_t width   = 0;
    if (parse_checkbox(t, checked, width)) { return true; }
    return false;
}

// --- the escape pair -------------------------------------------------------

std::string unescape_text_line(std::string_view trimmed) {
    // Position 1: a backslash before the first character. Note the absence of
    // a size guard — unescape("\\") is "", which is what makes an EMPTY text
    // line representable (see escape_text_line).
    if (!trimmed.empty() && trimmed[0] == '\\') {
        return std::string(trimmed.substr(1));
    }

    // Position 2: a backslash before an ordered-list delimiter. Reachable
    // only when the line begins with digits, so it cannot collide with
    // position 1.
    const std::size_t digits = digit_run(trimmed);
    if (digits >= 1 && digits <= kMaxOrderedMarkerDigits && digits + 1 < trimmed.size() &&
        trimmed[digits] == '\\' &&
        (trimmed[digits + 1] == '.' || trimmed[digits + 1] == ')')) {
        std::string out;
        out.reserve(trimmed.size() - 1);
        out.append(trimmed.substr(0, digits));
        out.append(trimmed.substr(digits + 1));
        return out;
    }

    return std::string(trimmed);
}

std::string escape_text_line(std::string_view trimmed) {
    // Three reasons a line needs protecting, and the third is the one that
    // makes the pair invertible on text that already contains backslashes:
    //
    //  1. It is EMPTY. A blank line ends a block in CommonMark, so an empty
    //     line inside a block's text would split the block in two on the way
    //     back in. `\` is the shortest line that unescapes to "".
    //  2. The reader would classify it as a different block.
    //  3. The unescaper would otherwise change it.
    //
    // The fourth clause is not about Markdown at all: a line with leading or
    // trailing whitespace survives the escape/unescape pair exactly, even
    // though the reader's own trim will not preserve it (markdown.h,
    // "LEADING / TRAILING WHITESPACE IN TEXT"). Keeping the pure functions
    // total over every input, not just over trimmed input, is what lets the
    // invariant be tested with generated adversarial strings.
    const bool needs = trimmed.empty() || looks_like_block_start(trimmed) ||
                       unescape_text_line(trimmed) != trimmed || trim(trimmed) != trimmed;
    if (!needs) { return std::string(trimmed); }

    // An ordered-list shape is escaped at its delimiter, because CommonMark
    // has no escape for a digit: `\1. x` renders a literal backslash.
    ListMarker marker;
    if (trimmed[0] != '\\' && parse_list_marker(trimmed, marker) &&
        marker.kind == ListKind::Ordered) {
        const std::size_t delim = ordered_delimiter_pos(trimmed);
        if (delim != std::string_view::npos) {
            std::string out;
            out.reserve(trimmed.size() + 1);
            out.append(trimmed.substr(0, delim));
            out.push_back('\\');
            out.append(trimmed.substr(delim));
            return out;
        }
    }

    std::string out;
    out.reserve(trimmed.size() + 1);
    out.push_back('\\');
    out.append(trimmed);
    return out;
}

std::size_t longest_leading_run(std::string_view text, char c) noexcept {
    std::size_t longest = 0;
    std::size_t pos     = 0;
    while (pos <= text.size()) {
        const std::size_t run = char_run(text.substr(pos), c);
        if (run > longest) { longest = run; }
        const std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) { break; }
        pos = nl + 1;
    }
    return longest;
}

bool has_whitespace(std::string_view s) noexcept {
    for (const char c : s) {
        if (is_space_or_tab(c)) { return true; }
    }
    return false;
}

}  // namespace uapp::conversion::detail
