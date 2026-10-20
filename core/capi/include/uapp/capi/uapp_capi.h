/* SPDX-License-Identifier: Apache-2.0 */
/*
 * uapp/capi/uapp_capi.h -- the common half of libuapp_capi.
 *
 * This is the ONLY supported boundary out of the native core
 * (architecture.md section 4: "libuapp_capi -- C99, opaque handles, explicit
 * ownership, versioned"). Everything above it -- uapp-kotlin over JNI, UAPPKit
 * over Swift/ObjC -- is a hand-written binding that calls these functions and
 * nothing else. No C++ type, no STL container and no C++ exception ever
 * crosses this line.
 *
 * WHAT IS IN THIS HEADER
 *   * the ABI version (spec section 30: "plugin interface must be versioned")
 *   * uapp_status_t, the C mirror of uapp::Status
 *   * the string-ownership rules and the one deallocator, uapp_free_string()
 *
 * The domain surfaces live in:
 *   * uapp/capi/uapp_notes.h   -- the flat note slice
 *   * uapp/capi/uapp_blocks.h  -- pages and blocks (the document model)
 *   * uapp/capi/uapp_search.h  -- unified search across notes and pages/blocks
 *   * uapp/capi/uapp_file.h    -- file-type detection (stateless; no handles)
 *   * uapp/capi/uapp_pdf.h     -- read-only PDF: open, measure, render
 *                                 OPTIONAL. See uapp_pdf_is_available() below;
 *                                 it is the one surface whose presence depends
 *                                 on a third-party binary the build may not
 *                                 have.
 *
 * Those five are ALL of them. Every module that exists in this repository now
 * has a C ABI; the surfaces still absent (OCR, scanner, sync, AI) are absent
 * because the engines are, not because the boundary skipped them.
 *
 * LANGUAGE LEVEL
 *   Strict C99. architecture.md section 8 rule 5 requires that capi headers
 *   "compile clean under -std=c99 -Wall -Wextra -Wpedantic with a C compiler,
 *   not just a C++ one". tests/capi/note_capi_test.c is compiled exactly that
 *   way with -Werror, so a C++-ism landing in here is a build failure.
 *
 * STRING OWNERSHIP -- THE TWO RULES, AND THERE ARE ONLY TWO
 *
 *   RULE B (borrowed). A function returning `const char*`, or writing one
 *   through a `const char**` out-parameter, hands back a pointer OWNED BY THE
 *   HANDLE it was read from. The caller must NOT free it. It stays valid until
 *   the handle is mutated or destroyed, whichever comes first. A binding that
 *   wants to outlive the handle must copy (JNI NewStringUTF, Swift String(cString:))
 *   before returning to managed code.
 *
 *   RULE O (owned). A function writing a `char*` through a `char**`
 *   out-parameter transfers ownership to the caller. The caller MUST release
 *   it with uapp_free_string() and with nothing else -- never free(), never
 *   delete[]. This matters because the core may be linked against a different
 *   allocator than the host application (api-design.md section 10, ABI rule 3:
 *   "No malloc/free across module boundaries").
 *
 *   Functions named uapp_*_get_* follow rule B. Functions named uapp_*_copy_*
 *   follow rule O. The name is the contract.
 *
 *   Strings returned by uapp_status_code(), uapp_status_message() and
 *   uapp_abi_version_string() are static storage with program lifetime: never
 *   freed, never invalidated. They are the third case only because "do
 *   nothing" is not really a rule.
 *
 * ERROR DISCIPLINE
 *   Every fallible function returns uapp_status_t. The only functions that do
 *   not are (a) destructors -- uapp_free_string() and the uapp_*_free/close
 *   family, which are total and null-safe by construction -- and (b) the
 *   version and status-string accessors, which are pure lookups that cannot
 *   fail. Out-parameters are written only on UAPP_STATUS_OK unless a function
 *   says otherwise.
 *
 * THREADING
 *   A handle is "use from one thread at a time". Distinct handles are fully
 *   independent and may be used concurrently from different threads; there is
 *   no shared mutable state behind this ABI, no thread-local error slot and no
 *   process-wide registry. See tests/capi/note_capi_test.c, case
 *   "slice: two independent services do not see each other".
 */

