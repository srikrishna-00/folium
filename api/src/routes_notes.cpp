#include "routes_notes.h"

#include <mutex>

#include <nlohmann/json.hpp>

#include "json_helpers.h"
#include "status_to_http.h"
#include "uapp/capi/uapp_notes.h"

namespace uapp::api {

namespace {

nlohmann::json note_to_json(const uapp_note_t* note) {
    const char* id_hex = nullptr;
    const char* title  = nullptr;
    const char* body   = nullptr;
    int64_t     created_at_ms  = 0;
    int64_t     modified_at_ms = 0;
    uapp_note_get_id_hex(note, &id_hex);
    uapp_note_get_title(note, &title);
    uapp_note_get_body(note, &body);
    uapp_note_get_timestamps(note, &created_at_ms, &modified_at_ms);

    nlohmann::json j;
    j["id"]         = id_hex != nullptr ? id_hex : "";
    j["title"]      = title != nullptr ? title : "";
    j["body"]       = body != nullptr ? body : "";
    j["created_at"] = created_at_ms;
    j["modified_at"] = modified_at_ms;
    return j;
}

void write_error(transport::Response& res, uapp_status_t status) {
    res.status       = http_status_for(status);
    res.content_type = "application/json";
    res.body         = json_error_body(status);
}

void handle_create(AppState& state, const transport::Request& req, transport::Response& res) {
    const std::optional<nlohmann::json> parsed = parse_json_object(req.body);
    std::string                         title;
    std::string                         body_text;
    if (!parsed.has_value() || !get_required_string(*parsed, "title", title) ||
        !get_required_string(*parsed, "body", body_text)) {
        write_error(res, UAPP_STATUS_INVALID_ARGUMENT);
        return;
    }

    const std::lock_guard<std::mutex> lock(state.mutex);
    uapp_note_t*                      note   = nullptr;
    const uapp_status_t status = uapp_note_service_create(state.notes, title.c_str(),
                                                           body_text.c_str(), &note);
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        return;
    }

    res.status       = 201;
    res.content_type = "application/json";
    res.body         = dump_safe(note_to_json(note));
    uapp_note_free(&note);
}

void handle_get(AppState& state, const transport::Request& req, transport::Response& res) {
    if (req.path_params.empty()) {
        write_error(res, UAPP_STATUS_INVALID_ARGUMENT);
        return;
    }
    const std::lock_guard<std::mutex> lock(state.mutex);
    uapp_note_t*                      note = nullptr;
    const uapp_status_t status = uapp_note_service_open_note(
        state.notes, req.path_params[0].c_str(), &note);
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        return;
    }

    res.status       = 200;
    res.content_type = "application/json";
    res.body         = dump_safe(note_to_json(note));
    uapp_note_free(&note);
}

void handle_update(AppState& state, const transport::Request& req, transport::Response& res) {
    if (req.path_params.empty()) {
        write_error(res, UAPP_STATUS_INVALID_ARGUMENT);
        return;
    }
    const std::optional<nlohmann::json> parsed = parse_json_object(req.body);
    if (!parsed.has_value()) {
        write_error(res, UAPP_STATUS_INVALID_ARGUMENT);
        return;
    }

    const std::lock_guard<std::mutex> lock(state.mutex);
    uapp_note_t*                      note = nullptr;
    uapp_status_t status = uapp_note_service_open_note(
        state.notes, req.path_params[0].c_str(), &note);
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        return;
    }

    std::string title;
    if (get_required_string(*parsed, "title", title)) {
        status = uapp_note_set_title(note, title.c_str());
        if (status != UAPP_STATUS_OK) {
            write_error(res, status);
            uapp_note_free(&note);
            return;
        }
    }
    std::string body_text;
    if (get_required_string(*parsed, "body", body_text)) {
        status = uapp_note_set_body(note, body_text.c_str());
        if (status != UAPP_STATUS_OK) {
            write_error(res, status);
            uapp_note_free(&note);
            return;
        }
    }

    status = uapp_note_service_save(state.notes, note);
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        uapp_note_free(&note);
        return;
    }

    res.status       = 200;
    res.content_type = "application/json";
    res.body         = dump_safe(note_to_json(note));
    uapp_note_free(&note);
}

void handle_delete(AppState& state, const transport::Request& req, transport::Response& res) {
    if (req.path_params.empty()) {
        write_error(res, UAPP_STATUS_INVALID_ARGUMENT);
        return;
    }
    const std::lock_guard<std::mutex> lock(state.mutex);
    const uapp_status_t status =
        uapp_note_service_delete(state.notes, req.path_params[0].c_str());
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        return;
    }
    res.status = 204;
    res.body.clear();
}

// GET /notes?q=... — there is no "list every note" call in uapp_notes.h
// (notes are opened by id or found by query), so this route requires a
// query rather than faking a list-all that the ABI does not offer.
void handle_search(AppState& state, const transport::Request& req, transport::Response& res) {
    const std::string query = req.query_param("q");
    if (query.empty()) {
        write_error(res, UAPP_STATUS_SEARCH_QUERY_EMPTY);
        return;
    }

    const std::lock_guard<std::mutex> lock(state.mutex);
    uapp_search_results_t*            results = nullptr;
    const uapp_status_t               status =
        uapp_note_service_search(state.notes, query.c_str(), 0, &results);
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        return;
    }

    size_t count = 0;
    uapp_search_results_count(results, &count);

    nlohmann::json array = nlohmann::json::array();
    for (size_t i = 0; i < count; ++i) {
        const char* id_hex  = nullptr;
        const char* title   = nullptr;
        const char* snippet = nullptr;
        double      rank    = 0.0;
        uapp_search_results_get_id_hex(results, i, &id_hex);
        uapp_search_results_get_title(results, i, &title);
        uapp_search_results_get_snippet(results, i, &snippet);
        uapp_search_results_get_rank(results, i, &rank);

        nlohmann::json hit;
        hit["id"]      = id_hex != nullptr ? id_hex : "";
        hit["title"]   = title != nullptr ? title : "";
        hit["snippet"] = snippet != nullptr ? snippet : "";
        hit["rank"]    = rank;
        array.push_back(std::move(hit));
    }
    uapp_search_results_free(&results);

    res.status       = 200;
    res.content_type = "application/json";
    res.body         = dump_safe(array);
}

}  // namespace

void register_notes_routes(transport::HttpServer& server, AppState& state) {
    server.post("/notes", [&state](const transport::Request& req, transport::Response& res) {
        handle_create(state, req, res);
    });
    server.get("/notes", [&state](const transport::Request& req, transport::Response& res) {
        handle_search(state, req, res);
    });
    server.get(R"(/notes/([0-9a-f]{32}))",
              [&state](const transport::Request& req, transport::Response& res) {
                  handle_get(state, req, res);
              });
    server.put(R"(/notes/([0-9a-f]{32}))",
              [&state](const transport::Request& req, transport::Response& res) {
                  handle_update(state, req, res);
              });
    server.del(R"(/notes/([0-9a-f]{32}))",
              [&state](const transport::Request& req, transport::Response& res) {
                  handle_delete(state, req, res);
              });
}

}  // namespace uapp::api
