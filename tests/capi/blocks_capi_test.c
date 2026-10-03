/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/capi/blocks_capi_test.c -- the page/block C ABI, exercised from C.
 *
 * C99, compiled by a C compiler at -std=c99 with -Wall -Wextra -Wpedantic
 * -Wconversion -Werror. That is not incidental: it is the test. If a C++-ism
 * lands in uapp_blocks.h -- a default argument, a `bool`, an anonymous struct
 * member, a cast only C++ accepts -- this file stops building.
 *
 * It includes the PUBLIC headers and the C test harness and nothing else from
 * the project. No test_framework.h (that is C++), no core header, no
 * CMake-injected path into src/. Whatever is not reachable from here is not
 * reachable from Kotlin or Swift either.
 *
 * Two groups, run as two separate CTest entries:
 *
 *   "slice: ..."     the document slice through the ABI -- create a page, add
 *                    one block of every implemented type, nest them, open the
 *                    page and verify the whole flattened tree, update, delete
 *                    with cascade, reorder, move, rename, delete the page,
 *                    persistence across a close/reopen, and two independent
 *                    services proving there is no shared global state.
 *
 *   "boundary: ..."  what happens when the caller is wrong. Null handles, a
 *                    handle of the wrong type (including a NOTE handle, which
 *                    is the mistake a generated binding actually makes), a
 *                    block-type discriminant outside the enum, stale row
 *                    indices, a move that would form a cycle, and struct_size
 *                    versioning. Every one of these would be a native crash
 *                    in a careless C API; every one must return a status.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uapp/capi/uapp_blocks.h"
#include "uapp/capi/uapp_capi.h"
#include "uapp/capi/uapp_notes.h"

#include "capi_test_harness.h"

/* --- shared helpers -------------------------------------------------------- */

/* Each call gets its OWN private database: ":memory:" is per-connection in
 * SQLite, which is what makes the independent-handles case below meaningful
 * rather than accidental. */
static uapp_status_t open_memory_service(uapp_document_service_t** out) {
    uapp_document_service_config_t config;
    const uapp_status_t init = uapp_document_service_config_init(&config);
    if (init != UAPP_STATUS_OK) {
        return init;
    }
    config.database_path = ":memory:";
    return uapp_document_service_open(&config, out);
}

static uapp_status_t open_file_service(const char* path, uapp_document_service_t** out) {
    uapp_document_service_config_t config;
    const uapp_status_t init = uapp_document_service_config_init(&config);
    if (init != UAPP_STATUS_OK) {
        return init;
    }
    config.database_path = path;
    return uapp_document_service_open(&config, out);
}

/* A text-bearing content struct, built the way the header says to: type first,
 * then the fields that type reads. */
static uapp_block_content_t text_content(uapp_block_type_t type, const char* text) {
    uapp_block_content_t content;
    memset(&content, 0, sizeof content);
    if (uapp_block_content_init(type, &content) != UAPP_STATUS_OK) {
        /* init only fails on a NULL pointer, which cannot happen here. Leave
         * struct_size at 0 so any use is rejected rather than misread. */
        return content;
    }
    content.text = text;
    return content;
}

/* Add a block and hand back its owned id string, or NULL. The caller releases
 * with uapp_free_string(). */
static char* add_text_block(uapp_document_service_t* service,
                            const char* page_id,
                            const char* parent_id,
                            size_t position,
                            uapp_block_type_t type,
                            const char* text) {
    uapp_block_content_t content = text_content(type, text);
    char* id = NULL;
    if (uapp_document_service_add_block(service, page_id, parent_id, position, &content, &id) !=
        UAPP_STATUS_OK) {
        return NULL;
    }
    return id;
}

/* ===========================================================================
 * The slice
 * =========================================================================== */

static void test_block_limits(void) {
    uapp_block_limits_t limits;

    memset(&limits, 0, sizeof limits);
    limits.struct_size = (uint32_t)sizeof limits;
    REQUIRE_OK(uapp_block_get_limits(&limits));

    CHECK(limits.id_hex_length == 32u);
    CHECK(limits.max_page_title_bytes > 0u);
    CHECK(limits.max_block_text_bytes > limits.max_page_title_bytes);
    CHECK(limits.max_code_language_bytes > 0u);
    CHECK(limits.max_block_depth > 1u);
    CHECK(limits.max_blocks_per_page > limits.max_block_depth);
    CHECK(limits.min_heading_level == 1);
    CHECK(limits.max_heading_level >= limits.min_heading_level);
    /* Nineteen types declared, eight of them built. Read at runtime rather
     * than hard-coded in a binding, so implementing a ninth does not need a
     * client release. */
    CHECK(limits.block_type_count == 19);
    CHECK(limits.implemented_block_type_count == 8);
    CHECK(limits.implemented_block_type_count < limits.block_type_count);
}

static void test_block_type_metadata(void) {
    int32_t i;

    /* The eight implemented types, by name. These strings are persisted-ish:
     * they are what a Markdown/JSON exporter round-trips through. */
    CHECK_STR(uapp_block_type_name(UAPP_BLOCK_TYPE_PARAGRAPH), "paragraph");
    CHECK_STR(uapp_block_type_name(UAPP_BLOCK_TYPE_HEADING), "heading");
    CHECK_STR(uapp_block_type_name(UAPP_BLOCK_TYPE_BULLETED_LIST_ITEM), "bulleted_list_item");
    CHECK_STR(uapp_block_type_name(UAPP_BLOCK_TYPE_NUMBERED_LIST_ITEM), "numbered_list_item");
    CHECK_STR(uapp_block_type_name(UAPP_BLOCK_TYPE_CHECKLIST_ITEM), "checklist_item");
    CHECK_STR(uapp_block_type_name(UAPP_BLOCK_TYPE_CODE), "code");
    CHECK_STR(uapp_block_type_name(UAPP_BLOCK_TYPE_DIVIDER), "divider");
    CHECK_STR(uapp_block_type_name(UAPP_BLOCK_TYPE_QUOTE), "quote");
    /* A declared-but-absent type still has a name -- a database can contain
     * one, so a client must be able to say what it found. */
    CHECK_STR(uapp_block_type_name(UAPP_BLOCK_TYPE_TABLE), "table");
    CHECK_STR(uapp_block_type_name(UAPP_BLOCK_TYPE_BOOKMARK), "bookmark");

    /* Totally defined for any integer, including one nobody defined. */
    CHECK_STR(uapp_block_type_name((uapp_block_type_t)19), "unknown");
    CHECK_STR(uapp_block_type_name((uapp_block_type_t)424242), "unknown");
    CHECK_STR(uapp_block_type_name((uapp_block_type_t)-7), "unknown");

    /* 0..7 implemented, 8..18 declared only, anything else not a type. */
    for (i = 0; i < 8; ++i) {
        CHECK(uapp_block_type_is_implemented((uapp_block_type_t)i) == UAPP_TRUE);
        CHECK(uapp_block_type_not_implemented_reason((uapp_block_type_t)i) == NULL);
    }
    for (i = 8; i < 19; ++i) {
        const char* reason = uapp_block_type_not_implemented_reason((uapp_block_type_t)i);
        CHECK(uapp_block_type_is_implemented((uapp_block_type_t)i) == UAPP_FALSE);
        /* The spec section 5 NOT IMPLEMENTED marker, machine-readable. */
        CHECK(reason != NULL);
        if (reason != NULL) {
            CHECK(strstr(reason, "NOT IMPLEMENTED") == reason);
        }
    }
    CHECK(uapp_block_type_is_implemented((uapp_block_type_t)19) == UAPP_FALSE);
    CHECK(uapp_block_type_is_implemented((uapp_block_type_t)424242) == UAPP_FALSE);
    /* "Why is 424242 missing" is not a meaningful question, so NULL rather
     * than a reason string about a type that does not exist. */
    CHECK(uapp_block_type_not_implemented_reason((uapp_block_type_t)424242) == NULL);
    CHECK(uapp_block_type_not_implemented_reason((uapp_block_type_t)-1) == NULL);
}

/*
 * The enum range pin, at BOTH ends.
 *
 * note_capi_test.c already casts 424242 into uapp_status_t and asserts it
 * yields "UNKNOWN_STATUS" rather than undefined behaviour -- that is what
 * UAPP_STATUS_FORCE_INT32 buys. What no test covered until ABI 0.2 is the
 * NEGATIVE half: an enum whose enumerators are all non-negative has the valid
 * range [0, 0x7FFFFFFF], so reading -1 out of one was still undefined, and
 * UBSan said so the moment this suite tried it on uapp_source_kind_t.
 *
 * Hence UAPP_*_FORCE_INT32_MIN on all three ABI enums, and hence this case:
 * the claim is "a C caller can cast ANY int32 in and merely reading it is
 * defined", and an untested half of a claim is not a claim.
 */
static void test_enum_range_is_pinned_both_ways(void) {
    static const int32_t k_hostile[6] = {-1, -100, -424242, 424242, INT32_MIN, INT32_MAX};
    size_t i;

    CHECK(sizeof(uapp_status_t) == 4u);
    CHECK(sizeof(uapp_block_type_t) == 4u);

    for (i = 0; i < 6u; ++i) {
        const uapp_status_t status = (uapp_status_t)k_hostile[i];
        const uapp_block_type_t type = (uapp_block_type_t)k_hostile[i];

        /* Never NULL, never a crash, never UB -- for any integer at all. */
        CHECK(uapp_status_code(status) != NULL);
        CHECK(uapp_status_message(status) != NULL);
        CHECK(uapp_status_is_ok(status) == UAPP_FALSE);
        CHECK(uapp_block_type_name(type) != NULL);
        CHECK(uapp_block_type_is_implemented(type) == UAPP_FALSE);
        CHECK(uapp_block_type_not_implemented_reason(type) == NULL);
    }

    /* The two negative sentinels are not statuses or types either. */
    CHECK(uapp_status_is_ok(UAPP_STATUS_FORCE_INT32_MIN) == UAPP_FALSE);
    CHECK_STR(uapp_status_code(UAPP_STATUS_FORCE_INT32_MIN), "UNKNOWN_STATUS");
    CHECK_STR(uapp_block_type_name(UAPP_BLOCK_TYPE_FORCE_INT32_MIN), "unknown");

    /* And the 5xx codes added in 0.2 do have real strings, which is the whole
     * reason they were minted instead of flattened to UNEXPECTED. */
    CHECK_STR(uapp_status_code(UAPP_STATUS_ID_MALFORMED), "ID_MALFORMED");
    CHECK_STR(uapp_status_code(UAPP_STATUS_BLOCK_CYCLE_DETECTED), "BLOCK_CYCLE_DETECTED");
    CHECK_STR(uapp_status_code(UAPP_STATUS_PAGE_TITLE_TOO_LONG), "PAGE_TITLE_TOO_LONG");
    CHECK(uapp_status_message(UAPP_STATUS_BLOCK_DEPTH_EXCEEDED)[0] != '\0');
    CHECK(uapp_status_message(UAPP_STATUS_BLOCK_POSITION_INVALID)[0] != '\0');
    /* None of them is UNEXPECTED any more. */
    CHECK(strcmp(uapp_status_code(UAPP_STATUS_BLOCK_MALFORMED), "UNEXPECTED") != 0);
    CHECK(strcmp(uapp_status_code(UAPP_STATUS_BLOCK_TYPE_UNSUPPORTED), "UNEXPECTED") != 0);

    /* ABI 0.2, and a 0.1 caller still runs: the bump is additive. */
    CHECK(uapp_abi_version_major() == 0u);
    CHECK(uapp_abi_version_minor() == 2u);
    CHECK_STR(uapp_abi_version_string(), "0.2");
    CHECK(UAPP_ABI_ASSERT_COMPATIBLE() == UAPP_TRUE);
    CHECK(uapp_abi_is_compatible(0u, 1u) == UAPP_TRUE);
    CHECK(uapp_abi_is_compatible(0u, 3u) == UAPP_FALSE);
}

