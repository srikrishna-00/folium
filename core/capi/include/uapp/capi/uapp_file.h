/* SPDX-License-Identifier: Apache-2.0 */
/*
 * uapp/capi/uapp_file.h -- the FILE DETECTION surface of libuapp_capi.
 *
 * A one-to-one C99 face over file-engine/detection (uapp/file_engine/
 * detector.h, format.h, capabilities.h, checksum.h): spec section 8/9's
 * "what is this file, and what could we do with it?", answered from the bytes.
 *
 * ===========================================================================
 * THERE ARE NO HANDLES IN THIS HEADER, AND THAT IS A PROPERTY OF THE MODULE
 * ===========================================================================
 * Every other surface in this ABI opens something: a note service, a document
 * service, a search index, a PDF. This one opens nothing. file-engine/
 * detection is pure -- bytes in, a verdict out -- with no store, no
 * connection, no clock, no id source and no cached state of any kind. Its
 * CMakeLists.txt makes that a build-graph property rather than a claim (it
 * links uapp::core_common and nothing else; it cannot reach SQLite).
 *
 * So the ABI is stateless too: every function here is a pure call that writes
 * a caller-allocated struct and owns nothing afterwards. Consequences worth
 * stating, because they are the reason this is nicer to bind than the others:
 *
 *   * nothing to close, so no leak is possible and no destructor exists;
 *   * no "use after close" failure mode, so there is no magic cookie to
 *     check and no handle-mixup case to test for;
 *   * every call is reentrant and thread-safe with every other call, with no
 *     "one handle, one thread at a time" caveat -- the only shared state is
 *     the const format tables;
 *   * a binding needs no finalizer, no Cleaner, no deinit and no phantom
 *     reference queue, which on Android is the single largest source of
 *     native-lifetime bugs.
 *
 * The ONE exception to "owns nothing" is uapp_file_copy_extension(), which
 * follows rule O like every other uapp_*_copy_* function and is released with
 * uapp_free_string().
 *
 * ===========================================================================
 * THE RULE THIS SURFACE EXISTS TO ENFORCE: NEVER TRUST THE EXTENSION ALONE
 * ===========================================================================
 * Spec section 8 states it and detector.h enforces it structurally. This
 * header does not get to soften it:
 *
 *   `format` is ONLY EVER derived from the bytes. A filename can never put a
 *   value there. What the name claimed goes in `extension_hint`, and a
 *   disagreement sets `extension_conflicts` -- which is the
 *   security-interesting output, because an `invoice.pdf` whose bytes are a
 *   ZIP is worth showing a human before they open it.
 *
 * The consequence is deliberate: for a format whose sniffer is NOT
 * IMPLEMENTED (say MP3), `format` stays UAPP_FILE_FORMAT_UNKNOWN even when
 * the name says `.mp3`, with `extension_hint` = UAPP_FILE_FORMAT_MP3 and
 * `confidence` = EXTENSION_ONLY. That is the honest answer, and
 * uapp_file_format_detection_is_implemented() is how a caller tells it apart
 * from "we sniffed it and found nothing".
 *
 * ===========================================================================
 * WHAT IS NOT HERE
 * ===========================================================================
 * This module detects. It does not open, parse, render or convert, and
 * neither does this header. In particular:
 *
 *   * NO metadata key/value list. uapp::files::FileMetadata::entries exists,
 *     but every entry in it today is a STRING RENDERING of a field this
 *     header already hands back as a typed value ("detection.method",
 *     "detection.confidence", "container.truncated", "magic.offset", ...).
 *     Exposing it would need a handle to own the strings -- reintroducing
 *     everything the handle-free design above buys -- in exchange for giving
 *     a binding two sources of truth for the same fact, one of them
 *     stringly-typed. uapp_file_format_name(), uapp_detection_method_name()
 *     and uapp_detection_confidence_name() produce the same strings from the
 *     typed fields when a caller wants them for a log line.
 *
 *     The entry list becomes worth exposing when it carries something this
 *     struct does not: EXIF, XMP, the PDF Info dictionary, OOXML core
 *     properties. All of those are NOT IMPLEMENTED in the C++ layer -- they
 *     need the parsers a detector deliberately does not contain -- so there
 *     is nothing to expose yet.
 *
 *   * NO `name` echo. uapp::files::File carries the display name; it is
 *     exactly the string the caller just passed in as `file_name_hint`, and
 *     handing a copy back would be ownership for no information. The DERIVED
 *     part -- the lowercase, dot-stripped extension, which gets the dotfile,
 *     trailing-dot and directory-dot cases right -- is available through
 *     uapp_file_copy_extension(), because that one is a function a binding
 *     would otherwise reimplement slightly wrongly.
 *
 *   * NO id, and NO timestamps on the buffer path beyond what the caller
 *     supplies. The C++ layer takes both as parameters for the reason
 *     detector.h gives ("a detector that reaches for ambient identity is
 *     untestable"); this header does the same rather than inventing a clock.
 *
 *   * NO location resolution. UAPP_FILE_LOCATION_PLATFORM_URI is reported
 *     but never resolved -- that needs the platform vtable, which does not
 *     exist. See uapp_file_location_kind_t.
 *
 * ERRORS, OWNERSHIP, THREADING
 * uapp_capi.h's rules, unchanged. Out-parameters are written only on
 * UAPP_STATUS_OK. The 6xx status codes (FILE_EMPTY, FILE_PATH_INVALID,
 * FILE_OPEN_FAILED, FILE_READ_FAILED) are reachable from here and from
 * nowhere else in the ABI; they were minted in 0.3 for that reason.
 */

