/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/capi/search_capi_test.c -- the unified search C ABI, exercised from C.
 *
 * C99, compiled by a C compiler at -std=c99 with the same -Wall -Wextra
 * -Wpedantic -Wconversion -Werror posture as the other two capi test files. A
 * C++-ism in uapp_search.h breaks this build.
 *
 * WHAT THIS FILE IS FOR, AND WHY IT IS THE ONLY ONE THAT CAN DO IT
 *
 * The property under test only exists once all three surfaces are assembled:
 *
 *     ONE query string, through ONE C function, returning a note AND a block
 *     from a page in ONE result list, each row saying which it is.
 *
 * tests/capi/note_capi_test.c proves note-only search; blocks_capi_test.c
 * proves the document model. Neither can prove that a Kotlin binding holding
 * three handles gets one unified answer, because that needs the note service,
 * the document service and the search index alive at once -- which is exactly
 * what a mobile app does.
 *
 * EVERY CASE USES A REAL FILE, NOT ":memory:". Each in-memory open creates a
 * PRIVATE database, so three handles on ":memory:" are three databases that
 * cannot see each other's index rows -- and the symptom is zero results with
 * no error. One scratch file, shared by all three handles, is what "unified"
 * actually means. uapp_search.h says so at the top, and the "an in-memory
 * trio sees nothing" case below is the proof that it degrades honestly rather
 * than silently.
 *
 * Two groups, run as two separate CTest entries: "slice:" is "unified search
 * works through the ABI", "boundary:" is "the ABI does not crash when the
 * caller is wrong".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uapp/capi/uapp_blocks.h"
#include "uapp/capi/uapp_capi.h"
#include "uapp/capi/uapp_notes.h"
#include "uapp/capi/uapp_search.h"

#include "capi_test_harness.h"

/* --- the harness-under-test: three handles on one file --------------------- */

typedef struct trio {
    uapp_note_service_t*     notes;
    uapp_document_service_t* docs;
    uapp_search_index_t*     index;
    const char*              path;
} trio_t;

static void trio_init(trio_t* t, const char* path) {
    t->notes = NULL;
    t->docs  = NULL;
    t->index = NULL;
    t->path  = path;
    remove(path);
}

/* Open the two CONTENT services. They must come first and read-write: the
 * index tables are created by whichever content store opens the file first,
 * and the index handle is read-only by default. */
static uapp_status_t trio_open_content(trio_t* t) {
    uapp_note_service_config_t note_config;
    uapp_document_service_config_t doc_config;
    uapp_status_t status;

    status = uapp_note_service_config_init(&note_config);
    if (status != UAPP_STATUS_OK) { return status; }
    note_config.database_path = t->path;
    status = uapp_note_service_open(&note_config, &t->notes);
    if (status != UAPP_STATUS_OK) { return status; }

    status = uapp_document_service_config_init(&doc_config);
    if (status != UAPP_STATUS_OK) { return status; }
    doc_config.database_path = t->path;
    return uapp_document_service_open(&doc_config, &t->docs);
}

static uapp_status_t trio_open_index(trio_t* t) {
    uapp_search_index_config_t config;
    const uapp_status_t init = uapp_search_index_config_init(&config);
    if (init != UAPP_STATUS_OK) { return init; }
    config.database_path = t->path;
    return uapp_search_index_open(&config, &t->index);
}

static void trio_close(trio_t* t) {
    uapp_search_index_close(&t->index);
    uapp_document_service_close(&t->docs);
    uapp_note_service_close(&t->notes);
    if (t->path != NULL) { remove(t->path); }
}

/* Create a note. Returns its owned id, or NULL. */
static char* make_note(trio_t* t, const char* title, const char* body) {
    uapp_note_t* note = NULL;
    char* id = NULL;
    if (uapp_note_service_create(t->notes, title, body, &note) != UAPP_STATUS_OK) {
        return NULL;
    }
    if (uapp_note_copy_id_hex(note, &id) != UAPP_STATUS_OK) {
        uapp_note_free(&note);
        return NULL;
    }
    uapp_note_free(&note);
    return id;
}

/* Create a page. Returns its owned id, or NULL. */
static char* make_page(trio_t* t, const char* title) {
    uapp_page_t* page = NULL;
    char* id = NULL;
    if (uapp_document_service_create_page(t->docs, title, &page) != UAPP_STATUS_OK) {
        return NULL;
    }
    if (uapp_page_copy_id_hex(page, &id) != UAPP_STATUS_OK) {
        uapp_page_free(&page);
        return NULL;
    }
    uapp_page_free(&page);
    return id;
}

/* Append a paragraph block. Returns its owned id, or NULL. */
static char* make_block(trio_t* t, const char* page_id, const char* text) {
    uapp_block_content_t content;
    char* id = NULL;
    if (uapp_block_content_init(UAPP_BLOCK_TYPE_PARAGRAPH, &content) != UAPP_STATUS_OK) {
        return NULL;
    }
    content.text = text;
    if (uapp_document_service_add_block(t->docs, page_id, NULL, UAPP_BLOCK_POSITION_APPEND,
                                        &content, &id) != UAPP_STATUS_OK) {
        return NULL;
    }
    return id;
}

/* Find the first row of `kind` whose source id is `id_hex`. Returns 1 on a
 * match and writes the row index; 0 otherwise. */
