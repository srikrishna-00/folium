/* SPDX-License-Identifier: Apache-2.0 */
/*
 * uapp/capi/uapp_search.h -- the UNIFIED search surface of libuapp_capi.
 *
 * A one-to-one C99 face over uapp::search::SearchIndex
 * (core/search/include/uapp/search/search_index.h): ONE query string, results
 * from notes AND from pages/blocks, each hit self-describing about which kind
 * of object it is and which ids open it.
 *
 * HOW THIS DIFFERS FROM uapp_note_service_search()
 *
 *   uapp_note_service_search()  per-store search over ONE domain. Returns
 *                               uapp_search_results_t: note ids only. It still
 *                               exists, is unchanged, and is the right call
 *                               when the caller is a note list.
 *
 *   uapp_search_index_query()   the unified layer above it. Returns
 *                               uapp_unified_results_t, whose every row
 *                               carries a SOURCE KIND tag, so one result list
 *                               can mix a note and a block from a page.
 *
 * Two result types rather than one because they answer different questions and
 * the older one is frozen (uapp_notes.h's signatures do not change). A binding
 * that wants one list calls the unified one.
 *
 * ===========================================================================
 * THE ONE OPERATIONAL RULE: EVERY HANDLE MUST BE ON THE SAME DATABASE FILE
 * ===========================================================================
 * There is ONE index table in the database, and every content writer writes
 * into it inside its own content-write transaction (database-design.md
 * principle 7: "index and content commit together"). For a query to see notes
 * AND pages/blocks, uapp_note_service_open(), uapp_document_service_open() and
 * uapp_search_index_open() must all be given the SAME `database_path`.
 *
 * ":memory:" DOES NOT WORK HERE. Each in-memory open creates a PRIVATE
 * database, so three handles on ":memory:" are three databases that cannot see
 * each other's index rows -- and the symptom is zero results with no error,
 * which is a confusing afternoon. Use a real file path.
 *
 * Opened on different files, this degrades honestly: each file holds an index
 * covering only its own content, and a query against one returns that file's
 * hits rather than silently returning half an answer from a file that claims
 * to hold everything.
 *
 * READ-ONLY BY DESIGN. There is no index(), no reindex() and no handle that
 * can write: an index a caller has to remember to update is an index that
 * silently rots. The only way to put something into the index is to store a
 * note, a page or a block through the other two surfaces.
 *
 * OWNERSHIP, ERRORS, THREADING
 * uapp_capi.h's rules, unchanged: rule B for uapp_*_get_*, no out-parameter
 * written on a failure path, every handle released through a pointer TO the
 * handle which is then set to NULL, one handle used from one thread at a time.
 */

#ifndef UAPP_CAPI_UAPP_SEARCH_H
#define UAPP_CAPI_UAPP_SEARCH_H

#include <stddef.h>
#include <stdint.h>

#include "uapp/capi/uapp_capi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uapp_search_index   uapp_search_index_t;
typedef struct uapp_unified_results uapp_unified_results_t;

/* --- source kinds --------------------------------------------------------
 *
 * Which kind of object a hit names. Values are the SAME integers as
 * uapp::search::SourceKind and are persisted in the index's `source_kind`
 * column, so they are append-only: never renumbered, never reused.
 *
 * Note and Page/Block are what exists. Spec section 16's remaining source
 * kinds -- PDF, DOCX, OCR'd image, scanned document, file name, metadata,
 * database row -- deliberately have NO enumerator here, exactly as they have
 * none in the C++ enum: a value with no indexer behind it is a promise this
 * enum cannot keep, and it would become a persisted number nothing writes.
 */
typedef enum uapp_source_kind {
    /* A whole note -- title and body indexed as one unit. source_id is the
     * note id; container_id is nil. */
    UAPP_SOURCE_KIND_NOTE = 0,

    /* A page, indexed for its TITLE only -- the page's text lives in its
     * blocks and is indexed as BLOCK rows. source_id is the page id;
     * container_id is nil. */
    UAPP_SOURCE_KIND_PAGE = 1,

    /* One block's plain text. source_id is the block id and container_id is
     * the PAGE it sits on, which is what a UI needs to navigate: open page
     * container_id, scroll to block source_id. */
    UAPP_SOURCE_KIND_BLOCK = 2,

    /* Not a kind. Pins the enum to 32 bits and widens its representable range
     * so that reading an out-of-range integer a C caller cast in is defined
     * behaviour -- the same two jobs as UAPP_STATUS_FORCE_INT32. */
    UAPP_SOURCE_KIND_FORCE_INT32 = 0x7FFFFFFF,

    /*
     * The other half of the range pin. Without it the enum's valid range is
     * [0, 0x7FFFFFFF] and reading a negative integer a C caller cast in is
     * undefined behaviour -- which is not hypothetical: UBSan flagged
     * uapp_source_kind_name((uapp_source_kind_t)-1) in the run that added
     * this header. See UAPP_STATUS_FORCE_INT32_MIN in uapp_capi.h.
     */
    UAPP_SOURCE_KIND_FORCE_INT32_MIN = -0x7FFFFFFF - 1
} uapp_source_kind_t;