#ifndef UAPP_CAPI_UAPP_FILE_H
#define UAPP_CAPI_UAPP_FILE_H

#include <stddef.h>
#include <stdint.h>

#include "uapp/capi/uapp_capi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- the format registry -------------------------------------------------
 *
 * Every format spec section 9 names, with the SAME numeric values as
 * uapp::files::FileFormat -- core/capi/src/capi_file.cpp asserts that at
 * compile time for every one of them. The values are persisted in the
 * `file.format` column, so they are APPEND-ONLY: never renumbered, never
 * reused.
 *
 * TEN ARE IMPLEMENTED (1..10). The other thirty-odd are declared here on
 * purpose rather than omitted, for the reason format.h gives: a registry that
 * listed only what we sniff would leave every caller unable to tell "we
 * sniffed it and found nothing" (format == UNKNOWN) from "we have never
 * taught the sniffer about MP3"
 * (uapp_file_format_detection_is_implemented(MP3) == UAPP_FALSE). Those are
 * different facts and a UI says different things about them.
 *
 * Note what the gaps in the numbering are: they are the category boundaries
 * from spec section 9, left as room to append inside a category without
 * renumbering across one.
 */
typedef enum uapp_file_format {
    /* The sniffer ran and nothing matched. NOT "we have no sniffer for
     * this" -- see uapp_file_format_detection_is_implemented(). */
    UAPP_FILE_FORMAT_UNKNOWN = 0,

    /* --- detection IMPLEMENTED in this build --------------------------- */
    UAPP_FILE_FORMAT_PLAIN_TEXT = 1, /* UTF-8/ASCII, by content heuristic */
    UAPP_FILE_FORMAT_PDF        = 2,
    UAPP_FILE_FORMAT_PNG        = 3,
    UAPP_FILE_FORMAT_JPEG       = 4,
    UAPP_FILE_FORMAT_GIF        = 5,
    UAPP_FILE_FORMAT_ZIP        = 6, /* a ZIP we could not classify further */
    UAPP_FILE_FORMAT_DOCX       = 7,
    UAPP_FILE_FORMAT_XLSX       = 8,
    UAPP_FILE_FORMAT_PPTX       = 9,
    UAPP_FILE_FORMAT_ODT        = 10,

    /* --- declared, detection NOT IMPLEMENTED --------------------------- */
    /* documents */
    UAPP_FILE_FORMAT_MARKDOWN = 100,
    UAPP_FILE_FORMAT_RTF      = 101,
    UAPP_FILE_FORMAT_DOC      = 102,
    UAPP_FILE_FORMAT_HTML     = 103,
    UAPP_FILE_FORMAT_XML      = 104,
    UAPP_FILE_FORMAT_JSON     = 105,
    /* spreadsheets */
    UAPP_FILE_FORMAT_CSV = 120,
    UAPP_FILE_FORMAT_XLS = 121,
    UAPP_FILE_FORMAT_ODS = 122,
    /* presentations */
    UAPP_FILE_FORMAT_PPT = 140,
    UAPP_FILE_FORMAT_ODP = 141,
    /* images */
    UAPP_FILE_FORMAT_WEBP = 160,
    UAPP_FILE_FORMAT_TIFF = 161,
    UAPP_FILE_FORMAT_BMP  = 162,
    UAPP_FILE_FORMAT_SVG  = 163,
    UAPP_FILE_FORMAT_HEIC = 164,
    UAPP_FILE_FORMAT_AVIF = 165,
    /* e-books */
    UAPP_FILE_FORMAT_EPUB = 180,
    UAPP_FILE_FORMAT_MOBI = 181,
    UAPP_FILE_FORMAT_FB2  = 182,
    /* archives */
    UAPP_FILE_FORMAT_TAR     = 200,
    UAPP_FILE_FORMAT_GZIP    = 201,
    UAPP_FILE_FORMAT_SEVEN_Z = 202,
    UAPP_FILE_FORMAT_RAR     = 203,
    /* audio */
    UAPP_FILE_FORMAT_MP3  = 220,
    UAPP_FILE_FORMAT_WAV  = 221,
    UAPP_FILE_FORMAT_FLAC = 222,
    UAPP_FILE_FORMAT_AAC  = 223,
    UAPP_FILE_FORMAT_OGG  = 224,
    /* video */
    UAPP_FILE_FORMAT_MP4  = 240,
    UAPP_FILE_FORMAT_MOV  = 241,
    UAPP_FILE_FORMAT_MKV  = 242,
    UAPP_FILE_FORMAT_AVI  = 243,
    UAPP_FILE_FORMAT_WEBM = 244,
    /* code -- ONE entry, not twelve. format.h's argument: the twelve
     * languages spec section 9 lists are all plain text, distinguishable
     * only by extension and heuristics and never by a magic number, and
     * conflating "which language" with "what kind of file" is how a registry
     * ends up with a `.ts` entry that cannot decide between TypeScript and
     * MPEG transport stream. */
    UAPP_FILE_FORMAT_SOURCE_CODE = 260,

    /* Not a format. Pins the enum to 32 bits and widens its representable
     * range so that reading an out-of-range integer a C caller cast in is
     * defined behaviour. See UAPP_STATUS_FORCE_INT32 in uapp_capi.h. */
    UAPP_FILE_FORMAT_FORCE_INT32     = 0x7FFFFFFF,
    UAPP_FILE_FORMAT_FORCE_INT32_MIN = -0x7FFFFFFF - 1
} uapp_file_format_t;

