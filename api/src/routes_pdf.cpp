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
#include "uapp/pdf/writer.h"
#include "uapp/status.h"

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

// Renders page `index` of `doc` at `dpi` and appends the result, as RGB
// (alpha stripped — a merged page has no meaningful transparency, same
// reasoning uapp::pdf::PageImage's own comment gives), to `decoded_pages`.
// Returns false (and writes an error into `res`) on any failure.
//
// `decoded_pages` is a vector of vectors, not a single buffer, because
// PageImage only BORROWS its pixel pointer (uapp::pdf::writer.h's own
// contract) — every page's pixels must stay alive, at a stable address,
// until write_images_as_pdf() runs at the very end, which is exactly what
// owning each page's buffer in a vector that is never reallocated after
// this function returns guarantees.
bool render_page_as_rgb(uapp_pdf_document_t* doc, int32_t index, double dpi,
                        std::vector<std::vector<uint8_t>>& decoded_pages,
                        std::vector<pdf::PageImage>& pages, transport::Response& res) {
    uapp_pdf_page_size_t size;
    size.struct_size = static_cast<uint32_t>(sizeof size);
    uapp_status_t status = uapp_pdf_page_size(doc, index, &size);
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        return false;
    }

    const auto width  = static_cast<int32_t>(size.width_points / 72.0 * dpi);
    const auto height = static_cast<int32_t>(size.height_points / 72.0 * dpi);
    const size_t required = uapp_pdf_required_buffer_bytes(width, height, 0);
    if (required == 0) {
        write_error(res, UAPP_STATUS_PDF_RENDER_SIZE_INVALID);
        return false;
    }

    std::vector<uint8_t> rgba(required);
    status = uapp_pdf_render_page(doc, index, rgba.data(), rgba.size(), width, height, 0);
    if (status != UAPP_STATUS_OK) {
        write_error(res, status);
        return false;
    }

    // RGBA8888 -> RGB: drop every 4th byte. required == width*height*4
    // exactly (stride 0 means "tightly packed" — uapp_pdf_required_buffer_bytes's
    // own contract), so this straight division is safe.
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
    for (size_t px = 0, n = rgb.size() / 3; px < n; ++px) {
        rgb[px * 3 + 0] = rgba[px * 4 + 0];
        rgb[px * 3 + 1] = rgba[px * 4 + 1];
        rgb[px * 3 + 2] = rgba[px * 4 + 2];
    }

    decoded_pages.push_back(std::move(rgb));
    pdf::PageImage page;
    page.rgb_pixels = decoded_pages.back().data();
    page.width      = width;
    page.height     = height;
    pages.push_back(page);
    return true;
}

void handle_merge(const transport::Request& req, transport::Response& res) {
    if (req.files.size() < 2) {
        res.status       = 400;
        res.content_type = "application/json";
        res.body         = R"({"error":{"code":"INVALID_ARGUMENT","message":"upload at least two PDF files to merge"}})";
        return;
    }

    constexpr double kRenderDpi = 150.0;

    std::vector<std::vector<uint8_t>> decoded_pages;
    std::vector<pdf::PageImage>        pages;
    std::vector<uapp_pdf_document_t*>  opened_docs;

    bool ok = true;
    for (const transport::UploadedFile& file : req.files) {
        uapp_pdf_document_t* doc    = nullptr;
        const uapp_status_t  status = uapp_pdf_document_open(file.content.data(), file.content.size(),
                                                             nullptr, &doc);
        if (status != UAPP_STATUS_OK) {
            write_error(res, status);
            ok = false;
            break;
        }
        opened_docs.push_back(doc);

        const int32_t page_count = uapp_pdf_page_count(doc);
        for (int32_t i = 0; i < page_count; ++i) {
            if (!render_page_as_rgb(doc, i, kRenderDpi, decoded_pages, pages, res)) {
                ok = false;
                break;
            }
        }
        if (!ok) { break; }
    }

    for (uapp_pdf_document_t* doc : opened_docs) {
        uapp_pdf_document_t* handle = doc;
        uapp_pdf_document_close(&handle);
    }
    if (!ok) { return; }

    std::vector<uint8_t> out;
    const Status          status = pdf::write_images_as_pdf(pages, pdf::WriteOptions{}, out);
    if (status != Status::Ok) {
        res.status       = (status == Status::InvalidArgument) ? 400 : 500;
        res.content_type = "application/json";
        nlohmann::json body;
        body["error"]["code"]    = status_code(status);
        body["error"]["message"] = status_message(status);
        res.body                 = body.dump();
        return;
    }

    res.status       = 200;
    res.content_type = "application/pdf";
    res.body.assign(reinterpret_cast<const char*>(out.data()), out.size());
}

}  // namespace

void register_pdf_routes(transport::HttpServer& server, AppState& state) {
    server.post("/tools/merge-pdf", [](const transport::Request& req, transport::Response& res) {
        handle_merge(req, res);
    });
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