#ifndef UAPP_CAPI_UAPP_CAPI_H
#define UAPP_CAPI_UAPP_CAPI_H

#include <stddef.h>
#include <stdint.h>

/* --- symbol visibility ---------------------------------------------------
 *
 * libuapp_capi is built as a static archive today and linked into the host.
 * The JNI .so and the iOS framework will want a shared build with a closed
 * export set, so the annotation is here from the start: adding it later means
 * touching every declaration at exactly the moment nobody wants churn.
 */
#if defined(_WIN32)
#  if defined(UAPP_CAPI_BUILD_SHARED)
#    define UAPP_CAPI_API __declspec(dllexport)
#  elif defined(UAPP_CAPI_USE_SHARED)
#    define UAPP_CAPI_API __declspec(dllimport)
#  else
#    define UAPP_CAPI_API
#  endif
#elif defined(UAPP_CAPI_BUILD_SHARED) || defined(UAPP_CAPI_USE_SHARED)
#  define UAPP_CAPI_API __attribute__((visibility("default")))
#else
#  define UAPP_CAPI_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --- ABI version ---------------------------------------------------------
 *
 * Versioned independently of the product version and of the REST API
 * (api-design.md section 10). Contract, mirroring that document:
 *
 *   * Within a major, changes are ADDITIVE ONLY. New functions and new enum
 *     values may appear; existing signatures, enum values and semantics may
 *     not change. A caller built against minor N runs against library minor
 *     M >= N.
 *   * Structs crossing the boundary carry a leading uint32_t struct_size so
 *     they can grow without breaking callers compiled against an older header.
 *   * Status codes are flat and append-only. A value is never renumbered and
 *     never reused -- these travel into host logs and crash reports.
 *
 * MAJOR 0 is deliberate and is not a formality: it says the surface is not
 * frozen yet. The versioning MACHINERY is complete and enforced (the accessors
 * below are real exported symbols, not macros, so a binding can check the
 * library it actually loaded rather than the header it compiled against), but
 * the freeze -- and the ABI-diff CI job over the exported symbol list required
 * by api-design.md section 10 rule 5 -- lands with the first shipped release.
 * Until then, breaking changes bump MINOR and are recorded in the changelog.
 *
 * VERSION HISTORY
 *   0.1  the note slice (uapp_notes.h).
 *   0.2  ADDITIVE. uapp_blocks.h (pages/blocks) and uapp_search.h (unified
 *        search) appear; the 1xx code ID_MALFORMED and the 5xx block/page
 *        codes appear in uapp_status_t. No existing function signature, enum
 *        value or semantic changed, so a caller built against 0.1 runs
 *        unmodified against 0.2 -- which is exactly what the additive-only
 *        rule above says and what uapp_abi_is_compatible(0, 1) still answers
 *        UAPP_TRUE to.
 *   0.3  ADDITIVE. uapp_file.h (file-type detection) and uapp_pdf.h
 *        (read-only PDF) appear, and with them the 6xx file-engine and 7xx
 *        pdf-engine codes in uapp_status_t. Those codes already existed in
 *        uapp::Status and were FLATTENED to UAPP_STATUS_UNEXPECTED for exactly
 *        as long as nothing behind this boundary could produce one; these two
 *        headers end that, so they are minted. Same reasoning, same shape and
 *        same minor-bump conclusion as 0.2 -- see the long note in
 *        core/capi/src/capi_common.cpp, which now records the SECOND
 *        application of the rule rather than its first exception.
 *
 *        ONE WRINKLE, AND IT IS THE ONLY ONE IN THE VERSION HISTORY: the PDF
 *        surface is OPTIONAL at build time, because pdf-engine/ is built only
 *        when a PDFium is vendored (UAPP_ENABLE_PDF, see the root
 *        CMakeLists.txt). The ABI VERSION does not vary with that -- 0.3 is
 *        0.3 either way, the status codes are present either way, and
 *        uapp_abi_is_compatible() answers about the version and nothing else.
 *        What varies is whether the uapp_pdf_* FUNCTIONS are in the library,
 *        and the one symbol that answers THAT is uapp_pdf_is_available(),
 *        which is always present. Folding "PDF is missing" into the version
 *        would have been wrong twice over: it is not a version difference, and
 *        a binding needs a runtime answer, not a compile-time one.
 *
 *   0.4  ADDITIVE. uapp_crypto.h (security/crypto) and uapp_keys.h
 *        (security/keys) appear, and with them the 10xx crypto and 11xx key
 *        codes in uapp_status_t. Deferred since 0.1 on the explicit
 *        condition crypto.h's NOT IMPLEMENTED item 6 names -- "the shape of
 *        these functions will change when the key HIERARCHY above them
 *        exists" -- which landed on 2026-10-20 as security/keys. Same
 *        additive reasoning and minor-bump conclusion as 0.2 and 0.3: no
 *        existing function signature, enum value or semantic changed.
 *
 *        THE ONE THING THIS SURFACE DOES NOT DO: there is still no C
 *        accessor for a ContentKey's or any derived key's raw bytes.
 *        uapp_crypto_keypair_t exposes only its PUBLIC half; every secret key
 *        type crosses this boundary only as an opaque handle consumed by
 *        seal/open/wrap/unwrap, never read. A binding that needs the bytes
 *        is asking the wrong question of this ABI by design -- see
 *        secure_bytes.h's own limits for why no C accessor would be safe to
 *        add later either.
 */
