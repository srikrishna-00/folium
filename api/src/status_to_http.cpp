#include "status_to_http.h"

#include <nlohmann/json.hpp>

namespace uapp::api {

int http_status_for(uapp_status_t status) noexcept {
    switch (status) {
        // --- 400: the request itself is malformed / structurally wrong ---
        case UAPP_STATUS_INVALID_ARGUMENT:
        case UAPP_STATUS_ID_MALFORMED:
        case UAPP_STATUS_NOTE_TITLE_TOO_LONG:
        case UAPP_STATUS_NOTE_BODY_TOO_LARGE:
        case UAPP_STATUS_NOTE_ID_MALFORMED:
        case UAPP_STATUS_SEARCH_QUERY_EMPTY:
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
        case UAPP_STATUS_PDF_MALFORMED:
        case UAPP_STATUS_PDF_RENDER_SIZE_INVALID:
        case UAPP_STATUS_PDF_BUFFER_TOO_SMALL:
        case UAPP_STATUS_CRYPTO_KEY_INVALID:
        case UAPP_STATUS_CRYPTO_MESSAGE_TOO_LARGE:
        case UAPP_STATUS_CRYPTO_SEALED_BLOB_BAD:
        case UAPP_STATUS_CRYPTO_PASSPHRASE_TOO_SHORT:
        case UAPP_STATUS_KEY_SCOPE_ID_INVALID:
        case UAPP_STATUS_KEY_RECORD_VERSION_UNSUPPORTED:
        case UAPP_STATUS_KEY_RECORD_ROLE_MISMATCH:
            return 400;

        // --- 401/403: authn/authz-shaped conditions ---
        case UAPP_STATUS_PDF_PASSWORD_REQUIRED:
            return 401;
        case UAPP_STATUS_CRYPTO_AUTH_FAILED:
            // Deliberately 400, not 401/403: this is a content-integrity
            // failure (wrong key, tampered bytes), not "log in again".
            return 400;

        // --- 404: the thing the request named does not exist ---
        case UAPP_STATUS_NOT_FOUND:
        case UAPP_STATUS_PDF_PAGE_NOT_FOUND:
        case UAPP_STATUS_PDF_NOT_OPEN:
            return 404;

        // --- 409: the request conflicts with existing state ---
        case UAPP_STATUS_ALREADY_EXISTS:
            return 409;

        // --- 500: everything else. INTERNAL, UNEXPECTED, OUT_OF_MEMORY,
        // every STORE_*/FILE_OPEN_FAILED/FILE_READ_FAILED/PDF_*_FAILED code,
        // and any status this function has not been taught about yet — a
        // new uapp_status_t value defaulting to 500 is always a SAFE
        // fallback (it never claims success, never leaks which specific
        // internal condition occurred), unlike core/capi's own to_c_status()
        // switch, where an unhandled case would silently misreport a
        // specific condition as a different one. There is deliberately no
        // -Wswitch enforcement here for that reason.
        default:
            return 500;
    }
}

std::string json_error_body(uapp_status_t status) {
    // Plain .dump() is safe ONLY because both inputs are this project's own
    // static C string literals (never user-controlled) — nlohmann/json
    // auto-detects -fno-exceptions and calls std::abort() instead of
    // throwing on malformed UTF-8, which would be a real concern for
    // anything built from request data. Route handlers serialising
    // caller-supplied strings must NOT copy this pattern; see routes_notes.cpp
    // for the error_handler::replace convention used there instead.
    nlohmann::json body;
    body["error"]["code"]    = uapp_status_code(status);
    body["error"]["message"] = uapp_status_message(status);
    return body.dump();
}

}  // namespace uapp::api
