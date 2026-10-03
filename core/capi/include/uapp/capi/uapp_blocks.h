/* SPDX-License-Identifier: Apache-2.0 */
/*
 * uapp/capi/uapp_blocks.h -- the page/block surface of libuapp_capi.
 *
 * A one-to-one C99 face over uapp::blocks::DocumentService
 * (core/blocks/include/uapp/blocks/document_service.h). It exposes exactly the
 * document slice that already exists and is tested:
 *
 *     CREATE PAGE -> ADD BLOCK -> OPEN PAGE (full tree) -> UPDATE -> DELETE
 *                 -> REORDER -> MOVE   (+ RENAME, DELETE PAGE, COUNT)
 *
 * Nothing here adds behaviour. Every function is a translation of one C++ call
 * plus argument validation; if a rule is not enforced by the core it is not
 * invented here either. The one place that statement needs care is block
 * content, and the long argument for the chosen shape is below.
 *
 * THE TWO HANDLE TYPES
 *
 *   uapp_document_service_t  owns the database connection, the clock and the
 *                            id source. Created by
 *                            uapp_document_service_open(), released by
 *                            uapp_document_service_close(). Multiple
 *                            independent services may exist in one process;
 *                            they share nothing.
 *
 *   uapp_page_t              a caller-owned SNAPSHOT of one page AND ITS WHOLE
 *                            BLOCK TREE, not a live view of rows. Mirrors the
 *                            C++ API, where open_page() hands back a value
 *                            type. Two handles on the same page are two
 *                            snapshots.
 *
 * There is deliberately NO uapp_block_t handle. A block is addressed by its
 * 32-character hex id for every mutation, and read out of a page snapshot by
 * index. A third handle type would buy nothing: a block has no independent
 * lifetime (deleting its page deletes it) and no mutation that does not go
 * through the service anyway, so a handle would be a cache that can go stale
 * behind the caller's back -- which is the one thing the note surface took
 * care to avoid by making uapp_note_t an explicit snapshot.
 *
 * ===========================================================================
 * HOW A TREE CROSSES A C BOUNDARY: THE FLATTENED PRE-ORDER VIEW
 * ===========================================================================
 * uapp::blocks::Block holds `std::vector<Block> children`, i.e. the tree is
 * recursive. Three shapes were available at the boundary:
 *
 *   (a) a handle per block plus child_count/child_at navigation,
 *   (b) a serialised buffer (FlatBuffers, per api-design.md section 10 rule 4),
 *   (c) the tree FLATTENED into pre-order (document order) and addressed by
 *       index, with each row carrying its depth, its parent id and its direct
 *       child count.
 *
 * (c), for reasons that are about the callers rather than about taste:
 *
 *   1. It is what the callers already want. A mobile document editor renders
 *      a block tree as a FLAT LIST with indentation -- UITableView,
 *      RecyclerView and LazyColumn are all index-addressed flat lists. Handing
 *      a binding a tree it must walk and re-flatten on every frame is work
 *      done twice, once in C and once in Kotlin.
 *   2. It costs no allocation per row. Every string a row exposes is already
 *      materialised inside the page handle, so an accessor hands back a
 *      borrowed pointer (rule B) and JNI builds a jstring straight from it --
 *      the same property that made uapp_search_results_t a handle rather than
 *      a copied-out array.
 *   3. The structure is NOT lost. depth, parent_id_hex and child_count are
 *      enough to reconstruct the tree exactly, and
 *      uapp_page_find_block_index() maps an id back to its row. Pre-order is
 *      the order in which a parent always precedes its descendants, so
 *      "indent by depth" renders correctly with no lookahead.
 *   4. (b) is the right answer for a tree an order of magnitude larger, and it
 *      is recorded as NOT IMPLEMENTED in docs/changelog.md rather than
 *      half-built here. (a) is the shape the comment at the top of
 *      core/blocks/block.h already rejects ("one accessor per field per type,
 *      which is where FFI surfaces go to die").
 *
 * Index validity: a row index is valid for ONE page handle and only until that
 * handle is freed. It is NOT an identifier -- adding or deleting a block and
 * reopening the page renumbers everything after it. Use the id for anything
 * you keep.
 *
 * OWNERSHIP, ERRORS, THREADING
 * The rules are uapp_capi.h's and uapp_notes.h's, unchanged: rule B for
 * uapp_*_get_*, rule O for uapp_*_copy_* (release with uapp_free_string), no
 * out-parameter written on a failure path, every handle released through a
 * pointer TO the handle which is then set to NULL, and one handle used from
 * one thread at a time.
 */