#define UAPP_ABI_VERSION_MAJOR  0
#define UAPP_ABI_VERSION_MINOR  4
#define UAPP_ABI_VERSION_STRING "0.4"

/* Boolean across the boundary. Not <stdbool.h>'s bool: its size is
 * implementation-defined, and an ABI cannot afford that. */
typedef int32_t uapp_bool_t;
#define UAPP_FALSE 0
#define UAPP_TRUE  1

/* --- object identifiers --------------------------------------------------
 *
 * Every identifier in the product is the same 128-bit value rendered as 32
 * LOWERCASE hex characters (database-design.md section 2). It crosses this
 * boundary as a NUL-terminated string rather than a uint8_t[16], because a
 * string is what both bindings and both UIs actually carry: Kotlin and Swift
 * would otherwise each hand-roll hex rendering, and a mismatch between two
 * such renderings is a bug that only shows up as "the note is not found".
 *
 * Read the exact length at runtime from uapp_note_limits_t::id_hex_length,
 * uapp_block_limits_t::id_hex_length or uapp_search_limits_t::id_hex_length;
 * the macro below is for the ONE value with a meaning rather than a length.
 */
#define UAPP_ID_HEX_LENGTH 32

/*
 * The nil id: 32 '0' characters. It is never a real object's id. It means
 * "unset", and in uapp_blocks.h it specifically means "this block is a direct
 * child of the page, at the top level". Accessors that can return it return
 * this exact string rather than NULL, so a caller never special-cases before
 * strlen or strcmp.
 */
#define UAPP_ID_NIL_HEX "00000000000000000000000000000000"

/*
 * The C mirror of uapp::Status (core/common/include/uapp/status.h).
 *
 * Numeric values are IDENTICAL to the C++ enum's, and core/capi/src/status.cpp
 * asserts that at compile time for every single code -- so a renumbering in
 * the core fails the build instead of silently changing what a shipped Android
 * client thinks "303" means.
 *
 * The 9xx block is C-ABI-only: conditions that exist because there is a
 * boundary, and which have no uapp::Status counterpart.
 */