static void test_content_init_defaults(void) {
    uapp_block_content_t content;

    memset(&content, 0xEE, sizeof content);
    REQUIRE_OK(uapp_block_content_init(UAPP_BLOCK_TYPE_PARAGRAPH, &content));
    CHECK(content.struct_size == (uint32_t)sizeof content);
    CHECK(content.type == UAPP_BLOCK_TYPE_PARAGRAPH);
    CHECK(content.text == NULL);
    CHECK(content.language == NULL);
    CHECK(content.heading_level == 0);
    CHECK(content.checked == UAPP_FALSE);

    /* A heading defaults to level 1, never 0: an uninitialised-looking zero
     * must not be mistakable for a valid level. */
    memset(&content, 0xEE, sizeof content);
    REQUIRE_OK(uapp_block_content_init(UAPP_BLOCK_TYPE_HEADING, &content));
    CHECK(content.heading_level == 1);

    /* init accepts a type it cannot save, so a UI can hold the struct. */
    REQUIRE_OK(uapp_block_content_init(UAPP_BLOCK_TYPE_TABLE, &content));
    CHECK(content.type == UAPP_BLOCK_TYPE_TABLE);
    REQUIRE_OK(uapp_block_content_init((uapp_block_type_t)424242, &content));
    CHECK(content.type == (uapp_block_type_t)424242);
}

/*
 * The core of the slice: a page with one block of every implemented type, two
 * of them nested, read back through the flattened pre-order view and verified
 * field by field.
 */
static void test_slice_page_with_every_block_type(void) {
    uapp_document_service_t* service = NULL;
    uapp_page_t* page = NULL;
    char* page_id = NULL;
    char* bullet_id = NULL;
    char* nested_id = NULL;
    const char* borrowed = NULL;
    size_t count = 0;
    size_t top_level = 0;

    REQUIRE_OK(open_memory_service(&service));

    REQUIRE_OK(uapp_document_service_page_count(service, &count));
    CHECK(count == 0u);

    /* --- CREATE PAGE ------------------------------------------------------ */
    REQUIRE_OK(uapp_document_service_create_page(service, "Data structures", &page));
    REQUIRE_OK(uapp_page_copy_id_hex(page, &page_id));
    CHECK(page_id != NULL);
    CHECK(strlen(page_id) == 32u);
    REQUIRE_OK(uapp_page_get_title(page, &borrowed));
    CHECK_STR(borrowed, "Data structures");
    /* A new page has no blocks: a page and its tree are not created in one
     * call, and the handle says so rather than lying. */
    REQUIRE_OK(uapp_page_get_block_count(page, &count));
    CHECK(count == 0u);
    REQUIRE_OK(uapp_document_service_page_count(service, &count));
    CHECK(count == 1u);
    uapp_page_free(&page);
    CHECK(page == NULL);

    /* --- ADD BLOCKS, one of every implemented type ------------------------ */
    {
        char* id = add_text_block(service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND,
                                  UAPP_BLOCK_TYPE_PARAGRAPH, "A heap is a tree.");
        CHECK(id != NULL);
        uapp_free_string(&id);
    }
    {
        uapp_block_content_t content = text_content(UAPP_BLOCK_TYPE_HEADING, "Binary heaps");
        content.heading_level = 2;
        REQUIRE_OK(uapp_document_service_add_block(service, page_id, NULL,
                                                   UAPP_BLOCK_POSITION_APPEND, &content, NULL));
    }
    bullet_id = add_text_block(service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND,
                               UAPP_BLOCK_TYPE_BULLETED_LIST_ITEM, "sift up");
    CHECK(bullet_id != NULL);
    if (bullet_id == NULL) {
        uapp_free_string(&page_id);
        uapp_document_service_close(&service);
        return;
    }
    /* NESTED: a child of the bullet, which is what makes the tree a tree. */
    nested_id = add_text_block(service, page_id, bullet_id, UAPP_BLOCK_POSITION_APPEND,
                               UAPP_BLOCK_TYPE_NUMBERED_LIST_ITEM, "compare with parent");
    CHECK(nested_id != NULL);
    {
        uapp_block_content_t content =
            text_content(UAPP_BLOCK_TYPE_CHECKLIST_ITEM, "prove the bound");
        content.checked = UAPP_TRUE;
        REQUIRE_OK(uapp_document_service_add_block(service, page_id, NULL,
                                                   UAPP_BLOCK_POSITION_APPEND, &content, NULL));
    }
    {
        uapp_block_content_t content = text_content(UAPP_BLOCK_TYPE_CODE, "heapify(a, 0);");
        content.language = "cpp";
        REQUIRE_OK(uapp_document_service_add_block(service, page_id, NULL,
                                                   UAPP_BLOCK_POSITION_APPEND, &content, NULL));
    }
    {
        /* A divider is pure structure: text is ignored, and NULL text is fine
         * here and only here. */
        uapp_block_content_t content;
        REQUIRE_OK(uapp_block_content_init(UAPP_BLOCK_TYPE_DIVIDER, &content));
        CHECK(content.text == NULL);
        REQUIRE_OK(uapp_document_service_add_block(service, page_id, NULL,
                                                   UAPP_BLOCK_POSITION_APPEND, &content, NULL));
    }
    {
        uapp_block_content_t content = text_content(UAPP_BLOCK_TYPE_QUOTE, "Knuth said so.");
        REQUIRE_OK(uapp_document_service_add_block(service, page_id, NULL,
                                                   UAPP_BLOCK_POSITION_APPEND, &content, NULL));
    }

    /* block_count without opening the page. */
    REQUIRE_OK(uapp_document_service_block_count(service, page_id, &count));
    CHECK(count == 8u);

    /* --- OPEN PAGE and verify the whole flattened tree -------------------- */
    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    REQUIRE_OK(uapp_page_get_block_count(page, &count));
    CHECK(count == 8u);
    REQUIRE_OK(uapp_page_get_top_level_block_count(page, &top_level));
    /* Seven top-level blocks plus one nested under the bullet. */
    CHECK(top_level == 7u);

    if (count == 8u) {
        uapp_block_content_t content;
        uapp_block_type_t type = UAPP_BLOCK_TYPE_PARAGRAPH;
        size_t depth = 0;
        size_t children = 0;
        size_t i;
        size_t bullet_index = 99u;
        size_t nested_index = 99u;
        int64_t created = 0;
        int64_t modified = 0;

        /* PRE-ORDER: the paragraph, the heading, the bullet, the bullet's
         * child, then the rest. A parent always precedes its descendants,
         * which is what lets a UI indent by depth with no lookahead. */
        static const uapp_block_type_t k_expected[8] = {
            UAPP_BLOCK_TYPE_PARAGRAPH,
            UAPP_BLOCK_TYPE_HEADING,
            UAPP_BLOCK_TYPE_BULLETED_LIST_ITEM,
            UAPP_BLOCK_TYPE_NUMBERED_LIST_ITEM, /* nested */
            UAPP_BLOCK_TYPE_CHECKLIST_ITEM,
            UAPP_BLOCK_TYPE_CODE,
            UAPP_BLOCK_TYPE_DIVIDER,
            UAPP_BLOCK_TYPE_QUOTE
        };
        static const size_t k_expected_depth[8] = {1u, 1u, 1u, 2u, 1u, 1u, 1u, 1u};

        for (i = 0; i < 8u; ++i) {
            REQUIRE_OK(uapp_page_get_block_type(page, i, &type));
            CHECK(type == k_expected[i]);
            REQUIRE_OK(uapp_page_get_block_depth(page, i, &depth));
            CHECK(depth == k_expected_depth[i]);
            /* Every row's id is a real id and its text accessor never NULL. */
            REQUIRE_OK(uapp_page_get_block_id_hex(page, i, &borrowed));
            CHECK(strlen(borrowed) == 32u);
            REQUIRE_OK(uapp_page_get_block_parent_id_hex(page, i, &borrowed));
            CHECK(strlen(borrowed) == 32u);
            if (k_expected_depth[i] == 1u) {
                /* Top level is the NIL id, not an empty string. */
                CHECK_STR(borrowed, UAPP_ID_NIL_HEX);
            } else {
                CHECK(strcmp(borrowed, UAPP_ID_NIL_HEX) != 0);
            }
            REQUIRE_OK(uapp_page_get_block_timestamps(page, i, &created, &modified));
            CHECK(created > 0);
            CHECK(modified >= created);
        }

        /* The payloads, per type. */
        memset(&content, 0, sizeof content);
        content.struct_size = (uint32_t)sizeof content;
        REQUIRE_OK(uapp_page_get_block_content(page, 0u, &content));
        CHECK(content.type == UAPP_BLOCK_TYPE_PARAGRAPH);
        CHECK_STR(content.text, "A heap is a tree.");
        /* Fields this type does not define read as zero/empty, never garbage
         * and never NULL. */
        CHECK(content.heading_level == 0);
        CHECK(content.checked == UAPP_FALSE);
        CHECK_STR(content.language, "");

        content.struct_size = (uint32_t)sizeof content;
        REQUIRE_OK(uapp_page_get_block_content(page, 1u, &content));
        CHECK(content.type == UAPP_BLOCK_TYPE_HEADING);
        CHECK_STR(content.text, "Binary heaps");
        CHECK(content.heading_level == 2);

        content.struct_size = (uint32_t)sizeof content;
        REQUIRE_OK(uapp_page_get_block_content(page, 3u, &content));
        CHECK(content.type == UAPP_BLOCK_TYPE_NUMBERED_LIST_ITEM);
        CHECK_STR(content.text, "compare with parent");

        content.struct_size = (uint32_t)sizeof content;
        REQUIRE_OK(uapp_page_get_block_content(page, 4u, &content));
        CHECK(content.type == UAPP_BLOCK_TYPE_CHECKLIST_ITEM);
        CHECK_STR(content.text, "prove the bound");
        CHECK(content.checked == UAPP_TRUE);

        content.struct_size = (uint32_t)sizeof content;
        REQUIRE_OK(uapp_page_get_block_content(page, 5u, &content));
        CHECK(content.type == UAPP_BLOCK_TYPE_CODE);
        CHECK_STR(content.text, "heapify(a, 0);");
        CHECK_STR(content.language, "cpp");

        content.struct_size = (uint32_t)sizeof content;
        REQUIRE_OK(uapp_page_get_block_content(page, 6u, &content));
        CHECK(content.type == UAPP_BLOCK_TYPE_DIVIDER);
        /* Non-NULL and empty, so no special case before strlen. */
        CHECK_STR(content.text, "");
        CHECK_STR(content.language, "");

        content.struct_size = (uint32_t)sizeof content;
        REQUIRE_OK(uapp_page_get_block_content(page, 7u, &content));
        CHECK(content.type == UAPP_BLOCK_TYPE_QUOTE);
        CHECK_STR(content.text, "Knuth said so.");

        /* child_count is DIRECT children only. */
        REQUIRE_OK(uapp_page_get_block_child_count(page, 2u, &children));
        CHECK(children == 1u);
        REQUIRE_OK(uapp_page_get_block_child_count(page, 3u, &children));
        CHECK(children == 0u);
        REQUIRE_OK(uapp_page_get_block_child_count(page, 0u, &children));
        CHECK(children == 0u);

        /* find_block_index maps an id back to its row, and the parent link
         * agrees with the structure. */
        REQUIRE_OK(uapp_page_find_block_index(page, bullet_id, &bullet_index));
        CHECK(bullet_index == 2u);
        REQUIRE_OK(uapp_page_find_block_index(page, nested_id, &nested_index));
        CHECK(nested_index == 3u);
        REQUIRE_OK(uapp_page_get_block_parent_id_hex(page, nested_index, &borrowed));
        CHECK_STR(borrowed, bullet_id);

        /* An owned copy of a row's text outlives the handle (rule O). */
        {
            char* owned = NULL;
            REQUIRE_OK(uapp_page_copy_block_text(page, 0u, &owned));
            CHECK_STR(owned, "A heap is a tree.");
            uapp_free_string(&owned);
            CHECK(owned == NULL);
            /* A divider's text copies as "" rather than failing. */
            REQUIRE_OK(uapp_page_copy_block_text(page, 6u, &owned));
            CHECK_STR(owned, "");
            uapp_free_string(&owned);
        }
    }

    uapp_page_free(&page);
    uapp_free_string(&nested_id);
    uapp_free_string(&bullet_id);
    uapp_free_string(&page_id);
    uapp_document_service_close(&service);
}

