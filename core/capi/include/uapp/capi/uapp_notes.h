/* SPDX-License-Identifier: Apache-2.0 */
/*
 * uapp/capi/uapp_notes.h -- the note surface of libuapp_capi.
 *
 * A one-to-one C99 face over uapp::notes::NoteService
 * (core/notes/include/uapp/notes/note_service.h). It exposes exactly the
 * vertical slice that already exists and is tested:
 *
 *     CREATE -> STORE -> SEARCH -> OPEN -> EDIT -> SAVE   (+ DELETE, COUNT)
 *
 * Nothing here adds behaviour. Every function is a translation of one C++ call
 * plus argument validation; if a rule is not enforced by the core it is not
 * invented here either.
 *
 * THE THREE HANDLE TYPES
 *
 *   uapp_note_service_t   owns the database connection, the clock and the id
 *                         generator. Created by uapp_note_service_open(),
 *                         released by uapp_note_service_close(). Multiple
 *                         independent services may exist in one process; they
 *                         share nothing.
 *
 *   uapp_note_t           a caller-owned SNAPSHOT of one note, not a live view
 *                         of a row. This mirrors the C++ API, where open()
 *                         hands back a value type: you edit your copy and then
 *                         save it. Two handles on the same note are two
 *                         snapshots; last save wins, exactly as in the core.
 *
 *   uapp_search_results_t an immutable result set. Held as a handle rather
 *                         than copied into a caller array so that titles and
 *                         snippets can be borrowed (rule B) with no allocation
 *                         per row -- the JNI path builds jstrings straight
 *                         from them.
 *
 * Every handle is released by a function taking a POINTER TO the handle, which
 * it sets to NULL. Double-release is therefore a no-op and use-after-release
 * through the same variable is a clean UAPP_STATUS_INVALID_ARGUMENT rather
 * than a crash.
 *
 * THE OUT-PARAMETER RULE
 *
 * No function writes an out-parameter on a failure path. Ever. If it returns
 * anything but UAPP_STATUS_OK, every out-parameter it was given is exactly as
 * the caller left it.
 *
 * This matches uapp::notes::NoteService, which makes the same promise, so
 * there is one rule across the whole product rather than one per layer. It
 * also rules out a leak the caller cannot observe: the tempting alternative —
 * "set the handle to NULL on failure so the caller sees something safe" —
 * silently discards whatever that variable was already holding. Callers should
 * initialise out-parameters to NULL and check the status; those are the same
 * two habits, and neither is optional.
 *
 * Handles also carry an internal magic cookie that every entry point checks,
 * so a NULL, uninitialised, or wrong-type handle is rejected rather than
 * dereferenced. That is defence in depth, not a memory-safety guarantee:
 * calling through a pointer you already released AND separately kept a copy of
 * is use-after-free, and no C API can detect that. Release through the
 * pointer you use.
 */

#ifndef UAPP_CAPI_UAPP_NOTES_H
#define UAPP_CAPI_UAPP_NOTES_H

#include <stddef.h>
#include <stdint.h>

#include "uapp/capi/uapp_capi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uapp_note_service uapp_note_service_t;
typedef struct uapp_note         uapp_note_t;
typedef struct uapp_search_results uapp_search_results_t;

/* --- limits --------------------------------------------------------------
 *
 * Read at runtime rather than #define'd, so a binding compiled against an
 * older header still sees the limits the loaded library actually enforces. A
 * client that hard-codes 512 and a core that enforces 256 is a validation
 * message that lies to the user.
 */
typedef struct uapp_note_limits {
    uint32_t struct_size;          /* in: sizeof(uapp_note_limits_t) */
    size_t   id_hex_length;        /* out: characters in a note id, excluding NUL */
    size_t   max_title_bytes;      /* out: UTF-8 bytes, excluding NUL */
    size_t   max_body_bytes;       /* out */
    size_t   max_query_bytes;      /* out */
    size_t   max_search_limit;     /* out: a larger `limit` is clamped, not rejected */
    size_t   default_search_limit; /* out: what `limit == 0` means */
} uapp_note_limits_t;

/*
 * Fill `out_limits`. The caller MUST set out_limits->struct_size to
 * sizeof(uapp_note_limits_t) first; a value smaller than this build's struct
 * is rejected with UAPP_STATUS_INVALID_ARGUMENT, and a larger one (a caller
 * from the future) is accepted with the extra tail left untouched. That is the
 * leading-struct_size rule from api-design.md section 10 rule 2, and this is
 * the function that proves it works.
 */
UAPP_CAPI_API uapp_status_t uapp_note_get_limits(uapp_note_limits_t* out_limits);

/* --- service lifecycle --------------------------------------------------- */

