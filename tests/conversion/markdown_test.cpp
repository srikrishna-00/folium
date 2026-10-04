// SPDX-License-Identifier: Apache-2.0
//
// tests/conversion/markdown_test.cpp — Page <-> Markdown, both directions.
//
// Organised the way the header's own contract is organised, because that is
// what is under test:
//
//   conversion.roundtrip.*  WHAT ROUND-TRIPS EXACTLY. All eight block types,
//                           nesting under list items, multi-line text, and a
//                           realistic mixed document. The strong form is
//                           asserted wherever it holds: Page -> MD -> Page ->
//                           MD gives BYTE-IDENTICAL Markdown and an IDENTICAL
//                           block tree.
//   conversion.fidelity.*   WHAT IS LOSSY. One case per documented construct,
//                           asserting the construct id, the severity AND the
//                           behaviour -- not merely that nothing crashed.
//   conversion.bounds.*     The bounded failure modes, each returning its
//                           documented Status.
//
// Deliberately NO storage and NO SQLite: this binary links uapp::core_conversion
// only, so "a conversion is a pure in-memory transformation" is a property of
// the build graph rather than a claim in a comment.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common_fixtures.h"
#include "test_framework.h"
#include "uapp/blocks/block.h"
#include "uapp/blocks/page.h"
#include "uapp/conversion/fidelity.h"
#include "uapp/conversion/markdown.h"
#include "uapp/object_id.h"
#include "uapp/ports.h"
#include "uapp/status.h"

namespace {

using uapp::Status;
using uapp::blocks::Block;
using uapp::blocks::BlockContent;
using uapp::blocks::BlockId;
using uapp::blocks::BlockType;
using uapp::blocks::BulletedListItemBlock;
using uapp::blocks::ChecklistItemBlock;
using uapp::blocks::CodeBlock;
using uapp::blocks::DividerBlock;
using uapp::blocks::HeadingBlock;
using uapp::blocks::NumberedListItemBlock;
using uapp::blocks::Page;
using uapp::blocks::PageId;
using uapp::blocks::ParagraphBlock;
using uapp::blocks::QuoteBlock;
using uapp::conversion::Fidelity;
using uapp::conversion::FidelityReport;
using uapp::conversion::kMaxMarkdownInputBytes;
using uapp::conversion::markdown_to_page;
using uapp::conversion::MarkdownReadOptions;
using uapp::conversion::MarkdownWriteOptions;
using uapp::conversion::page_to_markdown;

// --- builders --------------------------------------------------------------

Block node(BlockContent content, std::vector<Block> children) {
    Block b;
    b.content  = std::move(content);
    b.children = std::move(children);
    return b;
}

Block para(std::string text, std::vector<Block> kids = {}) {
    return node(ParagraphBlock{std::move(text)}, std::move(kids));
}
Block head(std::string text, int level, std::vector<Block> kids = {}) {
    return node(HeadingBlock{std::move(text), level}, std::move(kids));
}
Block bullet(std::string text, std::vector<Block> kids = {}) {
    return node(BulletedListItemBlock{std::move(text)}, std::move(kids));
}
Block numbered(std::string text, std::vector<Block> kids = {}) {
    return node(NumberedListItemBlock{std::move(text)}, std::move(kids));
}
Block checklist(std::string text, bool checked, std::vector<Block> kids = {}) {
    return node(ChecklistItemBlock{std::move(text), checked}, std::move(kids));
}
Block code(std::string text, std::string language, std::vector<Block> kids = {}) {
    return node(CodeBlock{std::move(text), std::move(language)}, std::move(kids));
}
Block divider(std::vector<Block> kids = {}) {
    return node(DividerBlock{}, std::move(kids));
}
Block quote(std::string text, std::vector<Block> kids = {}) {
    return node(QuoteBlock{std::move(text)}, std::move(kids));
}

/// Fill in the ids and parent/page links a valid tree needs, so that
/// page_to_markdown's own validate_block_tree() call passes. Iterative, like
/// the validator it feeds.
void link_tree(Page& page) {
    std::uint64_t counter = 0;
    struct Frame final {
        std::vector<Block>* level;
        BlockId             parent;
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{&page.blocks, BlockId{}});
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        for (Block& b : *frame.level) {
            ++counter;
            uapp::ObjectIdBytes raw{};
            raw[0] = 0xB1u;
            for (std::size_t i = 0; i < 8; ++i) {
                raw[15 - i] = static_cast<std::uint8_t>((counter >> (i * 8)) & 0xFFu);
            }
            b.id             = BlockId(raw);
            b.parent_id      = frame.parent;
            b.page_id        = page.id;
            b.created_at_ms  = 1700000000000;
            b.modified_at_ms = 1700000000000;
            if (!b.children.empty()) { stack.push_back(Frame{&b.children, b.id}); }
        }
    }
}

Page make_page(std::string title, std::vector<Block> blocks) {
    Page p;
    uapp::ObjectIdBytes raw{};
    raw[0]  = 0xA9u;
    raw[15] = 0x07u;
    p.id              = PageId(raw);
    p.title           = std::move(title);
    p.created_at_ms   = 1700000000000;
    p.modified_at_ms  = 1700000000000;
    p.blocks          = std::move(blocks);
    link_tree(p);
    return p;
}

// --- comparison and dumping ------------------------------------------------

bool same_forest(const std::vector<Block>& a, const std::vector<Block>& b) {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!(a[i].content == b[i].content)) { return false; }
        if (!same_forest(a[i].children, b[i].children)) { return false; }
    }
    return true;
}

void dump_forest(const std::vector<Block>& level, std::size_t depth, std::string& out) {
    for (const Block& b : level) {
        out.append(depth * 2, ' ');
        out.append(uapp::blocks::block_type_name(uapp::blocks::block_type_of(b.content)));
        out.append("[");
        out.append(uapp::blocks::block_plain_text(b.content));
        out.append("]\n");
        dump_forest(b.children, depth + 1, out);
    }
}

std::string dump(const Page& p) {
    std::string out = "\n  title[" + p.title + "]\n";
    dump_forest(p.blocks, 1, out);
    return out;
}