typedef enum uapp_status {
    UAPP_STATUS_OK = 0,

    /* generic (1xx) */
    UAPP_STATUS_INVALID_ARGUMENT = 100, /* null, out of range, or structurally wrong */
    UAPP_STATUS_NOT_FOUND        = 101,
    UAPP_STATUS_ALREADY_EXISTS   = 102,
    UAPP_STATUS_INTERNAL         = 103,
    UAPP_STATUS_ID_MALFORMED     = 104, /* a page or block id was not 32 hex characters */

    /* note domain validation (2xx) */
    UAPP_STATUS_NOTE_TITLE_TOO_LONG = 200,
    UAPP_STATUS_NOTE_BODY_TOO_LARGE = 201,
    UAPP_STATUS_NOTE_ID_MALFORMED   = 202,

    /* storage port (3xx) */
    UAPP_STATUS_STORE_OPEN_FAILED   = 300,
    UAPP_STATUS_STORE_SCHEMA_FAILED = 301,
    UAPP_STATUS_STORE_READ_FAILED   = 302,
    UAPP_STATUS_STORE_WRITE_FAILED  = 303,
    UAPP_STATUS_STORE_READ_ONLY     = 304,

    /* search (4xx) */
    UAPP_STATUS_SEARCH_QUERY_EMPTY = 400,
    UAPP_STATUS_SEARCH_FAILED      = 401,

    /* block / page document model (5xx) -- added in ABI 0.2 alongside
     * uapp_blocks.h. Before 0.2 these codes existed in uapp::Status but were
     * mapped to UAPP_STATUS_UNEXPECTED, because nothing reachable through this
     * boundary could produce one. uapp_blocks.h makes every one of them
     * reachable, so they are now minted rather than flattened -- see the long
     * note in core/capi/src/capi_common.cpp for why that is the opposite of
     * the precedent set by the blocks, file-engine and pdf-engine commits and
     * why the precedent does not apply once the surface exists. */
    UAPP_STATUS_BLOCK_TYPE_UNSUPPORTED    = 500, /* a declared but NOT IMPLEMENTED block type */
    UAPP_STATUS_BLOCK_DEPTH_EXCEEDED      = 501,
    UAPP_STATUS_BLOCK_TEXT_TOO_LARGE      = 502,
    UAPP_STATUS_BLOCK_MALFORMED           = 503, /* payload breaks its own type's rules */
    UAPP_STATUS_BLOCK_POSITION_INVALID    = 504, /* insert/move index past the sibling list */
    UAPP_STATUS_BLOCK_CYCLE_DETECTED      = 505, /* a move would make a block its own ancestor */
    UAPP_STATUS_PAGE_TITLE_TOO_LONG       = 506,
    UAPP_STATUS_PAGE_BLOCK_LIMIT_EXCEEDED = 507,

    /* file engine (6xx) -- added in ABI 0.3 alongside uapp_file.h, on exactly
     * the reasoning the 5xx block records: these four existed in uapp::Status
     * from the file-engine commit and were mapped to UAPP_STATUS_UNEXPECTED
     * for precisely as long as no C caller could reach code that returns one.
     * uapp_file.h makes all four reachable -- FILE_EMPTY from a zero-byte
     * buffer or file, the other three from uapp_file_detect_path() and
     * uapp_file_describe_path() -- so they are minted. */
    UAPP_STATUS_FILE_EMPTY        = 600, /* zero bytes in; NOT "unrecognised" */
    UAPP_STATUS_FILE_PATH_INVALID = 601, /* empty path, or one containing a NUL */
    UAPP_STATUS_FILE_OPEN_FAILED  = 602,
    UAPP_STATUS_FILE_READ_FAILED  = 603, /* opened, then failed part-way through */

    /* PDF engine (7xx) -- added in ABI 0.3 alongside uapp_pdf.h.
     *
     * These nine are present in the enum UNCONDITIONALLY, even in a library
     * built without PDFium where no uapp_pdf_* function exists. That is
     * deliberate: uapp_status_t is one flat append-only numbering shared by
     * every build of this ABI, and a status code whose VALUE depends on a
     * CMake switch is not a stable code at all. A PDF-less build simply never
     * returns one.
     *
     * The grouping is the one capi_common.cpp singled out as "the subsystem
     * most likely to want a rich ABI vocabulary -- a viewer must tell the user
     * 'wrong password' differently from 'this file is damaged'". It now has
     * one, and it is the C++ reader's vocabulary verbatim: nine codes, no
     * distinction finer than uapp::Status can actually make. */
    UAPP_STATUS_PDF_MALFORMED            = 700, /* not a PDF, or damaged past recovery */
    UAPP_STATUS_PDF_PASSWORD_REQUIRED    = 701, /* encrypted; absent or wrong password */
    UAPP_STATUS_PDF_SECURITY_UNSUPPORTED = 702,
    UAPP_STATUS_PDF_PAGE_NOT_FOUND       = 703, /* index outside [0, page_count) */
    UAPP_STATUS_PDF_PAGE_LOAD_FAILED     = 704, /* index in range, page unusable */
    UAPP_STATUS_PDF_RENDER_FAILED        = 705,
    UAPP_STATUS_PDF_RENDER_SIZE_INVALID  = 706, /* non-positive/oversized/overflowing */
    UAPP_STATUS_PDF_BUFFER_TOO_SMALL     = 707, /* the caller's pixel buffer is short */
    UAPP_STATUS_PDF_NOT_OPEN             = 708,

    /* C ABI boundary only (9xx) -- no uapp::Status counterpart */
    UAPP_STATUS_OUT_OF_MEMORY = 900, /* a boundary allocation failed; nothing was changed */
    UAPP_STATUS_UNEXPECTED    = 901, /* the catch-all; see the note below */

    /* security/crypto (10xx) -- added in ABI 0.4 alongside uapp_crypto.h, on
     * the SAME "module grew a code, boundary now reaches it" test applied
     * for the sixth time in capi_common.cpp. Deferred since ABI 0.1 pending
     * the key HIERARCHY (security/keys) existing, per crypto.h's own NOT
     * IMPLEMENTED item 6 -- it landed, so this is minted now, not a reversal.
     *
     * CryptoAuthFailed is deliberately the ONE code here for wrong key,
     * tampered ciphertext, truncated input, substituted nonce AND mismatched
     * AAD. This is the opposite of the ai/§17 consent codes' rule below, and
     * for the opposite reason: splitting it would hand a remote peer a
     * decryption oracle. See uapp/status.h and crypto.h for the full
     * argument; it must never be split by a future minor bump. */
    UAPP_STATUS_CRYPTO_NOT_INITIALISED    = 1000,
    UAPP_STATUS_CRYPTO_RANDOM_FAILED      = 1001,
    UAPP_STATUS_CRYPTO_KEY_INVALID        = 1002,
    UAPP_STATUS_CRYPTO_AUTH_FAILED        = 1003, /* wrong key / tampered / truncated / bad nonce / bad AAD -- ONE code, see above */
    UAPP_STATUS_CRYPTO_SEALED_BLOB_BAD    = 1004,
    UAPP_STATUS_CRYPTO_MESSAGE_TOO_LARGE  = 1005,
    UAPP_STATUS_CRYPTO_KDF_FAILED         = 1006,
    UAPP_STATUS_CRYPTO_SECURE_ALLOC_FAILED = 1007,
    UAPP_STATUS_CRYPTO_PASSPHRASE_TOO_SHORT = 1008,

    /* security/keys (11xx) -- added in ABI 0.4 alongside uapp_keys.h, same
     * commit as the crypto block above: the hierarchy and the primitives it
     * sits on are minted together, since uapp_keys.h's handles are built
     * directly out of uapp_crypto.h's ContentKey wrapper.
     *
     * Deliberately ONLY three codes, not a vocabulary for "wrong device" /
     * "wrong passphrase" / "tampered record" -- those are
     * UAPP_STATUS_CRYPTO_AUTH_FAILED, unconditionally, for the same oracle
     * reason stated above. These three are plaintext-metadata checks (record
     * version, record role, scope id shape), never a judgement over secret
     * material, which is exactly why they ARE allowed to be distinct. */
    UAPP_STATUS_KEY_RECORD_VERSION_UNSUPPORTED = 1100,
    UAPP_STATUS_KEY_RECORD_ROLE_MISMATCH       = 1101,
    UAPP_STATUS_KEY_SCOPE_ID_INVALID           = 1102,

    /*
     * Not a status. It pins the enum's width and its representable range, and
     * it is load-bearing twice over:
     *
     *   1. WIDTH. A C enum's underlying type is implementation-defined; a
     *      compiler is free to pick `unsigned char` for a small one. An ABI
     *      cannot have a return type whose size depends on which compiler
     *      built which side. This forces 32 bits everywhere.
     *
     *   2. RANGE. Without it, the enum's valid range stops just above the
     *      largest enumerator, and merely READING a value outside that range
     *      is undefined behaviour -- which a C caller can trivially produce,
     *      since in C an enum is an integer type and nothing stops anyone
     *      passing 424242 to uapp_status_code(). UBSan flags it, and it is
     *      genuinely undefined, not just noisy. Widening the range makes the
     *      defensive "unknown code" path in the implementation legal C++
     *      rather than a latent bug in the code meant to handle bad input.
     *
     * Never returned, never compared against.
     */
    UAPP_STATUS_FORCE_INT32 = 0x7FFFFFFF,

    /*
     * Also not a status, and it exists because the sentinel above does only
     * HALF of job 2 above. Added in ABI 0.2, after UBSan said so.
     *
     * An unfixed-underlying-type enum's valid range is derived from its
     * enumerators. With every enumerator non-negative, that range is
     * [0, 0x7FFFFFFF] -- so `uapp_status_code((uapp_status_t)-1)` from C was
     * STILL undefined behaviour, for exactly the reason the comment above
     * gives for 424242, and the asymmetry was invisible because the test only
     * ever cast a positive integer in. Pinning the bottom of the range makes
     * the whole of int32 representable, which is what "a C caller can
     * trivially produce" actually means: in C an enum IS an integer type, and
     * nothing stops anyone passing -1.
     *
     * Spelled -0x7FFFFFFF - 1 rather than -0x80000000 because the latter is
     * `-(0x80000000)`, and 0x80000000 does not fit in an int.
     */
    UAPP_STATUS_FORCE_INT32_MIN = -0x7FFFFFFF - 1
} uapp_status_t;