/*
 * Stable lowercase identifier ("note", "page", "block"). Never NULL; returns
 * "unknown" for an integer that is not a source kind. Static storage, program
 * lifetime -- never free it. For logs and grouping keys, not for display.
 */
UAPP_CAPI_API const char* uapp_source_kind_name(uapp_source_kind_t kind);

/* --- limits -------------------------------------------------------------- */

typedef struct uapp_search_limits {
    uint32_t struct_size;      /* in: sizeof(uapp_search_limits_t) */
    size_t   id_hex_length;    /* out: characters in an id, excluding NUL */
    size_t   max_query_bytes;  /* out: longest raw query accepted */
    size_t   max_results;      /* out: a larger `limit` is clamped, not rejected */
    int32_t  source_kind_count;/* out: how many source kinds exist */
} uapp_search_limits_t;

/*
 * Fill `out_limits`. The caller MUST set out_limits->struct_size to
 * sizeof(uapp_search_limits_t) first; the usual rule applies -- smaller is
 * UAPP_STATUS_INVALID_ARGUMENT, larger leaves the tail untouched.
 */
UAPP_CAPI_API uapp_status_t uapp_search_get_limits(uapp_search_limits_t* out_limits);

/* --- index lifecycle ----------------------------------------------------- */

typedef struct uapp_search_index_config {
    uint32_t    struct_size;   /* in: sizeof(uapp_search_index_config_t) */

    /* UTF-8, NUL-terminated, required. Borrowed for the duration of the call.
     * MUST be the same real file path the note and document services were
     * opened on -- see the operational rule at the top of this header. */
    const char* database_path;

    /*
     * Open the query connection read-only. THE DEFAULT IS UAPP_TRUE, and that
     * default is the point: this surface cannot write, so the connection
     * behind it has no business holding write permission.
     *
     * With UAPP_TRUE the index tables must ALREADY EXIST -- i.e. a note or
     * document service must have been opened read-write on this path at least
     * once -- otherwise this returns UAPP_STATUS_STORE_OPEN_FAILED rather than
     * reporting "no results" for every query forever. Passing UAPP_FALSE gets
     * a read-write connection, which is useful only for creating those tables
     * in a database no store has opened yet.
     */
    uapp_bool_t read_only;

    int32_t     busy_timeout_ms; /* how long a reader waits for a writer */
} uapp_search_index_config_t;

/* Initialise a config to this build's defaults, including struct_size and
 * read_only = UAPP_TRUE. */
UAPP_CAPI_API uapp_status_t uapp_search_index_config_init(
    uapp_search_index_config_t* out_config);

/*
 * Open a query handle onto the unified index.
 *
 * On UAPP_STATUS_OK, *out_index is a handle the caller owns. On failure it is
 * LEFT UNTOUCHED, so initialise your variable to NULL.
 */
UAPP_CAPI_API uapp_status_t uapp_search_index_open(const uapp_search_index_config_t* config,
                                                   uapp_search_index_t** out_index);

/*
 * Close an index and set *index to NULL. Safe with NULL and with a pointer to
 * NULL; safe to call twice. Never fails.
 *
 * Result handles obtained from this index are self-contained snapshots and
 * remain valid after it closes. They must still be released individually.
 */
UAPP_CAPI_API void uapp_search_index_close(uapp_search_index_t** index);

/* --- the query ----------------------------------------------------------- */

