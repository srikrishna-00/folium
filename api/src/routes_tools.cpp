#include "routes_tools.h"

#include <vector>

#include <nlohmann/json.hpp>

#include "json_helpers.h"
#include "uapp/pdf/writer.h"
#include "uapp/status.h"

namespace uapp::api {

namespace {

// A SEPARATE mapping from status_to_http.h's, deliberately: that file maps
// uapp_status_t, the C ABI enum this route never touches (pdf-engine/writer
// has no C ABI yet — see core/capi/src/capi_common.cpp's own note on why —
// so this route links uapp::pdf_writer directly as a C++ library, same as
// api/ already does with uapp::capi). This maps uapp::Status, the C++ enum,
// over the small subset write_images_as_pdf()/decode_image_to_rgb() can
// actually produce.
int http_status_for_pdf_writer_status(Status status) noexcept {
    switch (status) {
        case Status::InvalidArgument:
        case Status::PdfWriterRotationInvalid:
        case Status::PdfWriterImageMalformed:
            return 400;
        case Status::PdfWriterPageCountExceeded:
        case Status::PdfWriterDimensionTooLarge:
        case Status::PdfWriterTotalPixelsTooLarge:
            return 413;  // Payload Too Large
        default:
            return 500;
    }
}

std::string json_error_body_for(Status status) {
    nlohmann::json body;
    body["error"]["code"]    = status_code(status);
    body["error"]["message"] = status_message(status);
    return body.dump();
}

void handle_images_to_pdf(const transport::Request& req, transport::Response& res) {
    if (req.files.empty()) {
        res.status       = 400;
        res.content_type = "application/json";
        res.body         = R"({"error":{"code":"INVALID_ARGUMENT","message":"upload at least one image file"}})";
        return;
    }

    // Decode every uploaded file to RGB first — the vectors of decoded
    // pixels must all stay alive until write_images_as_pdf() runs, since
    // PageImage only BORROWS its pixel buffer (same rule
    // uapp_pdf_render_page's caller-owned buffer follows).
    std::vector<std::vector<std::uint8_t>> decoded_pages;
    std::vector<pdf::PageImage>             pages;
    decoded_pages.reserve(req.files.size());
    pages.reserve(req.files.size());

    for (const transport::UploadedFile& file : req.files) {
        std::vector<std::uint8_t> pixels;
        std::int32_t               width  = 0;
        std::int32_t               height = 0;
        const auto*                bytes  = reinterpret_cast<const std::uint8_t*>(file.content.data());
        const Status                status = pdf::decode_image_to_rgb(bytes, file.content.size(),
                                                                      pixels, width, height);
        if (status != Status::Ok) {
            res.status       = http_status_for_pdf_writer_status(status);
            res.content_type = "application/json";
            res.body         = json_error_body_for(status);
            return;
        }
        decoded_pages.push_back(std::move(pixels));

        pdf::PageImage page;
        page.rgb_pixels = decoded_pages.back().data();
        page.width      = width;
        page.height     = height;
        pages.push_back(page);
    }

    std::vector<std::uint8_t> pdf_bytes;
    const Status               status = pdf::write_images_as_pdf(pages, pdf::WriteOptions{}, pdf_bytes);
    if (status != Status::Ok) {
        res.status       = http_status_for_pdf_writer_status(status);
        res.content_type = "application/json";
        res.body         = json_error_body_for(status);
        return;
    }

    res.status       = 200;
    res.content_type = "application/pdf";
    res.body.assign(reinterpret_cast<const char*>(pdf_bytes.data()), pdf_bytes.size());
}

}  // namespace

void register_tools_routes(transport::HttpServer& server) {
    server.post("/tools/images-to-pdf",
               [](const transport::Request& req, transport::Response& res) {
                   handle_images_to_pdf(req, res);
               });
}

}  // namespace uapp::api
