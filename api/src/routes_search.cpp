#include "routes_search.h"

#include <cstdlib>
#include <mutex>

#include <nlohmann/json.hpp>

#include "json_helpers.h"
#include "status_to_http.h"
#include "uapp/capi/uapp_search.h"

namespace uapp::api {

namespace {

void handle_query(AppState& state, const transport::Request& req, transport::Response& res) {
    const std::string query = req.query_param("q");
    if (query.empty()) {
        res.status       = http_status_for(UAPP_STATUS_SEARCH_QUERY_EMPTY);
        res.content_type = "application/json";
        res.body         = json_error_body(UAPP_STATUS_SEARCH_QUERY_EMPTY);
        return;
    }

    // uapp_search_index_query() takes limit == 0 as INVALID_ARGUMENT (the
    // unified surface has no "0 means default" convenience the per-store
    // uapp_note_service_search() has) — so this endpoint picks a real
    // default rather than passing through a possibly-absent query param.
    const std::string limit_param = req.query_param("limit");
    size_t            limit       = 50;
    if (!limit_param.empty()) {
        const long parsed = std::strtol(limit_param.c_str(), nullptr, 10);
        if (parsed > 0) {
            limit = static_cast<size_t>(parsed);
        }
    }

    const std::lock_guard<std::mutex> lock(state.mutex);
    uapp_unified_results_t*           results = nullptr;
    const uapp_status_t               status =
        uapp_search_index_query(state.search, query.c_str(), limit, &results);
    if (status != UAPP_STATUS_OK) {
        res.status       = http_status_for(status);
        res.content_type = "application/json";
        res.body         = json_error_body(status);
        return;
    }

    size_t count = 0;
    uapp_unified_results_count(results, &count);

    nlohmann::json array = nlohmann::json::array();
    for (size_t i = 0; i < count; ++i) {
        uapp_source_kind_t kind         = UAPP_SOURCE_KIND_NOTE;
        const char*        source_id    = nullptr;
        const char*        container_id = nullptr;
        const char*        title        = nullptr;
        const char*        snippet      = nullptr;
        double              rank         = 0.0;
        uapp_unified_results_get_kind(results, i, &kind);
        uapp_unified_results_get_source_id_hex(results, i, &source_id);
        uapp_unified_results_get_container_id_hex(results, i, &container_id);
        uapp_unified_results_get_title(results, i, &title);
        uapp_unified_results_get_snippet(results, i, &snippet);
        uapp_unified_results_get_rank(results, i, &rank);

        nlohmann::json hit;
        hit["kind"]         = uapp_source_kind_name(kind);
        hit["source_id"]    = source_id != nullptr ? source_id : "";
        hit["container_id"] = container_id != nullptr ? container_id : "";
        hit["title"]        = title != nullptr ? title : "";
        hit["snippet"]      = snippet != nullptr ? snippet : "";
        hit["rank"]         = rank;
        array.push_back(std::move(hit));
    }
    uapp_unified_results_free(&results);

    res.status       = 200;
    res.content_type = "application/json";
    res.body         = dump_safe(array);
}

}  // namespace

void register_search_routes(transport::HttpServer& server, AppState& state) {
    server.get("/search", [&state](const transport::Request& req, transport::Response& res) {
        handle_query(state, req, res);
    });
}

}  // namespace uapp::api
