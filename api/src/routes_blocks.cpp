#include "routes_blocks.h"

#include <cstring>
#include <mutex>

#include <nlohmann/json.hpp>

#include "json_helpers.h"
#include "status_to_http.h"
#include "uapp/capi/uapp_blocks.h"

namespace uapp::api {

namespace {

void write_error(transport::Response& res, uapp_status_t status) {
    res.status       = http_status_for(status);
    res.content_type = "application/json";
    res.body         = json_error_body(status);
}

// Reverse lookup over uapp_block_type_name() rather than a second, hand-
// written string table that could drift from the ABI's own names.
bool parse_block_type(const std::string& name, uapp_block_type_t& out) {
    for (int32_t i = 0; i <= 18; ++i) {
        const auto candidate = static_cast<uapp_block_type_t>(i);
        if (name == uapp_block_type_name(candidate)) {
            out = candidate;
            return true;
        }
    }
    return false;
}

nlohmann::json block_row_to_json(const uapp_page_t* page, size_t index) {
    const char*        id_hex        = nullptr;
    const char*        parent_id_hex = nullptr;
    uapp_block_type_t   type         = UAPP_BLOCK_TYPE_PARAGRAPH;
    size_t              depth         = 0;
    size_t              child_count   = 0;
    int64_t             created_at_ms  = 0;
    int64_t             modified_at_ms = 0;
    uapp_page_get_block_id_hex(page, index, &id_hex);
    uapp_page_get_block_parent_id_hex(page, index, &parent_id_hex);
    uapp_page_get_block_type(page, index, &type);
    uapp_page_get_block_depth(page, index, &depth);
    uapp_page_get_block_child_count(page, index, &child_count);
    uapp_page_get_block_timestamps(page, index, &created_at_ms, &modified_at_ms);

    uapp_block_content_t content;
    content.struct_size = static_cast<uint32_t>(sizeof content);
    uapp_page_get_block_content(page, index, &content);

    nlohmann::json j;
    j["id"]            = id_hex != nullptr ? id_hex : "";
    j["parent_id"]      = parent_id_hex != nullptr ? parent_id_hex : "";
    j["type"]          = uapp_block_type_name(type);
    j["depth"]         = depth;
    j["child_count"]    = child_count;
    j["text"]          = content.text != nullptr ? content.text : "";
    j["language"]      = content.language != nullptr ? content.language : "";
    j["heading_level"]  = content.heading_level;
    j["checked"]       = content.checked == UAPP_TRUE;
    j["created_at"]     = created_at_ms;
    j["modified_at"]    = modified_at_ms;
    return j;
}

nlohmann::json page_to_json(const uapp_page_t* page) {
    const char* id_hex = nullptr;
    const char* title  = nullptr;
    int64_t     created_at_ms  = 0;
    int64_t     modified_at_ms = 0;
    uapp_page_get_id_hex(page, &id_hex);
    uapp_page_get_title(page, &title);
    uapp_page_get_timestamps(page, &created_at_ms, &modified_at_ms);

    size_t block_count = 0;
    uapp_page_get_block_count(page, &block_count);

    nlohmann::json j;
    j["id"]          = id_hex != nullptr ? id_hex : "";
    j["title"]       = title != nullptr ? title : "";
    j["created_at"]  = created_at_ms;
    j["modified_at"] = modified_at_ms;

    nlohmann::json blocks = nlohmann::json::array();
    for (size_t i = 0; i < block_count; ++i) {
        blocks.push_back(block_row_to_json(page, i));
    }
    j["blocks"] = std::move(blocks);
    return j;
}

void handle_create_page(AppState& state, const transport::Request& req,
                        transport::Response& res) {
    const std::optional<nlohmann::json> parsed = parse_json_object(req.body);
    const std::string title = parsed.has_value() ? get_optional_string(*parsed, "title", "") : "";

    const std::lock_guard<std::mutex> lock(state.mutex);
    uapp_page_t*                      page = nullptr;
    const uapp_status_t status =
        uapp_document_service_create_page(state.documents, title.c_str(), &page);
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        return;
    }

