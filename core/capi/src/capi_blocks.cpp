// SPDX-License-Identifier: Apache-2.0
//
// core/capi/src/capi_blocks.cpp — the C99 face of uapp::blocks::DocumentService.
//
// The house rules from capi_notes.cpp apply here unchanged, and are repeated
// because they are the contract rather than a style preference:
//
//   1. Validate every pointer before dereferencing it. The caller is JNI or
//      Swift interop; "the binding would never pass null" is not an argument
//      that survives contact with a generated wrapper.
//   2. Wrap the body in UAPP_CAPI_GUARD_*, so nothing can unwind out of a C
//      frame. See capi_internal.h for why this is not theatre under
//      -fno-exceptions.
//   3. Write out-parameters only on success.
//   4. Return a status. Never a sentinel, never a nullable pointer as the
//      error signal.
//
// This layer adds no behaviour. There is exactly one translation step with any
// substance in it — std::variant <-> the flat tagged struct — and both halves
// of it live in this file's anonymous namespace with the argument for the
// shape recorded in uapp/capi/uapp_blocks.h.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "capi_internal.h"
#include "uapp/blocks/block.h"
#include "uapp/blocks/block_store.h"
#include "uapp/blocks/document_service.h"
#include "uapp/blocks/page.h"
#include "uapp/capi/uapp_blocks.h"
#include "uapp/capi/uapp_capi.h"
#include "uapp/object_id.h"
#include "uapp/ports.h"
#include "uapp/status.h"
#include "uapp/storage/sqlite/sqlite_block_store.h"

