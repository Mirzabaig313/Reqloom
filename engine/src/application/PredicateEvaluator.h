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

#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace chainapi::engine {

/// One evaluation outcome. Structurally invalid expressions or misses
/// produce `False`, never an error.
enum class PredicateValue { False, True };

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

class PredicateEvaluator {
public:
    PredicateEvaluator();
    ~PredicateEvaluator();

    /// Parse and validate. Returns `ChainApiError{SchemaInvalid}` when the
    /// expression is malformed.
    [[nodiscard]] std::expected<ParsedPredicate, ChainApiError> parse(
        std::string_view expression) const;

    /// Evaluate a previously-parsed expression against a JSON document.
    /// Total: never throws, never returns an error.
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