std::string visible(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (c == '\n') {
            out.append("\\n");
        } else if (c == '\t') {
            out.append("\\t");
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/// The block type of one node, for order assertions.
BlockType type_at(const std::vector<Block>& level, std::size_t i) {
    return uapp::blocks::block_type_of(level[i].content);
}

std::string_view text_at(const std::vector<Block>& level, std::size_t i) {
    return uapp::blocks::block_plain_text(level[i].content);
}

}  // namespace

// ===========================================================================
// WHAT ROUND-TRIPS EXACTLY
// ===========================================================================

UAPP_TEST("conversion.roundtrip: all eight block types survive a full cycle") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    const Page page = make_page(
        "Eight Types",
        {para("A plain paragraph."), head("Level one", 1), head("Level two", 2),
         head("Level three", 3), bullet("A bullet"), numbered("An ordered item"),
         checklist("Done thing", true), checklist("Pending thing", false),
         code("int main() {\n    return 0;\n}", "cpp"), divider(), quote("A quotation.")});

    std::string    md;
    FidelityReport wr;
    UAPP_REQUIRE_OK(page_to_markdown(page, MarkdownWriteOptions{}, md, wr));
    UAPP_CHECK_MSG(wr.empty(), visible(md));

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page(md, MarkdownReadOptions{}, clock, ids, back, rr));
    UAPP_CHECK_MSG(rr.empty(), visible(md));

    UAPP_CHECK_MSG(back.title == page.title, back.title);
    UAPP_CHECK_MSG(same_forest(page.blocks, back.blocks),
                   dump(page) + "  --- got ---" + dump(back) + "  md: " + visible(md));

    // The strong form: a second export is byte-identical, so the Markdown is a
    // fixed point and not merely "close enough".
    std::string    md2;
    FidelityReport wr2;
    UAPP_REQUIRE_OK(page_to_markdown(back, MarkdownWriteOptions{}, md2, wr2));
    UAPP_CHECK_MSG(md2 == md, visible(md) + "  !=  " + visible(md2));

    // And the Markdown really is CommonMark, not a private encoding.
    UAPP_CHECK_MSG(md.find("# Eight Types\n") == 0, visible(md));
    UAPP_CHECK_MSG(md.find("\n### Level three\n") != std::string::npos, visible(md));
    UAPP_CHECK_MSG(md.find("\n- A bullet\n") != std::string::npos, visible(md));
    UAPP_CHECK_MSG(md.find("\n1. An ordered item\n") != std::string::npos, visible(md));
    UAPP_CHECK_MSG(md.find("\n- [x] Done thing\n") != std::string::npos, visible(md));
    UAPP_CHECK_MSG(md.find("\n- [ ] Pending thing\n") != std::string::npos, visible(md));
    UAPP_CHECK_MSG(md.find("\n```cpp\n") != std::string::npos, visible(md));
    UAPP_CHECK_MSG(md.find("\n---\n") != std::string::npos, visible(md));
    UAPP_CHECK_MSG(md.find("\n> A quotation.\n") != std::string::npos, visible(md));
}

UAPP_TEST("conversion.roundtrip: multi-line text, including lines that look like blocks") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    // Every line here would be read back as a DIFFERENT BLOCK if it were not
    // escaped, plus an interior blank line, which would split the block.
    const std::string tricky =
        "first line\n"
        "- not a bullet\n"
        "1. not an ordered item\n"
        "# not a heading\n"
        "> not a quote\n"
        "``` not a fence\n"
        "---\n"
        "[ ] not a checkbox\n"
        "\n"
        "after a blank line\n"
        "\\ a literal backslash\n"
        "1\\. a literal escape";

    const Page page = make_page("", {para(tricky), quote(tricky), bullet(tricky),
                                     numbered(tricky), checklist(tricky, true)});

    std::string    md;
    FidelityReport wr;
    UAPP_REQUIRE_OK(page_to_markdown(page, MarkdownWriteOptions{}, md, wr));

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page(md, MarkdownReadOptions{}, clock, ids, back, rr));

    UAPP_REQUIRE_MSG(back.blocks.size() == 5,
                     dump(back) + "  md: " + visible(md));
    UAPP_CHECK_MSG(same_forest(page.blocks, back.blocks),
                   dump(page) + "  --- got ---" + dump(back) + "  md: " + visible(md));

    std::string    md2;
    FidelityReport wr2;
    UAPP_REQUIRE_OK(page_to_markdown(back, MarkdownWriteOptions{}, md2, wr2));
    UAPP_CHECK_MSG(md2 == md, visible(md) + "  !=  " + visible(md2));
}

UAPP_TEST("conversion.roundtrip: nested lists to several levels") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    const Page page = make_page(
        "Nested",
        {bullet("one", {bullet("one.one", {bullet("one.one.one")}), bullet("one.two")}),
         numbered("n one", {numbered("n one.one", {bullet("mixed leaf")})}),
         checklist("c one", false, {checklist("c one.one", true)})});

    std::string    md;
    FidelityReport wr;
    UAPP_REQUIRE_OK(page_to_markdown(page, MarkdownWriteOptions{}, md, wr));
    UAPP_CHECK_MSG(wr.empty(), visible(md));

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page(md, MarkdownReadOptions{}, clock, ids, back, rr));

    UAPP_CHECK_MSG(same_forest(page.blocks, back.blocks),
                   dump(page) + "  --- got ---" + dump(back) + "  md: " + visible(md));
    UAPP_CHECK(uapp::blocks::block_tree_depth(back.blocks) == 3);

    std::string    md2;
    FidelityReport wr2;
    UAPP_REQUIRE_OK(page_to_markdown(back, MarkdownWriteOptions{}, md2, wr2));
    UAPP_CHECK_MSG(md2 == md, visible(md) + "  !=  " + visible(md2));
}

UAPP_TEST("conversion.roundtrip: non-list blocks nested under a list item") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    // markdown.h: "A paragraph, heading, code block, divider or quote nested
    // under a list item therefore round-trips too, not only a nested list."
    const Page page = make_page(
        "", {bullet("the item", {para("a nested paragraph"), head("a nested heading", 2),
                                 code("nested = True", "python"), divider(),
                                 quote("a nested quote"), bullet("and a nested bullet")}),
             bullet("the next item")});

    std::string    md;
    FidelityReport wr;
    UAPP_REQUIRE_OK(page_to_markdown(page, MarkdownWriteOptions{}, md, wr));
    UAPP_CHECK_MSG(wr.count_of("nesting_flattened") == 0, visible(md));

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page(md, MarkdownReadOptions{}, clock, ids, back, rr));

    UAPP_REQUIRE_MSG(back.blocks.size() == 2, dump(back) + "  md: " + visible(md));
    UAPP_REQUIRE_MSG(back.blocks[0].children.size() == 6,
                     dump(back) + "  md: " + visible(md));
    UAPP_CHECK_MSG(same_forest(page.blocks, back.blocks),
                   dump(page) + "  --- got ---" + dump(back) + "  md: " + visible(md));

    std::string    md2;
    FidelityReport wr2;
    UAPP_REQUIRE_OK(page_to_markdown(back, MarkdownWriteOptions{}, md2, wr2));
    UAPP_CHECK_MSG(md2 == md, visible(md) + "  !=  " + visible(md2));
}