/* Top-level grouping, matching the subheadings of spec section 9. Same
 * numeric values as uapp::files::FormatCategory. */
typedef enum uapp_format_category {
    UAPP_FORMAT_CATEGORY_UNKNOWN      = 0,
    UAPP_FORMAT_CATEGORY_DOCUMENT     = 1,
    UAPP_FORMAT_CATEGORY_SPREADSHEET  = 2,
    UAPP_FORMAT_CATEGORY_PRESENTATION = 3,
    UAPP_FORMAT_CATEGORY_PDF          = 4,
    UAPP_FORMAT_CATEGORY_IMAGE        = 5,
    UAPP_FORMAT_CATEGORY_EBOOK        = 6,
    UAPP_FORMAT_CATEGORY_ARCHIVE      = 7,
    UAPP_FORMAT_CATEGORY_AUDIO        = 8,
    UAPP_FORMAT_CATEGORY_VIDEO        = 9,
    UAPP_FORMAT_CATEGORY_CODE         = 10,

    UAPP_FORMAT_CATEGORY_FORCE_INT32     = 0x7FFFFFFF,
    UAPP_FORMAT_CATEGORY_FORCE_INT32_MIN = -0x7FFFFFFF - 1
} uapp_format_category_t;

/* --- registry queries (pure lookups; cannot fail) ------------------------
 *
 * Every one of these is total: it accepts any integer, including one that is
 * not a format at all, and answers rather than failing. That is what makes
 * them usable from a UI iterating the registry with no prior validation.
 *
 * Returned strings are STATIC STORAGE with program lifetime -- never freed,
 * never invalidated (the third ownership case in uapp_capi.h).
 */

/* How many formats are in the registry. Pair with uapp_file_format_at() to
 * enumerate it; a UI listing "supported formats" must not hard-code a count
 * the loaded library disagrees with. */
UAPP_CAPI_API size_t uapp_file_format_count(void);

/* The format at `index` in [0, uapp_file_format_count()).
 * UAPP_FILE_FORMAT_UNKNOWN for an out-of-range index. */
UAPP_CAPI_API uapp_file_format_t uapp_file_format_at(size_t index);

/* Stable lowercase identifier, e.g. "docx". Never NULL; "unknown" for an
 * integer that is not a format. This is the string that goes into logs. */
UAPP_CAPI_API const char* uapp_file_format_name(uapp_file_format_t format);

UAPP_CAPI_API uapp_format_category_t uapp_file_format_category(uapp_file_format_t format);
UAPP_CAPI_API const char* uapp_format_category_name(uapp_format_category_t category);

/* The canonical IANA media type. Never NULL. UNKNOWN maps to
 * "application/octet-stream", which is the correct answer for "bytes we
 * cannot identify" rather than a placeholder. */
UAPP_CAPI_API const char* uapp_file_format_mime_type(uapp_file_format_t format);

/* The conventional lowercase extension WITHOUT a dot, e.g. "jpg" for JPEG.
 * Never NULL; empty for UNKNOWN. ADVISORY ONLY -- for display and for
 * suggesting a filename, NEVER for detection. */
UAPP_CAPI_API const char* uapp_file_format_primary_extension(uapp_file_format_t format);

/*
 * ===========================================================================
 * THE "IS DETECTION IMPLEMENTED FOR X?" QUERY PATH
 * ===========================================================================
 * Does this build contain a sniffer for `format`?
 *
 * This is the file-engine half of the precedent uapp_blocks.h set with
 * uapp_block_type_is_implemented(): the spec section 5 NOT IMPLEMENTED marker
 * lives in CODE, where it cannot rot, and is QUERYABLE by a UI and by a
 * binding rather than being a comment somebody has to find. format.h was
 * written against that precedent explicitly ("queryable by the UI and the
 * future C ABI"); this is that ABI.
 *
 * It answers a DIFFERENT question from `format == UNKNOWN`, and the pair is
 * the whole point:
 *
 *   format == UNKNOWN and detection_is_implemented(extension_hint) == TRUE
 *       -> we know this format and these bytes are not it. A `.png` whose
 *          bytes are not a PNG. Suspicious.
 *   format == UNKNOWN and detection_is_implemented(extension_hint) == FALSE
 *       -> we have never been taught this format. A `.mp3`. Not suspicious;
 *          just outside what we do.
 *
 * Totally defined for any integer.
 */
UAPP_CAPI_API uapp_bool_t uapp_file_format_detection_is_implemented(uapp_file_format_t format);

/*
 * WHY there is no sniffer -- "needs a full XML parser to tell it from HTML",
 * not "not implemented yet". Returns NULL for an implemented format and for
 * an integer that is not a format at all, so
 * `if (const char* why = uapp_file_format_detection_not_implemented_reason(f))`
 * is the natural spelling. Static storage, program lifetime.
 */
UAPP_CAPI_API const char* uapp_file_format_detection_not_implemented_reason(
    uapp_file_format_t format);