namespace {

using uapp::Status;
using uapp::blocks::Block;
using uapp::blocks::BlockContent;
using uapp::blocks::BlockId;
using uapp::blocks::BlockType;
using uapp::blocks::Page;
using uapp::blocks::PageId;
using uapp::capi::dup_string;
using uapp::capi::kDocServiceMagic;
using uapp::capi::kPageMagic;
using uapp::capi::to_c_status;

/// The C enum and the C++ enum must agree, forever — the same discipline the
/// status codes get in capi_common.cpp, and for the same reason: these values
/// are PERSISTED in the `block.type` column, so a renumbering must be a
/// compile error here rather than an Android client that renders a heading as
/// a divider one release later.
#define UAPP_CAPI_ASSERT_BLOCK_TYPE(cpp_name, c_name)                               \
    static_assert(static_cast<std::int32_t>(BlockType::cpp_name) ==                 \
                      static_cast<std::int32_t>(c_name),                            \
                  "uapp_block_type_t drifted from uapp::blocks::BlockType for " #cpp_name)

static_assert(sizeof(uapp_block_type_t) == 4u, "uapp_block_type_t must be exactly 32 bits");

UAPP_CAPI_ASSERT_BLOCK_TYPE(Paragraph, UAPP_BLOCK_TYPE_PARAGRAPH);
UAPP_CAPI_ASSERT_BLOCK_TYPE(Heading, UAPP_BLOCK_TYPE_HEADING);
UAPP_CAPI_ASSERT_BLOCK_TYPE(BulletedListItem, UAPP_BLOCK_TYPE_BULLETED_LIST_ITEM);
UAPP_CAPI_ASSERT_BLOCK_TYPE(NumberedListItem, UAPP_BLOCK_TYPE_NUMBERED_LIST_ITEM);
UAPP_CAPI_ASSERT_BLOCK_TYPE(ChecklistItem, UAPP_BLOCK_TYPE_CHECKLIST_ITEM);
UAPP_CAPI_ASSERT_BLOCK_TYPE(Code, UAPP_BLOCK_TYPE_CODE);
UAPP_CAPI_ASSERT_BLOCK_TYPE(Divider, UAPP_BLOCK_TYPE_DIVIDER);
UAPP_CAPI_ASSERT_BLOCK_TYPE(Quote, UAPP_BLOCK_TYPE_QUOTE);
UAPP_CAPI_ASSERT_BLOCK_TYPE(Toggle, UAPP_BLOCK_TYPE_TOGGLE);
UAPP_CAPI_ASSERT_BLOCK_TYPE(Callout, UAPP_BLOCK_TYPE_CALLOUT);
UAPP_CAPI_ASSERT_BLOCK_TYPE(Table, UAPP_BLOCK_TYPE_TABLE);
UAPP_CAPI_ASSERT_BLOCK_TYPE(Image, UAPP_BLOCK_TYPE_IMAGE);
UAPP_CAPI_ASSERT_BLOCK_TYPE(Video, UAPP_BLOCK_TYPE_VIDEO);
UAPP_CAPI_ASSERT_BLOCK_TYPE(Audio, UAPP_BLOCK_TYPE_AUDIO);
UAPP_CAPI_ASSERT_BLOCK_TYPE(File, UAPP_BLOCK_TYPE_FILE);
UAPP_CAPI_ASSERT_BLOCK_TYPE(Equation, UAPP_BLOCK_TYPE_EQUATION);
UAPP_CAPI_ASSERT_BLOCK_TYPE(Embed, UAPP_BLOCK_TYPE_EMBED);
UAPP_CAPI_ASSERT_BLOCK_TYPE(Database, UAPP_BLOCK_TYPE_DATABASE);
UAPP_CAPI_ASSERT_BLOCK_TYPE(Bookmark, UAPP_BLOCK_TYPE_BOOKMARK);

#undef UAPP_CAPI_ASSERT_BLOCK_TYPE

// kAppendPosition and UAPP_BLOCK_POSITION_APPEND are the same sentinel, and
// the ABI promises they are. If the core ever picks a different one, this is a
// compile error rather than a silent "append" that means "position 2^64-1".
static_assert(uapp::blocks::kAppendPosition == UAPP_BLOCK_POSITION_APPEND,
              "UAPP_BLOCK_POSITION_APPEND must be uapp::blocks::kAppendPosition");

/// `magic` is the first member of every handle struct, deliberately: the check
/// below then rejects a wrong-type handle as well as a null or zeroed one.
template <typename Handle>
[[nodiscard]] bool handle_valid(const Handle* handle, std::uint32_t expected) noexcept {
    return handle != nullptr && handle->magic == expected;
}

/// One block of a page snapshot, FLATTENED so that every string the ABI hands
/// out is already materialised and stable for the lifetime of the page handle.
///
/// `text` and `language` are always present (empty where the type has no such
/// field) so that an accessor can hand back a non-NULL borrowed pointer
/// without a special case — see the ON OUTPUT paragraph in uapp_blocks.h.
struct CapiBlockRow final {
    std::string  id_hex;
    std::string  parent_id_hex;
    std::string  text;
    std::string  language;
    std::int32_t type           = 0;
    std::int32_t heading_level  = 0;
    std::int32_t checked        = UAPP_FALSE;
    std::size_t  depth          = 0;
    std::size_t  child_count    = 0;
    std::int64_t created_at_ms  = 0;
    std::int64_t modified_at_ms = 0;
};

/// Is `value` an integer that names a block type at all? Told apart from "a
/// block type that is not implemented": the first is a caller bug
/// (UAPP_STATUS_INVALID_ARGUMENT), the second is a product state
/// (UAPP_STATUS_BLOCK_TYPE_UNSUPPORTED), and a UI must react differently.
[[nodiscard]] bool block_type_in_range(uapp_block_type_t type) noexcept {
    const auto value = static_cast<std::int32_t>(type);
    return value >= 0 && value < uapp::blocks::kBlockTypeCount;
}

/// Flatten one payload into the row's four content fields.
void fill_row_content(const BlockContent& content, CapiBlockRow& row) {
    row.type = static_cast<std::int32_t>(uapp::blocks::block_type_of(content));

    if (const auto* p = std::get_if<uapp::blocks::HeadingBlock>(&content)) {
        row.text          = p->text;
        row.heading_level = p->level;
        return;
    }
    if (const auto* p = std::get_if<uapp::blocks::ChecklistItemBlock>(&content)) {
        row.text    = p->text;
        row.checked = p->checked ? UAPP_TRUE : UAPP_FALSE;
        return;
    }
    if (const auto* p = std::get_if<uapp::blocks::CodeBlock>(&content)) {
        row.text     = p->code;
        row.language = p->language;
        return;
    }
    // Paragraph, bulleted, numbered, quote — text only. Divider — nothing;
    // block_plain_text() returns empty for it, which is the right answer and
    // keeps this branch from needing to know which types are text-bearing.
    row.text = std::string(uapp::blocks::block_plain_text(content));
}

/// Translate the flat tagged struct into the core's variant.
///
/// Ordering of the checks is the contract, and is the one place this function
/// decides anything:
///   1. struct_size, so a caller from an older build cannot be misread.
///   2. is this an integer that names a block type?  -> INVALID_ARGUMENT
///   3. is that block type implemented in this build? -> BLOCK_TYPE_UNSUPPORTED
///   4. is the required text pointer present?         -> INVALID_ARGUMENT
///   5. everything else — heading level range, text size, control bytes in a
///      language tag — is left to validate_block_content(), so the C ABI and
///      the C++ API cannot disagree about what a legal payload is.
[[nodiscard]] uapp_status_t to_block_content(const uapp_block_content_t* content,
                                             BlockContent& out) noexcept {
    if (content == nullptr || content->struct_size < sizeof(uapp_block_content_t)) {
        return UAPP_STATUS_INVALID_ARGUMENT;
    }
    if (!block_type_in_range(content->type)) {
        return UAPP_STATUS_INVALID_ARGUMENT;
    }

    const auto type = static_cast<BlockType>(static_cast<std::int32_t>(content->type));
    if (!uapp::blocks::block_type_is_implemented(type)) {
        return UAPP_STATUS_BLOCK_TYPE_UNSUPPORTED;
    }

    // A divider has no text, so NULL is fine there and only there. "" is a
    // legal block everywhere — an empty paragraph is what pressing Enter makes.
    if (type != BlockType::Divider && content->text == nullptr) {
        return UAPP_STATUS_INVALID_ARGUMENT;
    }

    switch (type) {
        case BlockType::Paragraph: {
            uapp::blocks::ParagraphBlock payload;
            payload.text = content->text;
            out          = std::move(payload);
            break;
        }
        case BlockType::Heading: {
            uapp::blocks::HeadingBlock payload;
            payload.text  = content->text;
            payload.level = static_cast<int>(content->heading_level);
            out           = std::move(payload);
            break;
        }
        case BlockType::BulletedListItem: {
            uapp::blocks::BulletedListItemBlock payload;
            payload.text = content->text;
            out          = std::move(payload);
            break;
        }
        case BlockType::NumberedListItem: {
            uapp::blocks::NumberedListItemBlock payload;
            payload.text = content->text;
            out          = std::move(payload);
            break;
        }
        case BlockType::ChecklistItem: {
            uapp::blocks::ChecklistItemBlock payload;
            payload.text    = content->text;
            payload.checked = (content->checked != UAPP_FALSE);
            out             = std::move(payload);
            break;
        }
        case BlockType::Code: {
            uapp::blocks::CodeBlock payload;
            payload.code = content->text;
            // NULL language means "no language", which is a legal code block.
            if (content->language != nullptr) {
                payload.language = content->language;
            }
            out = std::move(payload);
            break;
        }
        case BlockType::Divider:
            out = uapp::blocks::DividerBlock{};
            break;
        case BlockType::Quote: {
            uapp::blocks::QuoteBlock payload;
            payload.text = content->text;
            out          = std::move(payload);
            break;
        }

        // Unreachable: block_type_is_implemented() rejected all eleven above.
        // Listed rather than defaulted so that implementing one in the core
        // without teaching this translator is a compile error — the same
        // tripwire to_c_status uses, applied to block types.
        case BlockType::Toggle:
        case BlockType::Callout:
        case BlockType::Table:
        case BlockType::Image:
        case BlockType::Video:
        case BlockType::Audio:
        case BlockType::File:
        case BlockType::Equation:
        case BlockType::Embed:
        case BlockType::Database:
        case BlockType::Bookmark:
            return UAPP_STATUS_BLOCK_TYPE_UNSUPPORTED;
    }

    // The core's own validator, not a second opinion: a heading level of 9, a
    // 2 MiB paragraph and a control byte in a language tag are all rejected
    // here by exactly the code the C++ API uses.
    const Status valid = uapp::blocks::validate_block_content(out);
    if (!uapp::is_ok(valid)) {
        return to_c_status(valid);
    }
    return UAPP_STATUS_OK;
}

/// Parse an id that is REQUIRED. NULL is a caller bug, not a nil id.
template <typename Id>
[[nodiscard]] uapp_status_t parse_required_id(const char* hex, Id& out) noexcept {
    if (hex == nullptr) {
        return UAPP_STATUS_INVALID_ARGUMENT;
    }
    return to_c_status(Id::parse(std::string_view(hex), out));
}

/// Parse an id in a PARENT position, where absence has a meaning: NULL and the
/// nil id both mean "top level". Anything else must be well formed.
[[nodiscard]] uapp_status_t parse_parent_id(const char* hex, BlockId& out) noexcept {
    if (hex == nullptr) {
        out = BlockId();
        return UAPP_STATUS_OK;
    }
    return to_c_status(BlockId::parse(std::string_view(hex), out));
}

}  // namespace

// --- handle definitions -----------------------------------------------------
//
// Opaque to every consumer; the tags match the typedefs in uapp_blocks.h.

struct uapp_document_service final {
    std::uint32_t magic = kDocServiceMagic;

