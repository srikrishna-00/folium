#ifndef UAPP_API_STATUS_TO_HTTP_H
#define UAPP_API_STATUS_TO_HTTP_H

#include <string>

#include "uapp/capi/uapp_capi.h"

namespace uapp::api {

// uapp_status_t -> HTTP status code. One switch, one place, used by every
// route handler uniformly rather than each handler guessing its own
// mapping. UAPP_STATUS_OK is not a case here — a handler never calls this
// on success.
[[nodiscard]] int http_status_for(uapp_status_t status) noexcept;

// {"error":{"code":"...", "message":"..."}} built from uapp_status_code()
// (stable machine string) and uapp_status_message() (human string) — never
// from anything caller-supplied, so an error body can never echo untrusted
// input back as if it were the server's own diagnostic text.
[[nodiscard]] std::string json_error_body(uapp_status_t status);

}  // namespace uapp::api

#endif  // UAPP_API_STATUS_TO_HTTP_H
