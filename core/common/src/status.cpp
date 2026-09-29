// SPDX-License-Identifier: Apache-2.0
#include "uapp/status.h"

namespace uapp {

const char* status_code(Status s) noexcept {
    switch (s) {
        case Status::Ok:                return "OK";
        case Status::InvalidArgument:   return "INVALID_ARGUMENT";
        case Status::NotFound:          return "NOT_FOUND";
        case Status::AlreadyExists:     return "ALREADY_EXISTS";
        case Status::Internal:          return "INTERNAL";
        case Status::NoteTitleTooLong:  return "NOTE_TITLE_TOO_LONG";
        case Status::NoteBodyTooLarge:  return "NOTE_BODY_TOO_LARGE";
        case Status::NoteIdMalformed:   return "NOTE_ID_MALFORMED";
        case Status::StoreOpenFailed:   return "STORE_OPEN_FAILED";
        case Status::StoreSchemaFailed: return "STORE_SCHEMA_FAILED";
        case Status::StoreReadFailed:   return "STORE_READ_FAILED";
        case Status::StoreWriteFailed:  return "STORE_WRITE_FAILED";
        case Status::StoreReadOnly:     return "STORE_READ_ONLY";
        case Status::SearchQueryEmpty:  return "SEARCH_QUERY_EMPTY";
        case Status::SearchFailed:      return "SEARCH_FAILED";
    }
    // Reached only if a caller casts a bogus integer into Status. We do not
    // trust that it cannot happen just because the enum is closed.
    return "UNKNOWN_STATUS";
}

const char* status_message(Status s) noexcept {
    switch (s) {
        case Status::Ok:
            return "Success.";
        case Status::InvalidArgument:
            return "An argument was missing or structurally invalid.";
        case Status::NotFound:
            return "The requested note does not exist.";
        case Status::AlreadyExists:
            return "A note with that identifier already exists.";
        case Status::Internal:
            return "An internal error occurred; this is a defect, please report it.";
        case Status::NoteTitleTooLong:
            return "Note title exceeds the maximum allowed length.";
        case Status::NoteBodyTooLarge:
            return "Note body exceeds the maximum allowed size.";
        case Status::NoteIdMalformed:
            return "Note identifier is not 32 lowercase hexadecimal characters.";
        case Status::StoreOpenFailed:
            return "Could not open the local note database.";
        case Status::StoreSchemaFailed:
            return "Could not create or migrate the local note database schema.";
        case Status::StoreReadFailed:
            return "Reading from the local note database failed.";
        case Status::StoreWriteFailed:
            return "Writing to the local note database failed.";
        case Status::StoreReadOnly:
            return "The local note database was opened read-only; writes are rejected.";
        case Status::SearchQueryEmpty:
            return "The search query contained no searchable terms.";
        case Status::SearchFailed:
            return "The full-text search index could not answer the query.";
    }
    return "Unrecognised status code.";
}

}  // namespace uapp