/*
 * QUERY across every indexed source kind, best-first.
 *
 * `query` is RAW UNTRUSTED USER INPUT. Sanitising it is the core's job, never
 * the binding's: no caller is ever asked to pre-escape anything, and the
 * index's own query language is never handed raw user text.
 *
 * `limit` is clamped to uapp_search_limits_t::max_results.
 *
 * NOTE THE ONE DELIBERATE DIFFERENCE FROM uapp_note_service_search(): here
 * `limit == 0` is UAPP_STATUS_INVALID_ARGUMENT, not "use a default". That is
 * what uapp::search::SearchIndex::query() specifies -- a zero limit is a
 * caller bug that would otherwise look like "nothing matched" -- and this
 * layer does not invent a default the core does not have. The note surface has
 * a documented default limit of its own; the unified port has none to expose.
 *
 * NO MATCH is UAPP_STATUS_OK with a count of zero -- NOT a NOT_FOUND. A query
 * with no indexable term at all (empty, whitespace only, punctuation only) is
 * UAPP_STATUS_SEARCH_QUERY_EMPTY, which is a different condition and the UI
 * should say so differently: "ask for a better query", not "nothing matched".
 *
 * On success *out_results is a handle the caller owns and must release with
 * uapp_unified_results_free().
 */
UAPP_CAPI_API uapp_status_t uapp_search_index_query(uapp_search_index_t* index,
                                                    const char* query,
                                                    size_t limit,
                                                    uapp_unified_results_t** out_results);

/*
 * Total indexed entries across every source kind. Diagnostics, and for a
 * caller that wants to assert the index did not go stale.
 */
UAPP_CAPI_API uapp_status_t uapp_search_index_entry_count(uapp_search_index_t* index,
                                                          size_t* out_count);

/* --- results ------------------------------------------------------------- */

UAPP_CAPI_API uapp_status_t uapp_unified_results_count(const uapp_unified_results_t* results,
                                                       size_t* out_count);

/*
 * Row accessors. `index` is in [0, count); anything else is
 * UAPP_STATUS_INVALID_ARGUMENT -- an out-of-range index is a caller bug, not a
 * missing object.
 *
 * Every `const char*` follows rule B: owned by the results handle, valid until
 * uapp_unified_results_free(), never NULL.
 */

/* Which kind of object matched. Decides how to read the two ids. */
UAPP_CAPI_API uapp_status_t uapp_unified_results_get_kind(const uapp_unified_results_t* results,
                                                          size_t index,
                                                          uapp_source_kind_t* out_kind);

/* The id of the thing that matched: a note id, a page id, or a block id. */
UAPP_CAPI_API uapp_status_t uapp_unified_results_get_source_id_hex(
    const uapp_unified_results_t* results, size_t index, const char** out_id_hex);

/*
 * The id of the object that CONTAINS the match, when the match is not itself
 * openable: for a BLOCK this is the page id, which is what a UI needs to
 * navigate. UAPP_ID_NIL_HEX (32 zeros) for NOTE and PAGE, which are their own
 * containers -- never NULL, so no special-casing before strlen.
 */
UAPP_CAPI_API uapp_status_t uapp_unified_results_get_container_id_hex(
    const uapp_unified_results_t* results, size_t index, const char** out_id_hex);

/*
 * The CONTAINING object's title: the note's title, the page's title, or -- for
 * a BLOCK -- the title of the page it sits on, so a result list can be grouped
 * and labelled without a second query per row. May be empty (an untitled page
 * is a real state). User content: render it, never log it.
 */
UAPP_CAPI_API uapp_status_t uapp_unified_results_get_title(
    const uapp_unified_results_t* results, size_t index, const char** out_title);

/*
 * The match in context, with the matched terms delimited by '[' and ']',
 * generated by the index. USER CONTENT: render it, never log it (spec
 * section 34).
 */
UAPP_CAPI_API uapp_status_t uapp_unified_results_get_snippet(
    const uapp_unified_results_t* results, size_t index, const char** out_snippet);

/*
 * Relevance. BM25 convention: LOWER (more negative) is a better match, and
 * rows are already in that order. Comparable ACROSS KINDS, because there is
 * one index table and therefore one set of corpus statistics behind every row
 * -- which is the whole reason the index is one table rather than two merged
 * in the application.
 */
UAPP_CAPI_API uapp_status_t uapp_unified_results_get_rank(
    const uapp_unified_results_t* results, size_t index, double* out_rank);

/* Release a results handle and set *results to NULL. Null-safe, idempotent. */
UAPP_CAPI_API void uapp_unified_results_free(uapp_unified_results_t** results);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* UAPP_CAPI_UAPP_SEARCH_H */