#ifndef UAPP_CAPI_UAPP_BLOCKS_H
#define UAPP_CAPI_UAPP_BLOCKS_H

#include <stddef.h>
#include <stdint.h>

#include "uapp/capi/uapp_capi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uapp_document_service uapp_document_service_t;
typedef struct uapp_page             uapp_page_t;

/* --- block types ---------------------------------------------------------
 *
 * Every block type in spec section 14, with the SAME numeric values as
 * uapp::blocks::BlockType -- core/capi/src/capi_blocks.cpp asserts that at
 * compile time for all nineteen. The values are persisted in the database, so
 * they are append-only: never renumbered, never reused.
 *
 * EIGHT ARE IMPLEMENTED (0..7). The other eleven are declared here on purpose
 * rather than omitted: they already exist as persisted values that a database
 * written by a future build could contain, and a C enum that pretends
 * otherwise would force a binding to invent its own name for an integer it
 * reads back. Passing one of them to a mutating function returns
 * UAPP_STATUS_BLOCK_TYPE_UNSUPPORTED -- which is a different answer from
 * UAPP_STATUS_INVALID_ARGUMENT, and the difference is the point: "that block
 * type is not built yet" is a product state, "99 is not a block type" is a
 * caller bug. uapp_block_type_is_implemented() lets a UI grey out the former
 * without hard-coding the list.
 */
typedef enum uapp_block_type {
    /* implemented */
    UAPP_BLOCK_TYPE_PARAGRAPH           = 0,
    UAPP_BLOCK_TYPE_HEADING             = 1,
    UAPP_BLOCK_TYPE_BULLETED_LIST_ITEM  = 2,
    UAPP_BLOCK_TYPE_NUMBERED_LIST_ITEM  = 3,
    UAPP_BLOCK_TYPE_CHECKLIST_ITEM      = 4,
    UAPP_BLOCK_TYPE_CODE                = 5,
    UAPP_BLOCK_TYPE_DIVIDER             = 6,
    UAPP_BLOCK_TYPE_QUOTE               = 7,

    /* declared, NOT IMPLEMENTED -- see uapp_block_type_is_implemented() */
    UAPP_BLOCK_TYPE_TOGGLE   = 8,
    UAPP_BLOCK_TYPE_CALLOUT  = 9,
    UAPP_BLOCK_TYPE_TABLE    = 10,
    UAPP_BLOCK_TYPE_IMAGE    = 11,
    UAPP_BLOCK_TYPE_VIDEO    = 12,
    UAPP_BLOCK_TYPE_AUDIO    = 13,
    UAPP_BLOCK_TYPE_FILE     = 14,
    UAPP_BLOCK_TYPE_EQUATION = 15,
    UAPP_BLOCK_TYPE_EMBED    = 16,
    UAPP_BLOCK_TYPE_DATABASE = 17,
    UAPP_BLOCK_TYPE_BOOKMARK = 18,

    /*
     * Not a block type. Same two jobs as UAPP_STATUS_FORCE_INT32: it pins the
     * enum to 32 bits so the discriminant's size cannot depend on which
     * compiler built which side, and it widens the enum's representable range
     * so that merely READING an out-of-range integer a C caller cast in is
     * defined behaviour rather than something UBSan is right to flag.
     */
    UAPP_BLOCK_TYPE_FORCE_INT32 = 0x7FFFFFFF,

    /*
     * The other half of the range pin, and not redundant: an enum whose
     * enumerators are all non-negative has the range [0, 0x7FFFFFFF], so
     * reading a NEGATIVE integer cast in -- which a C caller produces by
     * typing -1 -- would still be undefined behaviour. UBSan caught exactly
     * that in uapp_search.h's twin of this enum during the run that added it.
     * See the longer note on UAPP_STATUS_FORCE_INT32_MIN in uapp_capi.h.
     */
    UAPP_BLOCK_TYPE_FORCE_INT32_MIN = -0x7FFFFFFF - 1
} uapp_block_type_t;

