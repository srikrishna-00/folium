# ---------------------------------------------------------------------------
# cmake/UappJson.cmake — define uapp::json, a header-only INTERFACE target
# over the vendored nlohmann/json single header.
#
# See third_party/httplib/README.md (shared rationale for both vendored
# single-header libraries) for why this is committed source text rather than
# a fetched binary or a system package. Never touches the network — the
# header is already committed.
#
# Contract with the rest of the build:
#   uapp::json — INTERFACE target, include path only, no linked library.
# ---------------------------------------------------------------------------

add_library(uapp_json INTERFACE)
add_library(uapp::json ALIAS uapp_json)

# Same /include suffix as cmake/UappHttp.cmake, same reason: keep this
# directory's VERSION/LICENSE.MIT files off the include path.
target_include_directories(uapp_json SYSTEM INTERFACE
    "${CMAKE_SOURCE_DIR}/third_party/nlohmann_json/include")