static int find_hit(const uapp_unified_results_t* results,
                    uapp_source_kind_t kind,
                    const char* id_hex,
                    size_t* out_index) {
    size_t count = 0;
    size_t i;
    if (uapp_unified_results_count(results, &count) != UAPP_STATUS_OK) { return 0; }
    for (i = 0; i < count; ++i) {
        uapp_source_kind_t row_kind = UAPP_SOURCE_KIND_NOTE;
        const char* row_id = NULL;
        if (uapp_unified_results_get_kind(results, i, &row_kind) != UAPP_STATUS_OK) { continue; }
        if (uapp_unified_results_get_source_id_hex(results, i, &row_id) != UAPP_STATUS_OK) {
            continue;
        }
        if (row_kind == kind && row_id != NULL && strcmp(row_id, id_hex) == 0) {
            *out_index = i;
            return 1;
        }
    }
    return 0;
}

/* ===========================================================================
 * The slice
 * =========================================================================== */

static void test_search_limits_and_kind_names(void) {
    uapp_search_limits_t limits;

    memset(&limits, 0, sizeof limits);
    limits.struct_size = (uint32_t)sizeof limits;
    REQUIRE_OK(uapp_search_get_limits(&limits));
    CHECK(limits.id_hex_length == 32u);
    CHECK(limits.max_query_bytes > 0u);
    CHECK(limits.max_results > 0u);
    /* Three kinds implemented. The rest of spec section 16's list has NO
     * enumerator on purpose: a value with no indexer behind it is a promise
     * this enum cannot keep. */
    CHECK(limits.source_kind_count == 3);

    CHECK_STR(uapp_source_kind_name(UAPP_SOURCE_KIND_NOTE), "note");
    CHECK_STR(uapp_source_kind_name(UAPP_SOURCE_KIND_PAGE), "page");
    CHECK_STR(uapp_source_kind_name(UAPP_SOURCE_KIND_BLOCK), "block");
    /* Totally defined for any integer. */
    CHECK_STR(uapp_source_kind_name((uapp_source_kind_t)3), "unknown");
    CHECK_STR(uapp_source_kind_name((uapp_source_kind_t)424242), "unknown");
    CHECK_STR(uapp_source_kind_name((uapp_source_kind_t)-1), "unknown");
    CHECK_STR(uapp_source_kind_name(UAPP_SOURCE_KIND_FORCE_INT32), "unknown");

    /* The nil-id string the header promises is one id's worth of hex. */
    CHECK(strlen(UAPP_ID_NIL_HEX) == (size_t)UAPP_ID_HEX_LENGTH);
    CHECK(strlen(UAPP_ID_NIL_HEX) == limits.id_hex_length);
}

/*
 * THE CASE THIS WHOLE FILE EXISTS FOR: one query, a note AND a block on a
 * page, both in one result list, each row typed and carrying the ids needed
 * to open it.
 */