/*
 * Stable lowercase identifier ("paragraph", "bulleted_list_item", ...).
 * Never NULL; returns "unknown" for an integer that is not a block type.
 * Static storage, program lifetime -- never free it. For logs and for
 * round-tripping through a JSON/Markdown exporter, not for display.
 */
UAPP_CAPI_API const char* uapp_block_type_name(uapp_block_type_t type);

/* Is this type constructible in this build? Totally defined for any integer. */
UAPP_CAPI_API uapp_bool_t uapp_block_type_is_implemented(uapp_block_type_t type);

/*
 * Why a declared-but-absent type is absent, e.g. "needs a table model".
 * Returns NULL for an implemented type and for an integer that is not a block
 * type at all. Static storage, program lifetime. This is the machine-readable
 * form of the spec section 5 NOT IMPLEMENTED marker, so a UI can explain
 * itself without the reason rotting in a comment.
 */
UAPP_CAPI_API const char* uapp_block_type_not_implemented_reason(uapp_block_type_t type);

/* --- block content: the tagged struct ------------------------------------
 *
 * ===========================================================================
 * THE DESIGN DECISION, STATED IN FULL BECAUSE IT IS THE ONE REAL CHOICE HERE
 * ===========================================================================
 * The C++ layer models a payload as `std::variant<ParagraphBlock, ...>` --
 * eight alternatives, closed, with the variant index equal to the persisted
 * type integer. core/blocks/block.h chose a variant over a class hierarchy
 * explicitly so that it "maps to C as `{ int32_t type; union { ... } payload; }`
 * -- mechanically, with no ownership question".
 *
 * This header does NOT use a C union. It uses a FLAT struct: one discriminant
 * plus the union of every alternative's fields, four of them in total. Three
 * reasons, in order of weight:
 *
 *  1. A UNION BREAKS THE struct_size VERSIONING RULE. api-design.md section 10
 *     rule 2 versions a boundary struct by a leading uint32_t struct_size, and
 *     that works because the struct only ever GROWS AT THE TAIL. A union does
 *     not grow at the tail: adding a field to one arm changes sizeof(union),
 *     which moves every member after it and silently changes the layout of a
 *     struct whose struct_size a 0.2 caller is still reporting correctly.
 *     Adding `UAPP_BLOCK_TYPE_CALLOUT`'s payload later is then a BREAKING
 *     change wearing an additive change's clothes. With a flat struct the new
 *     field appends, struct_size tells us the caller does not have it, and the
 *     rule does what it was written to do.
 *
 *  2. THE UNION SAVES ALMOST NOTHING AND COSTS EVERY BINDING. Six of the eight
 *     payloads are exactly one string; the other two add one int and one bool,
 *     and one string. The whole flat struct is four fields. Against that, a C
 *     union inside a struct is the single worst-supported construct at an FFI
 *     boundary: Swift's C importer maps it to an opaque type with no
 *     synthesised member-wise initialiser, and JNI needs hand-written
 *     per-arm marshalling. We would be trading ~16 bytes per CALL (not per
 *     block -- nothing stores this struct) for a marshalling layer in two
 *     bindings, forever.
 *
 *  3. NO OWNERSHIP QUESTION EITHER WAY, SO THE UNION'S ADVANTAGE IS MOOT. The
 *     payload is strings; a union of strings is a union of `const char*`, and
 *     the lifetime rules are identical to the flat struct's. The variant's
 *     real benefit in C++ -- you cannot read the wrong arm -- does not survive
 *     translation to C at all: nothing stops a C caller reading any member of
 *     a union regardless of the tag.
 *
 * WHAT THAT COSTS, STATED PLAINLY: the struct carries fields a given `type`
 * does not use, so the type/field relationship is documented and checked at
 * runtime rather than enforced by the compiler. The table below is the
 * contract, uapp_block_content_init() is the way to not get it wrong, and the
 * "ignored" column is deliberate -- see the paragraph after the table.
 *
 *   type                      reads text  reads heading_level  reads checked  reads language
 *   PARAGRAPH                 yes         -                    -              -
 *   HEADING                   yes         yes (1..3)           -              -
 *   BULLETED_LIST_ITEM        yes         -                    -              -
 *   NUMBERED_LIST_ITEM        yes         -                    -              -
 *   CHECKLIST_ITEM            yes         -                    yes            -
 *   CODE                      yes (code)  -                    -              yes
 *   DIVIDER                   -           -                    -              -
 *   QUOTE                     yes         -                    -              -
 *
 * ON INPUT, A FIELD THIS `type` DOES NOT READ IS IGNORED, NOT VALIDATED. It is
 * not an error to leave `language` set while writing a paragraph. The strict
 * alternative -- reject a non-zero unused field -- was rejected because it
 * turns the ordinary editor action "the user changed this heading into a
 * paragraph" into a bug in every caller that reuses its struct, and because it
 * makes every future field a tripwire for callers that cannot know about it.
 * The cost is that a caller which sets `language` and forgets to set `type` to
 * CODE loses the language silently; uapp_block_content_init() exists so that
 * the type is chosen first and the fields second.
 *
 * ON OUTPUT (uapp_page_get_block_content), `text` and `language` are ALWAYS
 * non-NULL -- empty strings where the type has no such field -- so a caller
 * never needs a NULL check before strcmp/strlen. heading_level is 0 and
 * `checked` is UAPP_FALSE where the type does not define them.
 */
