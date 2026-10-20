// SPDX-License-Identifier: Apache-2.0
//
// core/common/status.h — the one error type.
//
// Spec section 33 ("Error handling"): every subsystem must have error codes, a
// human-readable message and a machine-readable code; errors are never
// silently ignored. ADR-0001 forbids exceptions, so *every* fallible operation
// in the core returns a `uapp::Status` and every such return is [[nodiscard]].
//
// architecture.md section 7 describes the eventual richer `uapp_error_t`
// (code + domain + message + cause chain) that the C ABI will expose. This
// slice implements the code + message half of it; the cause chain and the C
// ABI struct are deferred (see docs/changelog.md, "Known gaps").
//
// Codes are a FLAT, APPEND-ONLY enum. Never renumber, never reuse a value:
// these travel into logs, telemetry and (later) the stable C ABI.

#ifndef UAPP_CORE_COMMON_STATUS_H
#define UAPP_CORE_COMMON_STATUS_H

#include <cstdint>

namespace uapp {

enum class Status : std::int32_t {
    Ok = 0,

    // --- generic (1xx) ---
    InvalidArgument = 100,  ///< caller passed something structurally wrong
    NotFound        = 101,
    AlreadyExists   = 102,
    Internal        = 103,  ///< a bug in us, not in the caller's input
    IdMalformed     = 104,  ///< an ObjectId was not 32 hexadecimal characters

    // --- note domain validation (2xx) ---
    NoteTitleTooLong = 200,
    NoteBodyTooLarge = 201,
    NoteIdMalformed  = 202,

    // --- storage port (3xx) ---
    StoreOpenFailed   = 300,
    StoreSchemaFailed = 301,
    StoreReadFailed   = 302,
    StoreWriteFailed  = 303,
    StoreReadOnly     = 304,

    // --- search (4xx) ---
    SearchQueryEmpty = 400,  ///< query had no indexable terms after sanitising
    SearchFailed     = 401,

    // --- block / page document model (5xx) ---
    BlockTypeUnsupported   = 500,  ///< a spec section 14 type that is NOT IMPLEMENTED yet
    BlockDepthExceeded     = 501,  ///< nesting deeper than kMaxBlockDepth
    BlockTextTooLarge      = 502,
    BlockMalformed         = 503,  ///< payload violates its own type's rules
    BlockPositionInvalid   = 504,  ///< insert/move index past the end of the sibling list
    BlockCycleDetected     = 505,  ///< a move would make a block its own ancestor
    PageTitleTooLong       = 506,
    PageBlockLimitExceeded = 507,  ///< page already holds kMaxBlocksPerPage blocks

    // --- file engine (6xx) -------------------------------------------------
    // Detection only at this point. Parse/convert/render errors get their own
    // codes when those engines exist; they are NOT squeezed in here.
    FileEmpty       = 600,  ///< zero bytes in: nothing to work with. NOT format-specific:
                            ///< detection and the PDF reader both answer an empty buffer
                            ///< with this, because "no bytes" is one condition and giving
                            ///< it two codes would make logs harder to read, not easier.
    FilePathInvalid = 601,  ///< empty path, or a path containing a NUL byte
    FileOpenFailed  = 602,  ///< the path does not exist or could not be opened
    FileReadFailed  = 603,  ///< the file opened but a read failed part-way through

    // --- pdf engine (7xx) --------------------------------------------------
    // VIEWING ONLY at this point: the open / page-count / page-size /
    // render-to-caller-buffer slice of spec section 11. Editing, merge/split,
    // forms, annotation, signatures and the rest of section 11 are NOT
    // IMPLEMENTED, and codes are NOT pre-allocated for them — a code with no
    // code path behind it is a promise this enum cannot keep.
    //
    // Deliberately coarse. PDFium reports document-level failure through
    // FPDF_GetLastError() as one of six values; inventing finer distinctions
    // than the library can actually make would be fiction.
    PdfMalformed           = 700,  ///< not a PDF, or damaged past PDFium's recovery
    PdfPasswordRequired    = 701,  ///< encrypted; no password given, or the wrong one
    PdfSecurityUnsupported = 702,  ///< a security handler PDFium will not open
    PdfPageNotFound        = 703,  ///< page index outside [0, page_count)
    PdfPageLoadFailed      = 704,  ///< index in range, but the page object is unusable
    PdfRenderFailed        = 705,  ///< PDFium refused the render target it was handed
    PdfRenderSizeInvalid   = 706,  ///< non-positive, oversized, or arithmetically
                                   ///< overflowing width / height / stride
    PdfBufferTooSmall      = 707,  ///< the caller's pixel buffer cannot hold the request
    PdfNotOpen             = 708,  ///< operation on a Document that holds no document

    // --- conversion engine (8xx) -------------------------------------------
    // MARKDOWN ONLY at this point: the Page/Block <-> Markdown pair of spec
    // section 10's "common intermediate representation" architecture
    // (core/conversion). DOCX/PDF/HTML/EPUB conversion is NOT IMPLEMENTED and
    // gets NO codes here, for the reason the 7xx block states for PDF editing:
    // a code with no code path behind it is a promise this enum cannot keep.
    //
    // Deliberately only THREE codes, and all three are about the *conversion
    // boundary* rather than about the document model. Everything the converter
    // can produce that is really a block-model violation -- too-deep nesting,
    // oversized text, too many blocks -- is reported with the EXISTING 5xx
    // code for exactly that condition, because the importer's output IS a
    // block tree and inventing a parallel vocabulary for its limits would
    // mean two codes for one condition.
    ConversionInputTooLarge  = 800,  ///< source document exceeds the conversion input cap
    ConversionInputMalformed = 801,  ///< source document contains an embedded NUL byte
    ConversionOutputTooLarge = 802,  ///< the emitted document would exceed the output cap