static void test_slice_unified_note_and_block(void) {
    trio_t t;
    char* note_id = NULL;
    char* page_id = NULL;
    char* block_id = NULL;
    uapp_unified_results_t* results = NULL;
    size_t count = 0;
    size_t note_row = 0;
    size_t block_row = 0;
    size_t page_row = 0;

    trio_init(&t, UAPP_CAPI_SEARCH_TEST_DB_PATH);
    REQUIRE_OK(trio_open_content(&t));

    /* The shared term is "entropy". It appears in a NOTE body and in a BLOCK
     * on a page -- two different source kinds, one index, one ranking. */
    note_id = make_note(&t, "Information theory", "entropy measures surprise");
    page_id = make_page(&t, "Thermodynamics");
    CHECK(note_id != NULL);
    CHECK(page_id != NULL);
    if (note_id == NULL || page_id == NULL) { goto cleanup; }
    block_id = make_block(&t, page_id, "entropy never decreases");
    CHECK(block_id != NULL);
    /* A second block with an unrelated term, so the query is not trivially
     * matching everything on the page. */
    {
        char* other = make_block(&t, page_id, "enthalpy is something else");
        CHECK(other != NULL);
        uapp_free_string(&other);
    }

    REQUIRE_OK(trio_open_index(&t));

    /* The index is populated by the CONTENT WRITES, not by a separate
     * indexing call -- there is no such call, by design. */
    REQUIRE_OK(uapp_search_index_entry_count(t.index, &count));
    /* 1 note + 1 page title + 2 blocks. */
    CHECK(count == 4u);

    REQUIRE_OK(uapp_search_index_query(t.index, "entropy", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 2u);

    if (block_id != NULL && count == 2u) {
        const char* borrowed = NULL;
        double rank = 1.0;

        /* The NOTE row. */
        CHECK(find_hit(results, UAPP_SOURCE_KIND_NOTE, note_id, &note_row) == 1);
        REQUIRE_OK(uapp_unified_results_get_title(results, note_row, &borrowed));
        CHECK_STR(borrowed, "Information theory");
        REQUIRE_OK(uapp_unified_results_get_snippet(results, note_row, &borrowed));
        CHECK(borrowed[0] != '\0');
        /* The match is delimited, which is what makes the snippet renderable. */
        CHECK(strstr(borrowed, "[entropy]") != NULL);
        /* A note is its own container, so the container id is the NIL id --
         * never NULL, so a binding needs no special case. */
        REQUIRE_OK(uapp_unified_results_get_container_id_hex(results, note_row, &borrowed));
        CHECK_STR(borrowed, UAPP_ID_NIL_HEX);
        REQUIRE_OK(uapp_unified_results_get_rank(results, note_row, &rank));
        CHECK(rank <= 0.0); /* BM25: lower is better, and negative */

        /* The BLOCK row. */
        CHECK(find_hit(results, UAPP_SOURCE_KIND_BLOCK, block_id, &block_row) == 1);
        CHECK(block_row != note_row);
        /* The container is the PAGE: "open page X, scroll to block Y" is the
         * navigation a UI can only do because the row carries both ids. */
        REQUIRE_OK(uapp_unified_results_get_container_id_hex(results, block_row, &borrowed));
        CHECK_STR(borrowed, page_id);
        /* And the title is the CONTAINING PAGE's title, so a result list can
         * be grouped and labelled without a second query per row. */
        REQUIRE_OK(uapp_unified_results_get_title(results, block_row, &borrowed));
        CHECK_STR(borrowed, "Thermodynamics");
        REQUIRE_OK(uapp_unified_results_get_snippet(results, block_row, &borrowed));
        CHECK(strstr(borrowed, "[entropy]") != NULL);

        /* Rows are already ordered best-first, across kinds. One index table
         * means one set of corpus statistics, so these two numbers are
         * genuinely comparable rather than normalised guesses. */
        {
            double first = 0.0;
            double second = 0.0;
            REQUIRE_OK(uapp_unified_results_get_rank(results, 0u, &first));
            REQUIRE_OK(uapp_unified_results_get_rank(results, 1u, &second));
            CHECK(first <= second);
        }
    }
    uapp_unified_results_free(&results);
    CHECK(results == NULL);

    /* A PAGE hit: a page is indexed for its TITLE, as its own row. Without
     * that row, searching for a page by name would find nothing, because a
     * page's title is not inside any block. */
    REQUIRE_OK(uapp_search_index_query(t.index, "thermodynamics", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 1u);
    if (count == 1u && page_id != NULL) {
        const char* borrowed = NULL;
        CHECK(find_hit(results, UAPP_SOURCE_KIND_PAGE, page_id, &page_row) == 1);
        CHECK(page_row == 0u);
        /* A page is its own container too. */
        REQUIRE_OK(uapp_unified_results_get_container_id_hex(results, page_row, &borrowed));
        CHECK_STR(borrowed, UAPP_ID_NIL_HEX);
        REQUIRE_OK(uapp_unified_results_get_source_id_hex(results, page_row, &borrowed));
        CHECK_STR(borrowed, page_id);
    }
    uapp_unified_results_free(&results);

    /* A term in only one kind returns only that kind. */
    REQUIRE_OK(uapp_search_index_query(t.index, "enthalpy", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 1u);
    if (count == 1u) {
        uapp_source_kind_t kind = UAPP_SOURCE_KIND_NOTE;
        REQUIRE_OK(uapp_unified_results_get_kind(results, 0u, &kind));
        CHECK(kind == UAPP_SOURCE_KIND_BLOCK);
        CHECK_STR(uapp_source_kind_name(kind), "block");
    }
    uapp_unified_results_free(&results);

    /* NO MATCH is OK with zero rows, NOT a NOT_FOUND. */
    REQUIRE_OK(uapp_search_index_query(t.index, "quicksort", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 0u);
    uapp_unified_results_free(&results);

cleanup:
    uapp_free_string(&block_id);
    uapp_free_string(&page_id);
    uapp_free_string(&note_id);
    trio_close(&t);
}

/*
 * The index cannot go stale behind an edit or a delete. This is the property
 * that rots silently if index maintenance is a separate step -- which is why
 * there is no index() function on this surface at all.
 */
static void test_slice_index_follows_content(void) {
    trio_t t;
    char* note_id = NULL;
    char* page_id = NULL;
    char* block_id = NULL;
    uapp_unified_results_t* results = NULL;
    size_t count = 0;

    trio_init(&t, UAPP_CAPI_SEARCH_TEST_EDITS_DB_PATH);
    REQUIRE_OK(trio_open_content(&t));

    note_id = make_note(&t, "Note title", "aardvark");
    page_id = make_page(&t, "Page title");
    CHECK(note_id != NULL && page_id != NULL);
    if (note_id == NULL || page_id == NULL) { goto cleanup; }
    block_id = make_block(&t, page_id, "narwhal");
    CHECK(block_id != NULL);
    if (block_id == NULL) { goto cleanup; }

    REQUIRE_OK(trio_open_index(&t));

    /* Both terms are findable to start with. */
    REQUIRE_OK(uapp_search_index_query(t.index, "aardvark", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 1u);
    uapp_unified_results_free(&results);
    REQUIRE_OK(uapp_search_index_query(t.index, "narwhal", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 1u);
    uapp_unified_results_free(&results);

    /* --- EDIT a block: the new term hits, the old one does not ------------- */
    {
        uapp_block_content_t content;
        REQUIRE_OK(uapp_block_content_init(UAPP_BLOCK_TYPE_PARAGRAPH, &content));
        content.text = "pangolin";
        REQUIRE_OK(uapp_document_service_update_block(t.docs, block_id, &content));
    }
    REQUIRE_OK(uapp_search_index_query(t.index, "pangolin", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 1u);
    uapp_unified_results_free(&results);
    REQUIRE_OK(uapp_search_index_query(t.index, "narwhal", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 0u); /* no stale hit left behind */
    uapp_unified_results_free(&results);

    /* --- EDIT a note, through the OTHER surface, same index --------------- */
    {
        uapp_note_t* note = NULL;
        REQUIRE_OK(uapp_note_service_open_note(t.notes, note_id, &note));
        REQUIRE_OK(uapp_note_set_body(note, "capybara"));
        REQUIRE_OK(uapp_note_service_save(t.notes, note));
        uapp_note_free(&note);
    }
    REQUIRE_OK(uapp_search_index_query(t.index, "capybara", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 1u);
    uapp_unified_results_free(&results);
    REQUIRE_OK(uapp_search_index_query(t.index, "aardvark", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 0u);
    uapp_unified_results_free(&results);

    /* --- RENAME the page: its title row follows ---------------------------- */
    REQUIRE_OK(uapp_document_service_rename_page(t.docs, page_id, "Renamed okapi"));
    REQUIRE_OK(uapp_search_index_query(t.index, "okapi", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 1u);
    uapp_unified_results_free(&results);

    /* --- DELETE the block: its row goes ------------------------------------ */
    REQUIRE_OK(uapp_document_service_delete_block(t.docs, block_id));
    REQUIRE_OK(uapp_search_index_query(t.index, "pangolin", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 0u);
    uapp_unified_results_free(&results);

    /* --- DELETE the page: its title row goes too --------------------------- */
    REQUIRE_OK(uapp_document_service_delete_page(t.docs, page_id));
    REQUIRE_OK(uapp_search_index_query(t.index, "okapi", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 0u);
    uapp_unified_results_free(&results);

    /* --- DELETE the note: only the note row remains to go ------------------ */
    REQUIRE_OK(uapp_note_service_delete(t.notes, note_id));
    REQUIRE_OK(uapp_search_index_query(t.index, "capybara", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 0u);
    uapp_unified_results_free(&results);

    /* Nothing is left in the index at all. */
    REQUIRE_OK(uapp_search_index_entry_count(t.index, &count));
    CHECK(count == 0u);

cleanup:
    uapp_free_string(&block_id);
    uapp_free_string(&page_id);
    uapp_free_string(&note_id);
    trio_close(&t);
}

static void test_slice_limit_is_clamped(void) {
    trio_t t;
    char* page_id = NULL;
    uapp_unified_results_t* results = NULL;
    size_t count = 0;
    int i;

    trio_init(&t, UAPP_CAPI_SEARCH_TEST_LIMIT_DB_PATH);
    REQUIRE_OK(trio_open_content(&t));
    page_id = make_page(&t, "Repetition");
    CHECK(page_id != NULL);
    if (page_id == NULL) { goto cleanup; }
    for (i = 0; i < 5; ++i) {
        char* id = make_block(&t, page_id, "wombat sighting");
        CHECK(id != NULL);
        uapp_free_string(&id);
    }
    REQUIRE_OK(trio_open_index(&t));

    REQUIRE_OK(uapp_search_index_query(t.index, "wombat", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 5u);
    uapp_unified_results_free(&results);

    REQUIRE_OK(uapp_search_index_query(t.index, "wombat", 2u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 2u);
    uapp_unified_results_free(&results);

    /* Above the maximum is CLAMPED, not rejected. */
    REQUIRE_OK(uapp_search_index_query(t.index, "wombat", (size_t)1000000, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 5u);
    uapp_unified_results_free(&results);

    /* (size_t)-1 is "clamp", not an overflow. */
    REQUIRE_OK(uapp_search_index_query(t.index, "wombat", (size_t)-1, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 5u);
    uapp_unified_results_free(&results);

cleanup:
    uapp_free_string(&page_id);
    trio_close(&t);
}

/*
 * The documented failure mode, proven rather than asserted: three handles on
 * ":memory:" are three PRIVATE databases, so the index sees nothing. It must
 * degrade HONESTLY -- a clean open and zero results, or a clean refusal to
 * open -- never a crash and never a half-answer.
 */
static void test_slice_in_memory_trio_is_not_unified(void) {
    uapp_note_service_t* notes = NULL;
    uapp_document_service_t* docs = NULL;
    uapp_search_index_t* index = NULL;
    uapp_note_t* note = NULL;
    uapp_search_index_config_t index_config;
    uapp_status_t opened;

    {
        uapp_note_service_config_t note_config;
        REQUIRE_OK(uapp_note_service_config_init(&note_config));
        note_config.database_path = ":memory:";
        REQUIRE_OK(uapp_note_service_open(&note_config, &notes));
    }
    {
        uapp_document_service_config_t doc_config;
        REQUIRE_OK(uapp_document_service_config_init(&doc_config));
        doc_config.database_path = ":memory:";
        REQUIRE_OK(uapp_document_service_open(&doc_config, &docs));
    }
    REQUIRE_OK(uapp_note_service_create(notes, "Invisible", "platypus", &note));
    uapp_note_free(&note);

    REQUIRE_OK(uapp_search_index_config_init(&index_config));
    index_config.database_path = ":memory:";
    opened = uapp_search_index_open(&index_config, &index);

    /* A read-only index handle on a fresh private in-memory database finds no
     * index tables, so it refuses to open rather than reporting "no results"
     * for every query forever. Either outcome is honest; what matters is that
     * it is not a silent half-answer. */
    if (opened == UAPP_STATUS_OK) {
        uapp_unified_results_t* results = NULL;
        size_t count = 99u;
        REQUIRE_OK(uapp_search_index_query(index, "platypus", 10u, &results));
        REQUIRE_OK(uapp_unified_results_count(results, &count));
        CHECK(count == 0u);
        uapp_unified_results_free(&results);
        uapp_search_index_close(&index);
    } else {
        CHECK_STATUS(opened, UAPP_STATUS_STORE_OPEN_FAILED);
        CHECK(index == NULL);
    }

    uapp_document_service_close(&docs);
    uapp_note_service_close(&notes);
}

static void test_slice_two_independent_indexes(void) {
    trio_t alpha;
    trio_t beta;
    char* alpha_note = NULL;
    char* beta_note = NULL;
    uapp_unified_results_t* results = NULL;
    size_t count = 0;

    trio_init(&alpha, UAPP_CAPI_SEARCH_TEST_ALPHA_DB_PATH);
    trio_init(&beta, UAPP_CAPI_SEARCH_TEST_BETA_DB_PATH);
    REQUIRE_OK(trio_open_content(&alpha));
    REQUIRE_OK(trio_open_content(&beta));

    alpha_note = make_note(&alpha, "Alpha", "kangaroo");
    beta_note  = make_note(&beta, "Beta", "platypus");
    CHECK(alpha_note != NULL && beta_note != NULL);

    REQUIRE_OK(trio_open_index(&alpha));
    REQUIRE_OK(trio_open_index(&beta));
    CHECK(alpha.index != beta.index);

    /* Two files, two indexes. Each sees exactly its own content -- which is
     * the "degrades honestly" property, stated as a test: you get correct
     * results for what is actually in the file you asked about. */
    REQUIRE_OK(uapp_search_index_query(alpha.index, "kangaroo", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 1u);
    uapp_unified_results_free(&results);
    REQUIRE_OK(uapp_search_index_query(alpha.index, "platypus", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 0u);
    uapp_unified_results_free(&results);
    REQUIRE_OK(uapp_search_index_query(beta.index, "platypus", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 1u);
    uapp_unified_results_free(&results);

    /* Closing one index must not disturb the other. */
    uapp_search_index_close(&alpha.index);
    REQUIRE_OK(uapp_search_index_query(beta.index, "platypus", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 1u);
    uapp_unified_results_free(&results);

    uapp_free_string(&beta_note);
    uapp_free_string(&alpha_note);
    trio_close(&beta);
    trio_close(&alpha);
}

static void test_slice_results_outlive_the_index(void) {
    trio_t t;
    char* note_id = NULL;
    uapp_unified_results_t* results = NULL;
    const char* borrowed = NULL;
    size_t count = 0;

    trio_init(&t, UAPP_CAPI_SEARCH_TEST_OUTLIVE_DB_PATH);
    REQUIRE_OK(trio_open_content(&t));
    note_id = make_note(&t, "Survivor", "hippopotamus");
    CHECK(note_id != NULL);
    REQUIRE_OK(trio_open_index(&t));
    REQUIRE_OK(uapp_search_index_query(t.index, "hippopotamus", 10u, &results));

    /* A results handle is a self-contained snapshot: it holds no reference to
     * the connection, so closing the index does not invalidate it. */
    uapp_search_index_close(&t.index);
    CHECK(t.index == NULL);
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 1u);
    REQUIRE_OK(uapp_unified_results_get_title(results, 0u, &borrowed));
    CHECK_STR(borrowed, "Survivor");

    uapp_unified_results_free(&results);
    uapp_free_string(&note_id);
    trio_close(&t);
}

/* ===========================================================================
 * Boundary safety
 * =========================================================================== */

static void test_boundary_null_handles(void) {
    uapp_search_index_t* index = NULL;
    uapp_unified_results_t* results = NULL;
    uapp_source_kind_t kind = UAPP_SOURCE_KIND_BLOCK;
    const char* borrowed = NULL;
    size_t count = 123u;
    double rank = 0.0;

    CHECK_STATUS(uapp_search_index_query(NULL, "q", 10u, &results),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(results == NULL);
    CHECK_STATUS(uapp_search_index_entry_count(NULL, &count), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(count == 123u); /* a failed call must not touch the out-parameter */

    CHECK_STATUS(uapp_unified_results_count(NULL, &count), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_unified_results_get_kind(NULL, 0u, &kind),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(kind == UAPP_SOURCE_KIND_BLOCK);
    CHECK_STATUS(uapp_unified_results_get_source_id_hex(NULL, 0u, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_unified_results_get_container_id_hex(NULL, 0u, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_unified_results_get_title(NULL, 0u, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_unified_results_get_snippet(NULL, 0u, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_unified_results_get_rank(NULL, 0u, &rank),
                 UAPP_STATUS_INVALID_ARGUMENT);

    /* Destructors are total: NULL, and a pointer to NULL, are both fine. */
    uapp_search_index_close(NULL);
    uapp_search_index_close(&index);
    uapp_unified_results_free(NULL);
    uapp_unified_results_free(&results);
}

static void test_boundary_config_and_struct_size(void) {
    uapp_search_index_config_t config;
    uapp_search_index_t* index = NULL;
    uapp_search_limits_t limits;

    CHECK_STATUS(uapp_search_index_config_init(NULL), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_search_index_open(NULL, &index), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(index == NULL);

    REQUIRE_OK(uapp_search_index_config_init(&config));
    CHECK(config.struct_size == (uint32_t)sizeof config);
    /* read_only defaults to TRUE, and that default is the point: this surface
     * cannot write, so the connection behind it holds no write permission. */
    CHECK(config.read_only == UAPP_TRUE);
    CHECK(config.database_path == NULL);
    CHECK_STATUS(uapp_search_index_open(&config, NULL), UAPP_STATUS_INVALID_ARGUMENT);
    /* No default path: inventing one would read a database the caller did not
     * name. */
    CHECK_STATUS(uapp_search_index_open(&config, &index), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(index == NULL);

    config.database_path = UAPP_CAPI_SEARCH_TEST_CONFIG_DB_PATH;
    config.struct_size   = 0u;
    CHECK_STATUS(uapp_search_index_open(&config, &index), UAPP_STATUS_INVALID_ARGUMENT);
    config.struct_size = (uint32_t)(sizeof config - 1u);
    CHECK_STATUS(uapp_search_index_open(&config, &index), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(index == NULL);

    REQUIRE_OK(uapp_search_index_config_init(&config));
    config.database_path   = UAPP_CAPI_SEARCH_TEST_CONFIG_DB_PATH;
    config.busy_timeout_ms = -1;
    CHECK_STATUS(uapp_search_index_open(&config, &index), UAPP_STATUS_INVALID_ARGUMENT);

    /* A read-only index on a path no store has ever opened has no index tables
     * to query, so it says STORE_OPEN_FAILED rather than reporting "no
     * results" for every query forever. */
    REQUIRE_OK(uapp_search_index_config_init(&config));
    config.database_path = "/nonexistent-directory-uapp-capi/search.db";
    CHECK_STATUS(uapp_search_index_open(&config, &index), UAPP_STATUS_STORE_OPEN_FAILED);
    CHECK(index == NULL);
    REQUIRE_OK(uapp_search_index_config_init(&config));
    config.database_path = "";
    CHECK_STATUS(uapp_search_index_open(&config, &index), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(index == NULL);

    /* Same rule on the limits struct. */
    CHECK_STATUS(uapp_search_get_limits(NULL), UAPP_STATUS_INVALID_ARGUMENT);
    memset(&limits, 0, sizeof limits);
    limits.struct_size = 0u;
    CHECK_STATUS(uapp_search_get_limits(&limits), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(limits.max_query_bytes == 0u); /* nothing written on rejection */
    /* A caller from the future, with a LARGER struct, is accepted. */
    limits.struct_size = (uint32_t)(sizeof limits + 64u);
    REQUIRE_OK(uapp_search_get_limits(&limits));
    CHECK(limits.max_query_bytes > 0u);
}

static void test_boundary_bad_queries(void) {
    trio_t t;
    char* note_id = NULL;
    uapp_unified_results_t* results = NULL;
    uapp_search_limits_t limits;
    char* oversized = NULL;

    memset(&limits, 0, sizeof limits);
    limits.struct_size = (uint32_t)sizeof limits;
    REQUIRE_OK(uapp_search_get_limits(&limits));

    trio_init(&t, UAPP_CAPI_SEARCH_TEST_QUERY_DB_PATH);
    REQUIRE_OK(trio_open_content(&t));
    note_id = make_note(&t, "Target", "indexed content here");
    CHECK(note_id != NULL);
    REQUIRE_OK(trio_open_index(&t));

    /* NULL query. */
    CHECK_STATUS(uapp_search_index_query(t.index, NULL, 10u, &results),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(results == NULL);
    /* NULL out-parameter. */
    CHECK_STATUS(uapp_search_index_query(t.index, "content", 10u, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_search_index_entry_count(t.index, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);

    /*
     * A ZERO LIMIT IS INVALID_ARGUMENT HERE, and that is the one place this
     * surface deliberately differs from uapp_note_service_search(), where 0
     * means "the default". The unified port has no default to expose, and
     * inventing one would be behaviour this layer is not allowed to add -- a
     * zero limit would otherwise look exactly like "nothing matched".
     */
    CHECK_STATUS(uapp_search_index_query(t.index, "content", 0u, &results),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(results == NULL);

    /* A query with no indexable term is its own status, distinct from
     * "searched and found nothing" -- the UI must say different things. */
    CHECK_STATUS(uapp_search_index_query(t.index, "", 10u, &results),
                 UAPP_STATUS_SEARCH_QUERY_EMPTY);
    CHECK(results == NULL);
    CHECK_STATUS(uapp_search_index_query(t.index, "   ", 10u, &results),
                 UAPP_STATUS_SEARCH_QUERY_EMPTY);
    CHECK_STATUS(uapp_search_index_query(t.index, "!!! ???", 10u, &results),
                 UAPP_STATUS_SEARCH_QUERY_EMPTY);

    /* HOSTILE QUERIES. The index's own query language must never see raw user
     * text, and a binding is never asked to pre-escape anything. Each of these
     * must come back as a status or as results -- never as a crash, and never
     * as a syntax error leaking out of FTS5. */
    {
        static const char* const k_hostile[] = {
            "\"",
            "content OR 1=1",
            "NEAR(",
            "content*",
            "\"content",
            "content AND (",
            "^content",
            "content : content",
            "*",
            "-content",
            "{content",
            "content\" NOT \"content",
            "'; DROP TABLE search_fts; --"
        };
        size_t h;
        for (h = 0; h < sizeof k_hostile / sizeof k_hostile[0]; ++h) {
            uapp_unified_results_t* hostile_results = NULL;
            const uapp_status_t status =
                uapp_search_index_query(t.index, k_hostile[h], 10u, &hostile_results);
            /* Any of these three is acceptable; anything else is not. */
            CHECK(status == UAPP_STATUS_OK || status == UAPP_STATUS_SEARCH_QUERY_EMPTY ||
                  status == UAPP_STATUS_INVALID_ARGUMENT);
            if (status == UAPP_STATUS_OK) {
                size_t count = 0;
                CHECK_OK(uapp_unified_results_count(hostile_results, &count));
                uapp_unified_results_free(&hostile_results);
            } else {
                CHECK(hostile_results == NULL);
            }
        }
    }

    /* The index survived all of that. */
    {
        size_t count = 0;
        REQUIRE_OK(uapp_search_index_query(t.index, "content", 10u, &results));
        REQUIRE_OK(uapp_unified_results_count(results, &count));
        CHECK(count == 1u);
        uapp_unified_results_free(&results);
    }

    /* An over-length query is rejected rather than truncated. */
    oversized = (char*)malloc(limits.max_query_bytes + 8u);
    REQUIRE_OK(oversized != NULL ? UAPP_STATUS_OK : UAPP_STATUS_OUT_OF_MEMORY);
    memset(oversized, 'x', limits.max_query_bytes + 7u);
    oversized[limits.max_query_bytes + 7u] = '\0';
    CHECK_STATUS(uapp_search_index_query(t.index, oversized, 10u, &results),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(results == NULL);
    free(oversized);

    uapp_free_string(&note_id);
    trio_close(&t);
}

static void test_boundary_row_index_bounds(void) {
    trio_t t;
    char* note_id = NULL;
    uapp_unified_results_t* results = NULL;
    const char* borrowed = NULL;
    uapp_source_kind_t kind = UAPP_SOURCE_KIND_PAGE;
    double rank = 0.0;
    size_t count = 0;

    trio_init(&t, UAPP_CAPI_SEARCH_TEST_BOUNDS_DB_PATH);
    REQUIRE_OK(trio_open_content(&t));
    note_id = make_note(&t, "Indexed", "rhinoceros");
    CHECK(note_id != NULL);
    REQUIRE_OK(trio_open_index(&t));

    REQUIRE_OK(uapp_search_index_query(t.index, "rhinoceros", 10u, &results));
    REQUIRE_OK(uapp_unified_results_count(results, &count));
    CHECK(count == 1u);

    /* One past the end, a wildly out-of-range index, and (size_t)-1. A
     * loop-bound bug is a caller error, not a missing object. */
    CHECK_STATUS(uapp_unified_results_get_kind(results, 1u, &kind),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(kind == UAPP_SOURCE_KIND_PAGE); /* out-parameter untouched */
    CHECK_STATUS(uapp_unified_results_get_source_id_hex(results, 99u, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_unified_results_get_container_id_hex(results, (size_t)-1, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_unified_results_get_title(results, 1u, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_unified_results_get_snippet(results, 1u, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_unified_results_get_rank(results, 1u, &rank),
                 UAPP_STATUS_INVALID_ARGUMENT);
    /* NULL out-parameter with a VALID index. */
    CHECK_STATUS(uapp_unified_results_get_title(results, 0u, NULL),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_unified_results_count(results, NULL), UAPP_STATUS_INVALID_ARGUMENT);

    /* An empty result set has no valid index at all. */
    uapp_unified_results_free(&results);
    REQUIRE_OK(uapp_search_index_query(t.index, "triceratops", 10u, &results));
    CHECK_STATUS(uapp_unified_results_get_title(results, 0u, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_unified_results_get_kind(results, 0u, &kind),
                 UAPP_STATUS_INVALID_ARGUMENT);

    uapp_unified_results_free(&results);
    uapp_free_string(&note_id);
    trio_close(&t);
}

static void test_boundary_use_after_close(void) {
    trio_t t;
    char* note_id = NULL;
    uapp_unified_results_t* results = NULL;
    size_t count = 7u;
    const char* borrowed = "untouched";

    trio_init(&t, UAPP_CAPI_SEARCH_TEST_CLOSE_DB_PATH);
    REQUIRE_OK(trio_open_content(&t));
    note_id = make_note(&t, "Closing time", "body");
    CHECK(note_id != NULL);
    REQUIRE_OK(trio_open_index(&t));
    REQUIRE_OK(uapp_search_index_query(t.index, "closing", 10u, &results));

    uapp_search_index_close(&t.index);
    CHECK(t.index == NULL);
    /* Closing twice is a no-op, not a double free. */
    uapp_search_index_close(&t.index);
    CHECK(t.index == NULL);

    /* Every operation on the closed index is now a defined rejection. */
    CHECK_STATUS(uapp_search_index_entry_count(t.index, &count),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(count == 7u);
    {
        uapp_unified_results_t* fresh = NULL;
        CHECK_STATUS(uapp_search_index_query(t.index, "closing", 10u, &fresh),
                     UAPP_STATUS_INVALID_ARGUMENT);
        CHECK(fresh == NULL);
    }

    /* Same discipline for the results handle. */
    uapp_unified_results_free(&results);
    CHECK(results == NULL);
    uapp_unified_results_free(&results);
    CHECK_STATUS(uapp_unified_results_count(results, &count), UAPP_STATUS_INVALID_ARGUMENT);
    CHECK(count == 7u);
    CHECK_STATUS(uapp_unified_results_get_title(results, 0u, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STR(borrowed, "untouched");

    uapp_free_string(&note_id);
    trio_close(&t);
}

/*
 * A handle of the wrong type, ACROSS ALL FOUR handle families now reachable
 * from one binding. On the Kotlin/Swift side every one of these is an opaque
 * integer with nothing to distinguish it, so swapping two fields in a
 * generated wrapper is the realistic failure -- and the per-type magic cookie
 * is what makes it a status instead of a dereference.
 */
static void test_boundary_wrong_handle_type(void) {
    trio_t t;
    char* note_id = NULL;
    uapp_unified_results_t* results = NULL;
    uapp_search_results_t* note_results = NULL;
    uapp_page_t* page = NULL;
    const char* borrowed = NULL;
    size_t count = 0;

    trio_init(&t, UAPP_CAPI_SEARCH_TEST_MIXUP_DB_PATH);
    REQUIRE_OK(trio_open_content(&t));
    note_id = make_note(&t, "Mixed up", "body");
    CHECK(note_id != NULL);
    REQUIRE_OK(uapp_document_service_create_page(t.docs, "Mixed up page", &page));
    REQUIRE_OK(trio_open_index(&t));
    REQUIRE_OK(uapp_search_index_query(t.index, "mixed", 10u, &results));
    REQUIRE_OK(uapp_note_service_search(t.notes, "mixed", 0u, &note_results));

    /* The index handle where a results handle belongs, and vice versa. */
    CHECK_STATUS(uapp_unified_results_count((const uapp_unified_results_t*)t.index, &count),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_search_index_entry_count((uapp_search_index_t*)results, &count),
                 UAPP_STATUS_INVALID_ARGUMENT);

    /*
     * THE ONE THAT MATTERS MOST: uapp_search_results_t (notes) and
     * uapp_unified_results_t (unified) are both "a list of search hits" and
     * will sit side by side in the same binding. Confusing them must be
     * caught, not dereferenced.
     */
    CHECK_STATUS(uapp_unified_results_count((const uapp_unified_results_t*)note_results,
                                            &count),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_search_results_count((const uapp_search_results_t*)results, &count),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_unified_results_get_title((const uapp_unified_results_t*)note_results, 0u,
                                                &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_search_results_get_title((const uapp_search_results_t*)results, 0u,
                                               &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);

    /* And against the other two surfaces' handles. */
    CHECK_STATUS(uapp_unified_results_count((const uapp_unified_results_t*)page, &count),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_search_index_entry_count((uapp_search_index_t*)t.docs, &count),
                 UAPP_STATUS_INVALID_ARGUMENT);
    CHECK_STATUS(uapp_page_get_title((const uapp_page_t*)results, &borrowed),
                 UAPP_STATUS_INVALID_ARGUMENT);

    /* A destructor refuses to delete something that is not its own: leaking is
     * strictly better than freeing a live handle of another type. */
    {
        uapp_unified_results_t* misdirected = (uapp_unified_results_t*)note_results;
        uapp_unified_results_free(&misdirected);
        CHECK(misdirected != NULL); /* refused, so the pointer is NOT nulled */
        REQUIRE_OK(uapp_search_results_count(note_results, &count)); /* still alive */
    }

    uapp_search_results_free(&note_results);
    uapp_unified_results_free(&results);
    uapp_page_free(&page);
    uapp_free_string(&note_id);
    trio_close(&t);
}

/* ===========================================================================
 * Harness
 * =========================================================================== */

static const uapp_test_case_t k_cases[] = {
    {"slice: search limits and source-kind names", test_search_limits_and_kind_names},
    {"slice: one query returns a note and a block together",
     test_slice_unified_note_and_block},
    {"slice: the index follows every content edit and delete",
     test_slice_index_follows_content},
    {"slice: the result limit defaults and clamps", test_slice_limit_is_clamped},
    {"slice: an in-memory trio is not unified and says so",
     test_slice_in_memory_trio_is_not_unified},
    {"slice: two independent indexes do not see each other",
     test_slice_two_independent_indexes},
    {"slice: a results handle outlives the index it came from",
     test_slice_results_outlive_the_index},

    {"boundary: null handles are rejected everywhere", test_boundary_null_handles},
    {"boundary: config and limits struct_size versioning",
     test_boundary_config_and_struct_size},
    {"boundary: empty, oversized and hostile queries", test_boundary_bad_queries},
    {"boundary: result row index bounds", test_boundary_row_index_bounds},
    {"boundary: use after close is a status, not a crash", test_boundary_use_after_close},
    {"boundary: a wrong-type handle is rejected, across surfaces",
     test_boundary_wrong_handle_type}
};

int main(int argc, char** argv) {
    const char* filter = (argc > 1) ? argv[1] : NULL;
    return uapp_run_test_cases("uapp unified search C ABI tests", k_cases,
                               sizeof k_cases / sizeof k_cases[0], filter);
}
