// SPDX-License-Identifier: Apache-2.0
//
// tests/conversion/markdown_syntax_test.cpp — the lexical layer, and above all
// the ESCAPE INVARIANT.
//
// This file is deliberately first and deliberately standalone. Every other
// test in this suite assumes
//
//     unescape_text_line(escape_text_line(t)) == t
//
// so if that pair is wrong, every round-trip test is testing a coincidence.
// It is checked here directly: on hand-picked adversarial strings chosen one
// per rule of the escaper, and then on ~41 000 GENERATED strings over the
// alphabet of every character that means something to the block classifiers.
//
// Two properties are asserted together, because the writer needs both and
// only one of them is the named invariant:
//
//   INVERTIBILITY   unescape(escape(t)) == t
//   SAFETY          escape(t) is neither blank nor classified as a block
//                   start, i.e. the reader will read it back as running text
//
// It includes src/markdown_syntax.h, which is NOT a public header. That is the
// point: the escaper is an implementation detail with a contract, and the
// contract is worth more test coverage than the public API that rests on it.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "markdown_syntax.h"
#include "test_framework.h"

namespace {

using uapp::conversion::detail::escape_text_line;
using uapp::conversion::detail::ListKind;
using uapp::conversion::detail::ListMarker;
using uapp::conversion::detail::looks_like_block_start;
using uapp::conversion::detail::unescape_text_line;

/// The two properties, for one input. Returns an empty string on success or a
/// description of the failure.
std::string check_pair(std::string_view t) {
    const std::string escaped = escape_text_line(t);
    const std::string back    = unescape_text_line(escaped);
    if (back != t) {
        return "invertibility: in=[" + std::string(t) + "] escaped=[" + escaped +
               "] back=[" + back + "]";
    }
    if (looks_like_block_start(escaped)) {
        return "safety: escaped line is a block start: [" + escaped + "]";
    }
    if (uapp::conversion::detail::trim(escaped).empty()) {
        return "safety: escaped line is blank: in=[" + std::string(t) + "]";
    }
    return std::string();
}

}  // namespace

// --- indentation and trimming ----------------------------------------------

UAPP_TEST("conversion.syntax: measure_indent expands tabs to a 4-column stop") {
    std::size_t bytes = 99;
    UAPP_CHECK(uapp::conversion::detail::measure_indent("x", bytes) == 0);
    UAPP_CHECK(bytes == 0);

    UAPP_CHECK(uapp::conversion::detail::measure_indent("   x", bytes) == 3);
    UAPP_CHECK(bytes == 3);

    // A tab at column 0 advances to column 4.
    UAPP_CHECK(uapp::conversion::detail::measure_indent("\tx", bytes) == 4);
    UAPP_CHECK(bytes == 1);

    // A tab at column 2 advances to column 4, not to column 6.
    UAPP_CHECK(uapp::conversion::detail::measure_indent("  \tx", bytes) == 4);
    UAPP_CHECK(bytes == 3);

    // Whitespace-only line: the whole line is indentation.
    UAPP_CHECK(uapp::conversion::detail::measure_indent("  ", bytes) == 2);
    UAPP_CHECK(bytes == 2);
}

UAPP_TEST("conversion.syntax: trim strips spaces and tabs only") {
    UAPP_CHECK(uapp::conversion::detail::trim(" \tx y\t ") == "x y");
    UAPP_CHECK(uapp::conversion::detail::trim("   ").empty());
    UAPP_CHECK(uapp::conversion::detail::trim("").empty());
    // Not a space or tab, so not trimmed by the STRUCTURE layer.
    UAPP_CHECK(uapp::conversion::detail::trim("\rx\r") == "\rx\r");
    UAPP_CHECK(uapp::conversion::detail::trim_right(" x ") == " x");
}

// --- block classifiers -----------------------------------------------------