    res.status       = 201;
    res.content_type = "application/json";
    res.body         = dump_safe(page_to_json(page));
    uapp_page_free(&page);
}

void handle_get_page(AppState& state, const transport::Request& req, transport::Response& res) {
    if (req.path_params.empty()) {
        write_error(res, UAPP_STATUS_INVALID_ARGUMENT);
        return;
    }
    const std::lock_guard<std::mutex> lock(state.mutex);
    uapp_page_t*                      page = nullptr;
    const uapp_status_t status =
        uapp_document_service_open_page(state.documents, req.path_params[0].c_str(), &page);
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        return;
    }
    res.status       = 200;
    res.content_type = "application/json";
    res.body         = dump_safe(page_to_json(page));
    uapp_page_free(&page);
}

void handle_rename_page(AppState& state, const transport::Request& req,
                        transport::Response& res) {
    if (req.path_params.empty()) {
        write_error(res, UAPP_STATUS_INVALID_ARGUMENT);
        return;
    }
    std::string title;
    const std::optional<nlohmann::json> parsed = parse_json_object(req.body);
    if (!parsed.has_value() || !get_required_string(*parsed, "title", title)) {
        write_error(res, UAPP_STATUS_INVALID_ARGUMENT);
        return;
    }

    const std::lock_guard<std::mutex> lock(state.mutex);
    const uapp_status_t status = uapp_document_service_rename_page(
        state.documents, req.path_params[0].c_str(), title.c_str());
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        return;
    }
    res.status = 204;
    res.body.clear();
}

void handle_delete_page(AppState& state, const transport::Request& req,
                        transport::Response& res) {
    if (req.path_params.empty()) {
        write_error(res, UAPP_STATUS_INVALID_ARGUMENT);
        return;
    }
    const std::lock_guard<std::mutex> lock(state.mutex);
    const uapp_status_t status =
        uapp_document_service_delete_page(state.documents, req.path_params[0].c_str());
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        return;
    }
    res.status = 204;
    res.body.clear();
}

void handle_add_block(AppState& state, const transport::Request& req,
                      transport::Response& res) {
    if (req.path_params.empty()) {
        write_error(res, UAPP_STATUS_INVALID_ARGUMENT);
        return;
    }
    const std::optional<nlohmann::json> parsed = parse_json_object(req.body);
    std::string                         type_name;
    if (!parsed.has_value() || !get_required_string(*parsed, "type", type_name)) {
        write_error(res, UAPP_STATUS_INVALID_ARGUMENT);
        return;
    }
    uapp_block_type_t type;
    if (!parse_block_type(type_name, type)) {
        write_error(res, UAPP_STATUS_BLOCK_TYPE_UNSUPPORTED);
        return;
    }

    uapp_block_content_t content;
    uapp_block_content_init(type, &content);
    const std::string text = get_optional_string(*parsed, "text", "");
    content.text           = text.c_str();
    const std::string language = get_optional_string(*parsed, "language", "");
    if (!language.empty()) {
        content.language = language.c_str();
    }
    if (parsed->contains("heading_level") && (*parsed)["heading_level"].is_number_integer()) {
        content.heading_level = (*parsed)["heading_level"].get<int32_t>();
    }
    if (parsed->contains("checked") && (*parsed)["checked"].is_boolean()) {
        content.checked = (*parsed)["checked"].get<bool>() ? UAPP_TRUE : UAPP_FALSE;
    }

    const std::string parent_id = get_optional_string(*parsed, "parent_id", "");
    size_t             position  = UAPP_BLOCK_POSITION_APPEND;
    if (parsed->contains("position") && (*parsed)["position"].is_number_unsigned()) {
        position = (*parsed)["position"].get<size_t>();
    }

    const std::lock_guard<std::mutex> lock(state.mutex);
    char*                             new_block_id = nullptr;
    const uapp_status_t               status        = uapp_document_service_add_block(
        state.documents, req.path_params[0].c_str(),
        parent_id.empty() ? nullptr : parent_id.c_str(), position, &content, &new_block_id);
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        return;
    }

    nlohmann::json j;
    j["id"] = new_block_id != nullptr ? std::string(new_block_id) : "";
    uapp_free_string(&new_block_id);

    res.status       = 201;
    res.content_type = "application/json";
    res.body         = dump_safe(j);
}

