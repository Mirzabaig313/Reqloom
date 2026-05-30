// HooksTest — confirms hooks are actually invoked by the executor:
//   - pre_request mutates outgoing headers (the mock SUT sees X-Signature)
//   - post_response can transform the body so extraction runs against
//     the unwrapped payload (envelope -> data)
//
// These tests exercise the full pipeline (engine + libcurl + QuickJS +
// mock SUT) and would silently pass with the old stub runner because the
// stub left ctx.request unchanged.

#include "MockSutHarness.h"

#include <chainapi/engine/Factories.h>
#include <chainapi/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;
namespace ce = chainapi::engine;
namespace ct = chainapi::tests;

namespace {

[[nodiscard]] fs::path fixturesDir() {
    return fs::path(CHAINAPI_FIXTURES_DIR);
}

[[nodiscard]] ce::Project loadProject(const std::string& mockBaseUrl) {
    auto project = ce::parseProject(fixturesDir() / "hooks-project" / "chainapi.yaml");
    EXPECT_TRUE(project.has_value()) << (project ? "" : project.error().detail);
    project->environments["local"]["baseUrl"] = mockBaseUrl;
    return std::move(*project);
}

[[nodiscard]] nlohmann::json fetchLastRequest(const std::string& baseUrl, const std::string& path) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{curl_easy_init(), &curl_easy_cleanup};
    EXPECT_NE(curl.get(), nullptr);

    const auto url = baseUrl + "/__mock/last-request?path=" + path;
    std::string body;
    auto writer = +[](char* ptr, std::size_t size, std::size_t nmemb, void* ud) -> std::size_t {
        auto* out = static_cast<std::string*>(ud);
        out->append(ptr, size * nmemb);
        return size * nmemb;
    };
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writer);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    auto rc = curl_easy_perform(curl.get());
    EXPECT_EQ(rc, CURLE_OK) << curl_easy_strerror(rc);
    return nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
}

}  // namespace

class HooksFixture : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ = std::make_unique<ct::MockSutHarness>(fixturesDir() / "hooks-routes.json");
    }
    void TearDown() override { harness_.reset(); }

    std::unique_ptr<ct::MockSutHarness> harness_;
};

TEST_F(HooksFixture, pre_request_hook_signs_the_outgoing_request) {
    auto project = loadProject(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"signed.send"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    ASSERT_TRUE(result->succeeded()) << "signed.send chain did not succeed";

    auto cap = fetchLastRequest(harness_->baseUrl(), "/api/v1/signed");
    ASSERT_TRUE(cap["found"].get<bool>()) << "mock SUT did not capture /api/v1/signed";

    // The mock SUT lower-cases header names so case-sensitivity drift in
    // libcurl's outbound serialization doesn't make the test flaky.
    const auto& headers = cap["headers"];
    ASSERT_TRUE(headers.contains("x-signature"))
        << "request did not carry the hook-set X-Signature header";

    // hmac.sha256('shared-key', '{"payload":"secret"}') in lowercase hex.
    EXPECT_EQ(headers["x-signature"].get<std::string>(),
              "b7ff9aa12db96abdb4bb78225867dfe8582b6dab2762bd5deb5fdb293d452a96");
}

TEST_F(HooksFixture, post_response_hook_transforms_body_before_extraction) {
    auto project = loadProject(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"wrapped.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    ASSERT_TRUE(result->succeeded())
        << "wrapped.get must succeed — post_response unwraps the envelope";

    // The schema declares `extract: { id: $.data.id }`, but the raw
    // response is `{ envelope: { data: { id: "deeply-nested" } } }`.
    // Without the post_response hook, the extraction would miss and the
    // step would fail with E_EXTRACTION_FAILED. So a green run + a
    // resolved trace value is proof the hook ran AND its mutation
    // reached the extractor.
    bool sawExtraction = false;
    for (const auto& trace : ctx.extractionTrace()) {
        if (trace.variableName == "id" && trace.op.value == "wrapped.get") {
            EXPECT_EQ(trace.outcome, ce::ExtractionTrace::Outcome::Resolved);
            EXPECT_EQ(trace.value, "deeply-nested");
            sawExtraction = true;
        }
    }
    EXPECT_TRUE(sawExtraction);
}