static void test_slice_update_block(void) {
    uapp_document_service_t* service = NULL;
    uapp_page_t* page = NULL;
    char* page_id = NULL;
    char* block_id = NULL;
    char* child_id = NULL;
    uapp_block_content_t content;

    REQUIRE_OK(open_memory_service(&service));
    REQUIRE_OK(uapp_document_service_create_page(service, "Edits", &page));
    REQUIRE_OK(uapp_page_copy_id_hex(page, &page_id));
    uapp_page_free(&page);

    block_id = add_text_block(service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND,
                              UAPP_BLOCK_TYPE_PARAGRAPH, "first draft");
    CHECK(block_id != NULL);
    if (block_id == NULL) {
        uapp_free_string(&page_id);
        uapp_document_service_close(&service);
        return;
    }
    /* Give it a child, so "children survive an update" is testable rather than
     * merely claimed. */
    child_id = add_text_block(service, page_id, block_id, UAPP_BLOCK_POSITION_APPEND,
                              UAPP_BLOCK_TYPE_PARAGRAPH, "a footnote");
    CHECK(child_id != NULL);

    /* --- same type, new text --------------------------------------------- */
    content = text_content(UAPP_BLOCK_TYPE_PARAGRAPH, "second draft");
    REQUIRE_OK(uapp_document_service_update_block(service, block_id, &content));

    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    {
        char* text = NULL;
        size_t index = 0;
        int64_t created = 0;
        int64_t modified = 0;

        REQUIRE_OK(uapp_page_find_block_index(page, block_id, &index));
        CHECK(index == 0u);
        REQUIRE_OK(uapp_page_copy_block_text(page, index, &text));
        CHECK_STR(text, "second draft");
        uapp_free_string(&text);
        /* An update moves modified_at but never created_at: a client cannot
         * rewrite history through this boundary. */
        REQUIRE_OK(uapp_page_get_block_timestamps(page, index, &created, &modified));
        CHECK(created > 0);
        CHECK(modified >= created);
    }
    uapp_page_free(&page);

    /* --- CHANGE THE TYPE -------------------------------------------------- */
    /* Replacing a paragraph with a heading is a normal editor action, and the
     * children must survive it. */
    content               = text_content(UAPP_BLOCK_TYPE_HEADING, "second draft");
    content.heading_level = 3;
    REQUIRE_OK(uapp_document_service_update_block(service, block_id, &content));

    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    {
        uapp_block_content_t read_back;
        uapp_block_type_t type = UAPP_BLOCK_TYPE_PARAGRAPH;
        size_t children = 0;
        size_t count = 0;

        REQUIRE_OK(uapp_page_get_block_count(page, &count));
        CHECK(count == 2u); /* the block and its surviving child */
        REQUIRE_OK(uapp_page_get_block_type(page, 0u, &type));
        CHECK(type == UAPP_BLOCK_TYPE_HEADING);
        REQUIRE_OK(uapp_page_get_block_child_count(page, 0u, &children));
        CHECK(children == 1u);

        memset(&read_back, 0, sizeof read_back);
        read_back.struct_size = (uint32_t)sizeof read_back;
        REQUIRE_OK(uapp_page_get_block_content(page, 0u, &read_back));
        CHECK(read_back.heading_level == 3);
        CHECK_STR(read_back.text, "second draft");
        /* The child is untouched, and is still a paragraph. */
        REQUIRE_OK(uapp_page_get_block_type(page, 1u, &type));
        CHECK(type == UAPP_BLOCK_TYPE_PARAGRAPH);
        read_back.struct_size = (uint32_t)sizeof read_back;
        REQUIRE_OK(uapp_page_get_block_content(page, 1u, &read_back));
        CHECK_STR(read_back.text, "a footnote");
    }
    uapp_page_free(&page);

    /* A rejected update leaves the block exactly as it was. */
    content               = text_content(UAPP_BLOCK_TYPE_HEADING, "not applied");
    content.heading_level = 42;
    CHECK_STATUS(uapp_document_service_update_block(service, block_id, &content),
                 UAPP_STATUS_BLOCK_MALFORMED);
    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    {
        char* text = NULL;
        REQUIRE_OK(uapp_page_copy_block_text(page, 0u, &text));
        CHECK_STR(text, "second draft");
        uapp_free_string(&text);
    }
    uapp_page_free(&page);

    /* Updating a block that is gone is NOT_FOUND, and never a create. */
    content = text_content(UAPP_BLOCK_TYPE_PARAGRAPH, "ghost");
    CHECK_STATUS(uapp_document_service_update_block(
                     service, "0123456789abcdef0123456789abcdef", &content),
                 UAPP_STATUS_NOT_FOUND);
    {
        size_t count = 0;
        REQUIRE_OK(uapp_document_service_block_count(service, page_id, &count));
        CHECK(count == 2u);
    }

    uapp_free_string(&child_id);
    uapp_free_string(&block_id);
    uapp_free_string(&page_id);
    uapp_document_service_close(&service);
}

static void test_slice_delete_block_cascades(void) {
    uapp_document_service_t* service = NULL;
    uapp_page_t* page = NULL;
    char* page_id = NULL;
    char* parent_id = NULL;
    char* child_id = NULL;
    char* grandchild_id = NULL;
    char* sibling_id = NULL;
    size_t count = 0;
    size_t index = 0;

    REQUIRE_OK(open_memory_service(&service));
    REQUIRE_OK(uapp_document_service_create_page(service, "Cascade", &page));
    REQUIRE_OK(uapp_page_copy_id_hex(page, &page_id));
    uapp_page_free(&page);

    parent_id = add_text_block(service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND,
                               UAPP_BLOCK_TYPE_BULLETED_LIST_ITEM, "doomed parent");
    CHECK(parent_id != NULL);
    if (parent_id == NULL) { goto cleanup; }
    child_id = add_text_block(service, page_id, parent_id, UAPP_BLOCK_POSITION_APPEND,
                              UAPP_BLOCK_TYPE_BULLETED_LIST_ITEM, "doomed child");
    CHECK(child_id != NULL);
    if (child_id == NULL) { goto cleanup; }
    grandchild_id = add_text_block(service, page_id, child_id, UAPP_BLOCK_POSITION_APPEND,
                                   UAPP_BLOCK_TYPE_PARAGRAPH, "doomed grandchild");
    CHECK(grandchild_id != NULL);
    sibling_id = add_text_block(service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND,
                                UAPP_BLOCK_TYPE_PARAGRAPH, "survivor");
    CHECK(sibling_id != NULL);

    REQUIRE_OK(uapp_document_service_block_count(service, page_id, &count));
    CHECK(count == 4u);

    /* Delete the parent: the whole subtree goes, the sibling stays. Cascade,
     * not reparent -- a promoted orphan is a document the user did not write. */
    REQUIRE_OK(uapp_document_service_delete_block(service, parent_id));
    REQUIRE_OK(uapp_document_service_block_count(service, page_id, &count));
    CHECK(count == 1u);

    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    REQUIRE_OK(uapp_page_get_block_count(page, &count));
    CHECK(count == 1u);
    /* None of the three is reachable any more, and the survivor still is. */
    CHECK_STATUS(uapp_page_find_block_index(page, parent_id, &index), UAPP_STATUS_NOT_FOUND);
    CHECK_STATUS(uapp_page_find_block_index(page, child_id, &index), UAPP_STATUS_NOT_FOUND);
    if (grandchild_id != NULL) {
        CHECK_STATUS(uapp_page_find_block_index(page, grandchild_id, &index),
                     UAPP_STATUS_NOT_FOUND);
    }
    if (sibling_id != NULL) {
        REQUIRE_OK(uapp_page_find_block_index(page, sibling_id, &index));
        CHECK(index == 0u); /* the gap in the sibling list was closed */
    }
    uapp_page_free(&page);

    /* Deleting twice is NOT_FOUND, not a crash and not a silent success. */
    CHECK_STATUS(uapp_document_service_delete_block(service, parent_id), UAPP_STATUS_NOT_FOUND);

cleanup:
    uapp_free_string(&sibling_id);
    uapp_free_string(&grandchild_id);
    uapp_free_string(&child_id);
    uapp_free_string(&parent_id);
    uapp_free_string(&page_id);
    uapp_document_service_close(&service);
}

