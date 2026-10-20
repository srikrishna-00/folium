#ifndef UAPP_API_ROUTES_FILE_H
#define UAPP_API_ROUTES_FILE_H

#include "transport/http_server.h"

namespace uapp::api {

// Stateless — file detection takes no AppState (no handle, see
// uapp_file.h: pure bytes-in/verdict-out).
void register_file_routes(transport::HttpServer& server);

}  // namespace uapp::api

#endif  // UAPP_API_ROUTES_FILE_H
