#ifndef UAPP_API_JSON_HELPERS_H
#define UAPP_API_JSON_HELPERS_H

// Safe-by-construction helpers over nlohmann::json for parsing UNTRUSTED
// request bodies.
//
// WHY THIS FILE EXISTS: nlohmann/json auto-detects this project's
// -fno-exceptions build and replaces every throwing path with
// std::abort() instead (confirmed empirically while vendoring it — see
// third_party/httplib/README.md's sibling note on nlohmann_json). That is
// fine for a parse error over OUR OWN static strings (status_to_http.cpp),
// and NOT fine for anything built from a request body: a malformed
// request must never be able to crash the whole server process. Every
// function here is written to never hit one of those paths — parse with
// allow_exceptions=false and check is_discarded(); access a field only
// after checking contains()+is_string()/is_number() first, never via
// .at() or an unchecked get<T>(). Every route handler MUST go through
// these rather than calling nlohmann::json's own API directly on
// request-derived data.

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace uapp::api {

// Parses `body` as a JSON object. Returns std::nullopt if it is not valid
// JSON, or not a JSON object (an array/string/number at the top level is
// never a valid request body in this API).
[[nodiscard]] std::optional<nlohmann::json> parse_json_object(const std::string& body);

// Reads a required string field. Returns false (leaving `out` untouched)
// if the key is absent or not a string.
[[nodiscard]] bool get_required_string(const nlohmann::json& j, const char* key,
                                       std::string& out);

// Reads an optional string field with a default. Never fails: a present
// non-string value is treated the same as an absent one.
[[nodiscard]] std::string get_optional_string(const nlohmann::json& j, const char* key,
                                              const std::string& fallback);

// Dumps `j` to a string without ever aborting on malformed UTF-8 in a
// string field that ultimately came from user input — replaces invalid
// sequences rather than crashing. Use this, never a bare .dump(), for any
// JSON object that contains caller-supplied strings.
[[nodiscard]] std::string dump_safe(const nlohmann::json& j);

}  // namespace uapp::api

#endif  // UAPP_API_JSON_HELPERS_H
