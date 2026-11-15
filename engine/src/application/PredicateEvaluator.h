// PredicateEvaluator — boolean expression engine for poll_until predicates.
//
// Grammar (intentionally shallow):
//
//   expr     := and ( ('||' | 'or') and )*     // '&&' binds tighter than '||'
//   and      := compare ( ('&&' | 'and') compare )*
//   compare  := '(' expr ')' | term op term | jsonpath   // bare jsonpath = truthiness
//   op       := '==' | '!=' | '<' | '<=' | '>' | '>='
//             | 'in' | 'matches'
//   term     := jsonpath | string | number | boolean | null | array
//   jsonpath := '$' ('.' ident | '[' index ']' | '[' '"' key '"' ']')*
//   string   := '"' chars '"' | "'" chars "'"
//   array    := '[' term ( ',' term )* ']'
//
// Parentheses and array literals may nest at most 64 levels deep.
//
// Lives in the application layer because it parses JSON (a third-party dep the domain layer may not
// pull in).
#pragma once

#include <reqloom/engine/ErrorCodes.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace reqloom::engine {

/// Deepest array/object nesting accepted in a JSON response body; deeper input is rejected
/// before parsing because the walkers recurse once per level.
inline constexpr std::size_t kMaxJsonNestingDepth = 256;

/// True when `text` nests JSON arrays/objects more than `limit` levels deep. Iterative and
/// string-aware, so safe to call on hostile input before parsing.
[[nodiscard]] bool jsonNestingExceeds(std::string_view text,
                                      std::size_t limit = kMaxJsonNestingDepth) noexcept;

/// Largest subject (bytes) a regex is run over; larger input is refused because std::regex
/// can recurse per character and overflow the stack.
inline constexpr std::size_t kMaxRegexSubjectBytes = std::size_t{1024} * 1024;

/// Longest regex pattern (bytes) accepted from a project.
inline constexpr std::size_t kMaxRegexPatternBytes = 1024;

struct RegexSearchResult {
    enum class Outcome : std::uint8_t { Matched, NoMatch, Refused };
    Outcome outcome{Outcome::NoMatch};
    std::string value;   ///< Matched: capture group 1 when present, else the whole match.
    std::string reason;  ///< Refused: why (pattern guard, invalid pattern, subject size).
};

/// Check a pattern against the regex guard and that std::regex (ECMAScript) compiles it;
/// the error says how to rewrite it.
[[nodiscard]] std::expected<void, std::string> checkRegexPattern(std::string_view pattern);

/// The one place a project-supplied regex runs: pattern guard, subject cap, shared compile cache,
/// search. No time limit: a slow pattern the heuristic misses can still run long.
[[nodiscard]] RegexSearchResult guardedRegexSearch(std::string_view subject,
                                                   std::string_view pattern);

/// One evaluation outcome. Structurally invalid expressions or misses
/// produce `False`, never an error.
enum class PredicateValue : std::uint8_t { False, True };

/// Compiled expression handle; opaque (keeps the AST out of the header) and move-only (unique_ptr
/// nodes).
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

/// Pre-parsed response body so one body can be evaluated against several predicates without
/// re-parsing (the poll loop checks a success and a fail predicate per attempt). Opaque and
/// move-only so nlohmann::json stays in the .cpp.
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

    /// Parse and validate. Returns `ReqloomError{SchemaInvalid}` when the
    /// expression is malformed.
    [[nodiscard]] std::expected<ParsedPredicate, ReqloomError> parse(
        std::string_view expression) const;

    /// Parse a response body once for reuse across multiple `evaluate`
    /// calls. Total + noexcept: a malformed or too-deeply-nested body is captured as an empty
    /// object so `$.status_code`-only predicates still evaluate, as the string_view overload does.
    [[nodiscard]] ParsedBody parseBody(std::string_view jsonBody) const noexcept;

    /// Evaluate a parsed expression against a `ParsedBody`. Total: never throws or errors. Share
    /// one `ParsedBody` across predicates on the same response. `statusCode` is exposed as
    /// `$.status_code`.
    [[nodiscard]] PredicateValue evaluate(const ParsedPredicate& predicate,
                                          const ParsedBody& body,
                                          int statusCode = 0) const noexcept;

    /// Evaluate a parsed expression against a JSON document: parses `jsonBody` and forwards to the
    /// `ParsedBody` overload. Total. `statusCode` is `$.status_code`; pass 0 when there is no HTTP
    /// status.
    [[nodiscard]] PredicateValue evaluate(const ParsedPredicate& predicate,
                                          std::string_view jsonBody,
                                          int statusCode = 0) const noexcept;

    /// Convenience: parse and evaluate in one shot.
    [[nodiscard]] std::expected<PredicateValue, ReqloomError> eval(std::string_view expression,
                                                                   std::string_view jsonBody,
                                                                   int statusCode = 0) const;
};

}  // namespace reqloom::engine
