#ifndef UAPP_API_ROUTES_TOOLS_H
#define UAPP_API_ROUTES_TOOLS_H

#include "transport/http_server.h"

namespace uapp::api {

// The PDF-toolkit routes built on pdf-engine/writer — images-to-PDF today,
// and (composed with pdf-engine/reader's render_page) a rasterizing
// merge/split/rotate/organize as they land. Stateless — no AppState
// needed, unlike notes/blocks/search, since this module holds no
// persistent document store of its own.
void register_tools_routes(transport::HttpServer& server);

}  // namespace uapp::api

#endif  // UAPP_API_ROUTES_TOOLS_H
