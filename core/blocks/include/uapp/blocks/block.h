// SPDX-License-Identifier: Apache-2.0
//
// core/blocks/block.h — the block value type and its typed payloads.
//
// This is the real structure spec section 14 asks for:
//
//     Workspace -> Page -> Blocks / Child Pages / Databases / Files
//
// and it supersedes the flat `notes::Note` (one title, one body). See
// docs/adr/0003-page-supersedes-note.md for why both exist at once right now.
//
// ---------------------------------------------------------------------------
// WHY A VARIANT AND NOT A CLASS HIERARCHY
// ---------------------------------------------------------------------------
// The obvious C++ shape for "nineteen kinds of block" is an abstract
// `Block` with nineteen subclasses. It is rejected here for three concrete
// reasons, not on taste:
//
//  1. ADR-0001 exports a pure C99 ABI. A closed tagged union maps to C as
//     `{ int32_t type; union { ... } payload; }` -- mechanically, with no
//     ownership question. A virtual hierarchy maps to C as an opaque handle
//     plus one accessor per field per type, which is where FFI surfaces go
//     to die. core/capi is a later step, but the data model is chosen now.
//  2. ADR-0001 also bans RTTI (-fno-rtti), so there is no dynamic_cast to
//     recover the concrete type. A hierarchy would need a hand-rolled type
//     tag anyway -- i.e. a tagged union wearing a vtable.
//  3. The set of block types is CLOSED and owned by us. That is exactly the
//     case where a variant beats open polymorphism: adding a type should be
//     a compile error everywhere it matters, which `std::visit` over a
//     variant gives and a hierarchy does not.
//
// The cost is that every payload is in every Block's storage footprint
// (sizeof == the largest alternative). At the payload sizes here -- two
// std::strings worst case -- that is a rounding error next to the text.
//
// ---------------------------------------------------------------------------
// EXTENSION POINT: adding a block type
// ---------------------------------------------------------------------------
// `BlockType` below enumerates ALL nineteen spec section 14 types. Eight are
// implemented; the other eleven are declared, given a permanent numeric
// value, and reported as Status::BlockTypeUnsupported with a reason string
// (`block_type_not_implemented_reason`). They are NOT fake-implemented.
//
// To implement one:
//   1. Add a payload struct with real typed fields (never an opaque blob).
//   2. Append it to `BlockContent` **in the same ordinal position as its
//      BlockType value** -- the static_asserts at the bottom of this header
//      enforce that the variant index and the enum value agree, because the
//      persisted `block.type` column stores that integer.
//   3. Extend `validate_block_content`.
//   4. Extend the encode/decode pair in storage/sqlite/src/sqlite_block_store.cpp.
//      If the payload does not fit the narrow typed columns there, that is the
//      moment to switch `block.content` to the CBOR blob database-design.md
//      section 3.3 specifies; the `schema_ver` column already exists for it.
//   5. Drop its entry from the NOT-IMPLEMENTED table in block.cpp.

#ifndef UAPP_CORE_BLOCKS_BLOCK_H
#define UAPP_CORE_BLOCKS_BLOCK_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "uapp/object_id.h"
#include "uapp/status.h"

