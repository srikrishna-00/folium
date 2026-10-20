#include "json_helpers.h"

namespace uapp::api {

std::optional<nlohmann::json> parse_json_object(const std::string& body) {
    nlohmann::json parsed =
        nlohmann::json::parse(body, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::nullopt;
    }
    return parsed;
}

bool get_required_string(const nlohmann::json& j, const char* key, std::string& out) {
    if (!j.contains(key) || !j[key].is_string()) {
        return false;
    }
    out = j[key].get<std::string>();
    return true;
}

std::string get_optional_string(const nlohmann::json& j, const char* key,
                                const std::string& fallback) {
    if (!j.contains(key) || !j[key].is_string()) {
        return fallback;
    }
    return j[key].get<std::string>();
}

std::string dump_safe(const nlohmann::json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace uapp::api
