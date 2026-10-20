// SPDX-License-Identifier: Apache-2.0
//
// core/capi/src/capi_common.cpp — ABI version, status mapping, uapp_free_string.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "capi_internal.h"
#include "uapp/capi/uapp_capi.h"
#include "uapp/status.h"

// ---------------------------------------------------------------------------
// The C enum and the C++ enum must agree, forever.
//
// These asserts are the mechanism behind "codes are flat and append-only". A
// renumbering in core/common/include/uapp/status.h — the kind of edit that
// looks harmless in review — becomes a compile error here instead of becoming
// an Android client that reports the wrong failure a release later.
// ---------------------------------------------------------------------------

#define UAPP_CAPI_ASSERT_CODE(cpp_name, c_name)                                    \
    static_assert(static_cast<std::int32_t>(::uapp::Status::cpp_name) ==           \
                      static_cast<std::int32_t>(c_name),                           \
                  "uapp_status_t drifted from uapp::Status for " #cpp_name)

// The width the UAPP_STATUS_FORCE_INT32 sentinel exists to guarantee. If a
// toolchain ever disagrees, every function in this ABI has the wrong return
// size and the failure mode is silent corruption — so assert it here, where it
// is a compile error, rather than discover it in a crash report.
static_assert(sizeof(uapp_status_t) == 4u, "uapp_status_t must be exactly 32 bits wide");
static_assert(sizeof(uapp_bool_t) == 4u, "uapp_bool_t must be exactly 32 bits wide");

UAPP_CAPI_ASSERT_CODE(Ok, UAPP_STATUS_OK);
UAPP_CAPI_ASSERT_CODE(InvalidArgument, UAPP_STATUS_INVALID_ARGUMENT);
UAPP_CAPI_ASSERT_CODE(NotFound, UAPP_STATUS_NOT_FOUND);
UAPP_CAPI_ASSERT_CODE(AlreadyExists, UAPP_STATUS_ALREADY_EXISTS);
UAPP_CAPI_ASSERT_CODE(Internal, UAPP_STATUS_INTERNAL);
UAPP_CAPI_ASSERT_CODE(IdMalformed, UAPP_STATUS_ID_MALFORMED);
UAPP_CAPI_ASSERT_CODE(NoteTitleTooLong, UAPP_STATUS_NOTE_TITLE_TOO_LONG);
UAPP_CAPI_ASSERT_CODE(NoteBodyTooLarge, UAPP_STATUS_NOTE_BODY_TOO_LARGE);
UAPP_CAPI_ASSERT_CODE(NoteIdMalformed, UAPP_STATUS_NOTE_ID_MALFORMED);
UAPP_CAPI_ASSERT_CODE(StoreOpenFailed, UAPP_STATUS_STORE_OPEN_FAILED);
UAPP_CAPI_ASSERT_CODE(StoreSchemaFailed, UAPP_STATUS_STORE_SCHEMA_FAILED);
UAPP_CAPI_ASSERT_CODE(StoreReadFailed, UAPP_STATUS_STORE_READ_FAILED);
UAPP_CAPI_ASSERT_CODE(StoreWriteFailed, UAPP_STATUS_STORE_WRITE_FAILED);
UAPP_CAPI_ASSERT_CODE(StoreReadOnly, UAPP_STATUS_STORE_READ_ONLY);
UAPP_CAPI_ASSERT_CODE(SearchQueryEmpty, UAPP_STATUS_SEARCH_QUERY_EMPTY);
UAPP_CAPI_ASSERT_CODE(SearchFailed, UAPP_STATUS_SEARCH_FAILED);
UAPP_CAPI_ASSERT_CODE(BlockTypeUnsupported, UAPP_STATUS_BLOCK_TYPE_UNSUPPORTED);
UAPP_CAPI_ASSERT_CODE(BlockDepthExceeded, UAPP_STATUS_BLOCK_DEPTH_EXCEEDED);
UAPP_CAPI_ASSERT_CODE(BlockTextTooLarge, UAPP_STATUS_BLOCK_TEXT_TOO_LARGE);
UAPP_CAPI_ASSERT_CODE(BlockMalformed, UAPP_STATUS_BLOCK_MALFORMED);
UAPP_CAPI_ASSERT_CODE(BlockPositionInvalid, UAPP_STATUS_BLOCK_POSITION_INVALID);
UAPP_CAPI_ASSERT_CODE(BlockCycleDetected, UAPP_STATUS_BLOCK_CYCLE_DETECTED);
UAPP_CAPI_ASSERT_CODE(PageTitleTooLong, UAPP_STATUS_PAGE_TITLE_TOO_LONG);
UAPP_CAPI_ASSERT_CODE(PageBlockLimitExceeded, UAPP_STATUS_PAGE_BLOCK_LIMIT_EXCEEDED);
UAPP_CAPI_ASSERT_CODE(FileEmpty, UAPP_STATUS_FILE_EMPTY);
UAPP_CAPI_ASSERT_CODE(FilePathInvalid, UAPP_STATUS_FILE_PATH_INVALID);
UAPP_CAPI_ASSERT_CODE(FileOpenFailed, UAPP_STATUS_FILE_OPEN_FAILED);
UAPP_CAPI_ASSERT_CODE(FileReadFailed, UAPP_STATUS_FILE_READ_FAILED);
UAPP_CAPI_ASSERT_CODE(PdfMalformed, UAPP_STATUS_PDF_MALFORMED);
UAPP_CAPI_ASSERT_CODE(PdfPasswordRequired, UAPP_STATUS_PDF_PASSWORD_REQUIRED);
UAPP_CAPI_ASSERT_CODE(PdfSecurityUnsupported, UAPP_STATUS_PDF_SECURITY_UNSUPPORTED);
UAPP_CAPI_ASSERT_CODE(PdfPageNotFound, UAPP_STATUS_PDF_PAGE_NOT_FOUND);
UAPP_CAPI_ASSERT_CODE(PdfPageLoadFailed, UAPP_STATUS_PDF_PAGE_LOAD_FAILED);
UAPP_CAPI_ASSERT_CODE(PdfRenderFailed, UAPP_STATUS_PDF_RENDER_FAILED);
UAPP_CAPI_ASSERT_CODE(PdfRenderSizeInvalid, UAPP_STATUS_PDF_RENDER_SIZE_INVALID);
UAPP_CAPI_ASSERT_CODE(PdfBufferTooSmall, UAPP_STATUS_PDF_BUFFER_TOO_SMALL);
UAPP_CAPI_ASSERT_CODE(PdfNotOpen, UAPP_STATUS_PDF_NOT_OPEN);

#undef UAPP_CAPI_ASSERT_CODE

namespace uapp::capi {

uapp_status_t to_c_status(Status s) noexcept {
    switch (s) {
        case Status::Ok:                return UAPP_STATUS_OK;
        case Status::InvalidArgument:   return UAPP_STATUS_INVALID_ARGUMENT;
        case Status::NotFound:          return UAPP_STATUS_NOT_FOUND;
        case Status::AlreadyExists:     return UAPP_STATUS_ALREADY_EXISTS;
        case Status::Internal:          return UAPP_STATUS_INTERNAL;
        case Status::NoteTitleTooLong:  return UAPP_STATUS_NOTE_TITLE_TOO_LONG;
        case Status::NoteBodyTooLarge:  return UAPP_STATUS_NOTE_BODY_TOO_LARGE;
        case Status::NoteIdMalformed:   return UAPP_STATUS_NOTE_ID_MALFORMED;
        case Status::StoreOpenFailed:   return UAPP_STATUS_STORE_OPEN_FAILED;
        case Status::StoreSchemaFailed: return UAPP_STATUS_STORE_SCHEMA_FAILED;
        case Status::StoreReadFailed:   return UAPP_STATUS_STORE_READ_FAILED;
        case Status::StoreWriteFailed:  return UAPP_STATUS_STORE_WRITE_FAILED;
        case Status::StoreReadOnly:     return UAPP_STATUS_STORE_READ_ONLY;
        case Status::SearchQueryEmpty:  return UAPP_STATUS_SEARCH_QUERY_EMPTY;
        case Status::SearchFailed:      return UAPP_STATUS_SEARCH_FAILED;

        // ===================================================================
        // core/blocks (spec section 14): NOW MAPPED TO REAL CODES, and this
        // is a DELIBERATE REVERSAL of the precedent set three commits running.
        // ===================================================================
        // Until ABI 0.2 these nine were listed here and mapped to
        // UAPP_STATUS_UNEXPECTED, with this reasoning, quoted verbatim from
        // the commit that added them:
        //
        //     "They map to UAPP_STATUS_UNEXPECTED ON PURPOSE, and no new
        //      UAPP_STATUS_* constant is minted: the C ABI is separately
        //      versioned and its codes are permanent, while NOTHING REACHABLE
        //      THROUGH THIS BOUNDARY CAN RETURN ONE OF THEM YET -- core/capi
        //      still exposes only NoteService. Minting an error vocabulary for
        //      a block ABI that does not exist would freeze a guess into a
        //      stable interface."
        //
        // The emphasised clause is the whole load-bearing part of that
        // argument, and uapp/capi/uapp_blocks.h has just made it FALSE. Every
        // one of these nine is now reachable from a C caller:
        // BlockPositionInvalid from uapp_document_service_add_block(),
        // BlockCycleDetected and BlockDepthExceeded from
        // uapp_document_service_move_block(), BlockMalformed from a heading
        // level of 9, PageTitleTooLong from a long title, IdMalformed from
        // every id argument on the surface, and so on.
        //
        // Keeping the old mapping would mean a Kotlin binding cannot tell
        // "you dragged that block into its own child" from "an unexpected
        // internal condition was contained at the C ABI boundary". That
        // directly contradicts the ERROR DISCIPLINE section of uapp_capi.h and
        // spec section 33 ("every subsystem must have error codes, a
        // human-readable message and a machine-readable code"), and it would
        // make UAPP_STATUS_UNEXPECTED -- documented as the boundary's LAST
        // LINE OF DEFENCE -- the single most common status a document editor
        // sees. It is also unfixable later without either a breaking change
        // or a second, parallel error channel.
        //
        // So the codes are minted, in the 5xx block, with the SAME NUMERIC
        // VALUES as uapp::Status, and the UAPP_CAPI_ASSERT_CODE lines above
        // now pin all nine at compile time. That is additive per the rule
        // uapp_capi.h documents for itself ("New functions and NEW ENUM VALUES
        // may appear"), so it is a MINOR bump (0.1 -> 0.2), not a major one: a
        // caller built against 0.1 never saw these codes because nothing could
        // produce them, so no existing behaviour changed for it.
        //
        // NOTE what did NOT happen: no code was invented that the core does
        // not have, and no distinction was made finer than the core can
        // actually make. The ABI mirrors uapp::Status exactly, which is the
        // same discipline the 2xx/3xx/4xx blocks already follow.
        case Status::IdMalformed:            return UAPP_STATUS_ID_MALFORMED;
        case Status::BlockTypeUnsupported:   return UAPP_STATUS_BLOCK_TYPE_UNSUPPORTED;
        case Status::BlockDepthExceeded:     return UAPP_STATUS_BLOCK_DEPTH_EXCEEDED;
        case Status::BlockTextTooLarge:      return UAPP_STATUS_BLOCK_TEXT_TOO_LARGE;
        case Status::BlockMalformed:         return UAPP_STATUS_BLOCK_MALFORMED;
        case Status::BlockPositionInvalid:   return UAPP_STATUS_BLOCK_POSITION_INVALID;
        case Status::BlockCycleDetected:     return UAPP_STATUS_BLOCK_CYCLE_DETECTED;
        case Status::PageTitleTooLong:       return UAPP_STATUS_PAGE_TITLE_TOO_LONG;
        case Status::PageBlockLimitExceeded: return UAPP_STATUS_PAGE_BLOCK_LIMIT_EXCEEDED;

        // core/search (spec section 16) added NO new Status code: the unified
        // port reports SearchQueryEmpty, SearchFailed, InvalidArgument and the
        // Store* family, every one of which was already mapped above for the
        // note surface. uapp/capi/uapp_search.h therefore needed no mint at
        // all, which is worth recording precisely because the paragraph above
        // argues for minting when a surface appears -- the test is whether the
        // core grew a code, not whether the ABI grew a header.

        // file-engine/detection (spec section 8/9) and pdf-engine/reader
        // (spec section 11, viewing only), ABI 0.3. These USED to be
        // flattened to UAPP_STATUS_UNEXPECTED with no constant minted, on the
        // reasoning the blocks/search commit established: "do not mint a
        // vocabulary for a surface that does not exist." uapp_file.h and
        // uapp_pdf.h are that surface now -- every one of these thirteen
        // codes is reachable through the C ABI for the first time -- so, on
        // the SAME reasoning applied a third time, they are minted as real
        // constants instead. See uapp_capi.h's ABI 0.3 version-history entry
        // and the note on uapp_pdf_is_available() for the one wrinkle this
        // group has that the blocks/search group did not: the PDF codes
        // exist in this enum UNCONDITIONALLY, even in a build with no
        // PDFium vendored, because a status code whose value depends on a
        // CMake switch is not a stable code -- a PDF-less build simply never
        // produces one.
        case Status::FileEmpty:        return UAPP_STATUS_FILE_EMPTY;
        case Status::FilePathInvalid:  return UAPP_STATUS_FILE_PATH_INVALID;
        case Status::FileOpenFailed:   return UAPP_STATUS_FILE_OPEN_FAILED;
        case Status::FileReadFailed:   return UAPP_STATUS_FILE_READ_FAILED;
        case Status::PdfMalformed:            return UAPP_STATUS_PDF_MALFORMED;
        case Status::PdfPasswordRequired:     return UAPP_STATUS_PDF_PASSWORD_REQUIRED;
        case Status::PdfSecurityUnsupported:  return UAPP_STATUS_PDF_SECURITY_UNSUPPORTED;
        case Status::PdfPageNotFound:         return UAPP_STATUS_PDF_PAGE_NOT_FOUND;
        case Status::PdfPageLoadFailed:       return UAPP_STATUS_PDF_PAGE_LOAD_FAILED;
        case Status::PdfRenderFailed:         return UAPP_STATUS_PDF_RENDER_FAILED;
        case Status::PdfRenderSizeInvalid:    return UAPP_STATUS_PDF_RENDER_SIZE_INVALID;
        case Status::PdfBufferTooSmall:       return UAPP_STATUS_PDF_BUFFER_TOO_SMALL;
        case Status::PdfNotOpen:              return UAPP_STATUS_PDF_NOT_OPEN;

        // ===================================================================
        // core/conversion (spec section 10): NOT MINTED, and this is the
        // ORIGINAL precedent applied for a fourth time, not a reversal of it.
        // ===================================================================
        // The test the block/search entry above states explicitly is "whether
        // the core grew a code", and the 0.3 entry adds the other half:
        // whether a C caller can reach one. The core grew three codes; a C
        // caller can reach none of them, because core/conversion has NO ABI
        // header. Markdown conversion landed without one on the same reasoning
        // blocks, search, file-engine and pdf-engine each landed on -- the
        // module stabilises first, the frozen boundary follows.
        //
        // So these three are listed (the switch has no `default:`, so -Wswitch
        // under -Werror is what forces whoever adds a Status code to come here
        // and make a decision) and flattened to UAPP_STATUS_UNEXPECTED, with no
        // UAPP_STATUS_CONVERSION_* constant minted. Nothing can produce one
        // through this boundary today, so the flattening is unobservable; when
        // a uapp_markdown.h exists, these become real constants in an 8xx
        // block mirroring uapp::Status, exactly as the 5xx and 6xx/7xx groups
        // did, and that is an additive MINOR bump.
        case Status::ConversionInputTooLarge:
        case Status::ConversionInputMalformed:
        case Status::ConversionOutputTooLarge:
            return UAPP_STATUS_UNEXPECTED;

        // ===================================================================
        // core/database (spec section 15): NOT MINTED — the same test, for a
        // fifth time.
        // ===================================================================
        // The test the 5xx entry states and the 0.3 entry completes is TWO
        // questions, not one: did the core grow a code, and can a C caller
        // reach it. The core grew sixteen; a C caller can reach none, because
        // core/database has NO ABI header — the property-typed database
        // landed the way blocks, search, file-engine, pdf-engine and
        // conversion each did: module first, frozen boundary after.
        //
        // So all sixteen are listed (the switch has no `default:`, so
        // -Wswitch under -Werror is what forces whoever adds a Status code to
        // come here and make a decision) and flattened to
        // UAPP_STATUS_UNEXPECTED, with no UAPP_STATUS_DATABASE_* constant
        // minted. The flattening is unobservable today; when a
        // uapp_database.h exists these become real constants in a 9xx block
        // mirroring uapp::Status numerically, exactly as the 5xx and 6xx/7xx
        // groups did, and that is an additive MINOR bump.
        //
        // Worth stating explicitly because sixteen is a lot to flatten at
        // once: the alternative is NOT "mint the eight that look important".
        // Minting a subset would give the boundary a vocabulary for some
        // validation failures of a surface it does not expose and not others,
        // and the choice of subset would be a guess frozen into a stable
        // interface — the exact failure mode this precedent exists to avoid.
        case Status::DatabasePropertyLimitExceeded:
        case Status::DatabaseRowLimitExceeded:
        case Status::DatabaseOptionLimitExceeded:
        case Status::DatabaseViewLimitExceeded:
        case Status::DatabaseViewSpecTooComplex:
        case Status::DatabaseNameTooLong:
        case Status::DatabaseValueTooLarge:
        case Status::DatabaseValueMalformed:
        case Status::DatabaseValueTypeMismatch:
        case Status::DatabaseOptionNotInSchema:
        case Status::DatabasePropertyNotInSchema:
        case Status::DatabasePropertyDuplicated:
        case Status::DatabasePropertyTypeUnsupported:
        case Status::DatabaseViewKindUnsupported:
        case Status::DatabaseFilterOperatorUnsupported:
        case Status::DatabaseSchemaMalformed:
            return UAPP_STATUS_UNEXPECTED;

        // ===================================================================
        // security/crypto (spec sections 23/38, ADR-0002 decision 1): NOT
        // MINTED — the same test, for a sixth time, and this time the answer
        // to the second question is "no, AND IT MUST STAY NO FOR LONGER".
        // ===================================================================
        // The two questions are: did the core grow a code, and can a C caller
        // reach it. The core grew nine; a C caller can reach none, because
        // security/crypto has NO ABI header — and unlike blocks, search,
        // file-engine, pdf-engine, conversion and database, this one is not
        // merely "module first, frozen boundary after".
        //
        // The crypto ABI is deliberately deferred on a STRONGER argument,
        // recorded in security/crypto/include/uapp/crypto/crypto.h under NOT
        // IMPLEMENTED item 6: the shape of these functions will change when
        // the key HIERARCHY above them exists (Recovery Secret -> URK -> WSK
        // -> document key -> per-file key, security-model.md §5A.1). An ABI
        // is a promise, and freezing `uapp_crypto_seal(key, pt, aad)` before
        // we know whether a caller is handed a CEK, a document id, or an
        // opaque key handle backed by the Secure Enclave would commit the
        // boundary to the wrong one of those three. Flattening to UNEXPECTED
        // costs nothing today because nothing can produce one of these
        // through the boundary.
        //
        // One of the nine deserves a specific note for whoever mints these
        // later. CryptoAuthFailed is intentionally the SINGLE code for wrong
        // key, tampered ciphertext, truncated input, substituted nonce and
        // mismatched AAD (see uapp/status.h). When a uapp_crypto.h exists,
        // that must survive into the C vocabulary as ONE constant. Splitting
        // it into UAPP_STATUS_CRYPTO_WRONG_KEY / _TAMPERED / _TRUNCATED would
        // look like a courtesy to binding authors and would in fact be a
        // decryption oracle frozen into a stable interface — the one
        // flattening in this file that is a security requirement rather than
        // a versioning convenience.
        case Status::CryptoNotInitialised:
        case Status::CryptoRandomFailed:
        case Status::CryptoKeyInvalid:
        case Status::CryptoAuthFailed:
        case Status::CryptoSealedBlobBad:
        case Status::CryptoMessageTooLarge:
        case Status::CryptoKdfFailed:
        case Status::CryptoSecureAllocFailed:
        case Status::CryptoPassphraseTooShort:
        // The key HIERARCHY layer's three codes (security/keys,
        // security-model.md §5A.1) flatten here for exactly the same reason
        // and under the same argument: there is no uapp_keys.h, and the
        // shape of a C-ABI key handle is the open question that keeps it
        // that way. Note that "wrong device", "wrong passphrase" and
        // "tampered record" are NOT among them — they are CryptoAuthFailed
        // above, and must stay one constant on the other side of any future
        // boundary for the oracle reason stated there.
        case Status::KeyRecordVersionUnsupported:
        case Status::KeyRecordRoleMismatch:
        case Status::KeyScopeIdInvalid:
            return UAPP_STATUS_UNEXPECTED;
    }
    // A code the core added and this layer has not been taught. We must not
    // guess a meaning for it; say so honestly instead.
    return UAPP_STATUS_UNEXPECTED;
}

char* dup_string(const char* s, std::size_t length) noexcept {
    if (s == nullptr) {
        return nullptr;
    }
    char* copy = new (std::nothrow) char[length + 1u];
    if (copy == nullptr) {
        return nullptr;
    }
    if (length > 0u) {
        std::memcpy(copy, s, length);
    }
    copy[length] = '\0';
    return copy;
}

}  // namespace uapp::capi

// ---------------------------------------------------------------------------
// Exported surface
// ---------------------------------------------------------------------------

extern "C" {

std::uint32_t uapp_abi_version_major(void) { return UAPP_ABI_VERSION_MAJOR; }

std::uint32_t uapp_abi_version_minor(void) { return UAPP_ABI_VERSION_MINOR; }

const char* uapp_abi_version_string(void) { return UAPP_ABI_VERSION_STRING; }

uapp_bool_t uapp_abi_is_compatible(std::uint32_t header_major, std::uint32_t header_minor) {
    // Additive-only within a major: a caller built against an older minor is
    // fine, a caller built against a NEWER minor is not — it may reference
    // symbols or enum values this library does not have.
    const bool compatible = (header_major == static_cast<std::uint32_t>(UAPP_ABI_VERSION_MAJOR)) &&
                            (header_minor <= static_cast<std::uint32_t>(UAPP_ABI_VERSION_MINOR));
    return compatible ? UAPP_TRUE : UAPP_FALSE;
}

uapp_bool_t uapp_status_is_ok(uapp_status_t status) {
    return (status == UAPP_STATUS_OK) ? UAPP_TRUE : UAPP_FALSE;
}

const char* uapp_status_code(uapp_status_t status) {
    switch (status) {
        case UAPP_STATUS_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
        case UAPP_STATUS_UNEXPECTED:    return "UNEXPECTED";

        case UAPP_STATUS_OK:
        case UAPP_STATUS_INVALID_ARGUMENT:
        case UAPP_STATUS_NOT_FOUND:
        case UAPP_STATUS_ALREADY_EXISTS:
        case UAPP_STATUS_INTERNAL:
        case UAPP_STATUS_NOTE_TITLE_TOO_LONG:
        case UAPP_STATUS_NOTE_BODY_TOO_LARGE:
        case UAPP_STATUS_NOTE_ID_MALFORMED:
        case UAPP_STATUS_STORE_OPEN_FAILED:
        case UAPP_STATUS_STORE_SCHEMA_FAILED:
        case UAPP_STATUS_STORE_READ_FAILED:
        case UAPP_STATUS_STORE_WRITE_FAILED:
        case UAPP_STATUS_STORE_READ_ONLY:
        case UAPP_STATUS_SEARCH_QUERY_EMPTY:
        case UAPP_STATUS_SEARCH_FAILED:
        case UAPP_STATUS_ID_MALFORMED:
        case UAPP_STATUS_BLOCK_TYPE_UNSUPPORTED:
        case UAPP_STATUS_BLOCK_DEPTH_EXCEEDED:
        case UAPP_STATUS_BLOCK_TEXT_TOO_LARGE:
        case UAPP_STATUS_BLOCK_MALFORMED:
        case UAPP_STATUS_BLOCK_POSITION_INVALID:
        case UAPP_STATUS_BLOCK_CYCLE_DETECTED:
        case UAPP_STATUS_PAGE_TITLE_TOO_LONG:
        case UAPP_STATUS_PAGE_BLOCK_LIMIT_EXCEEDED:
        case UAPP_STATUS_FILE_EMPTY:
        case UAPP_STATUS_FILE_PATH_INVALID:
        case UAPP_STATUS_FILE_OPEN_FAILED:
        case UAPP_STATUS_FILE_READ_FAILED:
        case UAPP_STATUS_PDF_MALFORMED:
        case UAPP_STATUS_PDF_PASSWORD_REQUIRED:
        case UAPP_STATUS_PDF_SECURITY_UNSUPPORTED:
        case UAPP_STATUS_PDF_PAGE_NOT_FOUND:
        case UAPP_STATUS_PDF_PAGE_LOAD_FAILED:
        case UAPP_STATUS_PDF_RENDER_FAILED:
        case UAPP_STATUS_PDF_RENDER_SIZE_INVALID:
        case UAPP_STATUS_PDF_BUFFER_TOO_SMALL:
        case UAPP_STATUS_PDF_NOT_OPEN:
        case UAPP_STATUS_FORCE_INT32:
        case UAPP_STATUS_FORCE_INT32_MIN:
            break;
    }
    // Values in the shared range delegate, so there is exactly one table of
    // code strings in the product rather than two that can disagree. Anything
    // else (a caller casting a bogus integer in) yields "UNKNOWN_STATUS" from
    // the core, which is the right answer.
    return ::uapp::status_code(static_cast<::uapp::Status>(status));
}

const char* uapp_status_message(uapp_status_t status) {
    switch (status) {
        case UAPP_STATUS_OUT_OF_MEMORY:
            return "A memory allocation at the C ABI boundary failed; nothing was changed.";
        case UAPP_STATUS_UNEXPECTED:
            return "An unexpected internal condition was contained at the C ABI boundary.";

        case UAPP_STATUS_OK:
        case UAPP_STATUS_INVALID_ARGUMENT:
        case UAPP_STATUS_NOT_FOUND:
        case UAPP_STATUS_ALREADY_EXISTS:
        case UAPP_STATUS_INTERNAL:
        case UAPP_STATUS_NOTE_TITLE_TOO_LONG:
        case UAPP_STATUS_NOTE_BODY_TOO_LARGE:
        case UAPP_STATUS_NOTE_ID_MALFORMED:
        case UAPP_STATUS_STORE_OPEN_FAILED:
        case UAPP_STATUS_STORE_SCHEMA_FAILED:
        case UAPP_STATUS_STORE_READ_FAILED:
        case UAPP_STATUS_STORE_WRITE_FAILED:
        case UAPP_STATUS_STORE_READ_ONLY:
        case UAPP_STATUS_SEARCH_QUERY_EMPTY:
        case UAPP_STATUS_SEARCH_FAILED:
        case UAPP_STATUS_ID_MALFORMED:
        case UAPP_STATUS_BLOCK_TYPE_UNSUPPORTED:
        case UAPP_STATUS_BLOCK_DEPTH_EXCEEDED:
        case UAPP_STATUS_BLOCK_TEXT_TOO_LARGE:
        case UAPP_STATUS_BLOCK_MALFORMED:
        case UAPP_STATUS_BLOCK_POSITION_INVALID:
        case UAPP_STATUS_BLOCK_CYCLE_DETECTED:
        case UAPP_STATUS_PAGE_TITLE_TOO_LONG:
        case UAPP_STATUS_PAGE_BLOCK_LIMIT_EXCEEDED:
        case UAPP_STATUS_FILE_EMPTY:
        case UAPP_STATUS_FILE_PATH_INVALID:
        case UAPP_STATUS_FILE_OPEN_FAILED:
        case UAPP_STATUS_FILE_READ_FAILED:
        case UAPP_STATUS_PDF_MALFORMED:
        case UAPP_STATUS_PDF_PASSWORD_REQUIRED:
        case UAPP_STATUS_PDF_SECURITY_UNSUPPORTED:
        case UAPP_STATUS_PDF_PAGE_NOT_FOUND:
        case UAPP_STATUS_PDF_PAGE_LOAD_FAILED:
        case UAPP_STATUS_PDF_RENDER_FAILED:
        case UAPP_STATUS_PDF_RENDER_SIZE_INVALID:
        case UAPP_STATUS_PDF_BUFFER_TOO_SMALL:
        case UAPP_STATUS_PDF_NOT_OPEN:
        case UAPP_STATUS_FORCE_INT32:
        case UAPP_STATUS_FORCE_INT32_MIN:
            break;
    }
    return ::uapp::status_message(static_cast<::uapp::Status>(status));
}

void uapp_free_string(char** string) {
    UAPP_CAPI_GUARD_BEGIN
        if (string == nullptr || *string == nullptr) {
            return;
        }
        delete[] *string;
        *string = nullptr;
    UAPP_CAPI_GUARD_END_VOID
}

// Defined here, in the translation unit that is ALWAYS built, rather than in
// capi_pdf.cpp, which is built only when UAPP_PDF_ENABLED is set. A caller
// must be able to ask this question before knowing whether capi_pdf.cpp's
// symbols exist in the library at all -- see the long note on this function
// in uapp_capi.h. UAPP_PDF_ENABLED is a compile definition the root
// CMakeLists.txt attaches to this file specifically when, and only when,
// pdf-engine/ itself is built (UAPP_ENABLE_PDF resolved to ON via AUTO or
// explicitly).
uapp_bool_t uapp_pdf_is_available(void) {
    UAPP_CAPI_GUARD_BEGIN
#if defined(UAPP_PDF_ENABLED) && UAPP_PDF_ENABLED
        return UAPP_TRUE;
#else
        return UAPP_FALSE;
#endif
    UAPP_CAPI_GUARD_END_FALLBACK(UAPP_FALSE)
}

}  // extern "C"
