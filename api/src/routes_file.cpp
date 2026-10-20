#include "routes_file.h"

#include <cstdio>

#include <nlohmann/json.hpp>

#include "json_helpers.h"
#include "status_to_http.h"
#include "uapp/capi/uapp_file.h"

namespace uapp::api {

namespace {

std::string hex_encode(const uint8_t* bytes, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string        out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(bytes[i] >> 4) & 0x0Fu]);
        out.push_back(kHex[bytes[i] & 0x0Fu]);
    }
    return out;
}

void handle_detect(const transport::Request& req, transport::Response& res) {
    const std::string file_name_hint = req.query_param("name");

    uapp_file_info_t info;
    info.struct_size = static_cast<uint32_t>(sizeof info);
    const uapp_status_t status = uapp_file_describe_buffer(
        req.body.empty() ? nullptr : req.body.data(), req.body.size(),
        file_name_hint.empty() ? nullptr : file_name_hint.c_str(), 0, 0, &info);
    if (status != UAPP_STATUS_OK) {
        res.status       = http_status_for(status);
        res.content_type = "application/json";
        res.body         = json_error_body(status);
        return;
    }

    nlohmann::json j;
    j["format"]                     = uapp_file_format_name(info.format);
    j["mime_type"]                  = info.mime_type;
    j["category"]                   = uapp_format_category_name(info.category);
    j["confidence"]                 = uapp_detection_confidence_name(info.confidence);
    j["size_bytes"]                 = info.size_bytes;
    j["extension_present"]          = info.extension_present == UAPP_TRUE;
    j["extension_matches_content"]  = info.extension_matches_content == UAPP_TRUE;
    j["extension_conflicts"]        = info.extension_conflicts == UAPP_TRUE;
    if (info.checksum_length > 0) {
        j["checksum_algorithm"] = uapp_checksum_algorithm_name(info.checksum_algorithm);
        j["checksum_hex"] = hex_encode(info.checksum_bytes, info.checksum_length);
    }

    res.status       = 200;
    res.content_type = "application/json";
    res.body         = dump_safe(j);
}

}  // namespace

void register_file_routes(transport::HttpServer& server) {
    server.post("/files/detect",
               [](const transport::Request& req, transport::Response& res) {
                   handle_detect(req, res);
               });
}

}  // namespace uapp::api
