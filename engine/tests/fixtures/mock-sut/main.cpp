// mock-sut — minimal HTTP server for engine integration tests.
//
// Reads a routes JSON file on startup. Each route specifies a method, path
// prefix, status, optional body, optional response headers, and optional
// scripted dynamic behavior (e.g. "fail-once-then-succeed").
//
// Usage:
//   mock-sut --routes <file.json> [--port 0]
//
// `--port 0` lets the OS pick a free port. The actual port is printed to
// stdout on a single line ("PORT: 53921") so the test harness can capture it.
//
// Routes JSON format:
//   {
//     "routes": [
//       { "method": "POST",  "path": "/api/v1/auth/admin/login",
//         "status": 200,
//         "body": { "data": { "accessToken": "admin-tok",
//                              "user": { "id": "adm-1" } } } },
//       { "method": "POST",  "path": "/api/v1/orders",
//         "status": 201,
//         "body": { "data": { "id": "ord-stub" } } }
//     ]
//   }

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <print>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Route {
    std::string method;
    std::string path;
    int status{200};
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    /// Optional response sequence for polling integration tests. When
    /// non-empty, the route returns sequence[0] on the first call,
    /// sequence[1] on the second, and so on. After exhaustion, it sticks
    /// on the last element.
    struct Step {
        int status{200};
        std::string body;
        std::vector<std::pair<std::string, std::string>> headers;
    };
    std::vector<Step> sequence;
};

/// Snapshot of the most recent request received on a captured route.
/// Tests poll `/__mock/last-request?path=/some/path` to assert on the
/// Content-Type, raw body, and parsed multipart parts.
struct CapturedRequest {
    std::string method;
    std::string contentType;
    std::string rawBody;
    /// All non-built-in request headers as a JSON object, lower-cased
    /// keys to defang case-sensitivity assertions in the test.
    nlohmann::json headers = nlohmann::json::object();
    /// One entry per multipart part. `filename` is empty for text fields.
    nlohmann::json parts = nlohmann::json::array();
};

struct CaptureStore {
    std::mutex mtx;
    std::map<std::string, CapturedRequest> byPath;
};

[[nodiscard]] std::vector<Route> loadRoutes(const fs::path& file) {
    std::ifstream in(file);
    if (!in) {
        std::println(stderr, "mock-sut: cannot open routes file {}", file.string());
        std::exit(2);
    }
    std::stringstream ss;
    ss << in.rdbuf();

    json doc;
    try {
        doc = json::parse(ss.str());
    } catch (const json::parse_error& e) {
        std::println(stderr, "mock-sut: invalid routes JSON: {}", e.what());
        std::exit(2);
    }

    std::vector<Route> routes;
    for (const auto& r : doc["routes"]) {
        Route route;
        route.method = r.value("method", "GET");
        route.path = r.value("path", "/");
        route.status = r.value("status", 200);
        if (r.contains("body")) {
            route.body = r["body"].is_string() ? r["body"].get<std::string>() : r["body"].dump();
        }
        if (r.contains("headers")) {
            // Two accepted shapes:
            //   - Object {"Name": "value"} — one header per name (default).
            //   - Array of [name, value] pairs — duplicate names allowed,
            //     used by tests that need the server to emit multiple
            //     Set-Cookie headers in one response.
            const auto& h = r["headers"];
            if (h.is_array()) {
                for (const auto& pair : h) {
                    if (pair.is_array() && pair.size() == 2) {
                        route.headers.emplace_back(pair[0].get<std::string>(),
                                                   pair[1].get<std::string>());
                    }
                }
            } else if (h.is_object()) {
                for (auto it = h.begin(); it != h.end(); ++it) {
                    route.headers.emplace_back(it.key(), it.value().get<std::string>());
                }
            }
        }
        if (r.contains("sequence") && r["sequence"].is_array()) {
            for (const auto& s : r["sequence"]) {
                Route::Step step;
                step.status = s.value("status", route.status);
                if (s.contains("body")) {
                    step.body =
                        s["body"].is_string() ? s["body"].get<std::string>() : s["body"].dump();
                }
                if (s.contains("headers")) {
                    const auto& sh = s["headers"];
                    if (sh.is_array()) {
                        for (const auto& pair : sh) {
                            if (pair.is_array() && pair.size() == 2) {
                                step.headers.emplace_back(pair[0].get<std::string>(),
                                                          pair[1].get<std::string>());
                            }
                        }
                    } else if (sh.is_object()) {
                        for (auto it = sh.begin(); it != sh.end(); ++it) {
                            step.headers.emplace_back(it.key(), it.value().get<std::string>());
                        }
                    }
                }
                route.sequence.push_back(std::move(step));
            }
        }
        routes.push_back(std::move(route));
    }
    return routes;
}