/*
 * UAPP_STATUS_UNEXPECTED is the boundary's last line of defence, and it is
 * deliberately not dead code even though ADR-0001 compiles the core with
 * -fno-exceptions.
 *
 * Two distinct things can produce it:
 *   1. A C++ exception reaching the boundary. Every exported function body is
 *      wrapped in a guard that, WHEN THE TRANSLATION UNIT IS BUILT WITH
 *      EXCEPTIONS ENABLED, catches everything and converts it to this code.
 *      Under the shipping -fno-exceptions build the guard compiles to nothing,
 *      so it costs no code size -- but the discipline is written into the
 *      source, so a future engine linked in with exceptions on cannot unwind
 *      through a JNI frame. (Under -fno-exceptions a throw calls
 *      std::terminate, which aborts. That is defined, diagnosable behaviour;
 *      undefined behaviour is what unwinding across a C frame would be.)
 *   2. A uapp::Status value the mapper does not recognise -- which means the
 *      core added a code and this layer was not updated. Reporting it as
 *      UNEXPECTED is correct: we must not guess a meaning for it.
 */

/* --- version accessors (cannot fail) ------------------------------------- */

UAPP_CAPI_API uint32_t    uapp_abi_version_major(void);
UAPP_CAPI_API uint32_t    uapp_abi_version_minor(void);
/* Static storage, program lifetime. Never free. */
UAPP_CAPI_API const char* uapp_abi_version_string(void);