void handle_update_block(AppState& state, const transport::Request& req,
                         transport::Response& res) {
    // path_params[0] = page id (unused by the capi call, kept for REST
    // nesting), path_params[1] = block id.
    if (req.path_params.size() < 2) {
        write_error(res, UAPP_STATUS_INVALID_ARGUMENT);
        return;
    }
    const std::optional<nlohmann::json> parsed = parse_json_object(req.body);
    std::string                         type_name;
    if (!parsed.has_value() || !get_required_string(*parsed, "type", type_name)) {
        write_error(res, UAPP_STATUS_INVALID_ARGUMENT);
        return;
    }
    uapp_block_type_t type;
    if (!parse_block_type(type_name, type)) {
        write_error(res, UAPP_STATUS_BLOCK_TYPE_UNSUPPORTED);
        return;
    }

    uapp_block_content_t content;
    uapp_block_content_init(type, &content);
    const std::string text = get_optional_string(*parsed, "text", "");
    content.text           = text.c_str();
    const std::string language = get_optional_string(*parsed, "language", "");
    if (!language.empty()) {
        content.language = language.c_str();
    }
    if (parsed->contains("heading_level") && (*parsed)["heading_level"].is_number_integer()) {
        content.heading_level = (*parsed)["heading_level"].get<int32_t>();
    }
    if (parsed->contains("checked") && (*parsed)["checked"].is_boolean()) {
        content.checked = (*parsed)["checked"].get<bool>() ? UAPP_TRUE : UAPP_FALSE;
    }

    const std::lock_guard<std::mutex> lock(state.mutex);
    const uapp_status_t               status = uapp_document_service_update_block(
        state.documents, req.path_params[1].c_str(), &content);
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        return;
    }
    res.status = 204;
    res.body.clear();
}

void handle_delete_block(AppState& state, const transport::Request& req,
                         transport::Response& res) {
    if (req.path_params.size() < 2) {
        write_error(res, UAPP_STATUS_INVALID_ARGUMENT);
        return;
    }
    const std::lock_guard<std::mutex> lock(state.mutex);
    const uapp_status_t               status =
        uapp_document_service_delete_block(state.documents, req.path_params[1].c_str());
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        return;
    }
    res.status = 204;
    res.body.clear();
}

}  // namespace

void register_blocks_routes(transport::HttpServer& server, AppState& state) {
    server.post("/documents", [&state](const transport::Request& req, transport::Response& res) {
        handle_create_page(state, req, res);
    });
    server.get(R"(/documents/([0-9a-f]{32}))",
              [&state](const transport::Request& req, transport::Response& res) {
                  handle_get_page(state, req, res);
              });
    server.patch(R"(/documents/([0-9a-f]{32}))",
                [&state](const transport::Request& req, transport::Response& res) {
                    handle_rename_page(state, req, res);
                });
    server.del(R"(/documents/([0-9a-f]{32}))",
              [&state](const transport::Request& req, transport::Response& res) {
                  handle_delete_page(state, req, res);
              });
    server.post(R"(/documents/([0-9a-f]{32})/blocks)",
               [&state](const transport::Request& req, transport::Response& res) {
                   handle_add_block(state, req, res);
               });
    server.patch(R"(/documents/([0-9a-f]{32})/blocks/([0-9a-f]{32}))",
                [&state](const transport::Request& req, transport::Response& res) {
                    handle_update_block(state, req, res);
                });
    server.del(R"(/documents/([0-9a-f]{32})/blocks/([0-9a-f]{32}))",
              [&state](const transport::Request& req, transport::Response& res) {
                  handle_delete_block(state, req, res);
              });
}

}  // namespace uapp::api