static void test_slice_reorder_block(void) {
    uapp_document_service_t* service = NULL;
    uapp_page_t* page = NULL;
    char* page_id = NULL;
    char* ids[3];
    const char* borrowed = NULL;
    int i;

    for (i = 0; i < 3; ++i) { ids[i] = NULL; }

    REQUIRE_OK(open_memory_service(&service));
    REQUIRE_OK(uapp_document_service_create_page(service, "Order", &page));
    REQUIRE_OK(uapp_page_copy_id_hex(page, &page_id));
    uapp_page_free(&page);

    ids[0] = add_text_block(service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND,
                            UAPP_BLOCK_TYPE_PARAGRAPH, "alpha");
    ids[1] = add_text_block(service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND,
                            UAPP_BLOCK_TYPE_PARAGRAPH, "beta");
    ids[2] = add_text_block(service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND,
                            UAPP_BLOCK_TYPE_PARAGRAPH, "gamma");
    CHECK(ids[0] != NULL && ids[1] != NULL && ids[2] != NULL);
    if (ids[0] == NULL || ids[1] == NULL || ids[2] == NULL) { goto cleanup; }

    /* Drag gamma to the front. new_position is the index it ENDS UP at. */
    REQUIRE_OK(uapp_document_service_reorder_block(service, ids[2], 0u));
    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    REQUIRE_OK(uapp_page_get_block_id_hex(page, 0u, &borrowed));
    CHECK_STR(borrowed, ids[2]);
    REQUIRE_OK(uapp_page_get_block_id_hex(page, 1u, &borrowed));
    CHECK_STR(borrowed, ids[0]);
    REQUIRE_OK(uapp_page_get_block_id_hex(page, 2u, &borrowed));
    CHECK_STR(borrowed, ids[1]);
    uapp_page_free(&page);

    /* And to the back, with the append sentinel. */
    REQUIRE_OK(
        uapp_document_service_reorder_block(service, ids[2], UAPP_BLOCK_POSITION_APPEND));
    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    REQUIRE_OK(uapp_page_get_block_id_hex(page, 2u, &borrowed));
    CHECK_STR(borrowed, ids[2]);
    uapp_page_free(&page);

    /* Past the end is rejected, NOT clamped: a caller working from a stale
     * tree should be told. */
    CHECK_STATUS(uapp_document_service_reorder_block(service, ids[0], 99u),
                 UAPP_STATUS_BLOCK_POSITION_INVALID);
    /* And the order is unchanged by the rejection. */
    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    REQUIRE_OK(uapp_page_get_block_id_hex(page, 2u, &borrowed));
    CHECK_STR(borrowed, ids[2]);
    uapp_page_free(&page);

cleanup:
    for (i = 0; i < 3; ++i) { uapp_free_string(&ids[i]); }
    uapp_free_string(&page_id);
    uapp_document_service_close(&service);
}

static void test_slice_move_block(void) {
    uapp_document_service_t* service = NULL;
    uapp_page_t* page = NULL;
    char* page_id = NULL;
    char* host_id = NULL;
    char* wanderer_id = NULL;
    char* child_id = NULL;
    const char* borrowed = NULL;
    size_t index = 0;
    size_t depth = 0;
    size_t children = 0;

    REQUIRE_OK(open_memory_service(&service));
    REQUIRE_OK(uapp_document_service_create_page(service, "Moves", &page));
    REQUIRE_OK(uapp_page_copy_id_hex(page, &page_id));
    uapp_page_free(&page);

    host_id = add_text_block(service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND,
                             UAPP_BLOCK_TYPE_BULLETED_LIST_ITEM, "host");
    wanderer_id = add_text_block(service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND,
                                 UAPP_BLOCK_TYPE_BULLETED_LIST_ITEM, "wanderer");
    CHECK(host_id != NULL && wanderer_id != NULL);
    if (host_id == NULL || wanderer_id == NULL) { goto cleanup; }
    /* The wanderer carries a child, so the subtree moves with it. */
    child_id = add_text_block(service, page_id, wanderer_id, UAPP_BLOCK_POSITION_APPEND,
                              UAPP_BLOCK_TYPE_PARAGRAPH, "luggage");
    CHECK(child_id != NULL);

    /* Drag the wanderer under the host. */
    REQUIRE_OK(uapp_document_service_move_block(service, wanderer_id, host_id, 0u));
    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    REQUIRE_OK(uapp_page_find_block_index(page, wanderer_id, &index));
    CHECK(index == 1u); /* pre-order: host, then its child */
    REQUIRE_OK(uapp_page_get_block_depth(page, index, &depth));
    CHECK(depth == 2u);
    REQUIRE_OK(uapp_page_get_block_parent_id_hex(page, index, &borrowed));
    CHECK_STR(borrowed, host_id);
    REQUIRE_OK(uapp_page_get_block_child_count(page, index, &children));
    CHECK(children == 1u); /* the luggage came too */
    if (child_id != NULL) {
        size_t child_index = 0;
        REQUIRE_OK(uapp_page_find_block_index(page, child_id, &child_index));
        CHECK(child_index == 2u);
        REQUIRE_OK(uapp_page_get_block_depth(page, child_index, &depth));
        CHECK(depth == 3u);
    }
    uapp_page_free(&page);

    /* And back out to the top level: NULL parent means "the page". */
    REQUIRE_OK(uapp_document_service_move_block(service, wanderer_id, NULL, 0u));
    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    REQUIRE_OK(uapp_page_find_block_index(page, wanderer_id, &index));
    CHECK(index == 0u);
    REQUIRE_OK(uapp_page_get_block_depth(page, index, &depth));
    CHECK(depth == 1u);
    REQUIRE_OK(uapp_page_get_block_parent_id_hex(page, index, &borrowed));
    CHECK_STR(borrowed, UAPP_ID_NIL_HEX);
    uapp_page_free(&page);

    /* The explicit nil id means exactly the same thing as NULL. */
    REQUIRE_OK(uapp_document_service_move_block(service, wanderer_id, UAPP_ID_NIL_HEX, 1u));
    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    REQUIRE_OK(uapp_page_find_block_index(page, wanderer_id, &index));
    REQUIRE_OK(uapp_page_get_block_depth(page, index, &depth));
    CHECK(depth == 1u);
    uapp_page_free(&page);

cleanup:
    uapp_free_string(&child_id);
    uapp_free_string(&wanderer_id);
    uapp_free_string(&host_id);
    uapp_free_string(&page_id);
    uapp_document_service_close(&service);
}

static void test_slice_rename_and_delete_page(void) {
    uapp_document_service_t* service = NULL;
    uapp_page_t* page = NULL;
    char* page_id = NULL;
    const char* borrowed = NULL;
    size_t count = 0;

    REQUIRE_OK(open_memory_service(&service));
    REQUIRE_OK(uapp_document_service_create_page(service, "Working title", &page));
    REQUIRE_OK(uapp_page_copy_id_hex(page, &page_id));

    REQUIRE_OK(uapp_document_service_rename_page(service, page_id, "Final title"));
    /* The open SNAPSHOT is untouched by the rename -- it is a snapshot, not a
     * live view, exactly as uapp_note_t is. */
    REQUIRE_OK(uapp_page_get_title(page, &borrowed));
    CHECK_STR(borrowed, "Working title");
    uapp_page_free(&page);
    /* Reopening sees it. */
    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    REQUIRE_OK(uapp_page_get_title(page, &borrowed));
    CHECK_STR(borrowed, "Final title");
    uapp_page_free(&page);

    /* An empty title is legal: "Untitled" is a real state users live in. */
    REQUIRE_OK(uapp_document_service_rename_page(service, page_id, ""));
    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    REQUIRE_OK(uapp_page_get_title(page, &borrowed));
    CHECK(borrowed[0] == '\0');
    {
        size_t len = 99u;
        REQUIRE_OK(uapp_page_get_title_length(page, &len));
        CHECK(len == 0u);
    }
    uapp_page_free(&page);

    /* Add a block, then delete the page: the blocks go with it. */
    {
        char* block_id = add_text_block(service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND,
                                        UAPP_BLOCK_TYPE_PARAGRAPH, "transient");
        CHECK(block_id != NULL);
        uapp_free_string(&block_id);
    }
    REQUIRE_OK(uapp_document_service_block_count(service, page_id, &count));
    CHECK(count == 1u);

    REQUIRE_OK(uapp_document_service_delete_page(service, page_id));
    REQUIRE_OK(uapp_document_service_page_count(service, &count));
    CHECK(count == 0u);
    CHECK_STATUS(uapp_document_service_open_page(service, page_id, &page), UAPP_STATUS_NOT_FOUND);
    CHECK(page == NULL);
    CHECK_STATUS(uapp_document_service_delete_page(service, page_id), UAPP_STATUS_NOT_FOUND);
    /*
     * block_count on a page that no longer exists is OK WITH ZERO, not
     * NOT_FOUND: the core's block_count is a plain count of the rows carrying
     * that page id and never probes for the page itself. That is surprising
     * enough to pin down here. The ABI mirrors the core rather than adding an
     * existence check the C++ API does not have -- "this layer adds no
     * behaviour" has to mean it even when the invented behaviour would be
     * nicer. uapp_blocks.h says so at the declaration; a caller that needs
     * "does this page exist" asks uapp_document_service_open_page().
     */
    count = 99u;
    REQUIRE_OK(uapp_document_service_block_count(service, page_id, &count));
    CHECK(count == 0u);

    uapp_free_string(&page_id);
    uapp_document_service_close(&service);
}

