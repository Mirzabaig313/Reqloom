// LlmClient — provider-agnostic interface for the AI importer.
//
// The user supplies their own API key (no proxy through
// ChainAPI servers). The LLM call returns the assistant message verbatim;
// JSON-shape validation is the importer's job, not the client's.
#pragma once

#include <chainapi/engine/ErrorCodes.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace chainapi::engine {

enum class LlmProvider : std::uint8_t {
    OpenAI,     ///< POST <endpoint>/v1/chat/completions
    Anthropic,  ///< POST <endpoint>/v1/messages
    Ollama,     ///< POST <endpoint>/api/chat (local; no API key)
};

/// Static configuration for one provider. Keys live in the OS keychain
/// via `SecretStore` and are loaded into `apiKey` at call time; we never
/// persist the key on `LlmConfig`.
struct LlmConfig {
    LlmProvider provider{LlmProvider::OpenAI};
    std::string endpoint;          ///< e.g. "https://api.openai.com" — no trailing slash.
    std::string model;             ///< e.g. "gpt-4o", "claude-3-5-sonnet-20241022", "llama3.1".
    std::string apiKey;            ///< Bearer / x-api-key value. Empty for Ollama.
    std::optional<int> maxTokens;  ///< Required by Anthropic; optional elsewhere.
    std::chrono::milliseconds timeout{60'000};
    bool jsonOnly{true};  ///< Hint the provider to emit JSON-only output.
};

/// One conversation turn. The system prompt lands in `messages[0]` for
/// OpenAI/Ollama and in the top-level `system` field for Anthropic.
struct LlmMessage {
    enum class Role : std::uint8_t { System, User, Assistant };

    Role role{Role::User};
    std::string content;
};

struct LlmRequest {
    LlmConfig config;
    std::vector<LlmMessage> messages;
};

struct LlmResponse {
    std::string content;       ///< Assistant message verbatim.
    std::string finishReason;  ///< Provider-specific (e.g. "stop", "length", "end_turn").
    std::optional<int> promptTokens;
    std::optional<int> completionTokens;
    std::chrono::milliseconds elapsed{};
};

class LlmClient {
public:
    LlmClient() = default;
    LlmClient(const LlmClient&) = delete;
    LlmClient& operator=(const LlmClient&) = delete;
    LlmClient(LlmClient&&) = delete;
    LlmClient& operator=(LlmClient&&) = delete;
    virtual ~LlmClient() = default;

    /// Synchronous. Returns `LlmRequestFailed` for transport / 4xx / 5xx
    /// failures (with the response body excerpt in `detail` for
    /// debugging); `LlmResponseInvalid` when the body decodes but is
    /// missing the assistant message.
    virtual std::expected<LlmResponse, ChainApiError> complete(const LlmRequest& request) = 0;
};

}  // namespace chainapi::engine