UAPP_TEST("conversion.syntax: atx heading needs 1..6 hashes and a space or EOL") {
    using uapp::conversion::detail::atx_heading_run;
    UAPP_CHECK(atx_heading_run("# h") == 1);
    UAPP_CHECK(atx_heading_run("### h") == 3);
    UAPP_CHECK(atx_heading_run("###### h") == 6);
    UAPP_CHECK(atx_heading_run("#") == 1);       // empty heading
    UAPP_CHECK(atx_heading_run("#\tt") == 1);    // tab also delimits
    UAPP_CHECK(atx_heading_run("####### h") == 0);  // seven is not a heading
    UAPP_CHECK(atx_heading_run("#hashtag") == 0);   // no delimiter
    UAPP_CHECK(atx_heading_run("") == 0);
    UAPP_CHECK(atx_heading_run("a # b") == 0);
}

UAPP_TEST("conversion.syntax: thematic break needs three same marks") {
    using uapp::conversion::detail::is_thematic_break;
    UAPP_CHECK(is_thematic_break("---"));
    UAPP_CHECK(is_thematic_break("***"));
    UAPP_CHECK(is_thematic_break("___"));
    UAPP_CHECK(is_thematic_break("- - -"));
    UAPP_CHECK(is_thematic_break("-----"));
    UAPP_CHECK(!is_thematic_break("--"));
    UAPP_CHECK(!is_thematic_break("-*-"));
    UAPP_CHECK(!is_thematic_break("--- x"));
    UAPP_CHECK(!is_thematic_break(""));
    UAPP_CHECK(!is_thematic_break("==="));  // setext, not a break
}

UAPP_TEST("conversion.syntax: fence needs three or more backticks or tildes") {
    using uapp::conversion::detail::fence_run;
    char c = '\0';
    UAPP_CHECK(fence_run("```", c) == 3);
    UAPP_CHECK(c == '`');
    UAPP_CHECK(fence_run("~~~~ cpp", c) == 4);
    UAPP_CHECK(c == '~');
    UAPP_CHECK(fence_run("``", c) == 0);
    UAPP_CHECK(fence_run("", c) == 0);
    UAPP_CHECK(fence_run("x```", c) == 0);
}

UAPP_TEST("conversion.syntax: list markers, and up to four spaces of padding") {
    using uapp::conversion::detail::parse_list_marker;
    ListMarker m;

    UAPP_REQUIRE(parse_list_marker("- x", m));
    UAPP_CHECK(m.kind == ListKind::Bullet);
    UAPP_CHECK(m.width == 2);

    UAPP_REQUIRE(parse_list_marker("-", m));
    UAPP_CHECK(m.width == 1);

    UAPP_REQUIRE(parse_list_marker("+   deeply spaced", m));
    UAPP_CHECK(m.width == 4);  // 1 + three spaces

    UAPP_REQUIRE(parse_list_marker("-      five", m));
    UAPP_CHECK(m.width == 5);  // 1 + four absorbed; the fifth space is content

    UAPP_REQUIRE(parse_list_marker("1. x", m));
    UAPP_CHECK(m.kind == ListKind::Ordered);
    UAPP_CHECK(m.width == 3);

    UAPP_REQUIRE(parse_list_marker("12) x", m));
    UAPP_CHECK(m.kind == ListKind::Ordered);
    UAPP_CHECK(m.width == 4);

    UAPP_CHECK(!parse_list_marker("-x", m));
    UAPP_CHECK(!parse_list_marker("1.x", m));
    UAPP_CHECK(!parse_list_marker("1234567890. x", m));  // ten digits is not a marker
    UAPP_CHECK(!parse_list_marker("", m));
    // A thematic break is never a list item, whichever order the reader asks in.
    UAPP_CHECK(!parse_list_marker("---", m));
    UAPP_CHECK(!parse_list_marker("***", m));
    UAPP_CHECK(!parse_list_marker("- - -", m));
}

UAPP_TEST("conversion.syntax: checkbox parses both states and nothing else") {
    using uapp::conversion::detail::parse_checkbox;
    bool        checked = true;
    std::size_t width   = 0;

    UAPP_REQUIRE(parse_checkbox("[ ] todo", checked, width));
    UAPP_CHECK(!checked);
    UAPP_CHECK(width == 4);

    UAPP_REQUIRE(parse_checkbox("[x] done", checked, width));
    UAPP_CHECK(checked);
    UAPP_CHECK(width == 4);

    UAPP_REQUIRE(parse_checkbox("[X]", checked, width));
    UAPP_CHECK(checked);
    UAPP_CHECK(width == 3);

    UAPP_CHECK(!parse_checkbox("[y] no", checked, width));
    UAPP_CHECK(!parse_checkbox("[ ]no", checked, width));
    UAPP_CHECK(!parse_checkbox("[]", checked, width));
    UAPP_CHECK(!parse_checkbox("", checked, width));
}