/*
 * Map a filename extension (with or without a leading dot, any case) to the
 * format it CLAIMS to be. A HINT, never a detection.
 *
 * Exposed because a binding that wants to pre-filter a file picker needs the
 * same extension table the detector uses, and the alternative is each
 * binding carrying its own copy that drifts. Nothing in this ABI assigns
 * `format` from this function's output.
 *
 * UAPP_FILE_FORMAT_UNKNOWN for NULL, for an empty string, and for an
 * extension not in the registry.
 */
UAPP_CAPI_API uapp_file_format_t uapp_file_format_from_extension(const char* extension);

/* --- capabilities (spec section 8) ---------------------------------------
 *
 * The ten capabilities of spec section 8, as single bits, so a set is one
 * uint32_t rather than ten accessor calls -- which is what capabilities.h
 * was already designed for ("trivially describable to the future C99 ABI as
 * a uint32_t bitmask").
 *
 * THESE FLAGS ARE CLAIMS, AND A WRONG CLAIM IS A BUG. They are what a UI
 * uses to decide whether to show an "Edit" button, and granting EDIT for a
 * format we can only read produces a button that fails at the point of use,
 * which is worse than no button. They are DERIVED from the support matrix in
 * docs/file-format-support.md by the rules written down in capabilities.h,
 * never hand-set, and this layer passes the bitmask through unchanged rather
 * than forming a second opinion.
 *
 * Two of them are worth reading twice, because a caller will expect
 * otherwise:
 *
 *   SIGN is granted to NOTHING, including PDF. file-format-support.md §5.1:
 *       "Digital signature validation is P2/TBD ... A half-implemented
 *       signature check is worse than none." A flag that lights up a "Sign"
 *       affordance is exactly that half-implementation.
 *   UNKNOWN gets the EMPTY set. A file we could not identify is not
 *       "viewable as text by default" -- that guess is how a renderer gets
 *       handed a payload it was never meant to see.
 *
 * The values are the same bits as uapp::files::Capability, asserted at
 * compile time in capi_file.cpp. APPEND-ONLY: the bitmask is persisted.
 */
typedef enum uapp_file_capability {
    UAPP_FILE_CAPABILITY_VIEW     = 1 << 0,
    UAPP_FILE_CAPABILITY_EDIT     = 1 << 1,
    UAPP_FILE_CAPABILITY_CONVERT  = 1 << 2,
    UAPP_FILE_CAPABILITY_EXPORT   = 1 << 3, /* := the engine can WRITE this format out */
    UAPP_FILE_CAPABILITY_OCR      = 1 << 4,
    UAPP_FILE_CAPABILITY_ANNOTATE = 1 << 5,
    UAPP_FILE_CAPABILITY_COMPRESS = 1 << 6, /* := lossless repack in place */
    UAPP_FILE_CAPABILITY_SIGN     = 1 << 7, /* granted to nothing; see above */
    UAPP_FILE_CAPABILITY_ENCRYPT  = 1 << 8, /* := encrypted copy in the format's own scheme */
    UAPP_FILE_CAPABILITY_EXTRACT  = 1 << 9, /* := enumerate/pull out embedded sub-objects */

    UAPP_FILE_CAPABILITY_FORCE_INT32     = 0x7FFFFFFF,
    UAPP_FILE_CAPABILITY_FORCE_INT32_MIN = -0x7FFFFFFF - 1
} uapp_file_capability_t;

/* How many capabilities exist. Read at runtime for the same reason the
 * limits structs are: a client that hard-codes ten and is linked against a
 * library with eleven silently ignores one. */
UAPP_CAPI_API uint32_t uapp_file_capability_count(void);

/* The capability at `index` in [0, uapp_file_capability_count()). Returns
 * UAPP_FILE_CAPABILITY_VIEW for an out-of-range index -- bound your loop. */
UAPP_CAPI_API uapp_file_capability_t uapp_file_capability_at(uint32_t index);

/* Stable lowercase name, e.g. "annotate". Never NULL; "unknown" for an
 * integer that is not exactly one capability bit. Static storage. */
UAPP_CAPI_API const char* uapp_file_capability_name(uapp_file_capability_t capability);

/*
 * Is `capability` in `capabilities`? A one-line helper rather than making
 * every binding re-derive bit arithmetic over an enum whose values it would
 * have to cast. UAPP_FALSE for a value that is not exactly one bit, so
 * passing a mask of two capabilities is not quietly answered "yes".
 */
UAPP_CAPI_API uapp_bool_t uapp_file_capabilities_have(uint32_t capabilities,
                                                      uapp_file_capability_t capability);

/* What this product can actually do with `format`, as a bitmask of
 * uapp_file_capability_t. Queryable WITHOUT a file in hand, because a UI
 * building a "formats we support" table needs exactly that. */
UAPP_CAPI_API uint32_t uapp_file_format_capabilities(uapp_file_format_t format);

/* --- detection verdict metadata ------------------------------------------ */

/* How much the result should be believed. ORDERED: higher is stronger. Same
 * values as uapp::files::DetectionConfidence. */
