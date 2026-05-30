// PredicateEvaluator — boolean expression engine for poll_until predicates.
//
// Grammar (intentionally shallow):
//
//   expr     := compare ( ('&&' | '||') compare )*
//   compare  := term op term | jsonpath        // bare jsonpath = truthiness
//   op       := '==' | '!=' | '<' | '<=' | '>' | '>='
//             | 'in' | 'matches'
//   term     := jsonpath | string | number | boolean | null | array
//   jsonpath := '$' ('.' ident | '[' index ']' | '[' '"' key '"' ']')*
//   string   := '"' chars '"' | "'" chars "'"
//   array    := '[' term ( ',' term )* ']'
//
// Lives in the application layer because it parses JSON (third-party dep),
// which the domain layer is not allowed to pull in.
#pragma once

#include <chainapi/engine/ErrorCodes.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace chainapi::engine {

/// One evaluation outcome. Structurally invalid expressions or misses
/// produce `False`, never an error.
enum class PredicateValue : std::uint8_t { False, True };

/// Compiled expression handle. Opaque to callers — keeps the AST out of
/// the header. Move-only because the AST contains unique_ptr nodes.
class ParsedPredicate {
public:
    ParsedPredicate();
    ~ParsedPredicate();

    ParsedPredicate(ParsedPredicate&&) noexcept;
    ParsedPredicate& operator=(ParsedPredicate&&) noexcept;

    ParsedPredicate(const ParsedPredicate&) = delete;
    ParsedPredicate& operator=(const ParsedPredicate&) = delete;

    struct Node;  ///< AST root. Forward-declared in PredicateEvaluator.cpp.

private:
    friend class PredicateEvaluator;
    explicit ParsedPredicate(std::unique_ptr<Node> root);
    std::unique_ptr<Node> root_;
};

/// Pre-parsed response body. Holds the parsed JSON document (and the
/// "was it valid JSON" outcome) so a single body can be evaluated against
/// several predicates without re-parsing each time — the hot path is the
/// poll loop, which checks both a success and a fail predicate per attempt.
///
/// Opaque + move-only on purpose: the nlohmann::json type stays in the
/// .cpp so it never leaks onto the application-layer public surface.
class ParsedBody {
public:
    ParsedBody();
    ~ParsedBody();

    ParsedBody(ParsedBody&&) noexcept;
    ParsedBody& operator=(ParsedBody&&) noexcept;

    ParsedBody(const ParsedBody&) = delete;
    ParsedBody& operator=(const ParsedBody&) = delete;

    struct Impl;  ///< Defined in PredicateEvaluator.cpp.

private:
    friend class PredicateEvaluator;
    explicit ParsedBody(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class PredicateEvaluator {
public:
    PredicateEvaluator();

    /// Parse and validate. Returns `ChainApiError{SchemaInvalid}` when the
    /// expression is malformed.
    [[nodiscard]] std::expected<ParsedPredicate, ChainApiError> parse(
        std::string_view expression) const;

    /// Parse a response body once for reuse across multiple `evaluate`
    /// calls. Total + noexcept: a malformed / non-JSON body is captured as
    /// an empty object so `$.status_code`-only predicates still evaluate,
    /// exactly matching the string_view `evaluate` overload's fallback.
    [[nodiscard]] ParsedBody parseBody(std::string_view jsonBody) const noexcept;

    /// Evaluate a previously-parsed expression against a previously-parsed
    /// body. Total: never throws, never returns an error. Use this (with a
    /// shared `ParsedBody`) when checking several predicates against the
    /// same response to avoid re-parsing the body each time.
    ///
    /// `statusCode` is exposed inside expressions as `$.status_code`.
    [[nodiscard]] PredicateValue evaluate(const ParsedPredicate& predicate,
                                          const ParsedBody& body,
                                          int statusCode = 0) const noexcept;

    /// Evaluate a previously-parsed expression against a JSON document.
    /// Total: never throws, never returns an error. Convenience wrapper
    /// that parses `jsonBody` and forwards to the `ParsedBody` overload.
    ///
    /// `statusCode` is exposed inside expressions as `$.status_code`.
    /// Pass 0 when there is no associated HTTP status.
    [[nodiscard]] PredicateValue evaluate(const ParsedPredicate& predicate,
                                          std::string_view jsonBody,
                                          int statusCode = 0) const noexcept;

    /// Convenience: parse and evaluate in one shot.
    [[nodiscard]] std::expected<PredicateValue, ChainApiError> eval(std::string_view expression,
                                                                    std::string_view jsonBody,
                                                                    int statusCode = 0) const;
};

}  // namespace chainapi::engine
