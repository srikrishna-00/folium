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

    // --- key hierarchy (11xx) ----------------------------------------------
    // security/keys — the key HIERARCHY layer (security-model.md §5A.1:
    // Recovery Secret -> URK -> Workspace Key -> document key). It owns the
    // RELATIONSHIPS between keys and the wrapped-key RECORD format. It owns
    // no cryptography at all, which is why most of its failures are 10xx
    // codes raised by security/crypto underneath it and only three are new.
    //
    // There is deliberately NO code here for "wrong device", "wrong
    // passphrase" or "tampered record". All three are CryptoAuthFailed, for
    // the oracle reason the 10xx block states: a hierarchy layer that
    // reported *which* of them happened would hand back the distinguisher
    // the primitives layer refuses to give. The three codes below are
    // reachable only from checks made BEFORE any key material is touched,
    // over fields that are plaintext metadata rather than secrets.
    //
    // Still NOT IMPLEMENTED, and therefore still codeless: device enrolment,
    // the out-of-band short-code check, WSK rotation and revocation (§5A.2,
    // §5A.3), and persistence of these records.
    KeyRecordVersionUnsupported = 1100,  ///< wrapped-key record version this build cannot read
    KeyRecordRoleMismatch       = 1101,  ///< record holds a different key role than was asked for
    KeyScopeIdInvalid           = 1102,  ///< workspace/document id is empty, null, or over the cap

    // --- AI provider abstraction (12xx) ------------------------------------
    // ai/provider + ai/local — the provider-independent AI abstraction of
    // spec §17 and ai-architecture.md §3/§13, plus the one real local
    // implementation behind it (ai::local::LocalHeuristicProvider).
    //
    // It does NOT cover RAG (spec §18), the agent framework (spec §20) or any
    // cloud provider: none of those exist, and per the rule the 7xx/8xx/9xx
    // blocks state for themselves, a code with no code path behind it is a
    // promise this enum cannot keep.
    //
    // TWO CODES HERE BEND THAT RULE, DELIBERATELY AND NARROWLY.
    // `AiRateLimited` and `AiContentPolicyRejected` are *provider-reported*
    // conditions, so no on-device provider can raise them and this build has
    // no network. They are allocated anyway because they are the two statuses
    // whose HANDLING is a property of the abstraction rather than of any one
    // provider: ai-architecture.md §5 forbids retrying a content-policy
    // refusal and forbids failing over across a locality boundary, and
    // `ai::is_failover_eligible()` encodes both. That predicate is real
    // compiled code with real tests, which is the closest thing to a code
    // path a remote-only condition can have here. Flagged for review in
    // docs/ai-architecture.md §14.
    //
    // Note the deliberate SPLIT of the consent failures into four codes
    // (Required / ScopeMismatch / Expired / TokenSpent) where the 10xx and
    // 11xx blocks deliberately COLLAPSE their failures into one. The reason is
    // the exact inverse of their oracle argument: these four are decided over
    // PLAINTEXT METADATA (a provider id, a timestamp, a content digest) before
    // any key material or content is touched; they are reported to the user's
    // own client, not to a remote peer; and "you never asked", "you asked
    // about something else", "your grant expired" and "that grant was already
    // used" need four different responses from the UI. One AI_CONSENT_FAILED
    // would make the consent boundary undebuggable exactly where it has to be
    // auditable.
    AiProviderUnavailable      = 1200,  ///< provider is registered but not usable right now
    AiCapabilityNotImplemented = 1201,  ///< this provider does not implement that §17 method
    AiProviderNotRegistered    = 1202,  ///< no provider with that id in the registry
    AiProviderDuplicated       = 1203,  ///< a provider with that id is already registered
    AiNoEligibleProvider       = 1204,  ///< no registered provider satisfies the selection
    AiPolicyBlocked            = 1205,  ///< workspace ai_policy forbids egress; fails closed
    AiConsentRequired          = 1206,  ///< off-device provider handed unauthorised content
    AiConsentDenied            = 1207,  ///< the user was asked and said no
    AiConsentScopeMismatch     = 1208,  ///< grant names a different provider, scope or content
    AiConsentExpired           = 1209,  ///< the grant's TTL had passed
    AiConsentTokenSpent        = 1210,  ///< that single-use consent token was already consumed
    AiInputEmpty               = 1211,  ///< nothing to work with: no content, or only whitespace
    AiInputTooLarge            = 1212,  ///< request content past this build's input cap
    AiInputMalformed           = 1213,  ///< embedded NUL, or a disallowed C0 control byte
    AiLabelSetInvalid          = 1214,  ///< classify() label set empty, oversized or duplicated
    AiResponseMalformed        = 1215,  ///< the PROVIDER broke its own declared output contract
    AiRateLimited              = 1216,  ///< provider-reported throttling — never retried here
    AiContentPolicyRejected    = 1217,  ///< provider refused the content — NEVER retried (§5)
    AiConsentRevoked           = 1218,  ///< the grant was revoked; stops FUTURE egress only

    // --- OCR engine (13xx) --------------------------------------------------
    // ocr/engine — the PRINTED-TEXT slice of spec §13: one raster image in, a
    // list of TextBlock{text, bounding_box, confidence, page, language, style}
    // out, with real coordinates from Tesseract's own page iterator.
    //
    // Section 13 also lists tables, forms, receipts, business cards and
    // handwriting ("where feasible"). None of those exist, and per the rule
    // the 7xx/8xx/9xx blocks state for themselves -- a code with no code path
    // behind it is a promise this enum cannot keep -- NONE of them gets a code
    // here. There is no OcrTableMalformed and no OcrHandwritingUnsupported.
    //
    // Note the DELIBERATE SPLIT between the three "no model" codes, which a
    // less careful enum would collapse into one "OCR unavailable":
    //
    //   OcrDataPathInvalid        — the directory we were pointed at is not a
    //         usable tessdata directory. A PACKAGING bug; the fix is in the
    //         installer, not in the UI.
    //   OcrLanguageUnavailable    — the directory is fine and
    //         <lang>.traineddata is not in it. A DOWNLOAD the user can be
    //         offered (api-design.md §4.6 already promises
    //         GET /v1/ocr/languages "which are on this device"), so the UI
    //         must be able to tell this apart from the case above.
    //   OcrLanguageNotImplemented — this BUILD will not ask for that language
    //         at all, because no pack for it has been vetted or vendored.
    //         Offering a download here would be a lie. Only "eng" is wired in
    //         this slice; see ocr/engine/include/uapp/ocr/engine.h.
    //
    // There is deliberately NO code for "the image was blank" or "the text was
    // illegible". Both are SUCCESSFUL recognitions of an image with no legible
    // text, and they return Status::Ok with zero or low-confidence blocks. An
    // OCR engine that reports failure for a photograph of a blank wall teaches
    // its caller to read Ok as "found something", which is exactly the
    // assumption a 14%-confidence noise result violates.
    OcrNotOpen                = 1300,  ///< recognise() on an Engine that holds no engine
    OcrDataPathInvalid        = 1301,  ///< tessdata path empty, NUL-bearing, or not a directory
    OcrLanguageUnavailable    = 1302,  ///< no <lang>.traineddata in that tessdata directory
    OcrLanguageNotImplemented = 1303,  ///< a §13 language this build has no vetted pack for
    OcrLanguageMalformed      = 1304,  ///< language tag empty, over-long, or not [a-z]+
    OcrEngineInitFailed       = 1305,  ///< Tesseract refused inputs that passed every check above
    OcrImageSizeInvalid       = 1306,  ///< non-positive, over a cap, or arithmetically overflowing
    OcrImageBufferTooSmall    = 1307,  ///< the caller's pixel buffer cannot hold that geometry
    OcrRecognitionFailed      = 1308,  ///< Tesseract's recogniser returned failure for the page
    OcrResultTooLarge         = 1309,  ///< more blocks, or more text, than this build will return

    // pdf-engine/writer (spec: new PDF toolkit work, see
    // pdf-engine/writer/include/uapp/pdf/writer.h). A separate block from
    // the 7xx PDF READER codes on purpose: writing and reading are different
    // engines with different failure shapes, and the reader's codes (wrong
    // password, page not found, render buffer too small) describe conditions
    // this module cannot produce.
    PdfWriterPageCountExceeded = 1400,  ///< more pages than WriterLimits::max_pages
    PdfWriterDimensionTooLarge = 1401,  ///< a page image's width or height past max_dimension
    PdfWriterTotalPixelsTooLarge = 1402,  ///< sum of every page's pixels past max_total_pixels
    PdfWriterRotationInvalid   = 1403,  ///< a PageImage::rotate_quarter_turns outside [0,3]
    PdfWriterEncodeFailed      = 1404,  ///< the JPEG encoder refused a page that passed every check above
    PdfWriterImageMalformed    = 1405,  ///< decode_image_to_rgb(): bytes are not a recognisable image
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
