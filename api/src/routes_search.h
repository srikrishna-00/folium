#ifndef UAPP_API_ROUTES_SEARCH_H
#define UAPP_API_ROUTES_SEARCH_H

#include "app_state.h"
#include "transport/http_server.h"

namespace uapp::api {

void register_search_routes(transport::HttpServer& server, AppState& state);

}  // namespace uapp::api

#endif  // UAPP_API_ROUTES_SEARCH_H