typedef struct uapp_block_content {
    uint32_t          struct_size;   /* in: sizeof(uapp_block_content_t) */
    uapp_block_type_t type;          /* the discriminant; chooses the fields below */

    /*
     * The block's text, UTF-8, NUL-terminated. For CODE this is the code
     * itself. Required (may be "", never NULL) for every type except DIVIDER,
     * which ignores it.
     *
     * AS AN INPUT it is BORROWED for the duration of the call only: the
     * implementation copies what it needs before returning, so a caller may
     * point it at a stack buffer. AS AN OUTPUT it follows rule B -- owned by
     * the uapp_page_t it was read from, valid until that handle is freed.
     *
     * An embedded NUL cannot be expressed here and is rejected by the core
     * anyway (UAPP_STATUS_BLOCK_MALFORMED); so is text over
     * uapp_block_limits_t::max_block_text_bytes.
     */
    const char* text;

    /*
     * CODE only: the free-form highlighter tag ("cpp", "python"). NULL is
     * accepted and means "no language". Not an allow-list -- an unknown tag
     * degrades to "no highlighting", which is not a security boundary -- but
     * control bytes are rejected (UAPP_STATUS_BLOCK_MALFORMED) because they
     * would reach a CSS class name, and over-length is
     * UAPP_STATUS_BLOCK_TEXT_TOO_LARGE.
     */
    const char* language;

    /*
     * HEADING only: 1, 2 or 3. Anything else is UAPP_STATUS_BLOCK_MALFORMED.
     * uapp_block_content_init() sets it to 1 for a heading and 0 otherwise.
     */
    int32_t heading_level;

    /* CHECKLIST_ITEM only: UAPP_TRUE if ticked. */
    uapp_bool_t checked;
} uapp_block_content_t;

/*
 * Initialise a content struct for `type`: sets struct_size, zeroes every
 * field, and applies the one non-zero default the table above needs
 * (heading_level = 1 for a heading, so that an uninitialised-looking 0 can
 * never be mistaken for a valid level).
 *
 * Always use it. It is the only way a caller can populate struct_size without
 * hard-coding a number that will be wrong after the struct grows, and it is
 * what makes "choose the type first, then the fields" the path of least
 * resistance.
 *
 * `type` may be any value: an unimplemented or bogus one is still written to
 * out_content->type, and the eventual mutating call is what reports it. This
 * function validates only the pointer, so a UI can hold a struct for a type it
 * cannot yet save.
 */
UAPP_CAPI_API uapp_status_t uapp_block_content_init(uapp_block_type_t type,
                                                    uapp_block_content_t* out_content);

/* --- limits --------------------------------------------------------------
 *
 * Read at runtime rather than #define'd, for the reason uapp_note_limits_t
 * gives: a client that hard-codes a bound the loaded library does not enforce
 * shows the user a validation message that lies.
 */
