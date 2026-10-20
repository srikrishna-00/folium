#ifndef UAPP_API_ROUTES_PDF_H
#define UAPP_API_ROUTES_PDF_H

#include "app_state.h"
#include "transport/http_server.h"

namespace uapp::api {

// Only declared/compiled when UAPP_PDF_ENABLED — see api/CMakeLists.txt.
void register_pdf_routes(transport::HttpServer& server, AppState& state);

}  // namespace uapp::api

#endif  // UAPP_API_ROUTES_PDF_H