void registerRoute(httplib::Server& server, const Route& route, CaptureStore& captures) {
    // Per-route call counter, shared across all copies of the handler lambda.
    auto callCount = std::make_shared<std::atomic<std::size_t>>(0);

    auto handler = [route, callCount, &captures](const httplib::Request& req,
                                                 httplib::Response& res) {
        // Capture every request so the integration tests can assert on
        // Content-Type, raw body, and multipart structure. The store is
        // keyed by path so a test can inspect the request to a specific
        // endpoint without worrying about other traffic.
        {
            CapturedRequest cap;
            cap.method = req.method;
            auto ctIt = req.headers.find("Content-Type");
            if (ctIt != req.headers.end()) cap.contentType = ctIt->second;
            cap.rawBody = req.body;
            for (const auto& [k, v] : req.headers) {
                std::string key = k;
                for (auto& ch : key)
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                cap.headers[key] = v;
            }
            for (const auto& [name, fd] : req.form.fields) {
                nlohmann::json part;
                part["name"] = name;
                part["filename"] = "";
                part["content_type"] = "";  // text fields in httplib don't carry a type
                part["bytes"] = fd.content.size();
                part["content_text"] = fd.content;
                cap.parts.push_back(std::move(part));
            }
            for (const auto& [name, fd] : req.form.files) {
                nlohmann::json part;
                part["name"] = name;
                part["filename"] = fd.filename;
                part["content_type"] = fd.content_type;
                part["bytes"] = fd.content.size();
                part["content_text"] = fd.content;  // tests use small fixtures only
                cap.parts.push_back(std::move(part));
            }
            const std::lock_guard lock{captures.mtx};
            captures.byPath[route.path] = std::move(cap);
        }

        if (!route.sequence.empty()) {
            const auto idx = std::min(callCount->fetch_add(1), route.sequence.size() - 1);
            const auto& step = route.sequence[idx];
            for (const auto& [k, v] : step.headers) {
                res.set_header(k, v);
            }
            res.status = step.status;
            if (!step.body.empty()) {
                const auto contentType =
                    step.body.front() == '{' ? "application/json" : "text/plain";
                res.set_content(step.body, contentType);
            }
            return;
        }

        for (const auto& [k, v] : route.headers) {
            res.set_header(k, v);
        }
        const auto contentType =
            !route.body.empty() && route.body.front() == '{' ? "application/json" : "text/plain";
        res.status = route.status;
        if (!route.body.empty()) {
            res.set_content(route.body, contentType);
        }
    };

    if (route.method == "GET")
        server.Get(route.path, handler);
    else if (route.method == "POST")
        server.Post(route.path, handler);
    else if (route.method == "PUT")
        server.Put(route.path, handler);
    else if (route.method == "PATCH")
        server.Patch(route.path, handler);
    else if (route.method == "DELETE")
        server.Delete(route.path, handler);
    else
        server.Get(route.path, handler);
}

}  // namespace

int main(int argc, char** argv) {
    fs::path routesFile;
    int requestedPort = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--routes" && i + 1 < argc) {
            routesFile = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            requestedPort = std::atoi(argv[++i]);
        }
    }

    if (routesFile.empty()) {
        std::println(stderr, "mock-sut: --routes <file.json> is required");
        return 2;
    }

    auto routes = loadRoutes(routesFile);

    httplib::Server server;
    CaptureStore captures;
    for (const auto& route : routes) {
        registerRoute(server, route, captures);
    }

    // Inspection endpoint: tests fetch GET /__mock/last-request?path=<p>
    // and read the captured Content-Type, body, and parsed parts.
    server.Get("/__mock/last-request",
               [&captures](const httplib::Request& req, httplib::Response& res) {
                   std::string path;
                   if (req.has_param("path")) path = req.get_param_value("path");

                   nlohmann::json out;
                   const std::lock_guard lock{captures.mtx};
                   auto it = captures.byPath.find(path);
                   if (it == captures.byPath.end()) {
                       out["found"] = false;
                   } else {
                       out["found"] = true;
                       out["method"] = it->second.method;
                       out["content_type"] = it->second.contentType;
                       out["raw_body"] = it->second.rawBody;
                       out["raw_body_size"] = it->second.rawBody.size();
                       out["headers"] = it->second.headers;
                       out["parts"] = it->second.parts;
                   }
                   res.status = 200;
                   res.set_content(out.dump(), "application/json");
               });

    // Health endpoint so the test harness can wait until ready.
    server.Get("/__mock/health", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200;
        res.set_content("OK", "text/plain");
    });

    auto port = server.bind_to_any_port("127.0.0.1");
    if (port < 0) {
        std::println(stderr, "mock-sut: failed to bind");
        return 2;
    }
    if (requestedPort != 0 && port != requestedPort) {
        // Caller wanted a specific port but we got something else; not fatal.
    }

    std::println("PORT: {}", port);
    std::cout.flush();

    server.listen_after_bind();
    return 0;
}