typedef struct uapp_block_limits {
    uint32_t struct_size;             /* in: sizeof(uapp_block_limits_t) */
    size_t   id_hex_length;           /* out: characters in a page/block id, excluding NUL */
    size_t   max_page_title_bytes;    /* out: UTF-8 bytes, excluding NUL */
    size_t   max_block_text_bytes;    /* out */
    size_t   max_code_language_bytes; /* out */
    size_t   max_block_depth;         /* out: a top-level block is at depth 1 */
    size_t   max_blocks_per_page;     /* out */
    int32_t  min_heading_level;       /* out */
    int32_t  max_heading_level;       /* out */
    int32_t  block_type_count;        /* out: how many block types exist at all */
    int32_t  implemented_block_type_count; /* out: how many are constructible here */
} uapp_block_limits_t;

/*
 * Fill `out_limits`. The caller MUST set out_limits->struct_size to
 * sizeof(uapp_block_limits_t) first; a smaller value is rejected with
 * UAPP_STATUS_INVALID_ARGUMENT (honouring it would mean writing past the end
 * of the caller's object) and a larger one -- a caller from the future -- is
 * accepted with the extra tail left untouched.
 */
UAPP_CAPI_API uapp_status_t uapp_block_get_limits(uapp_block_limits_t* out_limits);

/*
 * "After the last existing sibling". The only position value that is not a
 * plain index; anything else past the end of the sibling list is
 * UAPP_STATUS_BLOCK_POSITION_INVALID rather than being silently clamped,
 * because a caller who computed a stale index should be told.
 *
 * SIZE_MAX (<stdint.h>, C99 7.18.3) rather than the shorter `((size_t)-1)`:
 * that spelling is a C-style cast, which this project's C++ warning posture
 * forbids (-Wold-style-cast -Werror), so the implementation could not use its
 * own public macro. A macro only the C side can consume is not an ABI.
 */
#define UAPP_BLOCK_POSITION_APPEND SIZE_MAX

/*
 * UAPP_ID_NIL_HEX (uapp_capi.h) is what this surface means by "no parent": a
 * block whose parent is the nil id is a direct child of the page, at the top
 * level. Passing NULL in a parent position means the same thing, so a caller
 * need not spell it out, and uapp_page_get_block_parent_id_hex() returns the
 * nil id for such a block.
 */

/* --- service lifecycle --------------------------------------------------- */

typedef struct uapp_document_service_config {
    uint32_t    struct_size;     /* in: sizeof(uapp_document_service_config_t) */
    const char* database_path;   /* UTF-8, NUL-terminated, required. Borrowed for
                                  * the duration of the call only.
                                  *
                                  * ":memory:" gives a private in-memory
                                  * database. NOTE: each ":memory:" open is a
                                  * SEPARATE database, so a document service and
                                  * a note service both opened on ":memory:"
                                  * cannot see each other -- which also means
                                  * uapp_search.h's unified index would find
                                  * nothing. Unified search needs a real file
                                  * path shared by every handle. */
    uapp_bool_t read_only;       /* writes then fail with STORE_READ_ONLY */
    int32_t     busy_timeout_ms; /* how long a writer waits for a competing writer */
} uapp_document_service_config_t;

/* Initialise a config to this build's defaults, including struct_size. */
UAPP_CAPI_API uapp_status_t uapp_document_service_config_init(
    uapp_document_service_config_t* out_config);

/*
 * Open (creating if necessary) the local document database.
 *
 * On UAPP_STATUS_OK, *out_service is a handle the caller owns. On failure it is
 * LEFT UNTOUCHED, so initialise your variable to NULL. Expect
 * UAPP_STATUS_STORE_OPEN_FAILED for an unusable path and
 * UAPP_STATUS_STORE_SCHEMA_FAILED for a database this build cannot speak to.
 */
UAPP_CAPI_API uapp_status_t uapp_document_service_open(
    const uapp_document_service_config_t* config, uapp_document_service_t** out_service);

/*
 * Close a service and set *service to NULL. Safe with NULL and with a pointer
 * to NULL; safe to call twice. Never fails.
 *
 * Page handles obtained from this service are self-contained snapshots and
 * remain valid after it closes -- they hold no reference to the connection.
 * They must still be released individually.
 */
