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

        // file-engine/detection (spec section 8/9). These KEEP the old
        // treatment -- listed so the tripwire keeps working, mapped to
        // UAPP_STATUS_UNEXPECTED, no new constant minted -- and the reason is
        // the one the blocks block above says stopped applying to IT: there is
        // still no file ABI, so nothing reachable through this boundary can
        // return one of these. The precedent was never "flatten forever", it
        // was "do not mint a vocabulary for a surface that does not exist".
        // That is still exactly the case here. The file ABI will choose its
        // own codes when it exists, and this run explicitly did not build it.
        case Status::FileEmpty:
        case Status::FilePathInvalid:
        case Status::FileOpenFailed:
        case Status::FileReadFailed:
            return UAPP_STATUS_UNEXPECTED;

        // pdf-engine/reader (spec section 11, viewing only) added these, and
        // the file-engine paragraph above applies unchanged. This switch is an
        // exhaustive tripwire by design, so adding a core status without
        // listing it here is a compile error — which is why this block
        // exists at all, and it is the only edit pdf-engine made to core/capi.
        //
        // Worth restating for this group specifically: PDF is the subsystem
        // most likely to want a rich ABI vocabulary — a viewer must tell the
        // user "wrong password" differently from "this file is damaged" —
        // and that is exactly why no UAPP_STATUS_* constant is minted today.
        // The PDF ABI gets designed when there is a PDF ABI, not inferred
        // from the first nine statuses the C++ reader happened to need.
        case Status::PdfMalformed:
        case Status::PdfPasswordRequired:
        case Status::PdfSecurityUnsupported:
        case Status::PdfPageNotFound:
        case Status::PdfPageLoadFailed:
        case Status::PdfRenderFailed:
        case Status::PdfRenderSizeInvalid:
        case Status::PdfBufferTooSmall:
        case Status::PdfNotOpen:
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

}  // extern "C"