namespace uapp::blocks {

// --- identifiers -----------------------------------------------------------
// Distinct types over the same 16 bytes, so a BlockId cannot be passed where
// a PageId is wanted. See core/common/object_id.h.

struct PageIdTag;
struct BlockIdTag;

using PageId  = ObjectId<PageIdTag>;
using BlockId = ObjectId<BlockIdTag>;

// --- hard limits -----------------------------------------------------------
// Enforced at the domain boundary so nothing unbounded reaches storage
// (spec section 38: "Validate buffer sizes", "Validate external input").

/// Maximum text bytes in any single text-bearing block payload.
inline constexpr std::size_t kMaxBlockTextBytes = 1u * 1024u * 1024u;  // 1 MiB

/// Maximum bytes in a code block's language tag.
inline constexpr std::size_t kMaxCodeLanguageBytes = 64;

inline constexpr std::size_t kMaxPageTitleBytes = 512;

inline constexpr int kMinHeadingLevel = 1;
inline constexpr int kMaxHeadingLevel = 3;

/// Maximum block nesting depth. A top-level block is at depth 1.
///
/// WHY A LIMIT AT ALL (spec section 38, "validate external input"): the block
/// tree is user-controlled and every read of it is recursive -- assembly,
/// rendering, serialisation, the future CBOR codec. Unbounded user nesting is
/// therefore a stack-exhaustion vector reachable from a synced or restored
/// document, i.e. a denial of service that a plain "it's just their own data"
/// argument does not cover.
///
/// WHY 64: deep enough that no human outlining workflow reaches it (Notion's
/// own practical limit is far shallower), shallow enough that 64 frames of a
/// tree walk is a few kilobytes of stack on the smallest target in
/// performance-strategy.md. It is enforced on write, on move AND on read, so
/// a hand-edited database cannot smuggle a deeper tree past it.
inline constexpr std::size_t kMaxBlockDepth = 64;

/// Maximum blocks in one page. database-design.md section 9 budgets 10 000
/// blocks for a typical library and 500 000 across the heavy case; this caps
/// a single page well above any real document while keeping a full-page read
/// bounded.
inline constexpr std::size_t kMaxBlocksPerPage = 50000;

// --- block types -----------------------------------------------------------

/// Every block type in spec section 14. Values are PERSISTED in the
/// `block.type` column: append-only, never renumber, never reuse.
enum class BlockType : std::int32_t {
    // --- implemented -------------------------------------------------------
    Paragraph        = 0,
    Heading          = 1,
    BulletedListItem = 2,
    NumberedListItem = 3,
    ChecklistItem    = 4,
    Code             = 5,
    Divider          = 6,
    Quote            = 7,