typedef enum uapp_detection_confidence {
    /* Nothing matched and the name said nothing either. */
    UAPP_DETECTION_CONFIDENCE_NONE = 0,
    /* The bytes matched nothing; the filename extension is the ONLY signal.
     * `format` is still UNKNOWN -- read `extension_hint`. */
    UAPP_DETECTION_CONFIDENCE_EXTENSION_ONLY = 1,
    /* A content heuristic, not a signature, decided it. Plain text. */
    UAPP_DETECTION_CONFIDENCE_HEURISTIC = 2,
    /* A magic number matched. */
    UAPP_DETECTION_CONFIDENCE_MAGIC = 3,
    /* A magic number matched AND the container's own contents confirmed the
     * specific subtype. The strongest answer this module produces. */
    UAPP_DETECTION_CONFIDENCE_MAGIC_AND_CONTAINER = 4,

    UAPP_DETECTION_CONFIDENCE_FORCE_INT32     = 0x7FFFFFFF,
    UAPP_DETECTION_CONFIDENCE_FORCE_INT32_MIN = -0x7FFFFFFF - 1
} uapp_detection_confidence_t;

UAPP_CAPI_API const char* uapp_detection_confidence_name(
    uapp_detection_confidence_t confidence);

/*
 * WHAT produced the answer. Reported -- rather than collapsed into the
 * confidence -- so that a caller making a SECURITY decision can refuse to
 * act on a best-effort classification. The one that matters:
 * ZIP_LOCAL_HEADERS is defeatable by a crafted archive whose local header
 * lies about a name the central directory contradicts, which is precisely
 * why the method is surfaced instead of hidden.
 */
typedef enum uapp_detection_method {
    UAPP_DETECTION_METHOD_NO_MATCH                  = 0,
    UAPP_DETECTION_METHOD_MAGIC_SIGNATURE           = 1, /* signature at offset 0 */
    UAPP_DETECTION_METHOD_MAGIC_SIGNATURE_AT_OFFSET = 2, /* found after leading bytes (PDF) */
    UAPP_DETECTION_METHOD_TEXT_HEURISTIC            = 3,
    UAPP_DETECTION_METHOD_ZIP_CENTRAL_DIRECTORY     = 4, /* authoritative member list */
    UAPP_DETECTION_METHOD_ZIP_LOCAL_HEADERS         = 5, /* best effort; see above */
    UAPP_DETECTION_METHOD_ZIP_MIMETYPE_MEMBER       = 6, /* the ODF `mimetype` member */
    UAPP_DETECTION_METHOD_EXTENSION_HINT_ONLY       = 7,

    UAPP_DETECTION_METHOD_FORCE_INT32     = 0x7FFFFFFF,
    UAPP_DETECTION_METHOD_FORCE_INT32_MIN = -0x7FFFFFFF - 1
} uapp_detection_method_t;

UAPP_CAPI_API const char* uapp_detection_method_name(uapp_detection_method_t method);

/* --- checksum ------------------------------------------------------------
 *
 * APPEND-ONLY: these values are persisted alongside the digest bytes, which
 * is the whole reason the field carries an algorithm TAG and a
 * variable-length array rather than a bare uint64_t.
 *
 * WHAT THE DIGEST IS FOR: integrity ("did these bytes change since we last
 * looked?"), a cheap inequality test before an expensive byte comparison,
 * and a stable value to log.
 *
 * WHAT IT IS NOT: a signature, tamper evidence, or a content address. An
 * attacker who can change the bytes can change the stored digest. And a
 * 64-bit digest is NOT adequate for content-addressed storage -- at ~5
 * billion objects the birthday bound puts a collision at near-certainty, and
 * a collision in a content-addressed store means one user's file silently
 * resolving to another's. checksum.h has the full argument and the SHA-256
 * plan; UAPP_CHECKSUM_ALGORITHM_SHA256 is NOT IMPLEMENTED and deliberately
 * has no enumerator here, because a value with no producer behind it is a
 * promise this enum cannot keep.
 */
typedef enum uapp_checksum_algorithm {
    /* No digest computed. What uapp_file_detect_* reports -- detection never
     * reads every byte, and a digest over a 64 KiB window would be a digest
     * of the wrong thing wearing the right name. */
    UAPP_CHECKSUM_ALGORITHM_NONE = 0,
    /* FNV-1a 64 plus the splitmix64 avalanche finaliser. Integrity only. */
    UAPP_CHECKSUM_ALGORITHM_FNV1A64 = 1,

    UAPP_CHECKSUM_ALGORITHM_FORCE_INT32     = 0x7FFFFFFF,
    UAPP_CHECKSUM_ALGORITHM_FORCE_INT32_MIN = -0x7FFFFFFF - 1
} uapp_checksum_algorithm_t;

UAPP_CAPI_API const char* uapp_checksum_algorithm_name(uapp_checksum_algorithm_t algorithm);

/*
 * Widest digest uapp_file_info_t must hold, sized for SHA-256 so that adding
 * the second algorithm needs no layout change.
 *
 * Read the ACTUAL length of a digest from uapp_file_info_t::checksum_length;
 * this is the array bound, not a length. Confirmed against the C++
 * kMaxChecksumBytes by a static_assert in capi_file.cpp.
 */
#define UAPP_MAX_CHECKSUM_BYTES 32

/* --- where the bytes came from ------------------------------------------- */

/*
 * Tagged, NOT a string, and that is the decision file.h made for the same
 * reason this header keeps it: on mobile there is frequently no path at all
 * -- a picked file arrives as a security-scoped URI or as bytes -- and a
 * `const char* path` field would force every mobile call site to invent a
 * fake one. A fake path is a path something eventually tries to open.
 */