UAPP_TEST("conversion.syntax: looks_like_block_start agrees with the classifiers") {
    UAPP_CHECK(looks_like_block_start("# h"));
    UAPP_CHECK(looks_like_block_start("---"));
    UAPP_CHECK(looks_like_block_start("```"));
    UAPP_CHECK(looks_like_block_start("> q"));
    UAPP_CHECK(looks_like_block_start("- b"));
    UAPP_CHECK(looks_like_block_start("1. n"));
    UAPP_CHECK(looks_like_block_start("[x] c"));
    UAPP_CHECK(!looks_like_block_start("plain text"));
    UAPP_CHECK(!looks_like_block_start("==="));          // setext: read as text
    UAPP_CHECK(!looks_like_block_start("| a | b |"));    // table: read as text
    UAPP_CHECK(!looks_like_block_start("![a](b.png)"));  // image: read as text
    UAPP_CHECK(!looks_like_block_start("\\- not a bullet"));
    UAPP_CHECK(!looks_like_block_start(""));
}

UAPP_TEST("conversion.syntax: longest_leading_run finds the worst line") {
    using uapp::conversion::detail::longest_leading_run;
    UAPP_CHECK(longest_leading_run("", '`') == 0);
    UAPP_CHECK(longest_leading_run("``x", '`') == 2);
    UAPP_CHECK(longest_leading_run("a\n````\nb", '`') == 4);
    UAPP_CHECK(longest_leading_run("a\nb`", '`') == 0);  // not at a line start
    UAPP_CHECK(longest_leading_run("~~~\n~", '~') == 3);
}

// --- THE ESCAPE INVARIANT --------------------------------------------------

UAPP_TEST("conversion.escape: hand-picked adversarial lines, one per rule") {
    // One case per reason escape_text_line protects a line, plus the shapes
    // that caught bugs while it was being written.
    const char* const cases[] = {
        // Nothing to do.
        "plain text", "a - b", "text with # hash", "1 x", "x`y",
        // Every block start the writer must neutralise.
        "# heading", "###### six", "#", "--- break", "---", "***", "___",
        "``` fence", "~~~~", "> quote", "- bullet", "* bullet", "+ bullet",
        "-", "[ ] checkbox", "[x] done",
        // Ordered-list shapes: escaped at the DELIMITER, not at the digit.
        "1. one", "9) nine", "123456789. nine digits", "1.", "1)",
        // Already-escaped and backslash-heavy text: the pair must still invert.
        "\\", "\\\\", "\\x", "\\- not a bullet", "\\# not a heading",
        "1\\. literal", "12\\) literal", "\\1. literal", "x\\", "a\\\\b",
        // The empty line, which would otherwise split the block in two.
        "",
        // Whitespace, which the pure pair preserves even though the reader
        // will trim it (markdown.h, LEADING / TRAILING WHITESPACE IN TEXT).
        " leading", "trailing ", "\tx", " ", "  ",
        // Shapes that look dangerous but are not block starts.
        "| a | b |", "|---|---|", "===", "![alt](src.png)", "[id]: http://x",
        "<div>", "$$", "2 * 3 * 4", "snake_case_name",
        // Non-ASCII must pass through untouched.
        "\xC3\xA9t\xC3\xA9", "\xE2\x9C\x93 done",
    };

    for (const char* c : cases) {
        const std::string detail = check_pair(c);
        UAPP_CHECK_MSG(detail.empty(), detail);
    }
}