UAPP_CAPI_API void uapp_document_service_close(uapp_document_service_t** service);

/* --- pages --------------------------------------------------------------- */

/*
 * CREATE an empty page. `title` is required and NUL-terminated UTF-8; "" is
 * allowed ("Untitled" is a real state), NULL is UAPP_STATUS_INVALID_ARGUMENT,
 * and over-length is UAPP_STATUS_PAGE_TITLE_TOO_LONG.
 *
 * On success *out_page is a handle the caller owns and must release with
 * uapp_page_free(). Its block count is zero: a page and its tree are not
 * created in one call.
 */
UAPP_CAPI_API uapp_status_t uapp_document_service_create_page(uapp_document_service_t* service,
                                                              const char* title,
                                                              uapp_page_t** out_page);

/*
 * OPEN a page WITH ITS FULL BLOCK TREE. `page_id_hex` is the 32-character
 * lowercase hex id; anything else is UAPP_STATUS_ID_MALFORMED (told apart from
 * an id that simply is not there, which is UAPP_STATUS_NOT_FOUND).
 *
 * The tree is validated on read -- depth, size, and that every node's
 * parent/page links agree with where it actually sits -- because a database
 * file is attacker-reachable (spec section 28). A page that fails that check
 * reports the failure rather than handing back a half-tree.
 */
UAPP_CAPI_API uapp_status_t uapp_document_service_open_page(uapp_document_service_t* service,
                                                            const char* page_id_hex,
                                                            uapp_page_t** out_page);

/*
 * RENAME. Does not touch any open uapp_page_t snapshot -- reopen to see it.
 * UAPP_STATUS_NOT_FOUND if the page is absent.
 */
UAPP_CAPI_API uapp_status_t uapp_document_service_rename_page(uapp_document_service_t* service,
                                                              const char* page_id_hex,
                                                              const char* title);

/* DELETE a page and every block on it. UAPP_STATUS_NOT_FOUND if absent. */
UAPP_CAPI_API uapp_status_t uapp_document_service_delete_page(uapp_document_service_t* service,
                                                              const char* page_id_hex);

/* COUNT of pages in the store. */
UAPP_CAPI_API uapp_status_t uapp_document_service_page_count(uapp_document_service_t* service,
                                                             size_t* out_count);

/* --- blocks -------------------------------------------------------------- */

/*
 * ADD a block.
 *
 * `parent_id_hex` may be NULL or UAPP_ID_NIL_HEX for "top level", or the id of
 * a block ON THE SAME PAGE. `position` is the sibling index the block will
 * occupy, or UAPP_BLOCK_POSITION_APPEND for the end.
 *
 * `out_block_id_hex` is OPTIONAL: pass NULL if you do not need the new id. If
 * given, it follows rule O -- a fresh allocation the caller MUST release with
 * uapp_free_string().
 *
 * Expect UAPP_STATUS_NOT_FOUND (no such page or parent),
 * UAPP_STATUS_INVALID_ARGUMENT (the parent is on another page),
 * UAPP_STATUS_BLOCK_POSITION_INVALID, UAPP_STATUS_BLOCK_DEPTH_EXCEEDED,
 * UAPP_STATUS_PAGE_BLOCK_LIMIT_EXCEEDED, UAPP_STATUS_BLOCK_TYPE_UNSUPPORTED,
 * UAPP_STATUS_BLOCK_MALFORMED and UAPP_STATUS_BLOCK_TEXT_TOO_LARGE.
 */
UAPP_CAPI_API uapp_status_t uapp_document_service_add_block(
    uapp_document_service_t* service,
    const char* page_id_hex,
    const char* parent_id_hex,
    size_t position,
    const uapp_block_content_t* content,
    char** out_block_id_hex);

/*
 * UPDATE a block's content, keeping its children and its place. The TYPE may
 * change -- replacing a paragraph with a heading is a normal editor action and
 * the children survive it. UAPP_STATUS_NOT_FOUND if the block is absent.
 */
UAPP_CAPI_API uapp_status_t uapp_document_service_update_block(
    uapp_document_service_t* service,
    const char* block_id_hex,
    const uapp_block_content_t* content);