UAPP_TEST("conversion.roundtrip: numbered items are numbered from sibling position") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    // core/blocks: numbering is "DERIVED from sibling position at render time,
    // never stored" -- so the payload carries no number and a non-item sibling
    // restarts the run.
    const Page page = make_page("", {numbered("alpha"), numbered("beta"), numbered("gamma"),
                                     para("an interruption"), numbered("delta")});

    std::string    md;
    FidelityReport wr;
    UAPP_REQUIRE_OK(page_to_markdown(page, MarkdownWriteOptions{}, md, wr));

    UAPP_CHECK_MSG(md ==
                       "1. alpha\n2. beta\n3. gamma\n\nan interruption\n\n1. delta\n",
                   visible(md));

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page(md, MarkdownReadOptions{}, clock, ids, back, rr));
    UAPP_CHECK_MSG(same_forest(page.blocks, back.blocks), dump(back));

    // Re-importing a file whose numbers do NOT start at 1 still yields items;
    // the number is derived, so it is not part of the payload to lose.
    Page           renumbered;
    FidelityReport rr2;
    UAPP_REQUIRE_OK(markdown_to_page("7. seven\n9) nine\n", MarkdownReadOptions{}, clock, ids,
                                     renumbered, rr2));
    UAPP_REQUIRE(renumbered.blocks.size() == 2);
    UAPP_CHECK(type_at(renumbered.blocks, 0) == BlockType::NumberedListItem);
    UAPP_CHECK(type_at(renumbered.blocks, 1) == BlockType::NumberedListItem);
    std::string md3;
    FidelityReport wr3;
    UAPP_REQUIRE_OK(page_to_markdown(renumbered, MarkdownWriteOptions{}, md3, wr3));
    UAPP_CHECK_MSG(md3 == "1. seven\n2. nine\n", visible(md3));
}

UAPP_TEST("conversion.roundtrip: a realistic multi-block-type document") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    const Page page = make_page(
        "Release Checklist",
        {head("Before the build", 1),
         para("Read this first. It has two lines,\nthe second of which continues."),
         checklist("Run the unit suite", true,
                   {para("Including the sanitiser configuration."),
                    code("ctest --output-on-failure", "sh")}),
         checklist("Update the changelog", false),
         divider(),
         head("Notes", 2),
         quote("Offline-first means the network is optional,\nnot absent."),
         numbered("Tag the commit",
                  {numbered("Push the tag"), bullet("Verify it in CI")}),
         numbered("Publish"),
         code("cmake -S . -B build\ncmake --build build -j", "sh"),
         head("Appendix", 3),
         bullet("One"),
         bullet("Two", {para("With a child paragraph.")}),
         para("The end.")});

    std::string    md;
    FidelityReport wr;
    UAPP_REQUIRE_OK(page_to_markdown(page, MarkdownWriteOptions{}, md, wr));
    UAPP_CHECK_MSG(wr.empty(), visible(md));

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page(md, MarkdownReadOptions{}, clock, ids, back, rr));
    UAPP_CHECK_MSG(rr.empty(), visible(md));

    UAPP_CHECK_MSG(back.title == "Release Checklist", back.title);
    UAPP_CHECK_MSG(same_forest(page.blocks, back.blocks),
                   dump(page) + "  --- got ---" + dump(back) + "  md: " + visible(md));
    UAPP_CHECK(uapp::blocks::block_tree_size(back.blocks) ==
               uapp::blocks::block_tree_size(page.blocks));

    std::string    md2;
    FidelityReport wr2;
    UAPP_REQUIRE_OK(page_to_markdown(back, MarkdownWriteOptions{}, md2, wr2));
    UAPP_CHECK_MSG(md2 == md, visible(md) + "  !=  " + visible(md2));
}

UAPP_TEST("conversion.roundtrip: both halves of the title option, on and off") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    const Page page = make_page("The Title", {para("body")});

    // Defaults round-trip.
    {
        std::string    md;
        FidelityReport wr;
        UAPP_REQUIRE_OK(page_to_markdown(page, MarkdownWriteOptions{}, md, wr));
        UAPP_CHECK_MSG(md == "# The Title\n\nbody\n", visible(md));

        Page           back;
        FidelityReport rr;
        UAPP_REQUIRE_OK(markdown_to_page(md, MarkdownReadOptions{}, clock, ids, back, rr));
        UAPP_CHECK(back.title == "The Title");
        UAPP_REQUIRE(back.blocks.size() == 1);
        UAPP_CHECK(type_at(back.blocks, 0) == BlockType::Paragraph);
    }

    // title_as_heading = false: the file has no title line, so the title is
    // lost -- which is the documented consequence, not a bug.
    {
        MarkdownWriteOptions opts;
        opts.title_as_heading = false;
        std::string    md;
        FidelityReport wr;
        UAPP_REQUIRE_OK(page_to_markdown(page, opts, md, wr));
        UAPP_CHECK_MSG(md == "body\n", visible(md));

        Page           back;
        FidelityReport rr;
        UAPP_REQUIRE_OK(markdown_to_page(md, MarkdownReadOptions{}, clock, ids, back, rr));
        UAPP_CHECK(back.title.empty());
        UAPP_CHECK(back.blocks.size() == 1);
    }

    // leading_heading_as_title = false: the title line becomes a Heading
    // block, so the page gains a block instead of a title.
    {
        std::string    md;
        FidelityReport wr;
        UAPP_REQUIRE_OK(page_to_markdown(page, MarkdownWriteOptions{}, md, wr));

        MarkdownReadOptions ropts;
        ropts.leading_heading_as_title = false;
        ropts.default_title            = "Fallback";
        Page           back;
        FidelityReport rr;
        UAPP_REQUIRE_OK(markdown_to_page(md, ropts, clock, ids, back, rr));
        UAPP_CHECK(back.title == "Fallback");
        UAPP_REQUIRE_MSG(back.blocks.size() == 2, dump(back));
        UAPP_CHECK(type_at(back.blocks, 0) == BlockType::Heading);
        UAPP_CHECK(text_at(back.blocks, 0) == "The Title");
    }

    // An empty title writes no heading at all and re-imports as empty.
    {
        const Page untitled = make_page("", {para("body")});
        std::string    md;
        FidelityReport wr;
        UAPP_REQUIRE_OK(page_to_markdown(untitled, MarkdownWriteOptions{}, md, wr));
        UAPP_CHECK_MSG(md == "body\n", visible(md));

        Page           back;
        FidelityReport rr;
        UAPP_REQUIRE_OK(markdown_to_page(md, MarkdownReadOptions{}, clock, ids, back, rr));
        UAPP_CHECK(back.title.empty());
        UAPP_CHECK(back.blocks.size() == 1);
    }
}

