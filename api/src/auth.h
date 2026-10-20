#ifndef UAPP_API_AUTH_H
#define UAPP_API_AUTH_H

// The server's auth model, reasoned about explicitly rather than left
// implicit (see the plan's "Authentication/session model" section):
//
//   * Bound to 127.0.0.1 (the default): NO auth. Anything that can already
//     reach a loopback socket on this host can read the SQLite file
//     directly, so a check here would be theatre.
//   * Bound beyond localhost (needed so a phone on the same network can
//     reach the server): a single shared-secret bearer token, generated
//     once at first run and checked on every route except /healthz. This
//     is the same self-hosted single-user pattern tools like Jellyfin/
//     Home Assistant use for a LAN-reachable instance — named here as
//     exactly that, not oversold as real multi-user authentication.

#include <optional>
#include <string>

namespace uapp::api {

// Loads the token from `token_path` if the file exists, or generates a
// fresh random one and writes it there (0600) if not. Returns std::nullopt
// only on an unrecoverable filesystem error (directory not creatable,
// etc.) — the caller should treat that as a fatal startup error when auth
// is required.
[[nodiscard]] std::optional<std::string> load_or_create_token(const std::string& token_path);

// True iff `header_value` is exactly "Bearer <token>" for the given token,
// compared in constant time (the token is a credential, however locally-
// scoped; no reason to let a timing side channel exist for free).
[[nodiscard]] bool bearer_token_matches(const std::string& header_value,
                                        const std::string& token) noexcept;

}  // namespace uapp::api

#endif  // UAPP_API_AUTH_H