/*
 * DELETE a block AND ITS ENTIRE SUBTREE, then close the gap in its sibling
 * list. Cascade, not reparent: a child's meaning is bound to its parent, so
 * promoting orphans into the grandparent produces a document the user did not
 * write. Undo is NOT IMPLEMENTED. UAPP_STATUS_NOT_FOUND if absent.
 */
UAPP_CAPI_API uapp_status_t uapp_document_service_delete_block(
    uapp_document_service_t* service, const char* block_id_hex);

/*
 * REORDER within the current parent: the "drag it up two places" case. The
 * parent is read back from the store rather than taken from the caller, so a
 * caller working from a stale tree cannot accidentally re-parent a block while
 * thinking it is nudging it one slot.
 *
 * `new_position` is the index the block should END UP at among its siblings,
 * i.e. an index into the list AFTER the block has been removed from its old
 * place. UAPP_BLOCK_POSITION_APPEND moves it to the end.
 */
UAPP_CAPI_API uapp_status_t uapp_document_service_reorder_block(
    uapp_document_service_t* service, const char* block_id_hex, size_t new_position);

/*
 * MOVE: re-parent and position in one step -- the "drag it under that one"
 * case. NULL or UAPP_ID_NIL_HEX for `new_parent_id_hex` moves the block to the
 * page's top level. Same-parent is a reorder.
 *
 * UAPP_STATUS_BLOCK_CYCLE_DETECTED if the new parent is the block itself or
 * one of its descendants; UAPP_STATUS_BLOCK_DEPTH_EXCEEDED if the destination
 * depth plus the moved subtree's height would pass max_block_depth;
 * UAPP_STATUS_INVALID_ARGUMENT if the new parent is on another page
 * (cross-page move is NOT IMPLEMENTED in the core).
 */
UAPP_CAPI_API uapp_status_t uapp_document_service_move_block(
    uapp_document_service_t* service,
    const char* block_id_hex,
    const char* new_parent_id_hex,
    size_t new_position);

/*
 * COUNT of blocks on a page, at every nesting level. Cheaper than opening the
 * page when the tree is not needed.
 *
 * A page that does not exist (or no longer does) is UAPP_STATUS_OK WITH A
 * COUNT OF ZERO, not UAPP_STATUS_NOT_FOUND. This mirrors the core, whose
 * block_count counts the rows carrying that page id and never probes for the
 * page itself; adding an existence check here would be behaviour this layer
 * does not have the right to invent. A caller that needs "does this page
 * exist" asks uapp_document_service_open_page().
 */
UAPP_CAPI_API uapp_status_t uapp_document_service_block_count(
    uapp_document_service_t* service, const char* page_id_hex, size_t* out_count);

/* --- page handle: page metadata (rule B, borrowed) ----------------------- */

/*
 * These write a pointer OWNED BY THE PAGE HANDLE. Do not free it. It is valid
 * until uapp_page_free(). A page handle is immutable -- there are no
 * uapp_page_set_* functions, because every mutation goes through the service
 * and is seen by reopening -- so unlike uapp_note_t's accessors, nothing can
 * invalidate these early.
 */
UAPP_CAPI_API uapp_status_t uapp_page_get_id_hex(const uapp_page_t* page,
                                                 const char** out_id_hex);
UAPP_CAPI_API uapp_status_t uapp_page_get_title(const uapp_page_t* page,
                                                const char** out_title);
UAPP_CAPI_API uapp_status_t uapp_page_get_title_length(const uapp_page_t* page,
                                                       size_t* out_length);

/*
 * Unix epoch milliseconds, UTC. Either out-parameter may be NULL if the caller
 * only wants the other. Wall clocks step: do not derive ordering from these.
 */
UAPP_CAPI_API uapp_status_t uapp_page_get_timestamps(const uapp_page_t* page,
                                                     int64_t* out_created_at_ms,
                                                     int64_t* out_modified_at_ms);

/* The same two values as fresh allocations the CALLER owns and MUST release
 * with uapp_free_string() (rule O). Independent of the handle's lifetime --
 * what an async Swift Task or Kotlin coroutine needs. */
UAPP_CAPI_API uapp_status_t uapp_page_copy_id_hex(const uapp_page_t* page, char** out_id_hex);
UAPP_CAPI_API uapp_status_t uapp_page_copy_title(const uapp_page_t* page, char** out_title);