typedef enum uapp_file_location_kind {
    /* No location: built from bytes with no provenance. */
    UAPP_FILE_LOCATION_UNKNOWN = 0,
    /* A filesystem path we can reopen. */
    UAPP_FILE_LOCATION_LOCAL_PATH = 1,
    /* Bytes handed to us with no durable location -- a picker result, a
     * pasteboard item, an upload body. Re-reading is not possible. */
    UAPP_FILE_LOCATION_MEMORY_BUFFER = 2,
    /*
     * A platform URI (Android `content://`, iOS security-scoped URL).
     *
     * NOT IMPLEMENTED: resolving one. That needs the platform vtable
     * (architecture.md section 4's downward injection), which does not
     * exist. The kind is declared so a host can round-trip the value today
     * without it being mistaken for a path. Nothing in this ABI returns it
     * yet -- it is reachable only once a host can hand one in.
     */
    UAPP_FILE_LOCATION_PLATFORM_URI = 3,

    UAPP_FILE_LOCATION_FORCE_INT32     = 0x7FFFFFFF,
    UAPP_FILE_LOCATION_FORCE_INT32_MIN = -0x7FFFFFFF - 1
} uapp_file_location_kind_t;

UAPP_CAPI_API const char* uapp_file_location_kind_name(uapp_file_location_kind_t kind);

/* --- limits -------------------------------------------------------------- */

/*
 * Read at runtime rather than #define'd, for the reason uapp_note_limits_t
 * gives: a client that hard-codes a bound the loaded library does not
 * enforce shows the user a validation message that lies.
 */
typedef struct uapp_file_limits {
    uint32_t struct_size;             /* in: sizeof(uapp_file_limits_t) */
    size_t   head_sample_bytes;       /* out: bytes read from the front on the path route */
    size_t   tail_sample_bytes;       /* out: bytes read from the end, only if larger */
    size_t   text_probe_bytes;        /* out: bytes examined by the UTF-8/binary heuristic */
    size_t   max_checksum_bytes;      /* out: == UAPP_MAX_CHECKSUM_BYTES */
    size_t   format_count;            /* out: formats in the registry */
    size_t   implemented_format_count;/* out: how many have a sniffer here */
    uint32_t capability_count;        /* out: spec section 8 capabilities */
} uapp_file_limits_t;

/*
 * Fill `out_limits`. The caller MUST set out_limits->struct_size to
 * sizeof(uapp_file_limits_t) first; the usual rule applies -- a smaller
 * value is UAPP_STATUS_INVALID_ARGUMENT (honouring it would mean writing
 * past the end of the caller's object), a larger one is accepted with the
 * extra tail left untouched.
 */
UAPP_CAPI_API uapp_status_t uapp_file_get_limits(uapp_file_limits_t* out_limits);

/* --- the verdict -------------------------------------------------------- */

/*
 * ===========================================================================
 * ONE FLAT STRUCT FOR BOTH C++ RESULT TYPES, AND WHY
 * ===========================================================================
 * The C++ layer has two result types, and they overlap without nesting:
 *
 *   DetectionResult  the verdict: format, container_format, confidence,
 *                    method, extension_hint and the three container flags.
 *                    Produced by detect_sample/detect_buffer/detect_path.
 *   File             the spec section 8 value: mime_type, size, checksum,
 *                    capabilities, extension, the metadata flags. Produced
 *                    by describe_buffer/describe_path, which runs detection
 *                    internally and KEEPS NONE OF ITS VERDICT FIELDS.
 *
 * Mirroring that split would give this ABI two structs where neither is the
 * answer to "what is this file?": a caller wanting a confidence AND a
 * checksum would make two calls and marshal two structs. So there is ONE
 * struct carrying the union of both, filled by whichever route the caller
 * took, and every field says explicitly what it holds on the route that
 * does not fill it (see `checksum_algorithm` and `size_bytes`).
 *
 * This is NOT a composite the C++ layer refuses to produce -- it is the two
 * it does produce, side by side. The detect_* route leaves the File half at
 * its documented "not computed" values; the describe_* route fills both,
 * which costs one extra call to `detect_buffer`/`detect_sample` over the
 * same bytes. That is cheap and deliberate: detection is pure,
 * allocation-free and bounded by the head/tail windows, while the expensive
 * part -- hashing every byte -- still happens exactly once. The alternative
 * was for this layer to reconstruct the File fields itself from a
 * DetectionResult, which would mean reimplementing the capability and
 * mime-type derivation at the ABI boundary. That is forbidden: this layer
 * translates, it does not form second opinions.
 *
 * FIELD ORDER IS ABI. The struct grows only at the TAIL, which is what makes
 * struct_size versioning work (api-design.md section 10 rule 2). Note where
 * `checksum_bytes` sits: LAST, on purpose. It is the only fixed-size array
 * here, so it is the only member whose size could ever change (if a digest
 * wider than SHA-256 were ever needed); at the tail, such a change grows the
 * struct without moving a single existing field, exactly like an append.
 * Anywhere else it would be a silent relayout of a struct whose struct_size
 * an older caller is still reporting correctly.
 */
