# api/ — uapp-server, the HTTP service that lets a web frontend reach
# core/capi. See /Users/srikrishna/.claude/plans/squishy-roaming-comet.md
# for the full milestone sequence; this file grows with each milestone
# rather than being written once for the whole plan, so it reflects only
# what actually exists and is tested right now.
#
# uapp_server_transport is a SEPARATE target from uapp_server, and that
# split is load-bearing, not organisational: httplib.h cannot be compiled
# under this project's normal -fno-exceptions posture (verified empirically
# — see cmake/UappCompilerOptions.cmake's comment on
# uapp::compiler_options_exceptions_ok for the exact call sites that fail).
# This is the same "isolate the one file that needs an exception to the
# project's rules behind its own target" idiom security/crypto/CMakeLists.txt
# already uses for uapp::sodium being PRIVATE: nothing outside this one
# small library may include httplib.h directly, and this library's own
# public header (transport/http_server.h) exposes no httplib type and lets
# no exception escape.
add_library(uapp_server_transport STATIC
    src/transport/http_server.cpp)

target_include_directories(uapp_server_transport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(uapp_server_transport
    PRIVATE uapp::httplib
            uapp::compiler_options_exceptions_ok)

# uapp_server_routes holds the actual route handlers plus the shared
# server-side state and helpers (app_state, status_to_http, auth,
# json_helpers), kept as its own target so tests/api/*.cpp can link it and
# call a handler directly without a listening socket — "a unit test calls
# the handler directly" per the plan's M1 verification step. Normal strict
# posture: these files only ever see transport/http_server.h's plain
# types, never httplib.h.
add_library(uapp_server_routes STATIC
    src/app_state.cpp
    src/auth.cpp
    src/json_helpers.cpp
    src/status_to_http.cpp
    src/routes_health.cpp
    src/routes_notes.cpp
    src/routes_search.cpp
    src/routes_file.cpp
    src/routes_blocks.cpp
    src/routes_tools.cpp
    src/routes_ui.cpp)

target_link_libraries(uapp_server_routes
    PUBLIC  uapp_server_transport
            uapp::capi
            uapp::json
            uapp::pdf_writer
    PRIVATE uapp::compiler_options)

# routes_pdf.cpp exists only when pdf-engine/ itself was built
# (UAPP_PDF_ENABLED), same reason core/capi/CMakeLists.txt gates
# capi_pdf.cpp — a library built without a vendored PDFium does not export
# the uapp_pdf_* symbols this file calls. PUBLIC so uapp_server (which
# links this target PRIVATEly) still receives the define: PRIVATE/PUBLIC on
# the LINKING side controls propagation to whoever links the CONSUMER, not
# whether the consumer itself sees it.
if(UAPP_PDF_ENABLED)
    target_sources(uapp_server_routes PRIVATE src/routes_pdf.cpp)
    target_compile_definitions(uapp_server_routes PUBLIC UAPP_PDF_ENABLED=1)
else()
    target_compile_definitions(uapp_server_routes PUBLIC UAPP_PDF_ENABLED=0)
endif()

# uapp_server itself stays on the project's normal, strict posture
# (uapp::compiler_options, -fno-exceptions included) — it never sees
# httplib.h, only uapp_server_transport's exception-free interface.
add_executable(uapp_server
    src/main.cpp)

target_link_libraries(uapp_server
    PRIVATE uapp_server_routes
            uapp::compiler_options)

set_target_properties(uapp_server PROPERTIES OUTPUT_NAME "uapp-server")
