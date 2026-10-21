// uapp-server — M3 slice: notes, unified search, and file-type detection
// wired to a real server, with the shared-secret auth middleware.
// blocks/pages and PDF land in the rest of M3; OCR and AI are M4. See
// /Users/srikrishna/.claude/plans/squishy-roaming-comet.md for the full
// sequence.
//
// Compiled under the project's normal -fno-exceptions posture
// (uapp::compiler_options) — this file never includes httplib.h directly,
// only transport/http_server.h's exception-free interface.

#include "app_state.h"
#include "auth.h"
#include "routes_blocks.h"
#include "routes_file.h"
#include "routes_health.h"
#include "routes_notes.h"
#include "routes_search.h"
#include "routes_ui.h"
#include "transport/http_server.h"

#if UAPP_PDF_ENABLED
#include "routes_pdf.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr std::uint16_t kDefaultPort = 8080;

struct Args {
    std::string   host = "127.0.0.1";
    std::uint16_t port = kDefaultPort;
    std::string   database_path = "uapp-server.sqlite3";
    std::string   token_path    = "uapp-server-token.txt";
};

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            args.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            const long parsed = std::strtol(argv[++i], nullptr, 10);
            if (parsed > 0 && parsed <= 65535) {
                args.port = static_cast<std::uint16_t>(parsed);
            }
        } else if (arg == "--db" && i + 1 < argc) {
            args.database_path = argv[++i];
        } else if (arg == "--token-file" && i + 1 < argc) {
            args.token_path = argv[++i];
        }
    }
    return args;
}

bool is_loopback(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    uapp::api::AppState state;
    std::string          open_error;
    if (!state.open(args.database_path, open_error)) {
        std::fprintf(stderr, "uapp-server: failed to open the database: %s\n",
                     open_error.c_str());
        return 1;
    }

    uapp::api::transport::HttpServer server;

    // Auth model, reasoned about in auth.h: loopback gets no check at all
    // (equivalent to local file access); anything else requires a
    // shared-secret bearer token, generated once and reused across
    // restarts. /healthz is exempt everywhere — it carries no data and a
    // monitoring tool should not need the token to ask "is this up".
    std::string required_token;
    if (!is_loopback(args.host)) {
        const std::optional<std::string> token = uapp::api::load_or_create_token(
            args.token_path);
        if (!token.has_value()) {
            std::fprintf(stderr,
                         "uapp-server: bound beyond localhost but could not create a token "
                         "file at %s\n",
                         args.token_path.c_str());
            return 1;
        }
        required_token = *token;
        std::printf("uapp-server: auth token written to %s\n", args.token_path.c_str());
    }

    if (!required_token.empty()) {
        server.set_pre_route_hook([&required_token](const uapp::api::transport::Request& req,
                                                     uapp::api::transport::Response& res) {
            if (req.path == "/healthz" || req.path == "/") {
                // The static preview shell itself carries no data, so it
                // loads without the token; its own fetch() calls to the
                // real data routes below still need it.
                return true;
            }
            const std::string authorization = req.header("Authorization");
            if (uapp::api::bearer_token_matches(authorization, required_token)) {
                return true;
            }
            // Not a uapp_status_t code: "missing/wrong bearer token" is a
            // concept this HTTP boundary invented, with no core::Status
            // counterpart, so it does not go through json_error_body().
            res.status       = 401;
            res.content_type = "application/json";
            res.body         = R"({"error":{"code":"UNAUTHORIZED","message":"missing or incorrect bearer token"}})";
            return false;
        });
    }

    server.get("/healthz", uapp::api::handle_healthz);
    uapp::api::register_ui_routes(server);
    uapp::api::register_notes_routes(server, state);
    uapp::api::register_search_routes(server, state);
    uapp::api::register_file_routes(server);
    uapp::api::register_blocks_routes(server, state);
#if UAPP_PDF_ENABLED
    uapp::api::register_pdf_routes(server, state);
#endif

    std::printf("uapp-server: listening on %s:%u (db: %s)\n", args.host.c_str(),
               static_cast<unsigned>(args.port), args.database_path.c_str());
    if (!server.listen(args.host, args.port)) {
        std::fprintf(stderr, "uapp-server: failed to bind %s:%u\n", args.host.c_str(),
                     static_cast<unsigned>(args.port));
        return 1;
    }
    return 0;
}
