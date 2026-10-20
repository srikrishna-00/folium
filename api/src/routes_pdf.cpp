#include "routes_pdf.h"

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "json_helpers.h"
#include "status_to_http.h"
#include "uapp/capi/uapp_pdf.h"

namespace uapp::api {

namespace {

void write_error(transport::Response& res, uapp_status_t status) {
    res.status       = http_status_for(status);
    res.content_type = "application/json";
    res.body         = json_error_body(status);
}

// A locally-unique id for the in-memory document registry — not a
// document_id in the product's own id space (uapp_pdf.h's handle has no
// id of its own), so this does not need to be cryptographically random,
// only collision-resistant for one server process's lifetime.
std::string generate_pdf_id() {
    std::random_device rd;
    std::ostringstream oss;
    for (int i = 0; i < 16; ++i) {
        const unsigned byte = rd() & 0xFFu;
        oss << std::hex << (byte < 16 ? "0" : "") << byte;
    }
    return oss.str();
}

void handle_upload(AppState& state, const transport::Request& req, transport::Response& res) {
    if (req.body.empty()) {
        write_error(res, UAPP_STATUS_FILE_EMPTY);
        return;
    }

    const std::lock_guard<std::mutex> lock(state.mutex);

    PdfDocumentEntry entry;
    entry.buffer.assign(req.body.begin(), req.body.end());

    const uapp_status_t status =
        uapp_pdf_document_open(entry.buffer.data(), entry.buffer.size(), nullptr, &entry.doc);
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        return;
    }

    const std::string id    = generate_pdf_id();
    const int32_t      pages = uapp_pdf_page_count(entry.doc);
    state.pdf_documents.emplace(id, std::move(entry));

    nlohmann::json j;
    j["id"]         = id;
    j["page_count"] = pages;

    res.status       = 201;
    res.content_type = "application/json";
    res.body         = dump_safe(j);
}

// Shared lookup for the two /pdf/:id/page/:n/... routes below. Returns
// nullptr (and writes an error into `res`) if the id or page index is
// invalid, so callers can just check for null and return.
uapp_pdf_document_t* find_document_or_error(AppState& state, const transport::Request& req,
                                            transport::Response& res) {
    if (req.path_params.empty()) {
        write_error(res, UAPP_STATUS_INVALID_ARGUMENT);
        return nullptr;
    }
    const auto it = state.pdf_documents.find(req.path_params[0]);
    if (it == state.pdf_documents.end()) {
        write_error(res, UAPP_STATUS_PDF_NOT_OPEN);
        return nullptr;
    }
    return it->second.doc;
}

void handle_page_size(AppState& state, const transport::Request& req,
                      transport::Response& res) {
    const std::lock_guard<std::mutex> lock(state.mutex);
    uapp_pdf_document_t*              doc = find_document_or_error(state, req, res);
    if (doc == nullptr) {
        return;
    }
    if (req.path_params.size() < 2) {
        write_error(res, UAPP_STATUS_INVALID_ARGUMENT);
        return;
    }
    const long page_index = std::strtol(req.path_params[1].c_str(), nullptr, 10);

    uapp_pdf_page_size_t size;
    size.struct_size = static_cast<uint32_t>(sizeof size);
    const uapp_status_t status =
        uapp_pdf_page_size(doc, static_cast<int32_t>(page_index), &size);
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        return;
    }

    nlohmann::json j;
    j["width_points"]  = size.width_points;
    j["height_points"] = size.height_points;

    res.status       = 200;
    res.content_type = "application/json";
    res.body         = dump_safe(j);
}

void handle_render(AppState& state, const transport::Request& req, transport::Response& res) {
    const std::lock_guard<std::mutex> lock(state.mutex);
    uapp_pdf_document_t*              doc = find_document_or_error(state, req, res);
    if (doc == nullptr) {
        return;
    }
    if (req.path_params.size() < 2) {
        write_error(res, UAPP_STATUS_INVALID_ARGUMENT);
        return;
    }
    const long page_index = std::strtol(req.path_params[1].c_str(), nullptr, 10);

    const std::string width_param  = req.query_param("width");
    const std::string height_param = req.query_param("height");
    if (width_param.empty() || height_param.empty()) {
        write_error(res, UAPP_STATUS_PDF_RENDER_SIZE_INVALID);
        return;
    }
    const long width  = std::strtol(width_param.c_str(), nullptr, 10);
    const long height = std::strtol(height_param.c_str(), nullptr, 10);

    const size_t required =
        uapp_pdf_required_buffer_bytes(static_cast<int32_t>(width), static_cast<int32_t>(height), 0);
    if (required == 0) {
        write_error(res, UAPP_STATUS_PDF_RENDER_SIZE_INVALID);
        return;
    }

    std::vector<uint8_t> pixels(required);
    const uapp_status_t  status =
        uapp_pdf_render_page(doc, static_cast<int32_t>(page_index), pixels.data(), pixels.size(),
                             static_cast<int32_t>(width), static_cast<int32_t>(height), 0);
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        return;
    }

    res.status       = 200;
    res.content_type = "application/octet-stream";
    res.body.assign(reinterpret_cast<const char*>(pixels.data()), pixels.size());
}

}  // namespace

void register_pdf_routes(transport::HttpServer& server, AppState& state) {
    server.post("/pdf", [&state](const transport::Request& req, transport::Response& res) {
        handle_upload(state, req, res);
    });
    server.get(R"(/pdf/([0-9a-f]+)/page/(\d+)/size)",
              [&state](const transport::Request& req, transport::Response& res) {
                  handle_page_size(state, req, res);
              });
    server.post(R"(/pdf/([0-9a-f]+)/page/(\d+)/render)",
               [&state](const transport::Request& req, transport::Response& res) {
                   handle_render(state, req, res);
               });
}

}  // namespace uapp::api
