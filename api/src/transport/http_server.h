#ifndef UAPP_API_TRANSPORT_HTTP_SERVER_H
#define UAPP_API_TRANSPORT_HTTP_SERVER_H

// A plain, exception-free C++ interface over cpp-httplib.
//
// WHY THIS FILE EXISTS, AND WHY IT IS THE ONLY ONE OF ITS KIND: httplib.h
// cannot be compiled under -fno-exceptions (verified empirically — see
// cmake/UappCompilerOptions.cmake's comment on uapp::compiler_options_exceptions_ok
// for the exact call sites). Every other file in api/ (and in the rest of
// this project) is compiled under the project's normal -fno-exceptions
// posture, so httplib.h must never be #included outside of
// http_server.cpp. This header is the boundary: no httplib type appears in
// any signature below, and http_server.cpp guarantees no C++ exception ever
// crosses out of its own functions — every httplib call it makes is inside
// its own try/catch, converted to the plain types here. Same philosophy as
// core/capi/src/capi_internal.h's UAPP_CAPI_GUARD_BEGIN/END macros, applied
// by hand to one file instead of generated for a whole ABI module.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace uapp::api::transport {

struct Request {
    std::string method;        // "GET", "POST", ...
    std::string path;          // "/notes/ab12..."
    std::string body;          // raw request body, empty for GET
    std::vector<std::pair<std::string, std::string>> query;    // already-decoded ?key=value pairs
    std::vector<std::pair<std::string, std::string>> headers;

    // Looks up a header case-insensitively; returns an empty string if
    // absent, matching the rest of this project's "absence is not an
    // error" convention for optional metadata.
    [[nodiscard]] std::string header(const std::string& name) const noexcept;

    // Looks up the FIRST query parameter with this exact name (case
    // sensitive, matching URL query-string convention); returns an empty
    // string if absent. A repeated key is not disambiguated here — no
    // current route needs it.
    [[nodiscard]] std::string query_param(const std::string& name) const noexcept;
};

struct Response {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
};

using Handler = std::function<void(const Request&, Response&)>;

// A thin, growable route table. Paths registered here are matched
// EXACTLY (no wildcards, no regex capture groups) — M1 only needs
// `/healthz`; path-parameter routes (`/notes/:id`) are added when M3
// actually needs them, not speculatively here.
class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    HttpServer(const HttpServer&)            = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void get(const std::string& path, Handler handler);
    void post(const std::string& path, Handler handler);

    // Blocks serving until stop() is called from another thread, or the
    // process is signalled. Returns false if the listen socket could not
    // be bound (port in use, insufficient permissions, bad address) —
    // never throws, never aborts the process for a recoverable condition.
    [[nodiscard]] bool listen(const std::string& host, std::uint16_t port);

    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace uapp::api::transport

#endif  // UAPP_API_TRANSPORT_HTTP_SERVER_H