UAPP_TEST("conversion.roundtrip: empty and whitespace-only input are a legal empty page") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    for (const char* input : {"", "\n", "   \n\t\n\n", "\r\n\r\n"}) {
        Page           back;
        FidelityReport rr;
        UAPP_REQUIRE_OK(markdown_to_page(input, MarkdownReadOptions{}, clock, ids, back, rr));
        UAPP_CHECK_MSG(back.blocks.empty(), dump(back));
        UAPP_CHECK(back.title.empty());
        UAPP_CHECK(!back.id.is_nil());
        UAPP_CHECK(back.created_at_ms == 1700000000000);

        std::string    md;
        FidelityReport wr;
        UAPP_REQUIRE_OK(page_to_markdown(back, MarkdownWriteOptions{}, md, wr));
        UAPP_CHECK_MSG(md.empty(), visible(md));
    }
}

UAPP_TEST("conversion.roundtrip: a code fence outgrows its own content") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    const Page page = make_page(
        "", {code("```\nnot the end\n```", ""),
             // Tildes in the content do NOT need a longer BACKTICK fence.
             code("~~~~\nnor this\n~~~~", "text"),
             // A backtick in the info string switches the fence to '~', which
             // CAN carry it -- so this case is lossless, not truncated.
             code("print(1)", "md`inline"),
             // ... and that '~' fence still has to outgrow its own content.
             code("~~~\ninner\n~~~", "sh`x"),
             // Indentation inside a fence is content and is preserved exactly.
             code("def f():\n    return 1\n\n    # after a blank line", "python")});

    std::string    md;
    FidelityReport wr;
    UAPP_REQUIRE_OK(page_to_markdown(page, MarkdownWriteOptions{}, md, wr));
    UAPP_CHECK_MSG(wr.count_of("code_language_truncated") == 0, visible(md));
    UAPP_CHECK_MSG(md.find("````\n") != std::string::npos, visible(md));
    UAPP_CHECK_MSG(md.find("```text\n") != std::string::npos, visible(md));
    UAPP_CHECK_MSG(md.find("~~~md`inline\n") != std::string::npos, visible(md));
    UAPP_CHECK_MSG(md.find("~~~~sh`x\n") != std::string::npos, visible(md));

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page(md, MarkdownReadOptions{}, clock, ids, back, rr));
    UAPP_CHECK_MSG(same_forest(page.blocks, back.blocks),
                   dump(page) + "  --- got ---" + dump(back) + "  md: " + visible(md));

    std::string    md2;
    FidelityReport wr2;
    UAPP_REQUIRE_OK(page_to_markdown(back, MarkdownWriteOptions{}, md2, wr2));
    UAPP_CHECK_MSG(md2 == md, visible(md) + "  !=  " + visible(md2));
}

UAPP_TEST("conversion.roundtrip: CRLF input is normalised, not carried into the text") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page("# T\r\n\r\none\r\ntwo\r\n\r\n- item\r\n",
                                     MarkdownReadOptions{}, clock, ids, back, rr));
    UAPP_CHECK(back.title == "T");
    UAPP_REQUIRE_MSG(back.blocks.size() == 2, dump(back));
    UAPP_CHECK_MSG(text_at(back.blocks, 0) == "one\ntwo", dump(back));
    UAPP_CHECK(type_at(back.blocks, 1) == BlockType::BulletedListItem);
}

UAPP_TEST("conversion.roundtrip: list nesting at exactly kMaxBlockDepth is accepted") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    std::string input;
    for (std::size_t level = 0; level < uapp::blocks::kMaxBlockDepth; ++level) {
        input.append(level * 2, ' ');
        input.append("- item\n");
    }

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page(input, MarkdownReadOptions{}, clock, ids, back, rr));
    UAPP_CHECK(uapp::blocks::block_tree_depth(back.blocks) == uapp::blocks::kMaxBlockDepth);
    UAPP_CHECK(uapp::blocks::block_tree_size(back.blocks) == uapp::blocks::kMaxBlockDepth);

    std::string    md;
    FidelityReport wr;
    UAPP_REQUIRE_OK(page_to_markdown(back, MarkdownWriteOptions{}, md, wr));

    Page           again;
    FidelityReport rr2;
    UAPP_REQUIRE_OK(markdown_to_page(md, MarkdownReadOptions{}, clock, ids, again, rr2));
    UAPP_CHECK_MSG(same_forest(back.blocks, again.blocks), dump(back) + dump(again));
}

// ===========================================================================
// THE FIDELITY REPORT ITSELF
// ===========================================================================

UAPP_TEST("conversion.fidelity.report: entries aggregate by (severity, construct)") {
    FidelityReport r;
    UAPP_CHECK(r.empty());
    UAPP_CHECK(!r.lost_content());
    UAPP_CHECK(r.count_of("table") == 0);

    r.note(Fidelity::Approximated, "table", 7, "first message");
    r.note(Fidelity::Approximated, "table", 99, "second message");
    r.note(Fidelity::Degraded, "table", 5, "a different severity");
    r.note(Fidelity::PreservedNote, "inline_markup", 2, "note");

    UAPP_REQUIRE(r.entries().size() == 3);
    UAPP_CHECK(r.entries()[0].count == 2);
    // The FIRST occurrence is the one reported, and the message belongs to the
    // construct rather than to the occurrence.
    UAPP_CHECK(r.entries()[0].first_line == 7);
    UAPP_CHECK(r.entries()[0].message == "first message");

    UAPP_CHECK(r.count_of("table") == 3);  // summed over severities
    UAPP_CHECK(r.count_at(Fidelity::Approximated) == 2);
    UAPP_CHECK(r.count_at(Fidelity::Degraded) == 1);
    UAPP_CHECK(r.count_at(Fidelity::Dropped) == 0);
    UAPP_CHECK(!r.lost_content());

    r.clear();
    UAPP_CHECK(r.empty());
}