UAPP_TEST("conversion.escape: the two documented escape positions, exactly") {
    // Position 1: before the first character.
    UAPP_CHECK(escape_text_line("- x") == "\\- x");
    UAPP_CHECK(escape_text_line("# x") == "\\# x");
    UAPP_CHECK(escape_text_line("> x") == "\\> x");
    UAPP_CHECK(escape_text_line("```") == "\\```");
    // Position 2: before the ordered-list delimiter, because CommonMark has
    // no escape for a digit.
    UAPP_CHECK(escape_text_line("1. x") == "1\\. x");
    UAPP_CHECK(escape_text_line("42) x") == "42\\) x");
    // An empty line becomes the one-character line that unescapes to "".
    UAPP_CHECK(escape_text_line("") == "\\");
    UAPP_CHECK(unescape_text_line("\\").empty());
    // Untouched when untouchable.
    UAPP_CHECK(escape_text_line("plain") == "plain");
    // Unescape is defined on every input, not just on escaper output.
    UAPP_CHECK(unescape_text_line("plain") == "plain");
    UAPP_CHECK(unescape_text_line("\\- x") == "- x");
    UAPP_CHECK(unescape_text_line("1\\. x") == "1. x");
    UAPP_CHECK(unescape_text_line("1\\x") == "1\\x");  // not a delimiter: untouched
    UAPP_CHECK(unescape_text_line("") == "");
}

UAPP_TEST("conversion.escape: generated adversarial strings over the whole alphabet") {
    // Every character the block classifiers or the escaper react to, plus one
    // ordinary letter and one space so that mixed lines are generated too.
    static const char kAlphabet[] = {'\\', '-', '*', '_', '#', '`', '>',
                                     '1',  '.', ')', '[', ' ', 'x', '~'};
    constexpr std::size_t kN = sizeof(kAlphabet) / sizeof(kAlphabet[0]);

    std::size_t checked  = 0;
    std::size_t failures = 0;
    std::string first_failure;

    // All strings of length 0..4 over a 14-character alphabet: 1 + 14 + 196 +
    // 2744 + 38416 = 41 371 inputs. Exhaustive beats random at this size.
    for (std::size_t len = 0; len <= 4; ++len) {
        std::vector<std::size_t> digits(len, 0);
        for (;;) {
            std::string input;
            input.reserve(len);
            for (const std::size_t d : digits) { input.push_back(kAlphabet[d]); }

            const std::string detail = check_pair(input);
            ++checked;
            if (!detail.empty()) {
                ++failures;
                if (first_failure.empty()) { first_failure = detail; }
            }

            // Odometer increment; stops when it wraps.
            std::size_t pos = 0;
            while (pos < len) {
                ++digits[pos];
                if (digits[pos] < kN) { break; }
                digits[pos] = 0;
                ++pos;
            }
            if (pos == len) { break; }
        }
    }

    UAPP_CHECK_MSG(checked == 41371, "checked " + std::to_string(checked));
    UAPP_CHECK_MSG(failures == 0,
                   std::to_string(failures) + " failures, first: " + first_failure);
}

UAPP_TEST("conversion.escape: generated strings of length 5..6 over the hot subset") {
    // Longer strings over the characters that actually interact: a backslash,
    // a digit, the two ordered delimiters, a bullet and a space.
    static const char kAlphabet[] = {'\\', '1', '.', ')', '-', ' '};
    constexpr std::size_t kN = sizeof(kAlphabet) / sizeof(kAlphabet[0]);

    std::size_t checked  = 0;
    std::size_t failures = 0;
    std::string first_failure;

    for (std::size_t len = 5; len <= 6; ++len) {
        std::vector<std::size_t> digits(len, 0);
        for (;;) {
            std::string input;
            input.reserve(len);
            for (const std::size_t d : digits) { input.push_back(kAlphabet[d]); }

            const std::string detail = check_pair(input);
            ++checked;
            if (!detail.empty()) {
                ++failures;
                if (first_failure.empty()) { first_failure = detail; }
            }

            std::size_t pos = 0;
            while (pos < len) {
                ++digits[pos];
                if (digits[pos] < kN) { break; }
                digits[pos] = 0;
                ++pos;
            }
            if (pos == len) { break; }
        }
    }

    UAPP_CHECK_MSG(checked == 7776 + 46656, "checked " + std::to_string(checked));
    UAPP_CHECK_MSG(failures == 0,
                   std::to_string(failures) + " failures, first: " + first_failure);
}
