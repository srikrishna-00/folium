#include "http_server.h"

// The one file in this project allowed to include httplib.h — see this
// target's own CMakeLists.txt entry and cmake/UappCompilerOptions.cmake's
// uapp::compiler_options_exceptions_ok for why. Compiled WITH exceptions
// enabled; every function below still guarantees no exception escapes its
// own boundary, by catching everything itself.
#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <exception>

namespace uapp::api::transport {

std::string Request::header(const std::string& name) const noexcept {
    for (const auto& [key, value] : headers) {
        if (key.size() == name.size() &&
            std::equal(key.begin(), key.end(), name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            })) {
            return value;
        }
    }
    return {};
}

std::string Request::query_param(const std::string& name) const noexcept {
    for (const auto& [key, value] : query) {
        if (key == name) { return value; }
    }
    return {};
}

namespace {

Request to_request(const httplib::Request& req) {
    Request out;
    out.method = req.method;
    out.path   = req.path;
    out.body   = req.body;
    out.query.reserve(req.params.size());
    for (const auto& [key, value] : req.params) {
        out.query.emplace_back(key, value);
    }
    out.headers.reserve(req.headers.size());
    for (const auto& [key, value] : req.headers) {
        out.headers.emplace_back(key, value);
    }
    if (req.is_multipart_form_data()) {
        out.files.reserve(req.form.files.size());
        for (const auto& [field_name, file] : req.form.files) {
            out.files.push_back(UploadedFile{field_name, file.filename, file.content});
        }
    }
    // req.matches[0] is the whole match; capture groups start at [1].
    if (req.matches.size() > 1) {
        out.path_params.reserve(req.matches.size() - 1);
        for (std::size_t i = 1; i < req.matches.size(); ++i) {
            out.path_params.emplace_back(req.matches[i].str());
        }
    }
    return out;
}

// Runs `handler`, converting ANY exception httplib's own internals might
// raise while we're inside this call into a plain 500 response rather than
// letting it unwind into the rest of the process. httplib's Server already
// wraps dispatch in its own try/catch (see
// uapp::compiler_options_exceptions_ok's comment for the exact line), so
// this is defense in depth, not the only guard — but it is what makes the
// "no exception crosses this file's boundary" guarantee true regardless of
// what httplib does internally on a given release.
void dispatch(const Handler& handler, const httplib::Request& req, httplib::Response& res) {
    Response out;
    try {
        handler(to_request(req), out);
    } catch (const std::exception&) {
        out.status       = 500;
        out.content_type = "application/json";
        out.body          = R"({"error":{"code":"INTERNAL","message":"unhandled exception in route handler"}})";
    } catch (...) {
        out.status       = 500;
        out.content_type = "application/json";
        out.body          = R"({"error":{"code":"INTERNAL","message":"unhandled non-exception throw in route handler"}})";
    }
    res.status = out.status;
    res.set_content(out.body, out.content_type.c_str());
}

}  // namespace

struct HttpServer::Impl {
    httplib::Server server;
};

HttpServer::HttpServer() : impl_(std::make_unique<Impl>()) {}
HttpServer::~HttpServer() = default;

void HttpServer::get(const std::string& path, Handler handler) {
    impl_->server.Get(path, [h = std::move(handler)](const httplib::Request& req,
                                                       httplib::Response& res) {
        dispatch(h, req, res);
    });
}

void HttpServer::post(const std::string& path, Handler handler) {
    impl_->server.Post(path, [h = std::move(handler)](const httplib::Request& req,
                                                        httplib::Response& res) {
        dispatch(h, req, res);
    });
}

void HttpServer::put(const std::string& path, Handler handler) {
    impl_->server.Put(path, [h = std::move(handler)](const httplib::Request& req,
                                                       httplib::Response& res) {
        dispatch(h, req, res);
    });
}

void HttpServer::patch(const std::string& path, Handler handler) {
    impl_->server.Patch(path, [h = std::move(handler)](const httplib::Request& req,
                                                         httplib::Response& res) {
        dispatch(h, req, res);
    });
}

void HttpServer::del(const std::string& path, Handler handler) {
    impl_->server.Delete(path, [h = std::move(handler)](const httplib::Request& req,
                                                          httplib::Response& res) {
        dispatch(h, req, res);
    });
}

void HttpServer::set_pre_route_hook(PreRouteHook hook) {
    impl_->server.set_pre_routing_handler(
        [h = std::move(hook)](const httplib::Request& req,
                              httplib::Response& res) -> httplib::Server::HandlerResponse {
            Response out;
            bool     continue_routing = true;
            try {
                continue_routing = h(to_request(req), out);
            } catch (...) {
                out.status       = 500;
                out.content_type = "application/json";
                out.body = R"({"error":{"code":"INTERNAL","message":"unhandled exception in pre-route hook"}})";
                continue_routing = false;
            }
            if (continue_routing) {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            res.status = out.status;
            res.set_content(out.body, out.content_type.c_str());
            return httplib::Server::HandlerResponse::Handled;
        });
}

bool HttpServer::listen(const std::string& host, std::uint16_t port) {
    try {
        return impl_->server.listen(host, static_cast<int>(port));
    } catch (...) {
        return false;
    }
}

void HttpServer::stop() {
    impl_->server.stop();
}

}  // namespace uapp::api::transport
