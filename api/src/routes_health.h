#ifndef UAPP_API_ROUTES_HEALTH_H
#define UAPP_API_ROUTES_HEALTH_H

#include "transport/http_server.h"

namespace uapp::api {

// GET /healthz. Pulled out of main.cpp into its own translation unit so a
// test binary can call it directly without spinning up a real listening
// socket — the verification M1 actually needs ("a unit test calls the
// handler directly", not "a test opens a real port").
void handle_healthz(const transport::Request& req, transport::Response& res);

}  // namespace uapp::api

#endif  // UAPP_API_ROUTES_HEALTH_H
