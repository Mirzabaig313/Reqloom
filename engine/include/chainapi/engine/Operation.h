// Operation — a single endpoint action on a resource (PRD §4.2).
#pragma once

#include <chrono>
#include <compare>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace chainapi::engine {

/// Stable identifier for an operation: "<resource>.<op_name>", e.g. "order.create".
struct OperationId {
    std::string value;

    auto operator<=>(const OperationId&) const = default;
};

/// Stable identifier for a resource, e.g. "order".
struct ResourceId {
    std::string value;

    auto operator<=>(const ResourceId&) const = default;
};

/// Stable identifier for an actor, e.g. "vendor".
struct ActorId {
    std::string value;

    auto operator<=>(const ActorId&) const = default;
};

enum class HttpMethod { Get, Post, Put, Patch, Delete, Head, Options };

/// Where to pull a value out of a response.
struct Extraction {
    enum class Source { JsonPath, XPath, Header, StatusCode, Regex, Cookie };

    std::string variableName;  ///< Stored as <resource>.<variableName>.
    std::string sourcePath;    ///< JSONPath / XPath / header name / regex.
    Source source{Source::JsonPath};
};

/// Per-operation retry policy. Engine spec §3.5.
struct RetryPolicy {
    int maxAttempts{3};
    std::chrono::milliseconds baseBackoff{500};
    std::chrono::milliseconds maxBackoff{30'000};
};

/// One declared operation. 
struct Operation {
    OperationId id;
    ResourceId resource;
    ActorId actor;

    HttpMethod method{HttpMethod::Get};
    std::string pathTemplate;  ///< e.g. /api/v1/orders/{{order.order_id}}
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> queryParams;
    std::optional<std::string> bodyTemplate;  ///< Raw JSON body template, may contain {{X.y}}.
    std::optional<std::map<std::string, std::string>> bodyForm;  ///< x-www-form-urlencoded

    std::optional<int> expectStatus;
    std::vector<Extraction> extractions;

    /// Explicit dependencies declared by the user 
    std::vector<OperationId> explicitDependencies;

    /// Optional inline JS hook scripts 
    std::optional<std::string> preRequestScript;
    std::optional<std::string> postResponseScript;

    RetryPolicy retry;
    std::optional<std::chrono::milliseconds> timeout;
    bool force{false};  ///< Per-op force re-run flag.
};

}  // namespace chainapi::engine