    // --- declared, NOT IMPLEMENTED ----------------------------------------
    // Each has a reason; see block_type_not_implemented_reason(). Constructing
    // one is impossible (there is no variant alternative for it) and reading
    // one out of a database yields Status::BlockTypeUnsupported rather than a
    // silently-wrong paragraph.
    Toggle   = 8,
    Callout  = 9,
    Table    = 10,
    Image    = 11,
    Video    = 12,
    Audio    = 13,
    File     = 14,
    Equation = 15,
    Embed    = 16,
    Database = 17,
    Bookmark = 18,
};

/// The first BlockType value that is not implemented in this build.
inline constexpr std::int32_t kFirstUnimplementedBlockType = 8;

/// Number of BlockType values that exist at all (implemented or not).
inline constexpr std::int32_t kBlockTypeCount = 19;

// --- payloads --------------------------------------------------------------
// Real typed fields. None of these is a stringly-typed escape hatch.

struct ParagraphBlock final {
    std::string text;
};

struct HeadingBlock final {
    std::string text;
    /// 1, 2 or 3. Spec section 14 says "Heading" without levels; three is what
    /// every comparable editor exposes and what Markdown import/export needs.
    int level = 1;
};

struct BulletedListItemBlock final {
    std::string text;
};

struct NumberedListItemBlock final {
    std::string text;
    /// Numbering is DERIVED from sibling position at render time, never
    /// stored: storing it would go stale on every insert and reorder.
};

struct ChecklistItemBlock final {
    std::string text;
    bool        checked = false;
};

struct CodeBlock final {
    std::string code;
    /// Free-form language tag ("cpp", "python", ...). Validated for length and
    /// for the absence of control bytes only -- an allow-list here would have
    /// to track every highlighter grammar, and an unknown tag degrades to
    /// "no highlighting", which is not a security boundary.
    std::string language;
};

struct DividerBlock final {
    // Intentionally empty: a divider is pure structure. Kept as a named type
    // rather than std::monostate so the variant alternative is self-describing.
};

struct QuoteBlock final {
    std::string text;
};

inline bool operator==(const ParagraphBlock& a, const ParagraphBlock& b) {
    return a.text == b.text;
}
inline bool operator==(const HeadingBlock& a, const HeadingBlock& b) {
    return a.text == b.text && a.level == b.level;
}
inline bool operator==(const BulletedListItemBlock& a, const BulletedListItemBlock& b) {
    return a.text == b.text;
}
inline bool operator==(const NumberedListItemBlock& a, const NumberedListItemBlock& b) {
    return a.text == b.text;
}
inline bool operator==(const ChecklistItemBlock& a, const ChecklistItemBlock& b) {
    return a.text == b.text && a.checked == b.checked;
}
inline bool operator==(const CodeBlock& a, const CodeBlock& b) {
    return a.code == b.code && a.language == b.language;
}
inline bool operator==(const DividerBlock&, const DividerBlock&) { return true; }
inline bool operator==(const QuoteBlock& a, const QuoteBlock& b) { return a.text == b.text; }

/// The tagged union. ALTERNATIVE ORDER IS LOAD-BEARING: index N must be the
/// payload for BlockType == N, because that integer is what gets persisted.
/// The static_asserts below make a mis-ordered edit a compile error.
using BlockContent = std::variant<ParagraphBlock,         // 0
                                  HeadingBlock,           // 1
                                  BulletedListItemBlock,  // 2
                                  NumberedListItemBlock,  // 3
                                  ChecklistItemBlock,     // 4
                                  CodeBlock,              // 5
                                  DividerBlock,           // 6
                                  QuoteBlock>;            // 7

static_assert(std::variant_size_v<BlockContent> ==
                  static_cast<std::size_t>(kFirstUnimplementedBlockType),
              "BlockContent must hold exactly the implemented BlockType values");
static_assert(std::is_same_v<std::variant_alternative_t<0, BlockContent>, ParagraphBlock>);
static_assert(std::is_same_v<std::variant_alternative_t<1, BlockContent>, HeadingBlock>);
static_assert(
    std::is_same_v<std::variant_alternative_t<2, BlockContent>, BulletedListItemBlock>);
static_assert(
    std::is_same_v<std::variant_alternative_t<3, BlockContent>, NumberedListItemBlock>);
static_assert(std::is_same_v<std::variant_alternative_t<4, BlockContent>, ChecklistItemBlock>);
static_assert(std::is_same_v<std::variant_alternative_t<5, BlockContent>, CodeBlock>);
static_assert(std::is_same_v<std::variant_alternative_t<6, BlockContent>, DividerBlock>);
static_assert(std::is_same_v<std::variant_alternative_t<7, BlockContent>, QuoteBlock>);

/// The type tag of a payload. Total, and cannot disagree with the variant.
[[nodiscard]] BlockType block_type_of(const BlockContent& content) noexcept;

/// Stable lowercase identifier ("paragraph", "bulleted_list_item", ...).
/// Matches the `type` strings database-design.md section 3.3 sketches. Never
/// null; returns "unknown" for an out-of-range integer cast into the enum.
[[nodiscard]] const char* block_type_name(BlockType type) noexcept;

[[nodiscard]] bool block_type_is_implemented(BlockType type) noexcept;

/// Why a declared-but-absent type is absent. Returns nullptr for an
/// implemented type. This is the machine-readable form of the spec section 5
/// "NOT IMPLEMENTED" marker, so the reason cannot rot in a comment.
[[nodiscard]] const char* block_type_not_implemented_reason(BlockType type) noexcept;

/// Plain text of a payload, for previews and (later) the FTS `text_cache`
/// column. Empty for structural blocks such as a divider. This is user
/// content -- never log it (spec section 34).
[[nodiscard]] std::string_view block_plain_text(const BlockContent& content) noexcept;

/// Validate a payload against its own type's rules: size limits, no embedded
/// NUL (it would truncate at the first C API boundary), heading level range.
[[nodiscard]] Status validate_block_content(const BlockContent& content) noexcept;

// --- the block -------------------------------------------------------------

/// One block, with its children materialised.
///
/// `children` is populated by a full-tree read (`BlockStore::get_page`); the
/// shallow accessors leave it empty. The recursion is safe because every path
/// that can create or load a tree enforces kMaxBlockDepth.
struct Block final {
    BlockId      id;
    /// Nil means "direct child of the page", i.e. depth 1.
    BlockId      parent_id;
    PageId       page_id;
    BlockContent content;
    std::int64_t created_at_ms  = 0;
    std::int64_t modified_at_ms = 0;
    std::vector<Block> children;
};

/// Depth of the deepest node in `blocks`, counting a top-level block as 1.
/// Stops counting at `kMaxBlockDepth + 1`, so it is safe to call on a tree
/// that has not been validated yet.
[[nodiscard]] std::size_t block_tree_depth(const std::vector<Block>& blocks) noexcept;

/// Total node count of a block forest.
[[nodiscard]] std::size_t block_tree_size(const std::vector<Block>& blocks) noexcept;

}  // namespace uapp::blocks

#endif  // UAPP_CORE_BLOCKS_BLOCK_H