/* --- page handle: the flattened block tree ------------------------------- */

/*
 * Total blocks in the snapshot, at every nesting level. This is the exclusive
 * upper bound for every `index` below; zero is a normal answer for a new page,
 * and then no index is valid at all.
 */
UAPP_CAPI_API uapp_status_t uapp_page_get_block_count(const uapp_page_t* page,
                                                      size_t* out_count);

/*
 * Number of TOP-LEVEL blocks (depth 1). Together with child_count it is what a
 * collapsible outline view needs; it is not an index bound.
 */
UAPP_CAPI_API uapp_status_t uapp_page_get_top_level_block_count(const uapp_page_t* page,
                                                                size_t* out_count);

/*
 * Row accessors over the PRE-ORDER (document-order) flattening described at
 * the top of this header. `index` is in [0, uapp_page_get_block_count());
 * anything else is UAPP_STATUS_INVALID_ARGUMENT -- an out-of-range index is a
 * caller bug, not a missing block.
 *
 * Every `const char*` written here follows rule B: owned by the page handle,
 * valid until uapp_page_free(). None of them is ever NULL.
 */
UAPP_CAPI_API uapp_status_t uapp_page_get_block_id_hex(const uapp_page_t* page,
                                                       size_t index,
                                                       const char** out_id_hex);

/* UAPP_ID_NIL_HEX for a top-level block. */
UAPP_CAPI_API uapp_status_t uapp_page_get_block_parent_id_hex(const uapp_page_t* page,
                                                              size_t index,
                                                              const char** out_parent_id_hex);

UAPP_CAPI_API uapp_status_t uapp_page_get_block_type(const uapp_page_t* page,
                                                     size_t index,
                                                     uapp_block_type_t* out_type);

/* 1 for a top-level block. Render indentation straight from this. */
UAPP_CAPI_API uapp_status_t uapp_page_get_block_depth(const uapp_page_t* page,
                                                      size_t index,
                                                      size_t* out_depth);

/* DIRECT children only, not the whole subtree. 0 means a leaf. */
UAPP_CAPI_API uapp_status_t uapp_page_get_block_child_count(const uapp_page_t* page,
                                                            size_t index,
                                                            size_t* out_child_count);

/*
 * Fill `out_content` with row `index`'s payload. The caller MUST set
 * out_content->struct_size to sizeof(uapp_block_content_t) first; the usual
 * struct_size rule applies (smaller is rejected, larger leaves the tail
 * untouched).
 *
 * `text` and `language` point into the page handle (rule B) and are never
 * NULL -- empty where the type has no such field. See the ON OUTPUT paragraph
 * above uapp_block_content_t.
 */
UAPP_CAPI_API uapp_status_t uapp_page_get_block_content(const uapp_page_t* page,
                                                        size_t index,
                                                        uapp_block_content_t* out_content);

UAPP_CAPI_API uapp_status_t uapp_page_get_block_timestamps(const uapp_page_t* page,
                                                           size_t index,
                                                           int64_t* out_created_at_ms,
                                                           int64_t* out_modified_at_ms);

/*
 * The row's text as a fresh allocation the CALLER owns and MUST release with
 * uapp_free_string() (rule O). Empty string for a divider, never NULL on
 * success.
 */
UAPP_CAPI_API uapp_status_t uapp_page_copy_block_text(const uapp_page_t* page,
                                                      size_t index,
                                                      char** out_text);

/*
 * Map a block id back to its row index in this snapshot.
 *
 * UAPP_STATUS_ID_MALFORMED for anything that is not 32 hex characters, and
 * UAPP_STATUS_NOT_FOUND for a well-formed id that is not on this page -- which
 * is a real answer, not a caller bug: the block may have been deleted or moved
 * since the snapshot was taken.
 */
UAPP_CAPI_API uapp_status_t uapp_page_find_block_index(const uapp_page_t* page,
                                                       const char* block_id_hex,
                                                       size_t* out_index);

/* Release a page handle and set *page to NULL. Null-safe, idempotent, total. */
UAPP_CAPI_API void uapp_page_free(uapp_page_t** page);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* UAPP_CAPI_UAPP_BLOCKS_H */