UAPP_TEST("conversion.fidelity.report: severity names are the stable section 6.1 set") {
    UAPP_CHECK(std::string_view(uapp::conversion::fidelity_name(Fidelity::Dropped)) ==
               "dropped");
    UAPP_CHECK(std::string_view(uapp::conversion::fidelity_name(Fidelity::Approximated)) ==
               "approximated");
    UAPP_CHECK(std::string_view(uapp::conversion::fidelity_name(Fidelity::Degraded)) ==
               "degraded");
    UAPP_CHECK(std::string_view(uapp::conversion::fidelity_name(Fidelity::PreservedNote)) ==
               "preserved_note");
    UAPP_CHECK(std::string_view(uapp::conversion::fidelity_name(
                   static_cast<Fidelity>(4242))) == "unknown");
}

// ===========================================================================
// WHAT IS LOSSY — one case per documented construct
// ===========================================================================

UAPP_TEST("conversion.fidelity: heading levels 4-6 are clamped to 3, not dropped") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page("#### four\n\n##### five\n\n###### six\n",
                                     MarkdownReadOptions{}, clock, ids, back, rr));

    UAPP_REQUIRE_MSG(back.blocks.size() == 3, dump(back));
    for (std::size_t i = 0; i < 3; ++i) {
        UAPP_CHECK(type_at(back.blocks, i) == BlockType::Heading);
        const auto* h = std::get_if<HeadingBlock>(&back.blocks[i].content);
        UAPP_REQUIRE(h != nullptr);
        UAPP_CHECK_MSG(h->level == uapp::blocks::kMaxHeadingLevel, std::to_string(h->level));
    }
    UAPP_CHECK(text_at(back.blocks, 0) == "four");
    UAPP_CHECK_MSG(rr.count_of("heading_level_clamped") == 3,
                   std::to_string(rr.count_of("heading_level_clamped")));
    UAPP_CHECK(rr.count_at(Fidelity::Approximated) == 3);
    UAPP_CHECK(!rr.lost_content());

    // The documented consequence: a re-export is NOT byte-identical, because
    // the heading-ness was kept and one level of it was not.
    std::string    md;
    FidelityReport wr;
    UAPP_REQUIRE_OK(page_to_markdown(back, MarkdownWriteOptions{}, md, wr));
    UAPP_CHECK_MSG(md == "### four\n\n### five\n\n### six\n", visible(md));
}

UAPP_TEST("conversion.fidelity: children of a non-list block are flattened, in order") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    // A paragraph and a quote, each with children. A QuoteBlock is a LEAF with
    // a text field, so its children flatten for the same reason a paragraph's
    // do even though '>' is a Markdown container.
    const Page page = make_page(
        "", {para("the parent", {para("first child"), bullet("second child")}),
             quote("the quote", {para("quote child")}), para("after everything")});

    std::string    md;
    FidelityReport wr;
    UAPP_REQUIRE_OK(page_to_markdown(page, MarkdownWriteOptions{}, md, wr));

    UAPP_CHECK_MSG(wr.count_of("nesting_flattened") == 2,
                   std::to_string(wr.count_of("nesting_flattened")));
    UAPP_CHECK(wr.count_at(Fidelity::Approximated) == 2);
    UAPP_CHECK(!wr.lost_content());

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page(md, MarkdownReadOptions{}, clock, ids, back, rr));

    // Content and ORDER are preserved; one level of depth is not. Three
    // top-level blocks holding three children become six siblings.
    UAPP_REQUIRE_MSG(back.blocks.size() == 6, dump(back) + "  md: " + visible(md));
    UAPP_CHECK(uapp::blocks::block_tree_depth(back.blocks) == 1);
    UAPP_CHECK(text_at(back.blocks, 0) == "the parent");
    UAPP_CHECK(text_at(back.blocks, 1) == "first child");
    UAPP_CHECK(type_at(back.blocks, 2) == BlockType::BulletedListItem);
    UAPP_CHECK(text_at(back.blocks, 2) == "second child");
    UAPP_CHECK(type_at(back.blocks, 3) == BlockType::Quote);
    UAPP_CHECK(text_at(back.blocks, 3) == "the quote");
    UAPP_CHECK(text_at(back.blocks, 4) == "quote child");
    UAPP_CHECK(text_at(back.blocks, 5) == "after everything");
}

UAPP_TEST("conversion.fidelity: a table keeps its content as paragraph text") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    const std::string table =
        "| name | count |\n"
        "|------|------:|\n"
        "| a    | 1     |\n"
        "| b    | 2     |";

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page(table + "\n", MarkdownReadOptions{}, clock, ids, back, rr));

    UAPP_REQUIRE_MSG(back.blocks.size() == 1, dump(back));
    UAPP_CHECK(type_at(back.blocks, 0) == BlockType::Paragraph);
    UAPP_CHECK_MSG(text_at(back.blocks, 0) == table, std::string(text_at(back.blocks, 0)));
    UAPP_CHECK_MSG(rr.count_of("table") >= 1, std::to_string(rr.count_of("table")));
    UAPP_CHECK(rr.count_at(Fidelity::Approximated) >= 1);
    UAPP_CHECK(!rr.lost_content());

    // The lines come back out unchanged, so a future Table block type can
    // re-parse them -- and the export side reports the same construct.
    std::string    md;
    FidelityReport wr;
    UAPP_REQUIRE_OK(page_to_markdown(back, MarkdownWriteOptions{}, md, wr));
    UAPP_CHECK_MSG(md == table + "\n", visible(md));
    UAPP_CHECK(wr.count_of("table") >= 1);
}

