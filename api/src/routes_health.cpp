#include "routes_health.h"

namespace uapp::api {

void handle_healthz(const transport::Request&, transport::Response& res) {
    res.status       = 200;
    res.content_type = "application/json";
    res.body         = R"({"status":"ok"})";
}

}  // namespace uapp::api
