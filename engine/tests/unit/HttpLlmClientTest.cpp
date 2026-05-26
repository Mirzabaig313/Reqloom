// Slice 6g — HttpLlmClient unit tests.
//
// Each test feeds a canned HTTP response through a fake transport and
// asserts the request shape per provider plus the response decoding.

#include "infrastructure/http/HttpClient.h"
#include "infrastructure/llm/HttpLlmClient.h"
#include "infrastructure/llm/LlmClient.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <queue>
#include <string>

namespace ce = chainapi::engine;

namespace {

class FakeHttpTransport final : public ce::HttpClient {
public:
    void enqueue(int status, std::string body) {
        Response r;
        r.response.status = status;
        r.response.body = std::move(body);
        r.response.elapsed = std::chrono::milliseconds{1};
        responses_.push(std::move(r));
    }

    void enqueueError(ce::ChainApiError err) {
        Response r;
        r.error = std::move(err);
        responses_.push(std::move(r));
    }

    [[nodiscard]] const std::vector<ce::HttpRequest>& recorded() const { return recorded_; }

    std::expected<ce::HttpResponse, ce::ChainApiError> send(const ce::HttpRequest& req) override {
        recorded_.push_back(req);
        if (responses_.empty()) {
            return std::unexpected(ce::ChainApiError{
                ce::ErrorCode::NetworkTimeout, ce::ErrorClass::Network, "no canned response"});
        }
        auto r = std::move(responses_.front());
        responses_.pop();
        if (r.error) return std::unexpected(*r.error);
        return r.response;
    }

private:
    struct Response {
        ce::HttpResponse response;
        std::optional<ce::ChainApiError> error;
    };
    std::queue<Response> responses_;
    std::vector<ce::HttpRequest> recorded_;
};

ce::LlmRequest makeRequest(ce::LlmProvider provider, std::string apiKey) {
    ce::LlmRequest r;
    r.config.provider = provider;
    r.config.endpoint = "https://example.test";
    r.config.model = "test-model";
    r.config.apiKey = std::move(apiKey);
    r.messages.push_back({ce::LlmMessage::Role::System, "you are a tester"});
    r.messages.push_back({ce::LlmMessage::Role::User, "ping"});
    return r;
}

}  // namespace

// ─── OpenAI ────────────────────────────────────────────────────────────────

TEST(HttpLlmClient, openai_builds_chat_completions_request_with_bearer) {
    FakeHttpTransport transport;
    transport.enqueue(200, R"({
        "choices": [{
            "message": {"content": "hello world"},
            "finish_reason": "stop"
        }],
        "usage": {"prompt_tokens": 12, "completion_tokens": 3}
    })");

    ce::HttpLlmClient client{transport};
    auto resp = client.complete(makeRequest(ce::LlmProvider::OpenAI, "sk-test"));
    ASSERT_TRUE(resp.has_value()) << resp.error().detail;

    EXPECT_EQ(resp->content, "hello world");
    EXPECT_EQ(resp->finishReason, "stop");
    EXPECT_EQ(resp->promptTokens.value_or(0), 12);
    EXPECT_EQ(resp->completionTokens.value_or(0), 3);

    ASSERT_EQ(transport.recorded().size(), 1u);
    const auto& req = transport.recorded()[0];
    EXPECT_EQ(req.method, ce::HttpMethod::Post);
    EXPECT_EQ(req.url, "https://example.test/v1/chat/completions");
    EXPECT_EQ(req.headers.at("Authorization"), "Bearer sk-test");
    EXPECT_EQ(req.headers.at("Content-Type"), "application/json");

    ASSERT_TRUE(req.body.has_value());
    const auto sent = nlohmann::json::parse(*req.body);
    EXPECT_EQ(sent["model"], "test-model");
    EXPECT_EQ(sent["messages"].size(), 2u);
    EXPECT_EQ(sent["messages"][0]["role"], "system");
    EXPECT_EQ(sent["messages"][0]["content"], "you are a tester");
    EXPECT_EQ(sent["messages"][1]["role"], "user");
    EXPECT_EQ(sent["response_format"]["type"], "json_object");
}