/*
 * Is a caller compiled against (header_major, header_minor) able to run
 * against this library? True iff the major matches exactly and the library's
 * minor is >= the header's, per the additive-only rule above.
 *
 * Bindings should call UAPP_ABI_ASSERT_COMPATIBLE() once at load time
 * (JNI_OnLoad / framework init) and refuse to proceed if it is false. A
 * mismatch discovered at the first real call is a crash report nobody can read.
 */
UAPP_CAPI_API uapp_bool_t uapp_abi_is_compatible(uint32_t header_major, uint32_t header_minor);

#define UAPP_ABI_ASSERT_COMPATIBLE() \
    uapp_abi_is_compatible(UAPP_ABI_VERSION_MAJOR, UAPP_ABI_VERSION_MINOR)

/* --- optional surfaces (cannot fail) -------------------------------------
 *
 * Is the uapp_pdf.h surface present in THIS library?
 *
 * Every other function in this ABI is unconditional. The PDF one is not,
 * because pdf-engine/ is built only when a PDFium is vendored
 * (UAPP_ENABLE_PDF in the root CMakeLists.txt: AUTO by default, so a fresh
 * clone with no third_party/pdfium gets a library with no PDF in it). This
 * function is compiled into the ALWAYS-BUILT translation unit, so it is a
 * symbol you can call before you know the answer -- which is the entire
 * point of it existing rather than being a macro.
 *
 * When it returns UAPP_FALSE, the uapp_pdf_* functions declared in
 * uapp/capi/uapp_pdf.h ARE NOT IN THE LIBRARY. Calling one is a link error
 * against a static archive and a missing-symbol abort against a shared one;
 * no uapp_pdf_* function returns a "PDF is unavailable" status, because there
 * is no function there to return it. A binding checks this once at load time,
 * next to UAPP_ABI_ASSERT_COMPATIBLE(), and hides its PDF feature if it is
 * false.
 *
 * NOT the same question as uapp_abi_is_compatible(): the version says what
 * the SHAPE of the ABI is, this says whether one optional surface was built.
 * See the 0.3 note in the VERSION HISTORY above for why the two are
 * deliberately not folded together.
 *
 * NOT IMPLEMENTED: a generated <uapp/capi/uapp_capi_config.h> carrying the
 * same answer as a preprocessor macro, so a binding could #if the PDF code
 * out at compile time rather than branch at runtime. That needs an installed,
 * configure-time-generated header, which this project does not have yet and
 * which is a packaging decision (shared-library build, export set, install
 * rules) rather than an ABI one.
 */