typedef struct uapp_file_info {
    uint32_t struct_size; /* in: sizeof(uapp_file_info_t) */

    /* ---- the detection verdict (always filled) ---------------------- */

    /* ALWAYS derived from bytes. NEVER from the filename. */
    uapp_file_format_t format;

    /* The outer container when `format` is a refinement of one -- ZIP for a
     * DOCX. UNKNOWN otherwise. */
    uapp_file_format_t container_format;

    uapp_detection_confidence_t confidence;
    uapp_detection_method_t     method;

    /* What the filename extension CLAIMED, or UNKNOWN if there was no name
     * or the extension is not in the registry. Advisory. */
    uapp_file_format_t extension_hint;

    /* The name claimed one format and the bytes said another. Not an error
     * -- a renamed file is common and legitimate -- but the one signal worth
     * showing a human before they open an attachment. */
    uapp_bool_t extension_conflicts;

    /* The container's AUTHORITATIVE member list could not be reached in the
     * sample: a truncated file, Zip64, or a head-only sample of a large
     * valid one. The detector cannot tell those apart and does not pretend
     * to. Any subtype claim came from local headers -- see
     * UAPP_DETECTION_METHOD_ZIP_LOCAL_HEADERS. */
    uapp_bool_t container_truncated;

    /* An OOXML package (`[Content_Types].xml` present) whose family we did
     * not recognise. `format` is ZIP in that case. Lets a caller say "this
     * is an Office file of a kind we do not handle" instead of "zip". */
    uapp_bool_t unrecognised_ooxml;

    /* Offset at which the signature was found. Non-zero only for formats
     * whose spec tolerates leading bytes (PDF). Reported because "a PDF with
     * 900 bytes of something else in front of it" is worth knowing. */
    uint64_t magic_offset;

    /* ---- the spec section 8 File half ------------------------------- */

    /* The media type of the DETECTED format, never of the claimed one.
     * Never NULL. STATIC STORAGE, program lifetime: it points into the
     * format registry, not into this struct and not into the heap, so it
     * outlives the struct and must never be freed. */
    const char* mime_type;

    /* Category of the DETECTED format. Convenience: identical to
     * uapp_file_format_category(format). */
    uapp_format_category_t category;

    /* Bitmask of uapp_file_capability_t for the DETECTED format. Identical
     * to uapp_file_format_capabilities(format) -- carried here so that a
     * UI deciding which buttons to show needs one call, not two. */
    uint32_t capabilities;

    /* True iff the filename carried a usable extension at all. Disambiguates
     * `extension_matches_content == UAPP_FALSE`, which covers BOTH "they
     * disagreed" and "there was no extension to agree with". */
    uapp_bool_t extension_present;

    /* True iff the extension was present AND agreed with the detected
     * format. */
    uapp_bool_t extension_matches_content;

    uapp_file_location_kind_t location_kind;

    /*
     * Total bytes of the file.
     *
     * On the describe_* routes this is the real size (for the path route,
     * the number of bytes actually STREAMED and hashed -- if the file was
     * truncated between the stat and the read, the size describes what was
     * hashed, because a size that disagrees with its own digest is a lie).
     * On the detect_* routes it is the size of the buffer you passed, and on
     * uapp_file_detect_path() it is 0: detection reads bounded windows and
     * never learns the total.
     */
    uint64_t size_bytes;

    /*
     * UAPP_CHECKSUM_ALGORITHM_NONE on both detect_* routes, with
     * checksum_length == 0 and checksum_bytes all zero. Detection does not
     * read every byte, and a digest over a 64 KiB window would be a digest
     * of the wrong thing wearing the right name.
     */
    uapp_checksum_algorithm_t checksum_algorithm;

    /* Valid bytes in `checksum_bytes`; 0 when the algorithm is NONE. Never
     * greater than UAPP_MAX_CHECKSUM_BYTES. */
    uint32_t checksum_length;

    /*
     * The digest, big-endian, so a hex rendering reads
     * most-significant-first on every platform. Bytes beyond
     * `checksum_length` are zeroed, not left as whatever was in the
     * caller's struct -- so a caller who memcmp's the whole array gets a
     * defined answer.
     *
     * LAST MEMBER ON PURPOSE. See the note above the struct.
     */
    uint8_t checksum_bytes[UAPP_MAX_CHECKSUM_BYTES];
} uapp_file_info_t;

/*
 * Initialise `out_info` to the "nothing detected" state and set its
 * struct_size.
 *
 * Always use it before a detect/describe call. It is the only way a caller
 * populates struct_size without hard-coding a number that will be wrong
 * after the struct grows, and it guarantees that a field a future library
 * does not know how to fill reads as a defined zero rather than as stack
 * garbage.
 */
UAPP_CAPI_API uapp_status_t uapp_file_info_init(uapp_file_info_t* out_info);

/* --- detection: bytes in, a verdict out ---------------------------------
 *
 * All four functions below require out_info->struct_size to be set (use
 * uapp_file_info_init), and all four write `*out_info` ONLY on
 * UAPP_STATUS_OK.
 *
 * `file_name_hint` may be NULL, which means "no name". Only its EXTENSION is
 * used, and only to populate `extension_hint` / `extension_conflicts`. It is
 * borrowed for the duration of the call.
 *
 * NOT IMPLEMENTED: a `declared_mime_type` hint (an HTTP Content-Type, an
 * Android `ContentResolver.getType()`). uapp::files::DetectionHints declares
 * the field and IGNORES it, because using it correctly means deciding a
 * trust order between a host-asserted type and our own sniffing -- a policy
 * question for security-model.md, not a default to guess at. This header
 * does not accept a parameter the core throws away: a parameter that is
 * documented as ignored is one a caller will eventually rely on. It appends
 * when the policy exists.
 */