TEST(HttpLlmClient, openai_rejects_empty_api_key) {
    FakeHttpTransport transport;
    ce::HttpLlmClient client{transport};

    auto resp = client.complete(makeRequest(ce::LlmProvider::OpenAI, ""));
    ASSERT_FALSE(resp.has_value());
    EXPECT_EQ(resp.error().code, ce::ErrorCode::LlmRequestFailed);
    EXPECT_EQ(transport.recorded().size(), 0u) << "must not call transport without an API key";
}

TEST(HttpLlmClient, openai_response_missing_choices_surfaces_LlmResponseInvalid) {
    FakeHttpTransport transport;
    transport.enqueue(200, R"({"id": "x"})");

    ce::HttpLlmClient client{transport};
    auto resp = client.complete(makeRequest(ce::LlmProvider::OpenAI, "sk-test"));
    ASSERT_FALSE(resp.has_value());
    EXPECT_EQ(resp.error().code, ce::ErrorCode::LlmResponseInvalid);
}

TEST(HttpLlmClient, provider_4xx_surfaces_LlmRequestFailed_with_excerpt) {
    FakeHttpTransport transport;
    transport.enqueue(401, R"({"error":{"message":"invalid api key"}})");

    ce::HttpLlmClient client{transport};
    auto resp = client.complete(makeRequest(ce::LlmProvider::OpenAI, "sk-bad"));
    ASSERT_FALSE(resp.has_value());
    EXPECT_EQ(resp.error().code, ce::ErrorCode::LlmRequestFailed);
    EXPECT_NE(resp.error().detail.find("HTTP 401"), std::string::npos);
    EXPECT_NE(resp.error().detail.find("invalid api key"), std::string::npos);
}

TEST(HttpLlmClient, transport_failure_propagates_as_LlmRequestFailed) {
    FakeHttpTransport transport;
    transport.enqueueError(ce::ChainApiError{
        ce::ErrorCode::NetworkTimeout, ce::ErrorClass::Network, "connection timed out"});

    ce::HttpLlmClient client{transport};
    auto resp = client.complete(makeRequest(ce::LlmProvider::OpenAI, "sk-test"));
    ASSERT_FALSE(resp.has_value());
    EXPECT_EQ(resp.error().code, ce::ErrorCode::LlmRequestFailed);
    EXPECT_NE(resp.error().detail.find("connection timed out"), std::string::npos);
}

// ─── Anthropic ──────────────────────────────────────────────────────────────

TEST(HttpLlmClient, anthropic_lifts_system_prompt_to_top_level_field) {
    FakeHttpTransport transport;
    transport.enqueue(200, R"({
        "content": [{"type": "text", "text": "ack"}],
        "stop_reason": "end_turn",
        "usage": {"input_tokens": 8, "output_tokens": 1}
    })");

    ce::HttpLlmClient client{transport};
    auto req = makeRequest(ce::LlmProvider::Anthropic, "anthropic-key");
    req.config.maxTokens = 256;
    auto resp = client.complete(req);
    ASSERT_TRUE(resp.has_value()) << resp.error().detail;

    EXPECT_EQ(resp->content, "ack");
    EXPECT_EQ(resp->finishReason, "end_turn");
    EXPECT_EQ(resp->promptTokens.value_or(0), 8);
    EXPECT_EQ(resp->completionTokens.value_or(0), 1);

    const auto& sent = transport.recorded()[0];
    EXPECT_EQ(sent.url, "https://example.test/v1/messages");
    EXPECT_EQ(sent.headers.at("x-api-key"), "anthropic-key");
    EXPECT_EQ(sent.headers.at("anthropic-version"), "2023-06-01");

    const auto body = nlohmann::json::parse(*sent.body);
    EXPECT_EQ(body["system"], "you are a tester");
    EXPECT_EQ(body["max_tokens"], 256);
    ASSERT_EQ(body["messages"].size(), 1u);
    EXPECT_EQ(body["messages"][0]["role"], "user");
    EXPECT_EQ(body["messages"][0]["content"], "ping");
}