typedef struct uapp_note_service_config {
    uint32_t    struct_size;     /* in: sizeof(uapp_note_service_config_t) */
    const char* database_path;   /* UTF-8, NUL-terminated, required. ":memory:"
                                  * gives a private in-memory database. Borrowed
                                  * for the duration of the call only. */
    uapp_bool_t read_only;       /* writes then fail with STORE_READ_ONLY */
    int32_t     busy_timeout_ms; /* how long a writer waits for a competing writer */
} uapp_note_service_config_t;

/*
 * Initialise a config to this build's defaults, including struct_size. Always
 * use it: it is the only way a caller can populate struct_size without
 * hard-coding a number that will be wrong after the struct grows.
 */
UAPP_CAPI_API uapp_status_t uapp_note_service_config_init(uapp_note_service_config_t* out_config);

/*
 * Open (creating if necessary) the local note database.
 *
 * On UAPP_STATUS_OK, *out_service is a handle the caller owns. On failure it
 * is LEFT UNTOUCHED — see the out-parameter rule below — so initialise your
 * variable to NULL. Expect UAPP_STATUS_STORE_OPEN_FAILED for an unusable path
 * and UAPP_STATUS_STORE_SCHEMA_FAILED for a database this build cannot speak
 * to.
 */
UAPP_CAPI_API uapp_status_t uapp_note_service_open(const uapp_note_service_config_t* config,
                                                   uapp_note_service_t** out_service);

/*
 * Close a service and set *service to NULL. Safe with NULL and with a pointer
 * to NULL; safe to call twice. Never fails.
 *
 * Note handles and search-result handles obtained from this service are
 * self-contained snapshots and remain valid after it closes -- they hold no
 * reference to the connection. They must still be released individually.
 */
UAPP_CAPI_API void uapp_note_service_close(uapp_note_service_t** service);

/* --- the slice ----------------------------------------------------------- */

/*
 * CREATE + STORE. `title` and `body` are required, NUL-terminated UTF-8; empty
 * is allowed, NULL is UAPP_STATUS_INVALID_ARGUMENT. On success *out_note is a
 * handle the caller owns and must release with uapp_note_free().
 */
UAPP_CAPI_API uapp_status_t uapp_note_service_create(uapp_note_service_t* service,
                                                     const char* title,
                                                     const char* body,
                                                     uapp_note_t** out_note);

/*
 * OPEN. `id_hex` is the 32-character lowercase hex id; anything else is
 * UAPP_STATUS_NOTE_ID_MALFORMED (a malformed id is told apart from an id that
 * simply is not there, which is UAPP_STATUS_NOT_FOUND).
 */
UAPP_CAPI_API uapp_status_t uapp_note_service_open_note(uapp_note_service_t* service,
                                                        const char* id_hex,
                                                        uapp_note_t** out_note);

/*
 * SAVE the edits made with uapp_note_set_title/uapp_note_set_body.
 *
 * The handle is updated in place with what was actually persisted -- notably
 * the new modified-at timestamp -- so the caller's copy never goes stale.
 * created_at is taken from the stored row, not from the handle: a client
 * cannot rewrite history by handing back a doctored value.
 *
 * UAPP_STATUS_NOT_FOUND if the note was deleted since it was opened.
 */
UAPP_CAPI_API uapp_status_t uapp_note_service_save(uapp_note_service_t* service,
                                                   uapp_note_t* note);

/* DELETE. UAPP_STATUS_NOT_FOUND if the id is absent. */
UAPP_CAPI_API uapp_status_t uapp_note_service_delete(uapp_note_service_t* service,
                                                     const char* id_hex);

/* COUNT. Number of notes in the store. */
UAPP_CAPI_API uapp_status_t uapp_note_service_count(uapp_note_service_t* service,
                                                    size_t* out_count);

/*
 * SEARCH. `query` is untrusted user input; sanitising it is the core's job,
 * never the binding's.
 *
 * `limit == 0` means uapp_note_limits_t::default_search_limit. A limit above
 * max_search_limit is clamped, not rejected.
 *
 * No match is UAPP_STATUS_OK with a count of zero -- NOT a NOT_FOUND. A query
 * with no searchable terms at all (punctuation only, say) is
 * UAPP_STATUS_SEARCH_QUERY_EMPTY, which is a different thing and the UI should
 * say so differently.
 *
 * On success *out_results is a handle the caller owns and must release with
 * uapp_search_results_free().
 */
UAPP_CAPI_API uapp_status_t uapp_note_service_search(uapp_note_service_t* service,
                                                     const char* query,
                                                     size_t limit,
                                                     uapp_search_results_t** out_results);

/* --- note handle: read (rule B, borrowed) -------------------------------- */

/*
 * These write a pointer OWNED BY THE NOTE HANDLE. Do not free it. It is
 * invalidated by the next uapp_note_set_* or uapp_note_service_save() on that
 * handle, and by uapp_note_free(). Copy it out before then if you need it to
 * live longer -- or use the uapp_note_copy_* family below.
 */