UAPP_TEST("conversion.fidelity: an image block keeps its src as paragraph text") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page("![a diagram](assets/diagram.png)\n",
                                     MarkdownReadOptions{}, clock, ids, back, rr));

    UAPP_REQUIRE_MSG(back.blocks.size() == 1, dump(back));
    UAPP_CHECK(type_at(back.blocks, 0) == BlockType::Paragraph);
    UAPP_CHECK(text_at(back.blocks, 0) == "![a diagram](assets/diagram.png)");
    UAPP_CHECK_MSG(rr.count_of("image") == 1, std::to_string(rr.count_of("image")));
    UAPP_CHECK(rr.count_at(Fidelity::Approximated) == 1);
    UAPP_CHECK(!rr.lost_content());

    std::string    md;
    FidelityReport wr;
    UAPP_REQUIRE_OK(page_to_markdown(back, MarkdownWriteOptions{}, md, wr));
    UAPP_CHECK_MSG(md == "![a diagram](assets/diagram.png)\n", visible(md));
    UAPP_CHECK(wr.count_of("image") == 1);
}

UAPP_TEST("conversion.fidelity: inline markup is kept VERBATIM, byte for byte") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    // The whole point of the decision: the href survives, because stripping
    // the syntax to its plain text is the one loss a future span model could
    // not reconstruct.
    const std::string line =
        "**bold**, _em_, `code`, [text](https://example.com/x), "
        "![inline](i.png), <b>html</b>";

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page(line + "\n", MarkdownReadOptions{}, clock, ids, back, rr));

    UAPP_REQUIRE_MSG(back.blocks.size() == 1, dump(back));
    UAPP_CHECK_MSG(text_at(back.blocks, 0) == line, std::string(text_at(back.blocks, 0)));
    UAPP_CHECK_MSG(rr.count_of("inline_markup") == 1,
                   std::to_string(rr.count_of("inline_markup")));
    UAPP_CHECK(rr.count_at(Fidelity::PreservedNote) >= 1);
    UAPP_CHECK(!rr.lost_content());

    // Byte-lossless, so it round-trips exactly; only INTERPRETATION is lost.
    std::string    md;
    FidelityReport wr;
    UAPP_REQUIRE_OK(page_to_markdown(back, MarkdownWriteOptions{}, md, wr));
    UAPP_CHECK_MSG(md == line + "\n", visible(md));
    UAPP_CHECK(wr.count_of("inline_markup") == 1);

    // And an identifier with underscores is NOT reported as emphasis.
    Page           plain;
    FidelityReport rr2;
    UAPP_REQUIRE_OK(markdown_to_page("call snake_case_name(x) twice\n", MarkdownReadOptions{},
                                     clock, ids, plain, rr2));
    UAPP_CHECK_MSG(rr2.count_of("inline_markup") == 0,
                   std::string(text_at(plain.blocks, 0)));
}

UAPP_TEST("conversion.fidelity: leading and trailing whitespace is normalised, reported") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    const Page page = make_page("", {para("   padded   "), para("tabbed\t"),
                                     para("carriage\r"), para("clean")});

    std::string    md;
    FidelityReport wr;
    UAPP_REQUIRE_OK(page_to_markdown(page, MarkdownWriteOptions{}, md, wr));

    UAPP_CHECK_MSG(md == "padded\n\ntabbed\n\ncarriage\n\nclean\n", visible(md));
    UAPP_CHECK_MSG(wr.count_of("text_line_whitespace") == 3,
                   std::to_string(wr.count_of("text_line_whitespace")));
    UAPP_CHECK(wr.count_at(Fidelity::Degraded) == 3);
    UAPP_CHECK(!wr.lost_content());

    // In scope for exact round-trip: text with no leading or trailing
    // whitespace. The normalised form is a fixed point.
    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page(md, MarkdownReadOptions{}, clock, ids, back, rr));
    UAPP_REQUIRE(back.blocks.size() == 4);
    UAPP_CHECK(text_at(back.blocks, 0) == "padded");
    UAPP_CHECK(text_at(back.blocks, 1) == "tabbed");
    UAPP_CHECK(text_at(back.blocks, 2) == "carriage");
    UAPP_CHECK(text_at(back.blocks, 3) == "clean");
    UAPP_CHECK(rr.count_of("text_line_whitespace") == 0);
}

UAPP_TEST("conversion.fidelity: a code info string with whitespace is truncated, reported") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    const Page page = make_page("", {code("auto x = 1;", "c++ 17")});

    std::string    md;
    FidelityReport wr;
    UAPP_REQUIRE_OK(page_to_markdown(page, MarkdownWriteOptions{}, md, wr));

    // Written AS-IS, so nothing is lost in the file itself.
    UAPP_CHECK_MSG(md == "```c++ 17\nauto x = 1;\n```\n", visible(md));
    UAPP_CHECK_MSG(wr.count_of("code_language_truncated") == 1,
                   std::to_string(wr.count_of("code_language_truncated")));
    UAPP_CHECK(wr.count_at(Fidelity::Degraded) == 1);
    UAPP_CHECK(!wr.lost_content());

    // CommonMark takes the first word, so it reads back as "c++" -- and the
    // read side reports the same construct rather than losing it quietly.
    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page(md, MarkdownReadOptions{}, clock, ids, back, rr));
    UAPP_REQUIRE(back.blocks.size() == 1);
    const auto* cb = std::get_if<CodeBlock>(&back.blocks[0].content);
    UAPP_REQUIRE(cb != nullptr);
    UAPP_CHECK_MSG(cb->language == "c++", cb->language);
    UAPP_CHECK(cb->code == "auto x = 1;");
    UAPP_CHECK(rr.count_of("code_language_truncated") == 1);
}