static void test_slice_persists_across_close(void) {
    /* The one case that needs a real file: prove the bytes survive the handle,
     * not just the process's memory. */
    const char* path = UAPP_CAPI_BLOCKS_TEST_DB_PATH;
    uapp_document_service_t* service = NULL;
    uapp_page_t* page = NULL;
    char* page_id = NULL;
    const char* borrowed = NULL;
    size_t count = 0;

    remove(path);

    REQUIRE_OK(open_file_service(path, &service));
    REQUIRE_OK(uapp_document_service_create_page(service, "Durable", &page));
    REQUIRE_OK(uapp_page_copy_id_hex(page, &page_id));
    uapp_page_free(&page);
    {
        char* block_id = add_text_block(service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND,
                                        UAPP_BLOCK_TYPE_QUOTE, "Written to disk.");
        CHECK(block_id != NULL);
        uapp_free_string(&block_id);
    }
    uapp_document_service_close(&service);

    REQUIRE_OK(open_file_service(path, &service));
    REQUIRE_OK(uapp_document_service_page_count(service, &count));
    CHECK(count == 1u);
    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    REQUIRE_OK(uapp_page_get_title(page, &borrowed));
    CHECK_STR(borrowed, "Durable");
    REQUIRE_OK(uapp_page_get_block_count(page, &count));
    CHECK(count == 1u);
    {
        char* text = NULL;
        REQUIRE_OK(uapp_page_copy_block_text(page, 0u, &text));
        CHECK_STR(text, "Written to disk.");
        uapp_free_string(&text);
    }

    uapp_page_free(&page);
    uapp_document_service_close(&service);
    uapp_free_string(&page_id);
    remove(path);
}

/*
 * Requirement: multiple independent handles in one process. Two services, two
 * private in-memory databases, alive at the same time. If anything behind the
 * ABI were a singleton -- a cached connection, a global service pointer, a
 * process-wide id table -- these assertions would fail.
 */
static void test_slice_independent_services(void) {
    uapp_document_service_t* alpha = NULL;
    uapp_document_service_t* beta = NULL;
    uapp_page_t* in_alpha = NULL;
    uapp_page_t* leaked = NULL;
    char* alpha_page = NULL;
    size_t count = 0;

    REQUIRE_OK(open_memory_service(&alpha));
    REQUIRE_OK(open_memory_service(&beta));
    CHECK(alpha != beta);

    REQUIRE_OK(uapp_document_service_create_page(alpha, "Alpha only", &in_alpha));
    REQUIRE_OK(uapp_page_copy_id_hex(in_alpha, &alpha_page));

    REQUIRE_OK(uapp_document_service_page_count(alpha, &count));
    CHECK(count == 1u);
    REQUIRE_OK(uapp_document_service_page_count(beta, &count));
    CHECK(count == 0u);
    CHECK_STATUS(uapp_document_service_open_page(beta, alpha_page, &leaked),
                 UAPP_STATUS_NOT_FOUND);
    CHECK(leaked == NULL);

    /* Closing one must not disturb the other. */
    uapp_document_service_close(&alpha);
    REQUIRE_OK(uapp_document_service_page_count(beta, &count));
    CHECK(count == 0u);

    /* A page handle is a self-contained snapshot: it outlives its service. */
    {
        const char* borrowed = NULL;
        REQUIRE_OK(uapp_page_get_title(in_alpha, &borrowed));
        CHECK_STR(borrowed, "Alpha only");
    }
    /* But mutating through the CLOSED service is impossible -- the caller's
     * pointer is NULL, so this is a clean rejection rather than a crash. */
    CHECK_STATUS(uapp_document_service_rename_page(alpha, alpha_page, "nope"),
                 UAPP_STATUS_INVALID_ARGUMENT);

    uapp_page_free(&in_alpha);
    uapp_free_string(&alpha_page);
    uapp_document_service_close(&beta);
}

static void test_slice_read_only_service(void) {
    const char* path = UAPP_CAPI_BLOCKS_TEST_RO_DB_PATH;
    uapp_document_service_t* service = NULL;
    uapp_document_service_config_t config;
    uapp_page_t* page = NULL;
    char* page_id = NULL;

    remove(path);

    /* Create something to read. */
    REQUIRE_OK(open_file_service(path, &service));
    REQUIRE_OK(uapp_document_service_create_page(service, "Read me", &page));
    REQUIRE_OK(uapp_page_copy_id_hex(page, &page_id));
    uapp_page_free(&page);
    uapp_document_service_close(&service);

    REQUIRE_OK(uapp_document_service_config_init(&config));
    config.database_path = path;
    config.read_only     = UAPP_TRUE;
    REQUIRE_OK(uapp_document_service_open(&config, &service));

    /* Reads work. */
    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    uapp_page_free(&page);

    /* Writes fail with a specific status, not a silent no-op. */
    CHECK_STATUS(uapp_document_service_create_page(service, "nope", &page),
                 UAPP_STATUS_STORE_READ_ONLY);
    CHECK(page == NULL);
    CHECK_STATUS(uapp_document_service_rename_page(service, page_id, "nope"),
                 UAPP_STATUS_STORE_READ_ONLY);
    {
        uapp_block_content_t content = text_content(UAPP_BLOCK_TYPE_PARAGRAPH, "nope");
        CHECK_STATUS(uapp_document_service_add_block(service, page_id, NULL,
                                                     UAPP_BLOCK_POSITION_APPEND, &content, NULL),
                     UAPP_STATUS_STORE_READ_ONLY);
    }

    uapp_document_service_close(&service);
    uapp_free_string(&page_id);
    remove(path);
}

/* ===========================================================================
 * Boundary safety
 * =========================================================================== */