UAPP_CAPI_API uapp_status_t uapp_note_get_id_hex(const uapp_note_t* note, const char** out_id_hex);
UAPP_CAPI_API uapp_status_t uapp_note_get_title(const uapp_note_t* note, const char** out_title);
UAPP_CAPI_API uapp_status_t uapp_note_get_body(const uapp_note_t* note, const char** out_body);

/* Byte lengths of the above, excluding the NUL. Cheap; no scan. */
UAPP_CAPI_API uapp_status_t uapp_note_get_title_length(const uapp_note_t* note, size_t* out_length);
UAPP_CAPI_API uapp_status_t uapp_note_get_body_length(const uapp_note_t* note, size_t* out_length);

/*
 * Unix epoch milliseconds, UTC. Either out-parameter may be NULL if the caller
 * only wants the other one. Wall clocks step: do not derive ordering from
 * these.
 */
UAPP_CAPI_API uapp_status_t uapp_note_get_timestamps(const uapp_note_t* note,
                                                     int64_t* out_created_at_ms,
                                                     int64_t* out_modified_at_ms);

/* --- note handle: read (rule O, owned) ----------------------------------- */

/*
 * The same three values, but as fresh allocations the CALLER owns and MUST
 * release with uapp_free_string(). Independent of the handle's lifetime.
 *
 * These exist because the borrowed pointers above are the wrong tool for an
 * asynchronous binding: a Swift Task or a Kotlin coroutine that outlives the
 * scope in which the handle was open needs storage it controls. Returning
 * UAPP_STATUS_OUT_OF_MEMORY here is a real outcome, not a formality -- a note
 * body may be megabytes.
 */
UAPP_CAPI_API uapp_status_t uapp_note_copy_id_hex(const uapp_note_t* note, char** out_id_hex);
UAPP_CAPI_API uapp_status_t uapp_note_copy_title(const uapp_note_t* note, char** out_title);
UAPP_CAPI_API uapp_status_t uapp_note_copy_body(const uapp_note_t* note, char** out_body);

/* --- note handle: EDIT --------------------------------------------------- */

/*
 * Edit the caller's snapshot. Nothing is persisted until
 * uapp_note_service_save().
 *
 * Validation happens HERE as well as in save(), so a UI gets the
 * UAPP_STATUS_NOTE_TITLE_TOO_LONG / UAPP_STATUS_NOTE_BODY_TOO_LARGE feedback
 * at the keystroke rather than at the save. On rejection the handle is left
 * exactly as it was -- a rejected edit never half-applies.
 *
 * NULL is UAPP_STATUS_INVALID_ARGUMENT; "" is a valid title and a valid body.
 */
UAPP_CAPI_API uapp_status_t uapp_note_set_title(uapp_note_t* note, const char* title);
UAPP_CAPI_API uapp_status_t uapp_note_set_body(uapp_note_t* note, const char* body);

/* Release a note handle and set *note to NULL. Null-safe, idempotent, total. */
UAPP_CAPI_API void uapp_note_free(uapp_note_t** note);

/* --- search results ------------------------------------------------------ */

UAPP_CAPI_API uapp_status_t uapp_search_results_count(const uapp_search_results_t* results,
                                                      size_t* out_count);

/*
 * Row accessors. `index` is in [0, count); anything else is
 * UAPP_STATUS_INVALID_ARGUMENT -- an out-of-range index is a caller bug, not a
 * missing note.
 *
 * The id, title and snippet follow rule B: owned by the results handle, valid
 * until uapp_search_results_free(). `rank` follows the BM25 convention, LOWER
 * IS BETTER, and rows are already in that order.
 *
 * The snippet is a fragment of the note body with the match delimited. It is
 * user content: render it, never log it.
 */
UAPP_CAPI_API uapp_status_t uapp_search_results_get_id_hex(const uapp_search_results_t* results,
                                                           size_t index,
                                                           const char** out_id_hex);
UAPP_CAPI_API uapp_status_t uapp_search_results_get_title(const uapp_search_results_t* results,
                                                          size_t index,
                                                          const char** out_title);
UAPP_CAPI_API uapp_status_t uapp_search_results_get_snippet(const uapp_search_results_t* results,
                                                            size_t index,
                                                            const char** out_snippet);
UAPP_CAPI_API uapp_status_t uapp_search_results_get_rank(const uapp_search_results_t* results,
                                                         size_t index,
                                                         double* out_rank);

/* Release a results handle and set *results to NULL. Null-safe, idempotent. */
UAPP_CAPI_API void uapp_search_results_free(uapp_search_results_t** results);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* UAPP_CAPI_UAPP_NOTES_H */