    // --- database / collection system (9xx) --------------------------------
    // The property-typed database of spec section 15 (core/database): a named
    // schema of typed property DEFINITIONS plus rows of property VALUES, with
    // a Table view over them.
    //
    // Every one of these sixteen has a code path behind it today -- the rule
    // the 7xx and 8xx blocks state for themselves ("a code with no code path
    // behind it is a promise this enum cannot keep") is why the NOT
    // IMPLEMENTED halves of section 15 get NO codes: there is no
    // DatabaseRelationCycle, no DatabaseFormulaError and no
    // DatabaseGroupingUnsupported here, because Relation, Formula and
    // grouping do not exist. `DatabasePropertyTypeUnsupported` and
    // `DatabaseViewKindUnsupported` cover "you named one of those" the same
    // way BlockTypeUnsupported covers a declared-but-absent block type.
    //
    // They are also deliberately COARSER than one-code-per-rule. All four
    // name limits (database, property, option, view) report
    // DatabaseNameTooLong, because "a name in this namespace is too long" is
    // one condition with one fix and four codes for it would make logs harder
    // to read, not easier -- the same argument FileEmpty already makes.
    DatabasePropertyLimitExceeded     = 900,  ///< more properties than kMaxPropertiesPerDatabase
    DatabaseRowLimitExceeded          = 901,  ///< more rows than kMaxRowsPerDatabase
    DatabaseOptionLimitExceeded       = 902,  ///< option set past kMaxSelectOptionsPerProperty
    DatabaseViewLimitExceeded         = 903,  ///< more saved views than kMaxViewsPerDatabase
    DatabaseViewSpecTooComplex        = 904,  ///< more filters/sorts than the compiler will join
    DatabaseNameTooLong               = 905,  ///< any name in this namespace is over its cap
    DatabaseValueTooLarge             = 906,  ///< a Text value over kMaxPropertyTextBytes
    DatabaseValueMalformed            = 907,  ///< a value violates its own type's rules
    DatabaseValueTypeMismatch         = 908,  ///< value's type is not the property's type
    DatabaseOptionNotInSchema         = 909,  ///< selected an option the property does not define
    DatabasePropertyNotInSchema       = 910,  ///< referenced a property the database does not define
    DatabasePropertyDuplicated        = 911,  ///< duplicate property/option id or name in a schema
    DatabasePropertyTypeUnsupported   = 912,  ///< a section 15 property type that is NOT IMPLEMENTED
    DatabaseViewKindUnsupported       = 913,  ///< a section 15 view kind that is NOT IMPLEMENTED
    DatabaseFilterOperatorUnsupported = 914,  ///< operator is not legal for that property type
    DatabaseSchemaMalformed           = 915,  ///< the schema itself is structurally invalid

    // --- cryptographic primitives (10xx) ----------------------------------
    // security/crypto — the libsodium-backed primitives layer behind
    // ADR-0002 decision 1 ("E2EE from v1.0") and security-model.md §5/§5A.
    // This block covers the PRIMITIVES only: key generation, AEAD, key
    // wrapping, passphrase KDF. The E2EE *protocol* (device enrolment, WSK
    // rotation, per-document consent, sealed sync payloads) is NOT
    // IMPLEMENTED and gets NO codes here, for the reason the 7xx/8xx blocks
    // state: a code with no code path behind it is a promise this enum
    // cannot keep.
    //
    // NOTE THE DELIBERATE COARSENESS OF CryptoAuthFailed. Wrong key,
    // flipped ciphertext bit, truncated tag, swapped nonce and mismatched
    // AAD all return the SAME code, because they are the same event —
    // "this ciphertext did not authenticate under this key" — and because
    // reporting *which* of them happened hands an attacker a distinguishing
    // oracle. This is the one place in the enum where being less specific
    // is the security requirement, not a shortcut.
    CryptoNotInitialised   = 1000,  ///< crypto::init() has not run, or it failed
    CryptoRandomFailed     = 1001,  ///< the platform CSPRNG could not be used
    CryptoKeyInvalid       = 1002,  ///< key/public-key wrong size, all-zero, or low-order
    CryptoAuthFailed       = 1003,  ///< AEAD tag did not verify — see the note above
    CryptoSealedBlobBad    = 1004,  ///< sealed blob is structurally unusable (short/absent nonce)
    CryptoMessageTooLarge  = 1005,  ///< plaintext or AAD past this build's AEAD limit
    CryptoKdfFailed        = 1006,  ///< Argon2id could not run (almost always: out of memory)
    CryptoSecureAllocFailed = 1007, ///< sodium_malloc() refused — guarded pages exhausted
    CryptoPassphraseTooShort = 1008,///< passphrase below kMinPassphraseBytes
};

/// True iff the operation succeeded. Prefer `if (!uapp::is_ok(s))` over
/// comparing against Status::Ok by hand.
[[nodiscard]] constexpr bool is_ok(Status s) noexcept { return s == Status::Ok; }

/// Stable machine-readable identifier, e.g. "NOTE_NOT_FOUND". Never null.
/// This is the string that goes into structured logs and telemetry.
[[nodiscard]] const char* status_code(Status s) noexcept;

/// Human-readable, non-localised explanation. Never null. Contains no user
/// content (spec section 34: "Do not collect sensitive document contents in
/// telemetry") -- callers add context, the status never carries a note body.
[[nodiscard]] const char* status_message(Status s) noexcept;

}  // namespace uapp

#endif  // UAPP_CORE_COMMON_STATUS_H