    // Declaration order is lifetime order. DocumentService holds references to
    // the other three, so it must be destroyed first — which, members being
    // destroyed in reverse order of declaration, it is.
    std::unique_ptr<uapp::blocks::BlockStore>      store;
    std::unique_ptr<uapp::Clock>                   clock;
    std::unique_ptr<uapp::IdSource>                ids;
    std::unique_ptr<uapp::blocks::DocumentService> service;
};

struct uapp_page final {
    std::uint32_t magic = kPageMagic;

    std::string  id_hex;
    std::string  title;
    std::int64_t created_at_ms  = 0;
    std::int64_t modified_at_ms = 0;

    /// Top-level block count, kept separately because `rows` is flat and
    /// counting depth-1 entries on every call would be O(n).
    std::size_t top_level_count = 0;

    /// The whole tree in PRE-ORDER (document order). See the long argument in
    /// uapp_blocks.h for why the tree is flattened at this boundary.
    std::vector<CapiBlockRow> rows;
};

namespace {

/// Flatten a page's block forest into pre-order rows.
///
/// ITERATIVE, with an explicit stack, for the same reason every tree walk in
/// core/blocks is: the tree comes out of a database file, which spec section 28
/// treats as attacker-reachable, and a recursive walk over user-controlled
/// nesting is a stack-exhaustion vector. The depth cap makes recursion *likely*
/// safe; not recursing makes it *certainly* safe, at the cost of six lines.
void flatten_tree(const std::vector<Block>& top_level, uapp_page& out) {
    out.top_level_count = top_level.size();
    out.rows.reserve(uapp::blocks::block_tree_size(top_level));

    struct Frame final {
        const std::vector<Block>* level;
        std::size_t               index;
        std::size_t               depth;
    };

    std::vector<Frame> stack;
    stack.push_back({&top_level, 0u, 1u});

    while (!stack.empty()) {
        const std::vector<Block>* level = stack.back().level;
        const std::size_t         index = stack.back().index;
        const std::size_t         depth = stack.back().depth;

        if (index >= level->size()) {
            stack.pop_back();
            continue;
        }
        stack.back().index = index + 1u;

        const Block& block = (*level)[index];

        CapiBlockRow row;
        row.id_hex = block.id.to_hex();
        // A nil parent renders as the 32-zero nil id rather than as an empty
        // string, so uapp_page_get_block_parent_id_hex() can promise a
        // non-NULL, fixed-length value for every row.
        row.parent_id_hex  = block.parent_id.to_hex();
        row.depth          = depth;
        row.child_count    = block.children.size();
        row.created_at_ms  = block.created_at_ms;
        row.modified_at_ms = block.modified_at_ms;
        fill_row_content(block.content, row);
        out.rows.push_back(std::move(row));

        if (!block.children.empty()) {
            // Push AFTER the row is emitted: pre-order is what makes "a parent
            // always precedes its descendants" true, which is what lets a UI
            // indent by depth with no lookahead.
            stack.push_back({&block.children, 0u, depth + 1u});
        }
    }
}

/// Build a page handle from a core Page. Returns nullptr on allocation
/// failure; the caller maps that to UAPP_STATUS_OUT_OF_MEMORY.
[[nodiscard]] uapp_page* make_page_handle(const Page& page) noexcept {
    uapp_page* handle = new (std::nothrow) uapp_page();
    if (handle == nullptr) {
        return nullptr;
    }
    handle->id_hex         = page.id.to_hex();
    handle->title          = page.title;
    handle->created_at_ms  = page.created_at_ms;
    handle->modified_at_ms = page.modified_at_ms;
    flatten_tree(page.blocks, *handle);
    return handle;
}

/// Shared prologue for every row accessor: validate the handle and the index.
[[nodiscard]] uapp_status_t row_at(const uapp_page* page,
                                   std::size_t index,
                                   const CapiBlockRow*& out) noexcept {
    if (!handle_valid(page, kPageMagic)) {
        return UAPP_STATUS_INVALID_ARGUMENT;
    }
    if (index >= page->rows.size()) {
        // An out-of-range index is a caller bug, not a missing block. Saying
        // NOT_FOUND here would make a loop-bound error look like data loss.
        return UAPP_STATUS_INVALID_ARGUMENT;
    }
    out = &page->rows[index];
    return UAPP_STATUS_OK;
}

/// Shared prologue for the mutating service calls that take one id.
[[nodiscard]] uapp_status_t service_and_block(const uapp_document_service* service,
                                              const char* block_id_hex,
                                              BlockId& out) noexcept {
    if (!handle_valid(service, kDocServiceMagic)) {
        return UAPP_STATUS_INVALID_ARGUMENT;
    }
    return parse_required_id(block_id_hex, out);
}

}  // namespace

// ---------------------------------------------------------------------------
// Exported surface
// ---------------------------------------------------------------------------

extern "C" {

// --- block types ------------------------------------------------------------

const char* uapp_block_type_name(uapp_block_type_t type) {
    UAPP_CAPI_GUARD_BEGIN
        if (!block_type_in_range(type)) {
            return "unknown";
        }
        return uapp::blocks::block_type_name(
            static_cast<BlockType>(static_cast<std::int32_t>(type)));
    UAPP_CAPI_GUARD_END_FALLBACK("unknown")
}

uapp_bool_t uapp_block_type_is_implemented(uapp_block_type_t type) {
    UAPP_CAPI_GUARD_BEGIN
        if (!block_type_in_range(type)) {
            return UAPP_FALSE;
        }
        return uapp::blocks::block_type_is_implemented(
                   static_cast<BlockType>(static_cast<std::int32_t>(type)))
                   ? UAPP_TRUE
                   : UAPP_FALSE;
    UAPP_CAPI_GUARD_END_FALLBACK(UAPP_FALSE)
}

const char* uapp_block_type_not_implemented_reason(uapp_block_type_t type) {
    UAPP_CAPI_GUARD_BEGIN
        // NULL for an integer that is not a block type at all, NOT the core's
        // "unrecognised block type" string: the question "why is this type
        // missing" is only meaningful about a type that exists.
        if (!block_type_in_range(type)) {
            return nullptr;
        }
        return uapp::blocks::block_type_not_implemented_reason(
            static_cast<BlockType>(static_cast<std::int32_t>(type)));
    UAPP_CAPI_GUARD_END_FALLBACK(nullptr)
}

// --- content and limits -----------------------------------------------------

uapp_status_t uapp_block_content_init(uapp_block_type_t type,
                                      uapp_block_content_t* out_content) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_content == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        // `type` is deliberately NOT validated: a UI may hold a struct for a
        // type it cannot yet save, and the mutating call is what reports that.
        out_content->struct_size = static_cast<std::uint32_t>(sizeof(uapp_block_content_t));
        out_content->type        = type;
        out_content->text        = nullptr;
        out_content->language    = nullptr;
        // 1 rather than 0 for a heading, so an uninitialised-looking zero can
        // never be mistaken for a valid level.
        out_content->heading_level =
            (type == UAPP_BLOCK_TYPE_HEADING) ? uapp::blocks::kMinHeadingLevel : 0;
        out_content->checked = UAPP_FALSE;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_block_get_limits(uapp_block_limits_t* out_limits) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_limits == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        // The leading-struct_size rule (api-design.md §10 rule 2). Smaller than
        // this build's struct cannot be served — we would write past the end of
        // the caller's object. Larger is fine: fill what we know, leave the
        // tail alone.
        if (out_limits->struct_size < sizeof(uapp_block_limits_t)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        out_limits->id_hex_length           = uapp::kObjectIdHexChars;
        out_limits->max_page_title_bytes    = uapp::blocks::kMaxPageTitleBytes;
        out_limits->max_block_text_bytes    = uapp::blocks::kMaxBlockTextBytes;
        out_limits->max_code_language_bytes = uapp::blocks::kMaxCodeLanguageBytes;
        out_limits->max_block_depth         = uapp::blocks::kMaxBlockDepth;
        out_limits->max_blocks_per_page     = uapp::blocks::kMaxBlocksPerPage;
        out_limits->min_heading_level       = uapp::blocks::kMinHeadingLevel;
        out_limits->max_heading_level       = uapp::blocks::kMaxHeadingLevel;
        out_limits->block_type_count        = uapp::blocks::kBlockTypeCount;
        out_limits->implemented_block_type_count =
            uapp::blocks::kFirstUnimplementedBlockType;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- service lifecycle ------------------------------------------------------

uapp_status_t uapp_document_service_config_init(uapp_document_service_config_t* out_config) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_config == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        out_config->struct_size =
            static_cast<std::uint32_t>(sizeof(uapp_document_service_config_t));
        // No default path, for the reason uapp_note_service_config_init gives:
        // inventing one would put a database somewhere the caller did not ask.
        out_config->database_path   = nullptr;
        out_config->read_only       = UAPP_FALSE;
        out_config->busy_timeout_ms = 5000;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_document_service_open(const uapp_document_service_config_t* config,
                                         uapp_document_service_t** out_service) {
    UAPP_CAPI_GUARD_BEGIN
        // NOTE the absence of a `*out_service = nullptr` prologue: out-parameters
        // are not written on a failure path. See the house rules above.
        if (out_service == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (config == nullptr || config->struct_size < sizeof(uapp_document_service_config_t) ||
            config->database_path == nullptr || config->busy_timeout_ms < 0) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        std::unique_ptr<uapp_document_service> handle(new (std::nothrow)
                                                          uapp_document_service());
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }

        uapp::storage::sqlite::SqliteBlockStoreOptions options;
        options.path            = config->database_path;
        options.read_only       = (config->read_only != UAPP_FALSE);
        options.busy_timeout_ms = static_cast<int>(config->busy_timeout_ms);

        const Status opened =
            uapp::storage::sqlite::open_sqlite_block_store(options, handle->store);
        if (!uapp::is_ok(opened)) {
            return to_c_status(opened);
        }

        handle->clock = uapp::make_system_clock();
        handle->ids   = uapp::make_random_id_source();
        if (handle->clock == nullptr || handle->ids == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }

        handle->service.reset(new (std::nothrow) uapp::blocks::DocumentService(
            *handle->store, *handle->clock, *handle->ids));
        if (handle->service == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }

        *out_service = handle.release();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

void uapp_document_service_close(uapp_document_service_t** service) {
    UAPP_CAPI_GUARD_BEGIN
        if (service == nullptr || *service == nullptr) {
            return;
        }
        if (!handle_valid(*service, kDocServiceMagic)) {
            // Not ours, or already scrubbed. Deleting it would be worse than
            // leaking it, and there is no channel to report on. Leave it.
            return;
        }
        (*service)->magic = 0u;
        delete *service;
        *service = nullptr;
    UAPP_CAPI_GUARD_END_VOID
}

// --- pages ------------------------------------------------------------------

uapp_status_t uapp_document_service_create_page(uapp_document_service_t* service,
                                                const char* title,
                                                uapp_page_t** out_page) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_page == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(service, kDocServiceMagic) || title == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        Page created;
        const Status status = service->service->create_page(std::string_view(title), created);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }

        uapp_page* handle = make_page_handle(created);
        if (handle == nullptr) {
            // The page IS persisted; only the handle failed. Reporting OOM
            // rather than success is right — the caller got no id back — and
            // the page remains reachable by search or by page_count.
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        *out_page = handle;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_document_service_open_page(uapp_document_service_t* service,
                                              const char* page_id_hex,
                                              uapp_page_t** out_page) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_page == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        if (!handle_valid(service, kDocServiceMagic)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        PageId id;
        const uapp_status_t parsed = parse_required_id(page_id_hex, id);
        if (parsed != UAPP_STATUS_OK) {
            return parsed;
        }

        Page loaded;
        const Status status = service->service->open_page(id, loaded);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }

        uapp_page* handle = make_page_handle(loaded);
        if (handle == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        *out_page = handle;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_document_service_rename_page(uapp_document_service_t* service,
                                                const char* page_id_hex,
                                                const char* title) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(service, kDocServiceMagic) || title == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        PageId id;
        const uapp_status_t parsed = parse_required_id(page_id_hex, id);
        if (parsed != UAPP_STATUS_OK) {
            return parsed;
        }
        return to_c_status(service->service->rename_page(id, std::string_view(title)));
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_document_service_delete_page(uapp_document_service_t* service,
                                                const char* page_id_hex) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(service, kDocServiceMagic)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        PageId id;
        const uapp_status_t parsed = parse_required_id(page_id_hex, id);
        if (parsed != UAPP_STATUS_OK) {
            return parsed;
        }
        return to_c_status(service->service->delete_page(id));
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_document_service_page_count(uapp_document_service_t* service,
                                               size_t* out_count) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(service, kDocServiceMagic) || out_count == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        std::size_t count = 0;
        const Status status = service->service->page_count(count);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        *out_count = count;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- blocks -----------------------------------------------------------------

uapp_status_t uapp_document_service_add_block(uapp_document_service_t* service,
                                              const char* page_id_hex,
                                              const char* parent_id_hex,
                                              size_t position,
                                              const uapp_block_content_t* content,
                                              char** out_block_id_hex) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(service, kDocServiceMagic)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }

        PageId page_id;
        const uapp_status_t page_parsed = parse_required_id(page_id_hex, page_id);
        if (page_parsed != UAPP_STATUS_OK) {
            return page_parsed;
        }

        BlockId parent_id;
        const uapp_status_t parent_parsed = parse_parent_id(parent_id_hex, parent_id);
        if (parent_parsed != UAPP_STATUS_OK) {
            return parent_parsed;
        }

        BlockContent payload;
        const uapp_status_t translated = to_block_content(content, payload);
        if (translated != UAPP_STATUS_OK) {
            return translated;
        }

        Block created;
        const Status status =
            service->service->add_block(page_id, parent_id, position, payload, created);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }

        // The id is handed back only if asked for. Doing the allocation after
        // the write means a caller who ignores the id never pays for it, and a
        // failed allocation here does not leave a half-done mutation — the
        // block is stored either way, which is why this is OUT_OF_MEMORY and
        // not a rollback.
        if (out_block_id_hex != nullptr) {
            const std::string hex = created.id.to_hex();
            char* copy = dup_string(hex.c_str(), hex.size());
            if (copy == nullptr) {
                return UAPP_STATUS_OUT_OF_MEMORY;
            }
            *out_block_id_hex = copy;
        }
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_document_service_update_block(uapp_document_service_t* service,
                                                 const char* block_id_hex,
                                                 const uapp_block_content_t* content) {
    UAPP_CAPI_GUARD_BEGIN
        BlockId id;
        const uapp_status_t prologue = service_and_block(service, block_id_hex, id);
        if (prologue != UAPP_STATUS_OK) {
            return prologue;
        }
        BlockContent payload;
        const uapp_status_t translated = to_block_content(content, payload);
        if (translated != UAPP_STATUS_OK) {
            return translated;
        }
        return to_c_status(service->service->update_block(id, payload));
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_document_service_delete_block(uapp_document_service_t* service,
                                                 const char* block_id_hex) {
    UAPP_CAPI_GUARD_BEGIN
        BlockId id;
        const uapp_status_t prologue = service_and_block(service, block_id_hex, id);
        if (prologue != UAPP_STATUS_OK) {
            return prologue;
        }
        return to_c_status(service->service->delete_block(id));
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_document_service_reorder_block(uapp_document_service_t* service,
                                                  const char* block_id_hex,
                                                  size_t new_position) {
    UAPP_CAPI_GUARD_BEGIN
        BlockId id;
        const uapp_status_t prologue = service_and_block(service, block_id_hex, id);
        if (prologue != UAPP_STATUS_OK) {
            return prologue;
        }
        return to_c_status(service->service->reorder_block(id, new_position));
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_document_service_move_block(uapp_document_service_t* service,
                                               const char* block_id_hex,
                                               const char* new_parent_id_hex,
                                               size_t new_position) {
    UAPP_CAPI_GUARD_BEGIN
        BlockId id;
        const uapp_status_t prologue = service_and_block(service, block_id_hex, id);
        if (prologue != UAPP_STATUS_OK) {
            return prologue;
        }
        BlockId new_parent;
        const uapp_status_t parent_parsed = parse_parent_id(new_parent_id_hex, new_parent);
        if (parent_parsed != UAPP_STATUS_OK) {
            return parent_parsed;
        }
        return to_c_status(service->service->move_block(id, new_parent, new_position));
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_document_service_block_count(uapp_document_service_t* service,
                                                const char* page_id_hex,
                                                size_t* out_count) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(service, kDocServiceMagic) || out_count == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        PageId id;
        const uapp_status_t parsed = parse_required_id(page_id_hex, id);
        if (parsed != UAPP_STATUS_OK) {
            return parsed;
        }
        std::size_t count = 0;
        const Status status = service->service->block_count(id, count);
        if (!uapp::is_ok(status)) {
            return to_c_status(status);
        }
        *out_count = count;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- page handle: metadata --------------------------------------------------

uapp_status_t uapp_page_get_id_hex(const uapp_page_t* page, const char** out_id_hex) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(page, kPageMagic) || out_id_hex == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        *out_id_hex = page->id_hex.c_str();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_page_get_title(const uapp_page_t* page, const char** out_title) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(page, kPageMagic) || out_title == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        *out_title = page->title.c_str();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_page_get_title_length(const uapp_page_t* page, size_t* out_length) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(page, kPageMagic) || out_length == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        *out_length = page->title.size();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_page_get_timestamps(const uapp_page_t* page,
                                       int64_t* out_created_at_ms,
                                       int64_t* out_modified_at_ms) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(page, kPageMagic)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        // Both out-parameters are optional; a caller wanting only one should
        // not have to invent storage for the other.
        if (out_created_at_ms != nullptr) {
            *out_created_at_ms = page->created_at_ms;
        }
        if (out_modified_at_ms != nullptr) {
            *out_modified_at_ms = page->modified_at_ms;
        }
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_page_copy_id_hex(const uapp_page_t* page, char** out_id_hex) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(page, kPageMagic) || out_id_hex == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        char* copy = dup_string(page->id_hex.c_str(), page->id_hex.size());
        if (copy == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        *out_id_hex = copy;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_page_copy_title(const uapp_page_t* page, char** out_title) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(page, kPageMagic) || out_title == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        char* copy = dup_string(page->title.c_str(), page->title.size());
        if (copy == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        *out_title = copy;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

// --- page handle: the flattened block tree ----------------------------------

uapp_status_t uapp_page_get_block_count(const uapp_page_t* page, size_t* out_count) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(page, kPageMagic) || out_count == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        *out_count = page->rows.size();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_page_get_top_level_block_count(const uapp_page_t* page, size_t* out_count) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(page, kPageMagic) || out_count == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        *out_count = page->top_level_count;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_page_get_block_id_hex(const uapp_page_t* page,
                                         size_t index,
                                         const char** out_id_hex) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_id_hex == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const CapiBlockRow* row = nullptr;
        const uapp_status_t status = row_at(page, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        *out_id_hex = row->id_hex.c_str();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_page_get_block_parent_id_hex(const uapp_page_t* page,
                                                size_t index,
                                                const char** out_parent_id_hex) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_parent_id_hex == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const CapiBlockRow* row = nullptr;
        const uapp_status_t status = row_at(page, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        *out_parent_id_hex = row->parent_id_hex.c_str();
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_page_get_block_type(const uapp_page_t* page,
                                       size_t index,
                                       uapp_block_type_t* out_type) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_type == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const CapiBlockRow* row = nullptr;
        const uapp_status_t status = row_at(page, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        *out_type = static_cast<uapp_block_type_t>(row->type);
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_page_get_block_depth(const uapp_page_t* page,
                                        size_t index,
                                        size_t* out_depth) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_depth == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const CapiBlockRow* row = nullptr;
        const uapp_status_t status = row_at(page, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        *out_depth = row->depth;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_page_get_block_child_count(const uapp_page_t* page,
                                              size_t index,
                                              size_t* out_child_count) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_child_count == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const CapiBlockRow* row = nullptr;
        const uapp_status_t status = row_at(page, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        *out_child_count = row->child_count;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_page_get_block_content(const uapp_page_t* page,
                                          size_t index,
                                          uapp_block_content_t* out_content) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_content == nullptr ||
            out_content->struct_size < sizeof(uapp_block_content_t)) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const CapiBlockRow* row = nullptr;
        const uapp_status_t status = row_at(page, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        out_content->type          = static_cast<uapp_block_type_t>(row->type);
        // Always non-NULL, including for a divider, so a caller never needs a
        // NULL check before strlen/strcmp. See uapp_blocks.h, "ON OUTPUT".
        out_content->text          = row->text.c_str();
        out_content->language      = row->language.c_str();
        out_content->heading_level = row->heading_level;
        out_content->checked       = row->checked;
        // struct_size is left as the caller set it: it is an IN parameter here,
        // and overwriting it would destroy the information a future caller with
        // a larger struct needs on the next call.
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_page_get_block_timestamps(const uapp_page_t* page,
                                             size_t index,
                                             int64_t* out_created_at_ms,
                                             int64_t* out_modified_at_ms) {
    UAPP_CAPI_GUARD_BEGIN
        const CapiBlockRow* row = nullptr;
        const uapp_status_t status = row_at(page, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        if (out_created_at_ms != nullptr) {
            *out_created_at_ms = row->created_at_ms;
        }
        if (out_modified_at_ms != nullptr) {
            *out_modified_at_ms = row->modified_at_ms;
        }
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_page_copy_block_text(const uapp_page_t* page, size_t index, char** out_text) {
    UAPP_CAPI_GUARD_BEGIN
        if (out_text == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        const CapiBlockRow* row = nullptr;
        const uapp_status_t status = row_at(page, index, row);
        if (status != UAPP_STATUS_OK) {
            return status;
        }
        char* copy = dup_string(row->text.c_str(), row->text.size());
        if (copy == nullptr) {
            return UAPP_STATUS_OUT_OF_MEMORY;
        }
        *out_text = copy;
        return UAPP_STATUS_OK;
    UAPP_CAPI_GUARD_END
}

uapp_status_t uapp_page_find_block_index(const uapp_page_t* page,
                                         const char* block_id_hex,
                                         size_t* out_index) {
    UAPP_CAPI_GUARD_BEGIN
        if (!handle_valid(page, kPageMagic) || out_index == nullptr) {
            return UAPP_STATUS_INVALID_ARGUMENT;
        }
        // Parse rather than string-compare, so that an uppercase or otherwise
        // differently-spelled-but-equal id still matches, and so that a
        // malformed id is reported as malformed instead of as "not on this
        // page" — which are different problems for the caller.
        BlockId wanted;
        const uapp_status_t parsed = parse_required_id(block_id_hex, wanted);
        if (parsed != UAPP_STATUS_OK) {
            return parsed;
        }
        const std::string wanted_hex = wanted.to_hex();

        for (std::size_t i = 0; i < page->rows.size(); ++i) {
            if (page->rows[i].id_hex == wanted_hex) {
                *out_index = i;
                return UAPP_STATUS_OK;
            }
        }
        // A well-formed id that is not here is a real answer, not a caller bug:
        // the block may have been deleted or moved since this snapshot.
        return UAPP_STATUS_NOT_FOUND;
    UAPP_CAPI_GUARD_END
}

void uapp_page_free(uapp_page_t** page) {
    UAPP_CAPI_GUARD_BEGIN
        if (page == nullptr || *page == nullptr) {
            return;
        }
        if (!handle_valid(*page, kPageMagic)) {
            return;
        }
        (*page)->magic = 0u;
        delete *page;
        *page = nullptr;
    UAPP_CAPI_GUARD_END_VOID
}

}  // extern "C"