TEST(HttpLlmClient, anthropic_max_tokens_defaults_when_unset) {
    FakeHttpTransport transport;
    transport.enqueue(200, R"({"content":[{"type":"text","text":"ok"}]})");

    ce::HttpLlmClient client{transport};
    auto resp = client.complete(makeRequest(ce::LlmProvider::Anthropic, "k"));
    ASSERT_TRUE(resp.has_value());

    const auto body = nlohmann::json::parse(*transport.recorded()[0].body);
    EXPECT_GT(body["max_tokens"].get<int>(), 0)
        << "anthropic requires max_tokens; client must default it";
}

TEST(HttpLlmClient, anthropic_rejects_empty_api_key) {
    FakeHttpTransport transport;
    ce::HttpLlmClient client{transport};
    auto resp = client.complete(makeRequest(ce::LlmProvider::Anthropic, ""));
    ASSERT_FALSE(resp.has_value());
    EXPECT_EQ(resp.error().code, ce::ErrorCode::LlmRequestFailed);
}

// ─── Ollama ────────────────────────────────────────────────────────────────

TEST(HttpLlmClient, ollama_calls_local_endpoint_without_auth) {
    FakeHttpTransport transport;
    transport.enqueue(200, R"({
        "message": {"role": "assistant", "content": "local pong"},
        "done_reason": "stop",
        "prompt_eval_count": 5,
        "eval_count": 2
    })");

    ce::HttpLlmClient client{transport};
    auto req = makeRequest(ce::LlmProvider::Ollama, "");  // no key
    req.config.endpoint = "http://localhost:11434";
    auto resp = client.complete(req);
    ASSERT_TRUE(resp.has_value()) << resp.error().detail;

    EXPECT_EQ(resp->content, "local pong");
    EXPECT_EQ(resp->finishReason, "stop");
    EXPECT_EQ(resp->promptTokens.value_or(0), 5);
    EXPECT_EQ(resp->completionTokens.value_or(0), 2);

    const auto& sent = transport.recorded()[0];
    EXPECT_EQ(sent.url, "http://localhost:11434/api/chat");
    EXPECT_EQ(sent.headers.find("Authorization"), sent.headers.end())
        << "ollama must not send an Authorization header";

    const auto body = nlohmann::json::parse(*sent.body);
    EXPECT_EQ(body["model"], "test-model");
    EXPECT_EQ(body["format"], "json");
    EXPECT_EQ(body["stream"], false);
}

// ─── Sanity ────────────────────────────────────────────────────────────────

TEST(HttpLlmClient, missing_messages_short_circuits) {
    FakeHttpTransport transport;
    ce::HttpLlmClient client{transport};

    ce::LlmRequest req;
    req.config.provider = ce::LlmProvider::OpenAI;
    req.config.endpoint = "https://example.test";
    req.config.model = "x";
    req.config.apiKey = "k";
    auto resp = client.complete(req);
    ASSERT_FALSE(resp.has_value());
    EXPECT_EQ(resp.error().code, ce::ErrorCode::LlmRequestFailed);
    EXPECT_EQ(transport.recorded().size(), 0u);
}

TEST(HttpLlmClient, undecodable_provider_body_surfaces_LlmResponseInvalid) {
    FakeHttpTransport transport;
    transport.enqueue(200, "not-json");

    ce::HttpLlmClient client{transport};
    auto resp = client.complete(makeRequest(ce::LlmProvider::OpenAI, "k"));
    ASSERT_FALSE(resp.has_value());
    EXPECT_EQ(resp.error().code, ce::ErrorCode::LlmResponseInvalid);
}
