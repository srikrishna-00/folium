// SPDX-License-Identifier: Apache-2.0
//
// M1 verification: a unit test calls the route handler directly, with no
// listening socket involved — httplib itself is exercised separately by
// actually running uapp-server and curling it (see the plan's M1 row), not
// here, since this test binary is compiled under the project's normal
// -fno-exceptions posture and never includes httplib.h.

#include "routes_health.h"
#include "test_framework.h"

UAPP_TEST("api.health: GET /healthz reports ok with a 200 and no input needed") {
    const uapp::api::transport::Request req{};  // method/path irrelevant to this handler
    uapp::api::transport::Response res;

    uapp::api::handle_healthz(req, res);

    UAPP_CHECK(res.status == 200);
    UAPP_CHECK(res.content_type == "application/json");
    UAPP_CHECK_MSG(res.body == R"({"status":"ok"})", "got: " + res.body);
}
