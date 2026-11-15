// HttpLlmClient — LLM client via HTTP with truncation.
#include "HttpLlmClient.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace reqloom::engine {

namespace {

using json = nlohmann::json;

ReqloomError requestFailed(std::string detail) {
    return ReqloomError{ErrorCode::LlmRequestFailed, ErrorClass::Llm, std::move(detail)};
}

ReqloomError responseInvalid(std::string detail) {
    return ReqloomError{ErrorCode::LlmResponseInvalid, ErrorClass::Llm, std::move(detail)};
}

constexpr std::string_view kBodyExcerptCap = "... (truncated)";
constexpr std::size_t kBodyExcerptBytes = 400;

std::string excerpt(const std::string& body) {
    if (body.size() <= kBodyExcerptBytes) {
        return body;
    }
    return body.substr(0, kBodyExcerptBytes) + std::string{kBodyExcerptCap};
}

/// True for `https://…`, or `http://` to a loopback host (local model servers such as Ollama).
bool endpointIsSafe(std::string_view endpoint) {
    const auto lower = [](std::string_view text) {
        std::string out{text};
        for (auto& c : out) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    };
    const auto schemeEnd = endpoint.find("://");
    if (schemeEnd == std::string_view::npos) {
        return false;
    }
    const auto scheme = lower(endpoint.substr(0, schemeEnd));
    if (scheme == "https") {
        return true;
    }
    if (scheme != "http") {
        return false;
    }
    auto authority = endpoint.substr(schemeEnd + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (authority.find('@') != std::string_view::npos) {
        return false;
    }
    std::string_view host = authority;
    if (host.starts_with('[')) {
        host = host.substr(0, host.find(']') + 1);
    } else {
        host = host.substr(0, host.find(':'));
    }
    const auto h = lower(host);
    return h == "localhost" || h == "127.0.0.1" || h == "[::1]";
}

std::string withoutKey(std::string detail, const std::string& apiKey) {
    if (apiKey.empty()) {
        return detail;
    }
    for (auto at = detail.find(apiKey); at != std::string::npos; at = detail.find(apiKey, at)) {
        detail.replace(at, apiKey.size(), "[REDACTED]");
    }
    return detail;
}

std::string_view roleAsString(LlmMessage::Role role) noexcept {
    switch (role) {
        case LlmMessage::Role::System:
            return "system";
        case LlmMessage::Role::User:
            return "user";
        case LlmMessage::Role::Assistant:
            return "assistant";
    }
    return "user";
}

/// OpenAI Chat Completions: every message, system prompt included, rides in `messages[]`.
std::expected<HttpRequest, ReqloomError> buildOpenAiRequest(const LlmRequest& req) {
    if (req.config.apiKey.empty()) {
        return std::unexpected(requestFailed("openai: API key is required"));
    }

    json body;
    body["model"] = req.config.model;
    body["messages"] = json::array();
    for (const auto& m : req.messages) {
        body["messages"].push_back(
            {{"role", std::string{roleAsString(m.role)}}, {"content", m.content}});
    }
    if (req.config.maxTokens) {
        body["max_tokens"] = *req.config.maxTokens;
    }
    if (req.config.jsonOnly) {
        body["response_format"] = {{"type", "json_object"}};
    }

    HttpRequest http;
    http.method = HttpMethod::Post;
    http.url = req.config.endpoint + "/v1/chat/completions";
    http.headers["Authorization"] = "Bearer " + req.config.apiKey;
    http.headers["Content-Type"] = "application/json";
    http.headers["Accept"] = "application/json";
    http.body = body.dump();
    http.timeout = req.config.timeout;
    return http;
}

/// Anthropic Messages API: `system` is a top-level field; `max_tokens` is required by the spec.
std::expected<HttpRequest, ReqloomError> buildAnthropicRequest(const LlmRequest& req) {
    if (req.config.apiKey.empty()) {
        return std::unexpected(requestFailed("anthropic: API key is required"));
    }

    json body;
    body["model"] = req.config.model;
    body["max_tokens"] = req.config.maxTokens.value_or(4096);

    std::string systemPrompt;
    json messages = json::array();
    for (const auto& m : req.messages) {
        if (m.role == LlmMessage::Role::System) {
            if (!systemPrompt.empty()) {
                systemPrompt += "\n\n";
            }
            systemPrompt += m.content;
            continue;
        }
        messages.push_back({{"role", std::string{roleAsString(m.role)}}, {"content", m.content}});
    }
    if (!systemPrompt.empty()) {
        body["system"] = std::move(systemPrompt);
    }
    body["messages"] = std::move(messages);

    HttpRequest http;
    http.method = HttpMethod::Post;
    http.url = req.config.endpoint + "/v1/messages";
    http.headers["x-api-key"] = req.config.apiKey;
    http.headers["anthropic-version"] = "2023-06-01";
    http.headers["Content-Type"] = "application/json";
    http.headers["Accept"] = "application/json";
    http.body = body.dump();
    http.timeout = req.config.timeout;
    return http;
}

std::expected<HttpRequest, ReqloomError> buildOllamaRequest(const LlmRequest& req) {
    json body;
    body["model"] = req.config.model;
    body["messages"] = json::array();
    for (const auto& m : req.messages) {
        body["messages"].push_back(
            {{"role", std::string{roleAsString(m.role)}}, {"content", m.content}});
    }
    if (req.config.jsonOnly) {
        body["format"] = "json";
    }
    body["stream"] = false;

    HttpRequest http;
    http.method = HttpMethod::Post;
    http.url = req.config.endpoint + "/api/chat";
    http.headers["Content-Type"] = "application/json";
    http.headers["Accept"] = "application/json";
    http.body = body.dump();
    http.timeout = req.config.timeout;
    return http;
}

std::expected<LlmResponse, ReqloomError> parseOpenAi(const json& doc) {
    if (!doc.contains("choices") || !doc["choices"].is_array() || doc["choices"].empty()) {
        return std::unexpected(responseInvalid("openai: response missing `choices[0]`"));
    }
    const auto& first = doc["choices"][0];
    if (!first.contains("message") || !first["message"].contains("content")) {
        return std::unexpected(
            responseInvalid("openai: response missing `choices[0].message.content`"));
    }
    LlmResponse out;
    out.content = first["message"]["content"].get<std::string>();
    if (first.contains("finish_reason")) {
        out.finishReason = first["finish_reason"].is_null()
                               ? std::string{}
                               : first["finish_reason"].get<std::string>();
    }
    if (doc.contains("usage")) {
        const auto& u = doc["usage"];
        if (u.contains("prompt_tokens")) {
            out.promptTokens = u["prompt_tokens"].get<int>();
        }
        if (u.contains("completion_tokens")) {
            out.completionTokens = u["completion_tokens"].get<int>();
        }
    }
    return out;
}

std::expected<LlmResponse, ReqloomError> parseAnthropic(const json& doc) {
    if (!doc.contains("content") || !doc["content"].is_array() || doc["content"].empty()) {
        return std::unexpected(responseInvalid("anthropic: response missing `content[0]`"));
    }
    LlmResponse out;
    for (const auto& block : doc["content"]) {
        if (block.value("type", std::string{}) == "text" && block.contains("text")) {
            if (!out.content.empty()) {
                out.content += "\n";
            }
            out.content += block["text"].get<std::string>();
        }
    }
    if (out.content.empty()) {
        return std::unexpected(responseInvalid("anthropic: no text blocks in `content[]`"));
    }
    if (doc.contains("stop_reason") && !doc["stop_reason"].is_null()) {
        out.finishReason = doc["stop_reason"].get<std::string>();
    }
    if (doc.contains("usage")) {
        const auto& u = doc["usage"];
        if (u.contains("input_tokens")) {
            out.promptTokens = u["input_tokens"].get<int>();
        }
        if (u.contains("output_tokens")) {
            out.completionTokens = u["output_tokens"].get<int>();
        }
    }
    return out;
}

std::expected<LlmResponse, ReqloomError> parseOllama(const json& doc) {
    if (!doc.contains("message") || !doc["message"].contains("content")) {
        return std::unexpected(responseInvalid("ollama: response missing `message.content`"));
    }
    LlmResponse out;
    out.content = doc["message"]["content"].get<std::string>();
    if (doc.contains("done_reason") && !doc["done_reason"].is_null()) {
        out.finishReason = doc["done_reason"].get<std::string>();
    }
    if (doc.contains("prompt_eval_count")) {
        out.promptTokens = doc["prompt_eval_count"].get<int>();
    }
    if (doc.contains("eval_count")) {
        out.completionTokens = doc["eval_count"].get<int>();
    }
    return out;
}

}  // namespace

std::expected<LlmResponse, ReqloomError> HttpLlmClient::complete(const LlmRequest& request) {
    if (transport_ == nullptr) {
        return std::unexpected(requestFailed("LlmClient wired without a transport"));
    }
    if (request.config.endpoint.empty()) {
        return std::unexpected(requestFailed("provider endpoint is empty"));
    }
    if (request.config.model.empty()) {
        return std::unexpected(requestFailed("provider model is empty"));
    }
    if (request.messages.empty()) {
        return std::unexpected(requestFailed("messages list is empty"));
    }
    // The API key must not cross the network in clear text.
    if (!endpointIsSafe(request.config.endpoint)) {
        return std::unexpected(
            requestFailed("provider endpoint must use https (plain http only for localhost)"));
    }

    std::expected<HttpRequest, ReqloomError> built;
    switch (request.config.provider) {
        case LlmProvider::OpenAI:
            built = buildOpenAiRequest(request);
            break;
        case LlmProvider::Anthropic:
            built = buildAnthropicRequest(request);
            break;
        case LlmProvider::Ollama:
            built = buildOllamaRequest(request);
            break;
    }
    if (!built) {
        return std::unexpected(built.error());
    }
    // A redirect would forward the API key header to whichever host it names.
    built->followRedirects = false;

    const auto& apiKey = request.config.apiKey;
    const auto sendStart = std::chrono::steady_clock::now();
    auto response = transport_->send(*built);
    if (!response) {
        return std::unexpected(
            requestFailed(withoutKey("transport: " + response.error().detail, apiKey)));
    }
    if (response->status < 200 || response->status >= 300) {
        return std::unexpected(requestFailed(withoutKey(
            "provider HTTP " + std::to_string(response->status) + " — " + excerpt(response->body),
            apiKey)));
    }

    json doc;
    try {
        doc = json::parse(response->body);
    } catch (const json::parse_error& e) {
        return std::unexpected(
            responseInvalid(std::string{"could not parse provider JSON: "} + e.what()));
    }

    std::expected<LlmResponse, ReqloomError> parsed;
    // Provider parsers' get<T>() throws on wrong value types in untrusted responses; contain it
    // here so it becomes ErrorCode::LlmResponseInvalid instead of escaping complete()'s
    // std::expected contract.
    try {
        switch (request.config.provider) {
            case LlmProvider::OpenAI:
                parsed = parseOpenAi(doc);
                break;
            case LlmProvider::Anthropic:
                parsed = parseAnthropic(doc);
                break;
            case LlmProvider::Ollama:
                parsed = parseOllama(doc);
                break;
        }
    } catch (const json::exception& e) {
        return std::unexpected(
            responseInvalid(std::string{"provider response has unexpected types: "} + e.what()));
    }
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    parsed->elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - sendStart);
    return parsed;
}

}  // namespace reqloom::engine
