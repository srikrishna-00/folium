# ---------------------------------------------------------------------------
# cmake/UappHttp.cmake — define uapp::httplib, a header-only INTERFACE target
# over the vendored cpp-httplib single header.
#
# See third_party/httplib/README.md for why this is vendored as committed
# source text rather than fetched as a binary (pdfium's strategy) or found as
# a system package (libsodium's strategy) — it's a third shape, not a repeat
# of either.
#
# THIS FILE NEVER TOUCHES THE NETWORK: the header is already committed, so
# there is nothing to fetch or find on the system. `cmake -S . -B build`
# stays offline-capable, same rule UappPdfium.cmake and UappSodium.cmake obey.
#
# Contract with the rest of the build:
#   uapp::httplib — INTERFACE target, include path only, no linked library.
# HTTPS (needed by ai/remote's Anthropic client) additionally requires
# CPPHTTPLIB_OPENSSL_SUPPORT + linking OpenSSL — that is cmake/UappOpenSsl.cmake's
# job, kept separate because the plain local HTTP server role (api/) does not
# need TLS at all in this phase.
# ---------------------------------------------------------------------------

add_library(uapp_httplib INTERFACE)
add_library(uapp::httplib ALIAS uapp_httplib)

# INTERFACE_INCLUDE_DIRECTORIES on a header-only target is treated as SYSTEM
# by consumers that mark it so; httplib.h is third-party source we do not
# hold to this project's -Wconversion -Wold-style-cast -Werror posture.
# Note the /include suffix: third_party/httplib/ itself also holds VERSION,
# README.md and LICENSE, and on a case-insensitive filesystem (macOS,
# Windows) a bare "third_party/httplib" include path makes any translation
# unit that pulls in the standard <version> header resolve it to our
# VERSION file instead (confirmed by hitting exactly this while bringing
# this target up) — hence the header lives in its own include/ directory
# with nothing else in it.
target_include_directories(uapp_httplib SYSTEM INTERFACE
    "${CMAKE_SOURCE_DIR}/third_party/httplib/include")
