#ifndef UAPP_API_ROUTES_UI_H
#define UAPP_API_ROUTES_UI_H

#include "transport/http_server.h"

namespace uapp::api {

// Serves a minimal, same-origin HTML+JS check-it-yourself page at GET /.
// This is NOT the real web frontend (that is M5 — a proper React/TS/Vite
// app per the plan) — it is a small, honest preview wired to the actual
// live routes below, so there is something real to click through before
// that milestone lands. Same-origin on purpose: avoids every CORS
// question a separately-hosted page would raise.
void register_ui_routes(transport::HttpServer& server);

}  // namespace uapp::api

#endif  // UAPP_API_ROUTES_UI_H
