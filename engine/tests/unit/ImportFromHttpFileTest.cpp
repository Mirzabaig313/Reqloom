// Direct parser for .http / .rest files (REST Client / JetBrains HTTP Client).
// Each test fails on the parent commit (importFromHttpFile did not exist).

#include <reqloom/engine/Factories.h>

#include <gtest/gtest.h>

#include <support/TempPath.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

namespace {

class ScratchDir {
public:
    ScratchDir() {
        path_ = reqloom::tests::uniqueTempPath("reqloom-http-import");
        fs::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    [[nodiscard]] const fs::path& path() const { return path_; }

    fs::path write(const std::string& filename, const std::string& body) {
        const auto full = path_ / filename;
        std::ofstream out{full};
        out << body;
        return full;
    }

private:
    fs::path path_;
};

// A @baseUrl var, a named POST with a JSON body + token header, and an unnamed
// GET with a query string.
constexpr const char* kHttp =
    "@baseUrl = https://api.example.com\n"
    "@token = abc123\n"
    "\n"
    "### Create Order\n"
    "POST {{baseUrl}}/api/v1/orders\n"
    "Content-Type: application/json\n"
    "X-Api-Key: {{token}}\n"
    "\n"
    "{\n"
    "  \"item\": \"widget\"\n"
    "}\n"
    "\n"
    "### List Orders\n"
    "GET {{baseUrl}}/api/v1/orders?status=open\n";

const ce::Operation* findOp(const ce::Project& p, const std::string& id) {
    for (const auto& [resId, res] : p.resources) {
        for (const auto& [opName, op] : res.operations) {
            if (op.id.value == id) {
                return &op;
            }
        }
    }
    return nullptr;
}

}  // namespace

TEST(ImportFromHttpFile, rejects_file_with_no_requests) {
    ScratchDir scratch;
    const auto file = scratch.write("empty.http", "@only = a var\n# just a comment\n");
    auto outcome = ce::importFromHttpFile(file, scratch.path());
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("no requests"), std::string::npos);
}

TEST(ImportFromHttpFile, requests_become_operations_under_a_file_resource) {
    ScratchDir scratch;
    const auto file = scratch.write("orders.http", kHttp);

    auto outcome = ce::importFromHttpFile(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& project = outcome->project;

    ASSERT_TRUE(project.resources.contains(ce::ResourceId{"orders"}));
    EXPECT_EQ(project.resources.at(ce::ResourceId{"orders"}).operations.size(), 2u);

    const auto* create = findOp(project, "orders.create_order");
    ASSERT_NE(create, nullptr);
    EXPECT_EQ(create->method, ce::HttpMethod::Post);
    EXPECT_EQ(create->pathTemplate, "/api/v1/orders");
    ASSERT_TRUE(create->bodyTemplate.has_value());
    EXPECT_NE(create->bodyTemplate->find("widget"), std::string::npos);
    ASSERT_TRUE(create->headers.contains("Content-Type"));
    ASSERT_TRUE(create->headers.contains("X-Api-Key"));
    EXPECT_EQ(create->headers.at("X-Api-Key"), "{{env.token}}");
}

TEST(ImportFromHttpFile, at_var_seeds_baseurl_and_query_is_kept) {
    ScratchDir scratch;
    const auto file = scratch.write("orders.rest", kHttp);

    auto outcome = ce::importFromHttpFile(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& project = outcome->project;

    ASSERT_TRUE(project.environments.contains("default"));
    EXPECT_EQ(project.environments.at("default").at("baseUrl"), "https://api.example.com");

    const auto* list = findOp(project, "orders.list_orders");
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->pathTemplate, "/api/v1/orders");
    ASSERT_TRUE(list->queryParams.contains("status"));
    EXPECT_EQ(list->queryParams.at("status"), "open");
}

TEST(ImportFromHttpFile, importAny_routes_http_extension) {
    ScratchDir scratch;
    const auto file = scratch.write("orders.http", kHttp);
    auto outcome = ce::importAny(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    EXPECT_TRUE(outcome->project.resources.contains(ce::ResourceId{"orders"}));
}

TEST(ImportFromHttpFile, rejects_traversal_outside_project_root) {
    ScratchDir scratch;
    const auto outside = scratch.path().parent_path() / "reqloom-http-escape.http";
    {
        std::ofstream out{outside};
        out << kHttp;
    }
    auto outcome = ce::importFromHttpFile(outside, scratch.path());
    std::error_code ec;
    fs::remove(outside, ec);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("project root"), std::string::npos);
}
