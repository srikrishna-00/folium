#ifndef UAPP_API_ROUTES_NOTES_H
#define UAPP_API_ROUTES_NOTES_H

#include "app_state.h"
#include "transport/http_server.h"

namespace uapp::api {

// Registers every /notes route on `server`, closing over `state`.
// state.mutex is held for the duration of each handler — see app_state.h.
void register_notes_routes(transport::HttpServer& server, AppState& state);

}  // namespace uapp::api

#endif  // UAPP_API_ROUTES_NOTES_H