/*
 * Detect from a COMPLETE in-memory buffer -- the mobile/sandboxed case where
 * there is no filesystem path, and the primary entry point. Because the
 * whole file is present, a ZIP's central directory is always reachable, so
 * this yields the strongest container classification available.
 *
 * `bytes` is borrowed for the duration of the call only; nothing retains it.
 *
 * Returns:
 *   UAPP_STATUS_OK                  -- including the legitimate outcome
 *                                      "nothing was found" (format UNKNOWN)
 *   UAPP_STATUS_FILE_EMPTY          -- size == 0. NOT reported as "no match":
 *                                      "there is nothing here" and "there is
 *                                      something here we do not recognise"
 *                                      are different facts and callers act
 *                                      on them differently.
 *   UAPP_STATUS_INVALID_ARGUMENT    -- NULL out_info, bad struct_size, or
 *                                      NULL `bytes` with a non-zero `size`
 *
 * A 1-byte, 3-byte or otherwise absurdly short buffer is a normal OK result,
 * not an error: every signature comparison inside goes through a span type
 * that cannot read past the end, so a 2-byte file checked against an 8-byte
 * PNG signature answers "no" rather than reading six bytes of adjacent heap.
 */
UAPP_CAPI_API uapp_status_t uapp_file_detect_buffer(const void* bytes,
                                                    size_t size,
                                                    const char* file_name_hint,
                                                    uapp_file_info_t* out_info);

/*
 * Detect from a filesystem path. Reads at most
 * uapp_file_limits_t::head_sample_bytes from the front and, only when the
 * file is larger than that, tail_sample_bytes from the end. NEVER the whole
 * file. The filename is used as the extension hint automatically, so there
 * is no hint parameter.
 *
 * `size_bytes` is left 0 and the checksum NONE -- see the field notes.
 *
 * Returns OK, UAPP_STATUS_FILE_PATH_INVALID (empty path, or one containing a
 * NUL), UAPP_STATUS_FILE_OPEN_FAILED, UAPP_STATUS_FILE_EMPTY (zero-length
 * file), UAPP_STATUS_FILE_READ_FAILED (a mid-read failure), or
 * UAPP_STATUS_INVALID_ARGUMENT for a NULL argument or bad struct_size.
 */
UAPP_CAPI_API uapp_status_t uapp_file_detect_path(const char* path,
                                                  uapp_file_info_t* out_info);

/*
 * Build the FULL spec section 8 description of a buffer: everything
 * uapp_file_detect_buffer() reports, PLUS size, checksum, capabilities and
 * the extension-agreement flags.
 *
 * The checksum covers EVERY BYTE of `bytes`, which is the only reason to
 * prefer this over uapp_file_detect_buffer(); if you do not need a digest,
 * do not pay for one.
 *
 * Timestamps are PARAMETERS, not read from a clock, for the reason
 * detector.h gives for the C++ signature: a detector that reaches for
 * ambient identity or ambient time is untestable. Pass 0 for both if you do
 * not have them; they are carried, not interpreted. (They are not echoed
 * back in uapp_file_info_t for the same reason the name is not -- they are
 * the caller's own values.)
 *
 * Same return set as uapp_file_detect_buffer().
 */
UAPP_CAPI_API uapp_status_t uapp_file_describe_buffer(const void* bytes,
                                                      size_t size,
                                                      const char* file_name_hint,
                                                      int64_t created_at_ms,
                                                      int64_t modified_at_ms,
                                                      uapp_file_info_t* out_info);

/*
 * Build the full description from a path. Unlike uapp_file_detect_path()
 * this DOES stream the whole file, because the checksum requires every byte;
 * it does so in bounded chunks and never holds more than one in memory.
 * Timestamps come from the filesystem where available.
 *
 * Same return set as uapp_file_detect_path().
 */
UAPP_CAPI_API uapp_status_t uapp_file_describe_path(const char* path,
                                                    uapp_file_info_t* out_info);

/* --- the one owned string in this header (rule O) ----------------------- */

/*
 * The lowercase extension of `name`, without the dot, or an empty string if
 * it has none. Released with uapp_free_string().
 *
 * Exposed because it handles the cases a naive `rfind('.')` gets wrong, and
 * a binding that reimplements it will get at least one of them wrong: a
 * dotfile (".bashrc" has NO extension -- it is a name), a trailing dot, a
 * dot that appears only in a directory component ("/a.b/c"), and an absurdly
 * long suffix. It does NOT strip double extensions: for "archive.tar.gz" the
 * extension is "gz", because that is what the last component claims.
 *
 * On UAPP_STATUS_OK, *out_extension is a NUL-terminated string the caller
 * owns -- NEVER NULL, possibly "". On failure it is left untouched.
 * UAPP_STATUS_INVALID_ARGUMENT for NULL arguments; UAPP_STATUS_OUT_OF_MEMORY
 * if the copy could not be allocated.
 */
UAPP_CAPI_API uapp_status_t uapp_file_copy_extension(const char* name, char** out_extension);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* UAPP_CAPI_UAPP_FILE_H */