static void test_boundary_null_handles(void) {
    uapp_document_service_t* service = NULL;
    uapp_page_t* page = NULL;
    uapp_block_content_t content = text_content(UAPP_BLOCK_TYPE_PARAGRAPH, "t");
    const char* borrowed = NULL;
    char* owned = NULL;
    size_t count = 123u;
    size_t index = 456u;
    uapp_block_type_t type = UAPP_BLOCK_TYPE_DIVIDER;
    int64_t when = 0;

    /* A NULL service handle on every entry point that takes one. */
    CHECK_STATUS(uapp_document_service_create_page(NULL, "t", &page),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(page == NULL);
    CHECK_STATUS(uapp_document_service_open_page(NULL, UAPP_ID_NIL_HEX, &page),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_rename_page(NULL, UAPP_ID_NIL_HEX, "t"),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_delete_page(NULL, UAPP_ID_NIL_HEX),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_page_count(NULL, &count), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(count == 123u); /* a failed call must not touch the out-parameter */
    CHECK_STATUS(uapp_document_service_add_block(NULL, UAPP_ID_NIL_HEX, NULL, 0u, &content,
                                                 &owned),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(owned == NULL);
    CHECK_STATUS(uapp_document_service_update_block(NULL, UAPP_ID_NIL_HEX, &content),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_delete_block(NULL, UAPP_ID_NIL_HEX),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_reorder_block(NULL, UAPP_ID_NIL_HEX, 0u),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_move_block(NULL, UAPP_ID_NIL_HEX, NULL, 0u),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_block_count(NULL, UAPP_ID_NIL_HEX, &count),
                 UAPP_STATUS_INVALID_ARGUMENT);

    /* A NULL page handle on every page accessor. */
    CHECK_STATUS(uapp_page_get_id_hex(NULL, &borrowed), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_title(NULL, &borrowed), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_title_length(NULL, &count), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_timestamps(NULL, &when, &when), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_copy_id_hex(NULL, &owned), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(owned == NULL);
    CHECK_STATUS(uapp_page_copy_title(NULL, &owned), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_block_count(NULL, &count), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_top_level_block_count(NULL, &count),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_block_id_hex(NULL, 0u, &borrowed), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_block_parent_id_hex(NULL, 0u, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_block_type(NULL, 0u, &type), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_block_depth(NULL, 0u, &count), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_block_child_count(NULL, 0u, &count),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_block_timestamps(NULL, 0u, &when, &when),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_copy_block_text(NULL, 0u, &owned), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_find_block_index(NULL, UAPP_ID_NIL_HEX, &index),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(index == 456u);
    {
        uapp_block_content_t out_content;
        memset(&out_content, 0, sizeof out_content);
        out_content.struct_size = (uint32_t)sizeof out_content;
        CHECK_STATUS(uapp_page_get_block_content(NULL, 0u, &out_content),
                     UAPP_STATUS_INVALID_ARGUMENT);
    }

    /* Destructors are total: NULL, and a pointer to NULL, are both fine. */
    uapp_document_service_close(NULL);
    uapp_document_service_close(&service);
    uapp_page_free(NULL);
    uapp_page_free(&page);
}

static void test_boundary_null_strings_and_out_params(void) {
    uapp_document_service_t* service = NULL;
    uapp_page_t* page = NULL;
    char* page_id = NULL;
    uapp_block_content_t content = text_content(UAPP_BLOCK_TYPE_PARAGRAPH, "t");

    REQUIRE_OK(open_memory_service(&service));

    /* NULL where a string is expected. "" is legal and NULL is not: an empty
     * title is a real thing, an absent pointer is a caller bug. */
    CHECK_STATUS(uapp_document_service_create_page(service, NULL, &page),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(page == NULL);
    REQUIRE_OK(uapp_document_service_create_page(service, "", &page));
    REQUIRE_OK(uapp_page_copy_id_hex(page, &page_id));
    uapp_page_free(&page);

    CHECK_STATUS(uapp_document_service_open_page(service, NULL, &page),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_rename_page(service, NULL, "t"),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_rename_page(service, page_id, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_delete_page(service, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_add_block(service, NULL, NULL, 0u, &content, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_update_block(service, NULL, &content),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_delete_block(service, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_reorder_block(service, NULL, 0u),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_move_block(service, NULL, NULL, 0u),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_block_count(service, NULL, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);

    /* NULL where a content struct is expected. */
    CHECK_STATUS(uapp_document_service_add_block(service, page_id, NULL, 0u, NULL, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_update_block(service, page_id, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);

    /* NULL where an out-parameter is expected. */
    CHECK_STATUS(uapp_document_service_create_page(service, "t", NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_open_page(service, page_id, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_page_count(service, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_block_content_init(UAPP_BLOCK_TYPE_PARAGRAPH, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_block_get_limits(NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_config_init(NULL), UAPP_STATUS_INVALID_ARGUMENT);

    /* out_block_id_hex IS optional -- this one is legal. */
    REQUIRE_OK(uapp_document_service_add_block(service, page_id, NULL,
                                               UAPP_BLOCK_POSITION_APPEND, &content, NULL));

    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    CHECK_STATUS(uapp_page_get_title(page, NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_id_hex(page, NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_copy_title(page, NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_block_count(page, NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_block_id_hex(page, 0u, NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_block_type(page, 0u, NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_block_content(page, 0u, NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_copy_block_text(page, 0u, NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_find_block_index(page, UAPP_ID_NIL_HEX, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    /* Both timestamp out-parameters are optional, so these ARE legal. */
    CHECK_OK(uapp_page_get_timestamps(page, NULL, NULL));
    CHECK_OK(uapp_page_get_block_timestamps(page, 0u, NULL, NULL));

    /* The out-parameter rule, stated as a test: a FAILING call leaves the
     * caller's variable exactly as it found it. The obvious-looking
     * alternative -- null the handle on failure -- would silently drop the
     * live handle held here, which is a leak the caller cannot observe. */
    {
        uapp_page_t* const live = page;
        CHECK_STATUS(uapp_document_service_create_page(service, NULL, &page),
                     UAPP_STATUS_INVALID_ARGUMENT);
        CHECK(page == live);
    }

    uapp_page_free(&page);
    uapp_free_string(&page_id);
    uapp_document_service_close(&service);
}

/*
 * The out-of-range and unimplemented block-type discriminant.
 *
 * This is the one place the flat tagged struct gives up a guarantee the C++
 * variant has: nothing stops a C caller writing any integer into `type`. The
 * ABI therefore has to tell three cases apart, and a UI reacts differently to
 * each:
 *
 *   0..7    implemented           -> OK
 *   8..18   declared, not built   -> BLOCK_TYPE_UNSUPPORTED ("not yet")
 *   other   not a block type      -> INVALID_ARGUMENT ("caller bug")
 */
static void test_boundary_block_type_discriminant(void) {
    uapp_document_service_t* service = NULL;
    uapp_page_t* page = NULL;
    char* page_id = NULL;
    char* block_id = NULL;
    uapp_block_content_t content;
    int32_t i;

    REQUIRE_OK(open_memory_service(&service));
    REQUIRE_OK(uapp_document_service_create_page(service, "Types", &page));
    REQUIRE_OK(uapp_page_copy_id_hex(page, &page_id));
    uapp_page_free(&page);
    block_id = add_text_block(service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND,
                              UAPP_BLOCK_TYPE_PARAGRAPH, "anchor");
    CHECK(block_id != NULL);

    /* All eleven declared-but-absent types: a product state, not a bug. */
    for (i = 8; i < 19; ++i) {
        content = text_content((uapp_block_type_t)i, "text");
        CHECK_STATUS(uapp_document_service_add_block(service, page_id, NULL,
                                                     UAPP_BLOCK_POSITION_APPEND, &content, NULL),
                     UAPP_STATUS_BLOCK_TYPE_UNSUPPORTED);
        if (block_id != NULL) {
            CHECK_STATUS(uapp_document_service_update_block(service, block_id, &content),
                         UAPP_STATUS_BLOCK_TYPE_UNSUPPORTED);
        }
    }

    /* Integers that are not block types at all: a caller bug. One past the
     * end, a wildly large value, and a negative one -- the last is why
     * uapp_block_type_t carries a FORCE_INT32 sentinel, so that merely READING
     * it is defined rather than something UBSan is right to flag. */
    {
        static const int32_t k_bogus[5] = {19, 20, 424242, -1, -424242};
        size_t b;
        for (b = 0; b < 5u; ++b) {
            content = text_content((uapp_block_type_t)k_bogus[b], "text");
            CHECK_STATUS(uapp_document_service_add_block(
                             service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND, &content, NULL),
                         UAPP_STATUS_INVALID_ARGUMENT);
            if (block_id != NULL) {
                CHECK_STATUS(uapp_document_service_update_block(service, block_id, &content),
                             UAPP_STATUS_INVALID_ARGUMENT);
            }
        }
    }

    /* INT32_MIN and INT32_MAX, the extremes of the pinned range. The second is
     * UAPP_BLOCK_TYPE_FORCE_INT32 itself, which is documented as "not a block
     * type" and must behave like one. */
    content = text_content((uapp_block_type_t)UAPP_BLOCK_TYPE_FORCE_INT32, "text");
    CHECK_STATUS(uapp_document_service_add_block(service, page_id, NULL,
                                                 UAPP_BLOCK_POSITION_APPEND, &content, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    content = text_content((uapp_block_type_t)INT32_MIN, "text");
    CHECK_STATUS(uapp_document_service_add_block(service, page_id, NULL,
                                                 UAPP_BLOCK_POSITION_APPEND, &content, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);

    /* Nothing was written by any of the above. */
    {
        size_t count = 0;
        REQUIRE_OK(uapp_document_service_block_count(service, page_id, &count));
        CHECK(count == 1u);
    }

    uapp_free_string(&block_id);
    uapp_free_string(&page_id);
    uapp_document_service_close(&service);
}

static void test_boundary_content_validation(void) {
    uapp_document_service_t* service = NULL;
    uapp_page_t* page = NULL;
    char* page_id = NULL;
    uapp_block_content_t content;
    uapp_block_limits_t limits;
    char* oversized = NULL;

    memset(&limits, 0, sizeof limits);
    limits.struct_size = (uint32_t)sizeof limits;
    REQUIRE_OK(uapp_block_get_limits(&limits));

    REQUIRE_OK(open_memory_service(&service));
    REQUIRE_OK(uapp_document_service_create_page(service, "Validation", &page));
    REQUIRE_OK(uapp_page_copy_id_hex(page, &page_id));
    uapp_page_free(&page);

    /* NULL text is an error for every type EXCEPT the divider. */
    {
        static const uapp_block_type_t k_text_types[7] = {
            UAPP_BLOCK_TYPE_PARAGRAPH,          UAPP_BLOCK_TYPE_HEADING,
            UAPP_BLOCK_TYPE_BULLETED_LIST_ITEM, UAPP_BLOCK_TYPE_NUMBERED_LIST_ITEM,
            UAPP_BLOCK_TYPE_CHECKLIST_ITEM,     UAPP_BLOCK_TYPE_CODE,
            UAPP_BLOCK_TYPE_QUOTE};
        size_t t;
        for (t = 0; t < 7u; ++t) {
            REQUIRE_OK(uapp_block_content_init(k_text_types[t], &content));
            CHECK(content.text == NULL);
            CHECK_STATUS(uapp_document_service_add_block(
                             service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND, &content, NULL),
                         UAPP_STATUS_INVALID_ARGUMENT);
            /* "" is legal for all of them: an empty paragraph is what pressing
             * Enter makes. */
            content.text = "";
            CHECK_OK(uapp_document_service_add_block(service, page_id, NULL,
                                                     UAPP_BLOCK_POSITION_APPEND, &content,
                                                     NULL));
        }
    }

    /* A heading level outside 1..max is BLOCK_MALFORMED, not a clamp. */
    {
        static const int32_t k_bad_levels[4] = {0, -1, 4, 999};
        size_t l;
        for (l = 0; l < 4u; ++l) {
            content = text_content(UAPP_BLOCK_TYPE_HEADING, "h");
            content.heading_level = k_bad_levels[l];
            CHECK_STATUS(uapp_document_service_add_block(
                             service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND, &content, NULL),
                         UAPP_STATUS_BLOCK_MALFORMED);
        }
        /* Every level in range is accepted. */
        for (l = (size_t)limits.min_heading_level;
             l <= (size_t)limits.max_heading_level; ++l) {
            content = text_content(UAPP_BLOCK_TYPE_HEADING, "h");
            content.heading_level = (int32_t)l;
            CHECK_OK(uapp_document_service_add_block(service, page_id, NULL,
                                                     UAPP_BLOCK_POSITION_APPEND, &content,
                                                     NULL));
        }
    }

    /* A control byte in a language tag is rejected: it would reach a CSS class
     * name or a highlighter lookup key. */
    content          = text_content(UAPP_BLOCK_TYPE_CODE, "x();");
    content.language = "c\x01pp";
    CHECK_STATUS(uapp_document_service_add_block(service, page_id, NULL,
                                                 UAPP_BLOCK_POSITION_APPEND, &content, NULL),
                 UAPP_STATUS_BLOCK_MALFORMED);
    /* A NULL language means "no language", which is a legal code block. */
    content          = text_content(UAPP_BLOCK_TYPE_CODE, "x();");
    content.language = NULL;
    CHECK_OK(uapp_document_service_add_block(service, page_id, NULL,
                                             UAPP_BLOCK_POSITION_APPEND, &content, NULL));

    /* An over-length language tag is BLOCK_TEXT_TOO_LARGE. */
    oversized = (char*)malloc(limits.max_code_language_bytes + 8u);
    REQUIRE_OK(oversized != NULL ? UAPP_STATUS_OK : UAPP_STATUS_OUT_OF_MEMORY);
    memset(oversized, 'x', limits.max_code_language_bytes + 7u);
    oversized[limits.max_code_language_bytes + 7u] = '\0';
    content          = text_content(UAPP_BLOCK_TYPE_CODE, "x();");
    content.language = oversized;
    CHECK_STATUS(uapp_document_service_add_block(service, page_id, NULL,
                                                 UAPP_BLOCK_POSITION_APPEND, &content, NULL),
                 UAPP_STATUS_BLOCK_TEXT_TOO_LARGE);
    free(oversized);

    /* An over-length page title is PAGE_TITLE_TOO_LONG, at create AND rename. */
    oversized = (char*)malloc(limits.max_page_title_bytes + 8u);
    REQUIRE_OK(oversized != NULL ? UAPP_STATUS_OK : UAPP_STATUS_OUT_OF_MEMORY);
    memset(oversized, 'x', limits.max_page_title_bytes + 7u);
    oversized[limits.max_page_title_bytes + 7u] = '\0';
    CHECK_STATUS(uapp_document_service_create_page(service, oversized, &page),
                 UAPP_STATUS_PAGE_TITLE_TOO_LONG);
    CHECK(page == NULL);
    CHECK_STATUS(uapp_document_service_rename_page(service, page_id, oversized),
                 UAPP_STATUS_PAGE_TITLE_TOO_LONG);
    free(oversized);

    /* Bytes in, identical bytes out -- the ABI must not transcode anything. */
    content = text_content(UAPP_BLOCK_TYPE_PARAGRAPH,
                           "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xF0\x9F\x93\x9D");
    {
        char* id = NULL;
        REQUIRE_OK(uapp_document_service_add_block(service, page_id, NULL,
                                                   UAPP_BLOCK_POSITION_APPEND, &content, &id));
        REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
        {
            size_t index = 0;
            char* text = NULL;
            REQUIRE_OK(uapp_page_find_block_index(page, id, &index));
            REQUIRE_OK(uapp_page_copy_block_text(page, index, &text));
            CHECK_STR(text, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xF0\x9F\x93\x9D");
            uapp_free_string(&text);
        }
        uapp_page_free(&page);
        uapp_free_string(&id);
    }

    uapp_free_string(&page_id);
    uapp_document_service_close(&service);
}

static void test_boundary_bad_ids(void) {
    uapp_document_service_t* service = NULL;
    uapp_page_t* page = NULL;
    uapp_block_content_t content = text_content(UAPP_BLOCK_TYPE_PARAGRAPH, "t");
    size_t count = 0;

    REQUIRE_OK(open_memory_service(&service));

    /* A malformed id is told apart from an id that is simply absent. */
    CHECK_STATUS(uapp_document_service_open_page(service, "", &page),
                 UAPP_STATUS_ID_MALFORMED);
    CHECK_STATUS(uapp_document_service_open_page(service, "not-hex", &page),
                 UAPP_STATUS_ID_MALFORMED);
    CHECK_STATUS(uapp_document_service_open_page(service, "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",
                                                 &page),
                 UAPP_STATUS_ID_MALFORMED);
    CHECK_STATUS(uapp_document_service_open_page(service, "0123456789abcdef", &page),
                 UAPP_STATUS_ID_MALFORMED);
    CHECK_STATUS(uapp_document_service_delete_page(service, "nope"), UAPP_STATUS_ID_MALFORMED);
    CHECK_STATUS(uapp_document_service_rename_page(service, "nope", "t"),
                 UAPP_STATUS_ID_MALFORMED);
    CHECK_STATUS(uapp_document_service_delete_block(service, "nope"), UAPP_STATUS_ID_MALFORMED);
    CHECK_STATUS(uapp_document_service_reorder_block(service, "nope", 0u),
                 UAPP_STATUS_ID_MALFORMED);
    CHECK_STATUS(uapp_document_service_move_block(service, "nope", NULL, 0u),
                 UAPP_STATUS_ID_MALFORMED);
    CHECK_STATUS(uapp_document_service_update_block(service, "nope", &content),
                 UAPP_STATUS_ID_MALFORMED);
    CHECK_STATUS(uapp_document_service_block_count(service, "nope", &count),
                 UAPP_STATUS_ID_MALFORMED);
    CHECK_STATUS(uapp_document_service_add_block(service, "nope", NULL, 0u, &content, NULL),
                 UAPP_STATUS_ID_MALFORMED);
    /* A malformed PARENT id too -- NULL means top level, "nope" means nothing. */
    CHECK_STATUS(uapp_document_service_add_block(service, "0123456789abcdef0123456789abcdef",
                                                 "nope", 0u, &content, NULL),
                 UAPP_STATUS_ID_MALFORMED);

    /* Well-formed but absent. The nil id is well-formed and can never name a
     * real object, so it is NOT_FOUND rather than malformed. */
    CHECK_STATUS(uapp_document_service_open_page(service, "0123456789abcdef0123456789abcdef",
                                                 &page),
                 UAPP_STATUS_NOT_FOUND);
    CHECK(page == NULL);
    CHECK_STATUS(uapp_document_service_open_page(service, UAPP_ID_NIL_HEX, &page),
                 UAPP_STATUS_ID_MALFORMED);
    CHECK_STATUS(uapp_document_service_delete_block(service,
                                                    "0123456789abcdef0123456789abcdef"),
                 UAPP_STATUS_NOT_FOUND);
    /* Uppercase hex parses -- the parser accepts either case and renders
     * lowercase, so a client that upper-cased an id is not punished for it. */
    CHECK_STATUS(uapp_document_service_open_page(service, "0123456789ABCDEF0123456789ABCDEF",
                                                 &page),
                 UAPP_STATUS_NOT_FOUND);

    uapp_document_service_close(&service);
}

static void test_boundary_row_index_bounds(void) {
    uapp_document_service_t* service = NULL;
    uapp_page_t* page = NULL;
    char* page_id = NULL;
    const char* borrowed = NULL;
    uapp_block_type_t type = UAPP_BLOCK_TYPE_DIVIDER;
    size_t count = 0;
    char* owned = NULL;

    REQUIRE_OK(open_memory_service(&service));
    REQUIRE_OK(uapp_document_service_create_page(service, "Bounds", &page));
    REQUIRE_OK(uapp_page_copy_id_hex(page, &page_id));
    /* An EMPTY page has no valid index at all. */
    CHECK_STATUS(uapp_page_get_block_id_hex(page, 0u, &borrowed), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_block_type(page, 0u, &type), UAPP_STATUS_INVALID_ARGUMENT);
    uapp_page_free(&page);

    {
        char* id = add_text_block(service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND,
                                  UAPP_BLOCK_TYPE_PARAGRAPH, "only one");
        CHECK(id != NULL);
        uapp_free_string(&id);
    }
    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    REQUIRE_OK(uapp_page_get_block_count(page, &count));
    CHECK(count == 1u);

    /* One past the end, a wildly out-of-range index, and (size_t)-1. A
     * loop-bound bug is a caller error, not a missing block. */
    CHECK_STATUS(uapp_page_get_block_id_hex(page, 1u, &borrowed), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_block_parent_id_hex(page, 99u, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_block_type(page, (size_t)-1, &type),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_block_depth(page, 1u, &count), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_block_child_count(page, 1u, &count),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_copy_block_text(page, 1u, &owned), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(owned == NULL);
    CHECK_STATUS(uapp_page_get_block_timestamps(page, 1u, NULL, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    {
        uapp_block_content_t content;
        memset(&content, 0, sizeof content);
        content.struct_size = (uint32_t)sizeof content;
        CHECK_STATUS(uapp_page_get_block_content(page, 1u, &content),
                     UAPP_STATUS_INVALID_ARGUMENT);
    }

    /* find_block_index: malformed is told apart from "not on this page". */
    {
        size_t index = 7u;
        CHECK_STATUS(uapp_page_find_block_index(page, "nope", &index),
                     UAPP_STATUS_ID_MALFORMED);
        CHECK(index == 7u);
        CHECK_STATUS(uapp_page_find_block_index(page, "0123456789abcdef0123456789abcdef",
                                                &index),
                     UAPP_STATUS_NOT_FOUND);
        CHECK(index == 7u);
        CHECK_STATUS(uapp_page_find_block_index(page, NULL, &index),
                     UAPP_STATUS_INVALID_ARGUMENT);
    }

    uapp_page_free(&page);
    uapp_free_string(&page_id);
    uapp_document_service_close(&service);
}

static void test_boundary_use_after_close(void) {
    uapp_document_service_t* service = NULL;
    uapp_page_t* page = NULL;
    uapp_block_content_t content = text_content(UAPP_BLOCK_TYPE_PARAGRAPH, "t");
    const char* borrowed = "untouched";
    size_t count = 7u;
    char* owned = NULL;

    REQUIRE_OK(open_memory_service(&service));
    REQUIRE_OK(uapp_document_service_create_page(service, "Closing time", &page));

    uapp_document_service_close(&service);
    CHECK(service == NULL);
    /* Closing twice is a no-op, not a double free. */
    uapp_document_service_close(&service);
    CHECK(service == NULL);

    /* Every operation on the closed service is now a defined rejection. */
    CHECK_STATUS(uapp_document_service_page_count(service, &count),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(count == 7u);
    CHECK_STATUS(uapp_document_service_delete_page(service, UAPP_ID_NIL_HEX),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_document_service_add_block(service, UAPP_ID_NIL_HEX, NULL, 0u, &content,
                                                 NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    {
        /* A fresh variable: `page` below is still a live handle, and a failed
         * call leaves out-parameters untouched, so reusing it here would be a
         * leak in this test rather than a bug in the library. */
        uapp_page_t* fresh = NULL;
        CHECK_STATUS(uapp_document_service_create_page(service, "t", &fresh),
                     UAPP_STATUS_INVALID_ARGUMENT);
        CHECK(fresh == NULL);
    }

    /* Same discipline for the page handle. */
    uapp_page_free(&page);
    CHECK(page == NULL);
    uapp_page_free(&page);
    CHECK_STATUS(uapp_page_get_title(page, &borrowed), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STR(borrowed, "untouched");
    CHECK_STATUS(uapp_page_copy_title(page, &owned), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(owned == NULL);
    CHECK_STATUS(uapp_page_get_block_count(page, &count), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(count == 7u);
}

/*
 * A handle of the wrong type.
 *
 * Not contrived: it is what a generated binding does when two `long` handle
 * fields get swapped, and on the Kotlin/Swift side both are just opaque
 * integers with nothing to distinguish them. Every handle carries a distinct
 * magic cookie as its first member precisely so this is caught instead of
 * dereferenced -- and this case deliberately mixes in a NOTE handle, because
 * the three surfaces now share one binding and one Kotlin `Long`.
 */
static void test_boundary_wrong_handle_type(void) {
    uapp_document_service_t* doc = NULL;
    uapp_note_service_t* notes = NULL;
    uapp_page_t* page = NULL;
    uapp_note_t* note = NULL;
    const char* borrowed = NULL;
    size_t count = 0;

    REQUIRE_OK(open_memory_service(&doc));
    REQUIRE_OK(uapp_document_service_create_page(doc, "Mixed up", &page));

    {
        uapp_note_service_config_t note_config;
        REQUIRE_OK(uapp_note_service_config_init(&note_config));
        note_config.database_path = ":memory:";
        REQUIRE_OK(uapp_note_service_open(&note_config, &notes));
    }
    REQUIRE_OK(uapp_note_service_create(notes, "a note", "body", &note));

    /* A page handle where a service is expected, and vice versa. */
    CHECK_STATUS(uapp_document_service_page_count((uapp_document_service_t*)page, &count),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_title((const uapp_page_t*)doc, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);

    /* ACROSS SURFACES: a note service where a document service belongs, a note
     * where a page belongs, and both the other way round. */
    CHECK_STATUS(uapp_document_service_page_count((uapp_document_service_t*)notes, &count),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_service_count((uapp_note_service_t*)doc, &count),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_title((const uapp_page_t*)note, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_note_get_title((const uapp_note_t*)page, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);

    /* And the destructors refuse to delete something that is not theirs --
     * leaking is strictly better than freeing a live handle of another type. */
    {
        uapp_page_t* misdirected = (uapp_page_t*)note;
        uapp_page_free(&misdirected);
        CHECK(misdirected != NULL); /* refused, so the pointer is NOT nulled */
        REQUIRE_OK(uapp_note_get_title(note, &borrowed));
        CHECK_STR(borrowed, "a note"); /* still alive and intact */
    }

    uapp_note_free(&note);
    uapp_note_service_close(&notes);
    uapp_page_free(&page);
    uapp_document_service_close(&doc);
}

static void test_boundary_config_and_struct_size(void) {
    uapp_document_service_config_t config;
    uapp_document_service_t* service = NULL;
    uapp_block_limits_t limits;
    uapp_block_content_t content;

    CHECK_STATUS(uapp_document_service_open(NULL, &service), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(service == NULL);
    REQUIRE_OK(uapp_document_service_config_init(&config));
    CHECK(config.struct_size == (uint32_t)sizeof config);
    CHECK(config.read_only == UAPP_FALSE);
    CHECK_STATUS(uapp_document_service_open(&config, NULL), UAPP_STATUS_INVALID_ARGUMENT);

    /* config_init leaves database_path NULL on purpose: there is no sensible
     * default, and inventing one would put a database somewhere the caller did
     * not ask for. */
    CHECK_STATUS(uapp_document_service_open(&config, &service), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(service == NULL);

    /* A struct_size smaller than this build's is rejected: honouring it would
     * mean reading past the end of the caller's object. */
    config.database_path = ":memory:";
    config.struct_size   = 0u;
    CHECK_STATUS(uapp_document_service_open(&config, &service), UAPP_STATUS_INVALID_ARGUMENT);
    config.struct_size = (uint32_t)(sizeof config - 1u);
    CHECK_STATUS(uapp_document_service_open(&config, &service), UAPP_STATUS_INVALID_ARGUMENT);
    /* A caller from the future, with a LARGER struct, is accepted. */
    config.struct_size = (uint32_t)(sizeof config + 64u);
    REQUIRE_OK(uapp_document_service_open(&config, &service));
    uapp_document_service_close(&service);

    /* Negative timeouts are nonsense, not a clamp. */
    REQUIRE_OK(uapp_document_service_config_init(&config));
    config.database_path   = ":memory:";
    config.busy_timeout_ms = -1;
    CHECK_STATUS(uapp_document_service_open(&config, &service), UAPP_STATUS_INVALID_ARGUMENT);

    /* An unopenable path arrives as a status, with the code intact, rather
     * than as an abort. */
    REQUIRE_OK(uapp_document_service_config_init(&config));
    config.database_path = "/nonexistent-directory-uapp-capi/blocks.db";
    CHECK_STATUS(uapp_document_service_open(&config, &service),
                 UAPP_STATUS_STORE_OPEN_FAILED);
    CHECK(service == NULL);
    REQUIRE_OK(uapp_document_service_config_init(&config));
    config.database_path = "";
    CHECK_STATUS(uapp_document_service_open(&config, &service), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(service == NULL);

    /* Same rule on the limits struct. */
    memset(&limits, 0, sizeof limits);
    limits.struct_size = 0u;
    CHECK_STATUS(uapp_block_get_limits(&limits), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(limits.max_block_text_bytes == 0u); /* nothing written on rejection */
    limits.struct_size = (uint32_t)(sizeof limits + 64u);
    REQUIRE_OK(uapp_block_get_limits(&limits));

    /* And on the content struct, in BOTH directions. A content struct whose
     * struct_size was never set -- the mistake a caller makes by memset'ing
     * instead of calling init -- is rejected rather than misread. */
    REQUIRE_OK(open_memory_service(&service));
    memset(&content, 0, sizeof content);
    content.type = UAPP_BLOCK_TYPE_PARAGRAPH;
    content.text = "t";
    CHECK(content.struct_size == 0u);
    CHECK_STATUS(uapp_document_service_add_block(service, "0123456789abcdef0123456789abcdef",
                                                 NULL, 0u, &content, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    content.struct_size = (uint32_t)(sizeof content - 1u);
    CHECK_STATUS(uapp_document_service_add_block(service, "0123456789abcdef0123456789abcdef",
                                                 NULL, 0u, &content, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    uapp_document_service_close(&service);
}

static void test_boundary_move_cycles_and_positions(void) {
    uapp_document_service_t* service = NULL;
    uapp_page_t* page = NULL;
    char* page_id = NULL;
    char* parent_id = NULL;
    char* child_id = NULL;
    char* grandchild_id = NULL;
    char* other_page_id = NULL;
    char* other_block_id = NULL;

    REQUIRE_OK(open_memory_service(&service));
    REQUIRE_OK(uapp_document_service_create_page(service, "Cycles", &page));
    REQUIRE_OK(uapp_page_copy_id_hex(page, &page_id));
    uapp_page_free(&page);
    REQUIRE_OK(uapp_document_service_create_page(service, "Elsewhere", &page));
    REQUIRE_OK(uapp_page_copy_id_hex(page, &other_page_id));
    uapp_page_free(&page);

    parent_id = add_text_block(service, page_id, NULL, UAPP_BLOCK_POSITION_APPEND,
                               UAPP_BLOCK_TYPE_BULLETED_LIST_ITEM, "parent");
    CHECK(parent_id != NULL);
    if (parent_id == NULL) { goto cleanup; }
    child_id = add_text_block(service, page_id, parent_id, UAPP_BLOCK_POSITION_APPEND,
                              UAPP_BLOCK_TYPE_BULLETED_LIST_ITEM, "child");
    CHECK(child_id != NULL);
    if (child_id == NULL) { goto cleanup; }
    grandchild_id = add_text_block(service, page_id, child_id, UAPP_BLOCK_POSITION_APPEND,
                                   UAPP_BLOCK_TYPE_BULLETED_LIST_ITEM, "grandchild");
    CHECK(grandchild_id != NULL);
    other_block_id = add_text_block(service, other_page_id, NULL, UAPP_BLOCK_POSITION_APPEND,
                                    UAPP_BLOCK_TYPE_PARAGRAPH, "stranger");
    CHECK(other_block_id != NULL);

    /* A block cannot become its own parent. */
    CHECK_STATUS(uapp_document_service_move_block(service, parent_id, parent_id, 0u),
                 UAPP_STATUS_BLOCK_CYCLE_DETECTED);
    /* Nor a child of its own child, or of any descendant. */
    CHECK_STATUS(uapp_document_service_move_block(service, parent_id, child_id, 0u),
                 UAPP_STATUS_BLOCK_CYCLE_DETECTED);
    if (grandchild_id != NULL) {
        CHECK_STATUS(uapp_document_service_move_block(service, parent_id, grandchild_id, 0u),
                     UAPP_STATUS_BLOCK_CYCLE_DETECTED);
    }

    /* A cross-page move is NOT IMPLEMENTED in the core and says so rather than
     * half-doing it. */
    if (other_block_id != NULL) {
        CHECK_STATUS(uapp_document_service_move_block(service, parent_id, other_block_id, 0u),
                     UAPP_STATUS_INVALID_ARGUMENT);
    }

    /* A position past the end is rejected, not clamped. */
    CHECK_STATUS(uapp_document_service_move_block(service, child_id, NULL, 99u),
                 UAPP_STATUS_BLOCK_POSITION_INVALID);
    /* A parent that does not exist is NOT_FOUND. */
    CHECK_STATUS(uapp_document_service_move_block(service, child_id,
                                                  "0123456789abcdef0123456789abcdef", 0u),
                 UAPP_STATUS_NOT_FOUND);
    /* Adding under a nonexistent parent, likewise. */
    {
        uapp_block_content_t content = text_content(UAPP_BLOCK_TYPE_PARAGRAPH, "orphan");
        CHECK_STATUS(uapp_document_service_add_block(service, page_id,
                                                     "0123456789abcdef0123456789abcdef", 0u,
                                                     &content, NULL),
                     UAPP_STATUS_NOT_FOUND);
        /* And a position past the end of an empty sibling list. */
        CHECK_STATUS(uapp_document_service_add_block(service, page_id, NULL, 99u, &content,
                                                     NULL),
                     UAPP_STATUS_BLOCK_POSITION_INVALID);
    }

    /* After all that, the tree is exactly as it was: a rejected move never
     * half-applies. */
    REQUIRE_OK(uapp_document_service_open_page(service, page_id, &page));
    {
        size_t count = 0;
        size_t depth = 0;
        REQUIRE_OK(uapp_page_get_block_count(page, &count));
        CHECK(count == 3u);
        REQUIRE_OK(uapp_page_get_block_depth(page, 0u, &depth));
        CHECK(depth == 1u);
        REQUIRE_OK(uapp_page_get_block_depth(page, 1u, &depth));
        CHECK(depth == 2u);
        REQUIRE_OK(uapp_page_get_block_depth(page, 2u, &depth));
        CHECK(depth == 3u);
    }
    uapp_page_free(&page);

cleanup:
    uapp_free_string(&other_block_id);
    uapp_free_string(&other_page_id);
    uapp_free_string(&grandchild_id);
    uapp_free_string(&child_id);
    uapp_free_string(&parent_id);
    uapp_free_string(&page_id);
    uapp_document_service_close(&service);
}

/* ===========================================================================
 * Harness
 * =========================================================================== */

static const uapp_test_case_t k_cases[] = {
    {"slice: block limits are readable at runtime", test_block_limits},
    {"slice: block type names and NOT IMPLEMENTED reasons", test_block_type_metadata},
    {"slice: the ABI enums are range-pinned at both ends",
     test_enum_range_is_pinned_both_ways},
    {"slice: content init sets struct_size and defaults", test_content_init_defaults},
    {"slice: a page with one block of every implemented type",
     test_slice_page_with_every_block_type},
    {"slice: update block content and change its type", test_slice_update_block},
    {"slice: delete block cascades to the whole subtree", test_slice_delete_block_cascades},
    {"slice: reorder within the parent", test_slice_reorder_block},
    {"slice: move re-parents the block and its subtree", test_slice_move_block},
    {"slice: rename and delete a page", test_slice_rename_and_delete_page},
    {"slice: pages persist across close and reopen", test_slice_persists_across_close},
    {"slice: two independent document services do not see each other",
     test_slice_independent_services},
    {"slice: a read-only service reads but does not write", test_slice_read_only_service},

    {"boundary: null handles are rejected everywhere", test_boundary_null_handles},
    {"boundary: null strings and null out-params", test_boundary_null_strings_and_out_params},
    {"boundary: an out-of-range block type discriminant",
     test_boundary_block_type_discriminant},
    {"boundary: content validation is the core's, not a second opinion",
     test_boundary_content_validation},
    {"boundary: malformed ids are told apart from absent ones", test_boundary_bad_ids},
    {"boundary: block row index bounds", test_boundary_row_index_bounds},
    {"boundary: use after close is a status, not a crash", test_boundary_use_after_close},
    {"boundary: a wrong-type handle is rejected, across surfaces",
     test_boundary_wrong_handle_type},
    {"boundary: config, limits and content struct_size versioning",
     test_boundary_config_and_struct_size},
    {"boundary: move cycles, cross-page moves and bad positions",
     test_boundary_move_cycles_and_positions}
};

int main(int argc, char** argv) {
    const char* filter = (argc > 1) ? argv[1] : NULL;
    return uapp_run_test_cases("uapp blocks C ABI tests", k_cases,
                               sizeof k_cases / sizeof k_cases[0], filter);
}