UAPP_TEST("conversion.fidelity: a setext underline is text, and --- is a divider") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    // `===` becomes paragraph text and is REPORTED, so it is visible rather
    // than quietly a paragraph.
    Page           eq;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page("Title\n===\n", MarkdownReadOptions{}, clock, ids, eq, rr));
    UAPP_REQUIRE_MSG(eq.blocks.size() == 1, dump(eq));
    UAPP_CHECK(type_at(eq.blocks, 0) == BlockType::Paragraph);
    UAPP_CHECK_MSG(text_at(eq.blocks, 0) == "Title\n===", std::string(text_at(eq.blocks, 0)));
    UAPP_CHECK_MSG(rr.count_of("setext_heading") == 1,
                   std::to_string(rr.count_of("setext_heading")));
    UAPP_CHECK(rr.count_at(Fidelity::Approximated) == 1);
    UAPP_CHECK(!rr.lost_content());

    // `---` under a paragraph is a THEMATIC BREAK, which is what this
    // converter's own exporter means by it. Resolving it as setext would break
    // every exported paragraph-then-divider pair.
    Page           dash;
    FidelityReport rr2;
    UAPP_REQUIRE_OK(markdown_to_page("Title\n---\n", MarkdownReadOptions{}, clock, ids, dash,
                                     rr2));
    UAPP_REQUIRE_MSG(dash.blocks.size() == 2, dump(dash));
    UAPP_CHECK(type_at(dash.blocks, 0) == BlockType::Paragraph);
    UAPP_CHECK(text_at(dash.blocks, 0) == "Title");
    UAPP_CHECK(type_at(dash.blocks, 1) == BlockType::Divider);
    UAPP_CHECK(rr2.count_of("setext_heading") == 0);

    // And a standalone `===` with nothing above it is not reported, because
    // there is no heading it could have been.
    Page           lonely;
    FidelityReport rr3;
    UAPP_REQUIRE_OK(markdown_to_page("===\n", MarkdownReadOptions{}, clock, ids, lonely, rr3));
    UAPP_CHECK(rr3.count_of("setext_heading") == 0);
    UAPP_CHECK(text_at(lonely.blocks, 0) == "===");
}

UAPP_TEST("conversion.fidelity: an indented code block is paragraph text, reported") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page("    indented one\n    indented two\n",
                                     MarkdownReadOptions{}, clock, ids, back, rr));

    UAPP_REQUIRE_MSG(back.blocks.size() == 1, dump(back));
    UAPP_CHECK(type_at(back.blocks, 0) == BlockType::Paragraph);
    UAPP_CHECK_MSG(text_at(back.blocks, 0) == "indented one\nindented two",
                   std::string(text_at(back.blocks, 0)));
    UAPP_CHECK_MSG(rr.count_of("indented_code_block") == 1,
                   std::to_string(rr.count_of("indented_code_block")));
    UAPP_CHECK(rr.count_at(Fidelity::Approximated) == 1);
    UAPP_CHECK(!rr.lost_content());

    // A tab is four columns, so it is the same case.
    Page           tabbed;
    FidelityReport rr2;
    UAPP_REQUIRE_OK(markdown_to_page("\tindented by tab\n", MarkdownReadOptions{}, clock, ids,
                                     tabbed, rr2));
    UAPP_CHECK(rr2.count_of("indented_code_block") == 1);
    UAPP_CHECK(text_at(tabbed.blocks, 0) == "indented by tab");
}

UAPP_TEST("conversion.fidelity: unmodelled blocks are kept as text and reported") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    struct Case final {
        const char* input;
        const char* text;
    };
    const Case cases[] = {
        {"[id]: https://example.com/target\n", "[id]: https://example.com/target"},
        {"[^note]: a footnote body\n", "[^note]: a footnote body"},
        {"$$\n", "$$"},
        {"<section id=\"x\">\n", "<section id=\"x\">"},
    };

    for (const Case& c : cases) {
        Page           back;
        FidelityReport rr;
        UAPP_REQUIRE_OK(markdown_to_page(c.input, MarkdownReadOptions{}, clock, ids, back, rr));
        UAPP_REQUIRE_MSG(back.blocks.size() == 1, dump(back));
        UAPP_CHECK_MSG(text_at(back.blocks, 0) == c.text,
                       std::string(text_at(back.blocks, 0)));
        UAPP_CHECK_MSG(rr.count_of("unmodelled_block") >= 1, c.input);
        UAPP_CHECK_MSG(rr.count_at(Fidelity::PreservedNote) >= 1, c.input);
        // Never dropped: that is the whole contract.
        UAPP_CHECK_MSG(!rr.lost_content(), c.input);
    }
}

UAPP_TEST("conversion.fidelity: nothing is ever Dropped, in either direction") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    // One document holding every lossy construct at once.
    const std::string input =
        "#### clamped heading\n"
        "\n"
        "a **bold** line with [a link](http://x) and ![an image](i.png)\n"
        "\n"
        "| a | b |\n"
        "|---|---|\n"
        "| 1 | 2 |\n"
        "\n"
        "![whole line image](big.png)\n"
        "\n"
        "Title\n"
        "===\n"
        "\n"
        "    indented code\n"
        "\n"
        "[ref]: http://example.com\n"
        "\n"
        "```c++ 17\n"
        "code();\n"
        "```\n";

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_OK(markdown_to_page(input, MarkdownReadOptions{}, clock, ids, back, rr));

    // Every documented construct present, and not one Dropped entry.
    UAPP_CHECK(rr.count_of("heading_level_clamped") >= 1);
    UAPP_CHECK(rr.count_of("inline_markup") >= 1);
    UAPP_CHECK(rr.count_of("table") >= 1);
    UAPP_CHECK(rr.count_of("image") >= 1);
    UAPP_CHECK(rr.count_of("setext_heading") >= 1);
    UAPP_CHECK(rr.count_of("indented_code_block") >= 1);
    UAPP_CHECK(rr.count_of("unmodelled_block") >= 1);
    UAPP_CHECK(rr.count_of("code_language_truncated") >= 1);
    UAPP_CHECK_MSG(rr.count_at(Fidelity::Dropped) == 0,
                   std::to_string(rr.count_at(Fidelity::Dropped)));
    UAPP_CHECK(!rr.lost_content());

    std::string    md;
    FidelityReport wr;
    UAPP_REQUIRE_OK(page_to_markdown(back, MarkdownWriteOptions{}, md, wr));
    UAPP_CHECK(wr.count_at(Fidelity::Dropped) == 0);
    UAPP_CHECK(!wr.lost_content());

    // Report entries are BOUNDED by the construct vocabulary, not by the
    // input size: that is why they aggregate.
    UAPP_CHECK_MSG(rr.entries().size() <= 12, std::to_string(rr.entries().size()));
    for (const uapp::conversion::FidelityEntry& e : rr.entries()) {
        UAPP_CHECK_MSG(e.count >= 1, e.construct);
        UAPP_CHECK_MSG(!e.message.empty(), e.construct);
        // A report may be logged; a document body may not (spec section 34).
        UAPP_CHECK_MSG(e.message.find("bold") == std::string::npos, e.message);
    }
}

// ===========================================================================
// BOUNDED FAILURE
// ===========================================================================

