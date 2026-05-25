// PredicateEvaluator — boolean expression engine for poll_until success/fail
// predicates (PRD §5.11) and AI-import verification (§10.3.1).
//
// Both features ask the same question: "given a JSON document, does this
// expression hold?" Building one evaluator earns both. PRD §5.11 wires
// the result into the polling loop; PRD §10.3.1 wires it into the AI
// importer's verification pass to check that proposed extractions
// resolve to non-null values against sample responses.
//
// Grammar (intentionally shallow — keep it dumb):
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
// Lives in the application layer because it parses JSON (third-party
// dep), which the domain layer is not allowed to pull in. Pure
// computation — no I/O, no engine state.
#pragma once

#include <chainapi/engine/ErrorCodes.h>

#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace chainapi::engine {

/// One evaluation outcome. Outcome is total: structurally invalid
/// expressions or misses produce `False`, never an error.
enum class PredicateValue { False, True };

/// Compiled expression handle. Opaque to callers — keeps the AST out
/// of the header. Move-only because the AST contains unique_ptr nodes.
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

    /// Parse-and-validate. Returns `ChainApiError` (with `SchemaInvalid`)
    /// when the expression is malformed. Successful parse hands the
    /// caller a `ParsedPredicate` ready for `evaluate(...)`.
    [[nodiscard]] std::expected<ParsedPredicate, ChainApiError>
    parse(std::string_view expression) const;

    /// Evaluate a previously-parsed expression against a JSON document.
    /// Total: never throws, never returns an error. PRD §5.11 polling
    /// semantics demand totality — a bad predicate must not crash a run.
    ///
    /// `statusCode` is exposed inside expressions as `$.status_code`
    /// per PRD §5.11 ("`$` is the response body; `$.status_code` is
    /// also available"). Pass 0 when there is no associated HTTP status.
    [[nodiscard]] PredicateValue
    evaluate(const ParsedPredicate& predicate,
             std::string_view jsonBody,
             int statusCode = 0) const noexcept;

    /// Convenience: parse and evaluate in one shot. Surfaces parse errors
    /// the same way `parse(...)` does. Most callers want this form.
    [[nodiscard]] std::expected<PredicateValue, ChainApiError>
    eval(std::string_view expression,
         std::string_view jsonBody,
         int statusCode = 0) const;
};

}  // namespace chainapi::engine