UAPP_CAPI_API uapp_bool_t uapp_pdf_is_available(void);

/* --- status helpers (cannot fail) ---------------------------------------- */

UAPP_CAPI_API uapp_bool_t uapp_status_is_ok(uapp_status_t status);

/*
 * Stable machine-readable identifier, e.g. "STORE_WRITE_FAILED". Never NULL,
 * even for an integer that is not a valid uapp_status_t (you get
 * "UNKNOWN_STATUS"). Static storage, program lifetime -- never free it.
 *
 * Bindings switch on the numeric code and localise from it. This string is for
 * logs and bug reports.
 */
UAPP_CAPI_API const char* uapp_status_code(uapp_status_t status);

/*
 * Human-readable, non-localised, developer-facing explanation. Never NULL.
 * Static storage, program lifetime -- never free it.
 *
 * Contains NO user content: no note title, no body, no search query, no file
 * path (spec section 34 / security-model.md redaction rules). That property is
 * what makes it safe for a binding to attach verbatim to a crash report, and
 * it is why the message is a fixed lookup rather than a formatted string.
 */
UAPP_CAPI_API const char* uapp_status_message(uapp_status_t status);

/* --- the one deallocator for owned strings (rule O) ---------------------- */

/*
 * Release a string obtained from any uapp_*_copy_* function, and set the
 * caller's pointer to NULL.
 *
 * Takes char** rather than char* on purpose. The single most common C-API bug
 * at an FFI boundary is use-after-free of a released pointer; nulling it here
 * means the caller's variable cannot survive as a dangling pointer by
 * accident, and it makes double-free a no-op instead of heap corruption.
 * Every destructor in this ABI has the same shape for the same reason.
 *
 * Safe to call with NULL, and with a pointer to NULL. Never fails.
 */
UAPP_CAPI_API void uapp_free_string(char** string);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* UAPP_CAPI_UAPP_CAPI_H */