UAPP_TEST("conversion.bounds: input over the cap fails without being parsed") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    const std::string too_big(kMaxMarkdownInputBytes + 1, 'a');

    Page sentinel = make_page("untouched", {para("still here")});
    FidelityReport rr;
    UAPP_REQUIRE_STATUS(markdown_to_page(too_big, MarkdownReadOptions{}, clock, ids, sentinel,
                                         rr),
                        Status::ConversionInputTooLarge);
    // "`out` is overwritten on success and left untouched on failure."
    UAPP_CHECK(sentinel.title == "untouched");
    UAPP_REQUIRE(sentinel.blocks.size() == 1);
    UAPP_CHECK(text_at(sentinel.blocks, 0) == "still here");
    UAPP_CHECK(rr.empty());
}

UAPP_TEST("conversion.bounds: an embedded NUL is malformed input, not a truncated page") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    const std::string with_nul("before\0after\n", 13);
    UAPP_REQUIRE(with_nul.size() == 13);

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_STATUS(markdown_to_page(with_nul, MarkdownReadOptions{}, clock, ids, back, rr),
                        Status::ConversionInputMalformed);
    UAPP_CHECK(back.blocks.empty());
    UAPP_CHECK(back.id.is_nil());
}

UAPP_TEST("conversion.bounds: list nesting past kMaxBlockDepth is refused, not recursed") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    std::string input;
    for (std::size_t level = 0; level < uapp::blocks::kMaxBlockDepth + 6; ++level) {
        input.append(level * 2, ' ');
        input.append("- item\n");
    }

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_STATUS(markdown_to_page(input, MarkdownReadOptions{}, clock, ids, back, rr),
                        Status::BlockDepthExceeded);
    UAPP_CHECK(back.blocks.empty());

    // A pathological file (thousands of levels) costs a Status, not a stack.
    std::string deeper;
    for (std::size_t level = 0; level < 4000; ++level) {
        deeper.append(level * 2, ' ');
        deeper.append("- x\n");
    }
    Page           back2;
    FidelityReport rr2;
    UAPP_REQUIRE_STATUS(markdown_to_page(deeper, MarkdownReadOptions{}, clock, ids, back2, rr2),
                        Status::BlockDepthExceeded);
}

UAPP_TEST("conversion.bounds: a single oversized block is BlockTextTooLarge") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    const std::string huge(uapp::blocks::kMaxBlockTextBytes + 1, 'a');

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_STATUS(markdown_to_page(huge, MarkdownReadOptions{}, clock, ids, back, rr),
                        Status::BlockTextTooLarge);
    UAPP_CHECK(back.blocks.empty());

    // Exactly at the limit is legal, and a limit-sized block is not a
    // conversion failure.
    const std::string at_limit(uapp::blocks::kMaxBlockTextBytes, 'a');
    Page              ok_page;
    FidelityReport    rr2;
    UAPP_REQUIRE_OK(markdown_to_page(at_limit, MarkdownReadOptions{}, clock, ids, ok_page, rr2));
    UAPP_REQUIRE(ok_page.blocks.size() == 1);
    UAPP_CHECK(text_at(ok_page.blocks, 0).size() == uapp::blocks::kMaxBlockTextBytes);
}

UAPP_TEST("conversion.bounds: more blocks than a page may hold is PageBlockLimitExceeded") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;

    std::string input;
    input.reserve((uapp::blocks::kMaxBlocksPerPage + 1) * 3);
    for (std::size_t i = 0; i <= uapp::blocks::kMaxBlocksPerPage; ++i) {
        input.append("x\n\n");
    }

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_STATUS(markdown_to_page(input, MarkdownReadOptions{}, clock, ids, back, rr),
                        Status::PageBlockLimitExceeded);
    UAPP_CHECK(back.blocks.empty());
}

UAPP_TEST("conversion.bounds: an export past the output cap leaves `out` empty") {
    // A page at the block model's own limits would serialise to ~50 GB, which
    // is why the export side has its own cap and cannot borrow the page's.
    std::vector<Block> blocks;
    for (std::size_t i = 0; i < 17; ++i) {
        blocks.push_back(para(std::string(uapp::blocks::kMaxBlockTextBytes, 'a')));
    }
    const Page page = make_page("", std::move(blocks));

    std::string    md = "sentinel";
    FidelityReport wr;
    UAPP_REQUIRE_STATUS(page_to_markdown(page, MarkdownWriteOptions{}, md, wr),
                        Status::ConversionOutputTooLarge);
    // Not half-written: a caller who ignores the Status cannot write a
    // truncated file.
    UAPP_CHECK_MSG(md.empty(), std::to_string(md.size()));
}

UAPP_TEST("conversion.bounds: an id-source failure is Internal, not a nil-id page") {
    uapp::test::FakeSystemClock  clock(1700000000000);
    uapp::test::CountingIdSource ids;
    ids.fail_next();

    Page           back;
    FidelityReport rr;
    UAPP_REQUIRE_STATUS(markdown_to_page("hello\n", MarkdownReadOptions{}, clock, ids, back, rr),
                        Status::Internal);
    UAPP_CHECK(back.id.is_nil());
    UAPP_CHECK(back.blocks.empty());
}

UAPP_TEST("conversion.bounds: the export path validates its own input") {
    // "a page may have arrived from a database file or a sync peer."
    std::string    md = "sentinel";
    FidelityReport wr;

    {
        Page nil_id;
        nil_id.title = "x";
        UAPP_REQUIRE_STATUS(page_to_markdown(nil_id, MarkdownWriteOptions{}, md, wr),
                            Status::IdMalformed);
        UAPP_CHECK(md.empty());
    }
    {
        // A block whose page_id does not match the page it sits in.
        Page page = make_page("t", {para("a")});
        uapp::ObjectIdBytes other{};
        other[0]              = 0x5Au;
        page.blocks[0].page_id = PageId(other);
        md                     = "sentinel";
        UAPP_REQUIRE_STATUS(page_to_markdown(page, MarkdownWriteOptions{}, md, wr),
                            Status::BlockMalformed);
        UAPP_CHECK(md.empty());
    }
    {
        // A title over the page-title limit.
        Page page        = make_page("t", {para("a")});
        page.title       = std::string(uapp::blocks::kMaxPageTitleBytes + 1, 't');
        md               = "sentinel";
        UAPP_REQUIRE_STATUS(page_to_markdown(page, MarkdownWriteOptions{}, md, wr),
                            Status::PageTitleTooLong);
        UAPP_CHECK(md.empty());
    }
}
