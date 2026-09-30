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

        // core/blocks (spec section 14) added these codes. They are listed
        // rather than swept into a `default:` because this exhaustive switch
        // is exactly the mechanism that turns every future core status into a
        // compile error here until someone decides what the ABI should say.
        //
        // They map to UAPP_STATUS_UNEXPECTED ON PURPOSE, and no new
        // UAPP_STATUS_* constant is minted: the C ABI is separately versioned
        // and its codes are permanent, while nothing reachable through this
        // boundary can return one of them yet -- core/capi still exposes only
        // NoteService. Minting an error vocabulary for a block ABI that does
        // not exist would freeze a guess into a stable interface.
        case Status::IdMalformed:
        case Status::BlockTypeUnsupported:
        case Status::BlockDepthExceeded:
        case Status::BlockTextTooLarge:
        case Status::BlockMalformed:
        case Status::BlockPositionInvalid:
        case Status::BlockCycleDetected:
        case Status::PageTitleTooLong:
        case Status::PageBlockLimitExceeded:
            return UAPP_STATUS_UNEXPECTED;

        // file-engine/detection (spec section 8/9) added these codes, and the
        // paragraph above applies to them unchanged: listed so that this
        // tripwire keeps working, mapped to UAPP_STATUS_UNEXPECTED, and NO
        // new UAPP_STATUS_* constant minted. core/capi still exposes only
        // NoteService, so nothing reachable through this boundary can return
        // one of them; the file ABI will choose its own vocabulary when it
        // exists.
        case Status::FileEmpty:
        case Status::FilePathInvalid:
        case Status::FileOpenFailed:
        case Status::FileReadFailed:
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
        case UAPP_STATUS_FORCE_INT32:
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
        case UAPP_STATUS_FORCE_INT32:
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
