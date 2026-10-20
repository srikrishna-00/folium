// uapp-server — M1 slice: a real HTTP server with one route, GET /healthz.
//
// Business-logic routes (notes/blocks/search/pdf/ocr/ai) land in later
// milestones per /Users/srikrishna/.claude/plans/squishy-roaming-comet.md;
// this file stays deliberately small until there is a real route to wire.
//
// Compiled under the project's normal -fno-exceptions posture
// (uapp::compiler_options) — this file never includes httplib.h directly,
// only transport/http_server.h's exception-free interface.

#include "routes_health.h"
#include "transport/http_server.h"

#include <cstdio>
#include <cstdlib>

namespace {

constexpr const char* kDefaultHost = "127.0.0.1";
constexpr std::uint16_t kDefaultPort = 8080;

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = kDefaultPort;
    if (argc > 1) {
        const long parsed = std::strtol(argv[1], nullptr, 10);
        if (parsed > 0 && parsed <= 65535) {
            port = static_cast<std::uint16_t>(parsed);
        }
    }

    uapp::api::transport::HttpServer server;
    server.get("/healthz", uapp::api::handle_healthz);

    std::printf("uapp-server: listening on %s:%u\n", kDefaultHost, static_cast<unsigned>(port));
    if (!server.listen(kDefaultHost, port)) {
        std::fprintf(stderr, "uapp-server: failed to bind %s:%u\n", kDefaultHost,
                      static_cast<unsigned>(port));
        return 1;
    }
    return 0;
}
