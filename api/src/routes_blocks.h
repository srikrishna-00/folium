#ifndef UAPP_API_ROUTES_BLOCKS_H
#define UAPP_API_ROUTES_BLOCKS_H

#include "app_state.h"
#include "transport/http_server.h"

namespace uapp::api {

void register_blocks_routes(transport::HttpServer& server, AppState& state);

}  // namespace uapp::api

#endif  // UAPP_API_ROUTES_BLOCKS_H
