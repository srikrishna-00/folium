// SPDX-License-Identifier: Apache-2.0
//
// core/conversion/src/markdown_syntax.h — INTERNAL. Not under include/.
//
// The lexical layer shared by the writer and the reader. It lives in one place
// for a reason that is the whole correctness argument of this module: the
// writer's escape and the reader's unescape MUST be exact inverses, and the
// writer's "would this line be mis-parsed?" test MUST agree with the reader's
// block classifier. Two copies of either pair would drift and the drift would
// show up as silent data corruption on round-trip, not as a build failure.
//
// Everything here is a pure function of one line of text. No allocation except
// where a std::string is returned.

#ifndef UAPP_CORE_CONVERSION_MARKDOWN_SYNTAX_H
#define UAPP_CORE_CONVERSION_MARKDOWN_SYNTAX_H

#include <cstddef>
#include <string>
#include <string_view>

namespace uapp::conversion::detail {

/// Tab stop used when measuring a line's indentation. CommonMark expands tabs
/// to a four-space tab stop for block-structure purposes; we only ever need
/// the COUNT, never the expanded text, so this does not allocate.
inline constexpr std::size_t kTabWidth = 4;

/// Number of leading spaces of `raw`, counting a tab as advancing to the next
/// multiple of kTabWidth. Also reports how many bytes of `raw` that covered.
[[nodiscard]] std::size_t measure_indent(std::string_view raw, std::size_t& bytes_out) noexcept;

/// Strip leading and trailing spaces and tabs.
[[nodiscard]] std::string_view trim(std::string_view s) noexcept;

/// Strip trailing spaces and tabs only.
[[nodiscard]] std::string_view trim_right(std::string_view s) noexcept;

// --- block-start classifiers (all take an ALREADY-TRIMMED line) ------------

/// Length of the leading '#' run iff `t` is an ATX heading (1..6 hashes
/// followed by a space, a tab, or end of line). 0 otherwise.
[[nodiscard]] std::size_t atx_heading_run(std::string_view t) noexcept;

/// True iff `t` is a thematic break: three or more of '-', '*' or '_', all the
/// same, with only spaces and tabs between them.
[[nodiscard]] bool is_thematic_break(std::string_view t) noexcept;

/// Length of the leading code-fence run (>= 3 of '`' or '~'), with the fence
/// character written to `fence_char`. 0 if `t` does not open or close a fence.
[[nodiscard]] std::size_t fence_run(std::string_view t, char& fence_char) noexcept;

enum class ListKind { Bullet, Ordered };

struct ListMarker final {
    ListKind    kind  = ListKind::Bullet;
    /// Bytes of `t` the marker occupies, INCLUDING the spaces after it, i.e.
    /// the column the item's content starts at relative to the marker.
    std::size_t width = 0;
};

/// True iff `t` starts with a list-item marker ("- ", "* ", "+ ", "1. ",
/// "7) ", or any of those alone on the line).
///
/// Up to four spaces after the delimiter are absorbed into the marker, which
/// is CommonMark's rule and is what makes `-   deeply spaced` a list item
/// whose content indent is 4 rather than a list item starting with spaces.
[[nodiscard]] bool parse_list_marker(std::string_view t, ListMarker& out) noexcept;

/// True iff `t` starts with a GFM task-list checkbox ("[ ]", "[x]" or "[X]"
/// followed by a space, a tab, or end of line). `checked` and `width` are
/// written only when it does.
[[nodiscard]] bool parse_checkbox(std::string_view t, bool& checked,
                                  std::size_t& width) noexcept;

/// True iff `t` would be read back as something other than running text:
/// a heading, a thematic break, a fence, a block quote, a list item, or a
/// task-list checkbox. This is the writer's "must escape" predicate and the
/// reader's "stop collecting this paragraph" predicate, and it has to be one
/// function for them to agree.
[[nodiscard]] bool looks_like_block_start(std::string_view t) noexcept;

// --- the escape pair -------------------------------------------------------
//
// INVARIANT, asserted by the round-trip tests for every text-bearing block:
//
//     unescape_text_line(escape_text_line(t)) == t     for every trimmed t
//
// The escape is CommonMark's own backslash escape, inserted in exactly one of
// two positions: before the first character, or (for an ordered-list shape
// like "1. ") before the '.' or ')' delimiter, because CommonMark has no way
// to escape a digit and `\1. x` would render as a literal backslash.
//
// `unescape_text_line` is the inverse and is defined on EVERY input, not only
// on escaper output: the writer protects any line that the unescaper would
// otherwise change, so the pair stays invertible on text that already
// contains backslashes.

[[nodiscard]] std::string escape_text_line(std::string_view trimmed);
[[nodiscard]] std::string unescape_text_line(std::string_view trimmed);

/// Longest run of `c` appearing at the start of any line of `text`. Used to
/// size a code fence so that it cannot be closed by its own content.
[[nodiscard]] std::size_t longest_leading_run(std::string_view text, char c) noexcept;

/// True iff `s` contains a space or a tab.
[[nodiscard]] bool has_whitespace(std::string_view s) noexcept;

}  // namespace uapp::conversion::detail

#endif  // UAPP_CORE_CONVERSION_MARKDOWN_SYNTAX_H
