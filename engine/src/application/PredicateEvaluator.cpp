// PredicateEvaluator — recursive-descent parser with total evaluation (returns False on failure).
#include "PredicateEvaluator.h"

#include "../domain/Codecs.h"
#include "../domain/SecretRegistry.h"

#include <reqloom/engine/Predicate.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace reqloom::engine {

namespace {

using Json = nlohmann::json;

struct LiteralNode;
struct JsonPathNode;
struct CompareNode;
struct LogicNode;
struct TruthyNode;
struct ArrayNode;

using NodePtr = std::unique_ptr<ParsedPredicate::Node>;

enum class CompareOp : std::uint8_t { Eq, Neq, Lt, Le, Gt, Ge, In, Matches };
enum class LogicOp : std::uint8_t { And, Or };

struct LiteralNode {
    Json value;
};
struct JsonPathNode {
    // Stored as segments so evaluation walks the JSON tree without re-tokenising.
    enum class SegKind : std::uint8_t { Field, Index };
    struct Seg {
        SegKind kind;
        std::string field;
        std::size_t index{};
    };
    std::vector<Seg> segments;
    bool isStatusCode{false};  ///< $.status_code shortcut, fed from `int`.
};
struct CompareNode {
    CompareOp op;
    NodePtr lhs;
    NodePtr rhs;
};
/// N-ary so a long `a && b && c` chain is one flat node and evaluation and destruction stay
/// iterative.
struct LogicNode {
    LogicOp op;
    std::vector<NodePtr> operands;
};
struct TruthyNode {
    NodePtr inner;
};
struct ArrayNode {
    std::vector<NodePtr> items;
};

}  // namespace

// Defined in the engine namespace so std::unique_ptr<Node> in the header stays well-formed.
struct ParsedPredicate::Node {
    std::variant<LiteralNode, JsonPathNode, CompareNode, LogicNode, TruthyNode, ArrayNode> kind;
};

namespace {

enum class Tok : std::uint8_t {
    End,
    LParen,
    RParen,
    LBracket,
    RBracket,
    Comma,
    Eq,
    Neq,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or,
    Dollar,
    Dot,
    Ident,  ///< [A-Za-z_][A-Za-z0-9_]*
    Number,
    String,  ///< quoted, value already unescaped
    True,
    False,
    Null,
    In,
    Matches,
};

struct Token {
    Tok kind;
    std::string text;  ///< for Ident / Number / String
    std::size_t pos{};
};

class Lexer {
public:
    explicit Lexer(std::string_view src) : src_(src) {}

    [[nodiscard]] std::expected<std::vector<Token>, std::string> tokenize() {
        std::vector<Token> out;
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (std::isspace(static_cast<unsigned char>(c)) != 0) {
                ++pos_;
                continue;
            }
            const auto start = pos_;
            if (c == '(') {
                out.push_back({Tok::LParen, {}, start});
                ++pos_;
            } else if (c == ')') {
                out.push_back({Tok::RParen, {}, start});
                ++pos_;
            } else if (c == '[') {
                out.push_back({Tok::LBracket, {}, start});
                ++pos_;
            } else if (c == ']') {
                out.push_back({Tok::RBracket, {}, start});
                ++pos_;
            } else if (c == ',') {
                out.push_back({Tok::Comma, {}, start});
                ++pos_;
            } else if (c == '$') {
                out.push_back({Tok::Dollar, {}, start});
                ++pos_;
            } else if (c == '.') {
                out.push_back({Tok::Dot, {}, start});
                ++pos_;
            } else if (c == '=' || c == '!' || c == '<' || c == '>') {
                if (auto t = readCompareOp(); t) {
                    out.push_back(*t);
                } else {
                    return std::unexpected(std::move(t).error());
                }
            } else if (c == '&' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '&') {
                out.push_back({Tok::And, {}, start});
                pos_ += 2;
            } else if (c == '|' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '|') {
                out.push_back({Tok::Or, {}, start});
                pos_ += 2;
            } else if (c == '"' || c == '\'') {
                if (auto t = readString(c); t) {
                    out.push_back(std::move(*t));
                } else {
                    return std::unexpected(std::move(t).error());
                }
            } else if ((std::isdigit(static_cast<unsigned char>(c)) != 0) ||
                       (c == '-' && pos_ + 1 < src_.size() &&
                        (std::isdigit(static_cast<unsigned char>(src_[pos_ + 1])) != 0))) {
                if (auto t = readNumber(); t) {
                    out.push_back(std::move(*t));
                } else {
                    return std::unexpected(std::move(t).error());
                }
            } else if ((std::isalpha(static_cast<unsigned char>(c)) != 0) || c == '_') {
                out.push_back(readIdentOrKeyword());
            } else {
                return std::unexpected("unexpected character at position " + std::to_string(pos_));
            }
        }
        out.push_back({Tok::End, {}, pos_});
        return out;
    }

private:
    std::expected<Token, std::string> readCompareOp() {
        const auto start = pos_;
        const char c = src_[pos_];
        const char next = (pos_ + 1 < src_.size()) ? src_[pos_ + 1] : '\0';
        if (c == '=' && next == '=') {
            pos_ += 2;
            return Token{Tok::Eq, {}, start};
        }
        if (c == '!' && next == '=') {
            pos_ += 2;
            return Token{Tok::Neq, {}, start};
        }
        if (c == '<' && next == '=') {
            pos_ += 2;
            return Token{Tok::Le, {}, start};
        }
        if (c == '>' && next == '=') {
            pos_ += 2;
            return Token{Tok::Ge, {}, start};
        }
        if (c == '<') {
            ++pos_;
            return Token{Tok::Lt, {}, start};
        }
        if (c == '>') {
            ++pos_;
            return Token{Tok::Gt, {}, start};
        }
        return std::unexpected(std::string{"bad operator at position "} + std::to_string(start));
    }

    std::expected<Token, std::string> readString(char quote) {
        const auto start = pos_;
        ++pos_;
        std::string buf;
        while (pos_ < src_.size() && src_[pos_] != quote) {
            const char c = src_[pos_];
            if (c == '\\' && pos_ + 1 < src_.size()) {
                const char n = src_[pos_ + 1];
                switch (n) {
                    case 'n':
                        buf.push_back('\n');
                        break;
                    case 't':
                        buf.push_back('\t');
                        break;
                    case 'r':
                        buf.push_back('\r');
                        break;
                    case '\\':
                        buf.push_back('\\');
                        break;
                    case '"':
                        buf.push_back('"');
                        break;
                    case '\'':
                        buf.push_back('\'');
                        break;
                    default:
                        buf.push_back(n);
                        break;
                }
                pos_ += 2;
            } else {
                buf.push_back(c);
                ++pos_;
            }
        }
        if (pos_ >= src_.size()) {
            return std::unexpected("unterminated string starting at position " +
                                   std::to_string(start));
        }
        ++pos_;
        return Token{Tok::String, std::move(buf), start};
    }

    std::expected<Token, std::string> readNumber() {
        const auto start = pos_;
        if (src_[pos_] == '-') {
            ++pos_;
        }
        bool sawDigit = false;
        bool sawDot = false;
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
                sawDigit = true;
                ++pos_;
            } else if (c == '.' && !sawDot) {
                sawDot = true;
                ++pos_;
            } else {
                break;
            }
        }
        if (!sawDigit) {
            return std::unexpected("malformed number at position " + std::to_string(start));
        }
        return Token{Tok::Number, std::string{src_.substr(start, pos_ - start)}, start};
    }

    Token readIdentOrKeyword() {
        const auto start = pos_;
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if ((std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_') {
                ++pos_;
            } else {
                break;
            }
        }
        std::string text{src_.substr(start, pos_ - start)};
        if (text == "true") {
            return Token{Tok::True, {}, start};
        }
        if (text == "false") {
            return Token{Tok::False, {}, start};
        }
        if (text == "null") {
            return Token{Tok::Null, {}, start};
        }
        if (text == "in") {
            return Token{Tok::In, {}, start};
        }
        if (text == "matches") {
            return Token{Tok::Matches, {}, start};
        }
        if (text == "and") {
            return Token{Tok::And, {}, start};
        }
        if (text == "or") {
            return Token{Tok::Or, {}, start};
        }
        return Token{Tok::Ident, std::move(text), start};
    }

    std::string_view src_;
    std::size_t pos_{0};
};

/// Deepest nesting of parentheses / array literals; the parser recurses per level, so this
/// bounds its stack use on hostile input.
constexpr std::size_t kMaxPredicateNesting = 64;

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    std::expected<NodePtr, std::string> parseExpr() {
        auto root = parseLogicOr();
        if (!root) {
            return std::unexpected(std::move(root).error());
        }
        if (peek().kind != Tok::End) {
            return std::unexpected("unexpected trailing tokens at position " +
                                   std::to_string(peek().pos));
        }
        return root;
    }

private:
    std::expected<NodePtr, std::string> parseLogicOr() {
        return parseLogicChain(Tok::Or, LogicOp::Or, &Parser::parseLogicAnd);
    }

    std::expected<NodePtr, std::string> parseLogicAnd() {
        return parseLogicChain(Tok::And, LogicOp::And, &Parser::parseCompare);
    }

    std::expected<NodePtr, std::string> parseLogicChain(
        Tok sep, LogicOp op, std::expected<NodePtr, std::string> (Parser::*operand)()) {
        auto first = (this->*operand)();
        if (!first || peek().kind != sep) {
            return first;
        }
        LogicNode logic{op, {}};
        logic.operands.push_back(std::move(*first));
        while (peek().kind == sep) {
            consume();
            auto next = (this->*operand)();
            if (!next) {
                return next;
            }
            logic.operands.push_back(std::move(*next));
        }
        auto node = std::make_unique<ParsedPredicate::Node>();
        node->kind = std::move(logic);
        return node;
    }

    std::expected<NodePtr, std::string> parseGroup() {
        const auto open = consume();  // '('
        if (auto entered = enterNesting(open.pos); !entered) {
            return std::unexpected(std::move(entered).error());
        }
        auto inner = parseLogicOr();
        --depth_;
        if (!inner) {
            return inner;
        }
        if (peek().kind != Tok::RParen) {
            return std::unexpected("expected ')' to close '(' at position " +
                                   std::to_string(open.pos));
        }
        consume();
        return inner;
    }

    std::expected<void, std::string> enterNesting(std::size_t pos) {
        if (depth_ >= kMaxPredicateNesting) {
            return std::unexpected("predicate nests deeper than " +
                                   std::to_string(kMaxPredicateNesting) + " levels at position " +
                                   std::to_string(pos));
        }
        ++depth_;
        return {};
    }

    std::expected<NodePtr, std::string> parseCompare() {
        if (peek().kind == Tok::LParen) {
            return parseGroup();
        }
        auto lhs = parseTerm();
        if (!lhs) {
            return lhs;
        }

        const auto k = peek().kind;
        const bool isCompareOp = k == Tok::Eq || k == Tok::Neq || k == Tok::Lt || k == Tok::Le ||
                                 k == Tok::Gt || k == Tok::Ge || k == Tok::In || k == Tok::Matches;
        if (!isCompareOp) {
            // A bare term must be a JSONPath truthiness check; bare literals can't stand alone.
            const auto* path = std::get_if<JsonPathNode>(&((*lhs)->kind));
            if (path == nullptr) {
                return std::unexpected("expected comparison operator at position " +
                                       std::to_string(peek().pos));
            }
            auto node = std::make_unique<ParsedPredicate::Node>();
            node->kind = TruthyNode{std::move(*lhs)};
            return node;
        }

        const Tok opTok = consume().kind;
        auto rhs = parseTerm();
        if (!rhs) {
            return rhs;
        }

        CompareOp op = CompareOp::Eq;
        switch (opTok) {
            case Tok::Eq:
                op = CompareOp::Eq;
                break;
            case Tok::Neq:
                op = CompareOp::Neq;
                break;
            case Tok::Lt:
                op = CompareOp::Lt;
                break;
            case Tok::Le:
                op = CompareOp::Le;
                break;
            case Tok::Gt:
                op = CompareOp::Gt;
                break;
            case Tok::Ge:
                op = CompareOp::Ge;
                break;
            case Tok::In:
                op = CompareOp::In;
                break;
            case Tok::Matches:
                op = CompareOp::Matches;
                break;
            default:
                break;
        }
        if (op == CompareOp::Matches) {
            if (const auto* lit = std::get_if<LiteralNode>(&((*rhs)->kind));
                lit != nullptr && lit->value.is_string()) {
                if (auto ok = checkRegexPattern(lit->value.get_ref<const std::string&>()); !ok) {
                    return std::unexpected(std::move(ok).error());
                }
            }
        }
        auto node = std::make_unique<ParsedPredicate::Node>();
        node->kind = CompareNode{op, std::move(*lhs), std::move(*rhs)};
        return node;
    }

    std::expected<NodePtr, std::string> parseTerm() {
        const auto& t = peek();
        switch (t.kind) {
            case Tok::Dollar:
                return parseJsonPath();
            // Parens, not braces: nlohmann's initializer_list constructor wraps the value in a
            // one-element array.
            case Tok::String:
                return literal(Json(consume().text));
            case Tok::Number:
                return parseNumberLiteral();
            case Tok::True:
                consume();
                return literal(Json(true));
            case Tok::False:
                consume();
                return literal(Json(false));
            case Tok::Null:
                consume();
                return literal(Json(nullptr));
            case Tok::LBracket:
                return parseArrayLiteral();
            default:
                return std::unexpected("expected term at position " + std::to_string(t.pos));
        }
    }

    std::expected<NodePtr, std::string> parseJsonPath() {
        consume();  // '$'
        JsonPathNode node;
        while (peek().kind == Tok::Dot || peek().kind == Tok::LBracket) {
            if (peek().kind == Tok::Dot) {
                consume();
                if (peek().kind != Tok::Ident) {
                    return std::unexpected("expected identifier after '.' at position " +
                                           std::to_string(peek().pos));
                }
                node.segments.push_back({JsonPathNode::SegKind::Field, consume().text, 0});
            } else {
                consume();  // '['
                const auto inner = consume();
                if (inner.kind == Tok::Number) {
                    std::size_t idx = 0;
                    auto fc = std::from_chars(
                        inner.text.data(), inner.text.data() + inner.text.size(), idx);
                    if (fc.ec != std::errc{}) {
                        return std::unexpected("bad array index at position " +
                                               std::to_string(inner.pos));
                    }
                    node.segments.push_back({JsonPathNode::SegKind::Index, {}, idx});
                } else if (inner.kind == Tok::String) {
                    node.segments.push_back({JsonPathNode::SegKind::Field, inner.text, 0});
                } else {
                    return std::unexpected("expected index or quoted key at position " +
                                           std::to_string(inner.pos));
                }
                if (peek().kind != Tok::RBracket) {
                    return std::unexpected("expected ']' at position " +
                                           std::to_string(peek().pos));
                }
                consume();
            }
        }
        if (node.segments.size() == 1 && node.segments[0].kind == JsonPathNode::SegKind::Field &&
            node.segments[0].field == "status_code") {
            node.isStatusCode = true;
        }

        auto out = std::make_unique<ParsedPredicate::Node>();
        out->kind = std::move(node);
        return out;
    }

    std::expected<NodePtr, std::string> parseNumberLiteral() {
        const auto t = consume();
        // Round-trip via the JSON parser to let nlohmann decide int vs float.
        try {
            return literal(Json::parse(t.text));
        } catch (const std::exception&) {
            return std::unexpected("malformed number at position " + std::to_string(t.pos));
        }
    }

    std::expected<NodePtr, std::string> parseArrayLiteral() {
        const auto open = consume();  // '['
        if (auto entered = enterNesting(open.pos); !entered) {
            return std::unexpected(std::move(entered).error());
        }
        auto out = parseArrayItems();
        --depth_;
        return out;
    }

    std::expected<NodePtr, std::string> parseArrayItems() {
        ArrayNode arr;
        if (peek().kind != Tok::RBracket) {
            for (;;) {
                auto item = parseTerm();
                if (!item) {
                    return item;
                }
                arr.items.push_back(std::move(*item));
                if (peek().kind == Tok::Comma) {
                    consume();
                    continue;
                }
                break;
            }
        }
        if (peek().kind != Tok::RBracket) {
            return std::unexpected("expected ']' at position " + std::to_string(peek().pos));
        }
        consume();
        auto out = std::make_unique<ParsedPredicate::Node>();
        out->kind = std::move(arr);
        return out;
    }

    NodePtr literal(Json j) {
        auto out = std::make_unique<ParsedPredicate::Node>();
        out->kind = LiteralNode{std::move(j)};
        return out;
    }

    [[nodiscard]] const Token& peek() const { return toks_[idx_]; }
    Token consume() { return toks_[idx_++]; }

    std::vector<Token> toks_;
    std::size_t idx_{0};
    std::size_t depth_{0};
};

std::optional<Json> walk(const Json& root, const JsonPathNode& path) {
    const Json* cur = &root;
    for (const auto& seg : path.segments) {
        if (seg.kind == JsonPathNode::SegKind::Field) {
            if (!cur->is_object()) {
                return std::nullopt;
            }
            auto it = cur->find(seg.field);
            if (it == cur->end()) {
                return std::nullopt;
            }
            cur = &(*it);
        } else {
            if (!cur->is_array()) {
                return std::nullopt;
            }
            if (seg.index >= cur->size()) {
                return std::nullopt;
            }
            cur = &((*cur)[seg.index]);
        }
    }
    return std::optional<Json>{*cur};
}

std::optional<Json> resolvePath(const JsonPathNode& path, const Json& body, int statusCode) {
    if (path.isStatusCode) {
        if (auto bodyHit = walk(body, path); bodyHit) {
            return bodyHit;
        }
        return std::optional<Json>{Json(statusCode)};
    }
    return walk(body, path);
}

std::optional<Json> evalTerm(const ParsedPredicate::Node& node, const Json& body, int statusCode) {
    return std::visit(
        [&](const auto& n) -> std::optional<Json> {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, LiteralNode>) {
                return std::optional<Json>{n.value};
            } else if constexpr (std::is_same_v<T, JsonPathNode>) {
                return resolvePath(n, body, statusCode);
            } else if constexpr (std::is_same_v<T, ArrayNode>) {
                Json arr = Json::array();
                for (const auto& item : n.items) {
                    auto v = evalTerm(*item, body, statusCode);
                    if (!v) {
                        return std::nullopt;
                    }
                    arr.push_back(std::move(*v));
                }
                return std::optional<Json>{std::move(arr)};
            } else {
                return std::nullopt;
            }
        },
        node.kind);
}

/// JSON-aware comparison; type-mismatched compares return false to keep evaluation total.
bool compareValues(CompareOp op, const Json& lhs, const Json& rhs) {
    switch (op) {
        case CompareOp::Eq:
            return lhs == rhs;
        case CompareOp::Neq:
            return lhs != rhs;
        case CompareOp::Lt:
        case CompareOp::Le:
        case CompareOp::Gt:
        case CompareOp::Ge: {
            // Stay in 64-bit integer space when both sides are integers.
            // Coercing to double loses precision for large IDs (e.g. Twitter snowflakes).
            if (lhs.is_number_integer() && rhs.is_number_integer()) {
                const auto a = lhs.get<std::int64_t>();
                const auto b = rhs.get<std::int64_t>();
                switch (op) {
                    case CompareOp::Lt:
                        return a < b;
                    case CompareOp::Le:
                        return a <= b;
                    case CompareOp::Gt:
                        return a > b;
                    case CompareOp::Ge:
                        return a >= b;
                    default:
                        return false;
                }
            }
            if (lhs.is_number() && rhs.is_number()) {
                const double a = lhs.get<double>();
                const double b = rhs.get<double>();
                switch (op) {
                    case CompareOp::Lt:
                        return a < b;
                    case CompareOp::Le:
                        return a <= b;
                    case CompareOp::Gt:
                        return a > b;
                    case CompareOp::Ge:
                        return a >= b;
                    default:
                        return false;
                }
            }
            if (lhs.is_string() && rhs.is_string()) {
                const auto a = lhs.get<std::string>();
                const auto b = rhs.get<std::string>();
                switch (op) {
                    case CompareOp::Lt:
                        return a < b;
                    case CompareOp::Le:
                        return a <= b;
                    case CompareOp::Gt:
                        return a > b;
                    case CompareOp::Ge:
                        return a >= b;
                    default:
                        return false;
                }
            }
            return false;
        }
        case CompareOp::In: {
            if (!rhs.is_array()) {
                return false;
            }
            return std::ranges::any_of(rhs, [&](const auto& el) { return el == lhs; });
        }
        case CompareOp::Matches: {
            if (!lhs.is_string() || !rhs.is_string()) {
                return false;
            }
            // Refused (guard, bad pattern, oversized subject) evaluates false; evaluation is total.
            return guardedRegexSearch(lhs.get_ref<const std::string&>(),
                                      rhs.get_ref<const std::string&>())
                       .outcome == RegexSearchResult::Outcome::Matched;
        }
    }
    return false;
}

bool isTruthy(const Json& v) {
    if (v.is_null()) {
        return false;
    }
    if (v.is_boolean()) {
        return v.get<bool>();
    }
    if (v.is_number()) {
        return v.get<double>() != 0.0;
    }
    if (v.is_string()) {
        return !v.get<std::string>().empty();
    }
    if (v.is_array()) {
        return !v.empty();
    }
    if (v.is_object()) {
        return !v.empty();
    }
    return false;
}

bool evalNode(const ParsedPredicate::Node& node, const Json& body, int statusCode) {
    return std::visit(
        [&](const auto& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, LogicNode>) {
                const bool stopOn = n.op == LogicOp::Or;
                for (const auto& operand : n.operands) {
                    if (evalNode(*operand, body, statusCode) == stopOn) {
                        return stopOn;
                    }
                }
                return !stopOn;
            } else if constexpr (std::is_same_v<T, CompareNode>) {
                const auto lhs = evalTerm(*n.lhs, body, statusCode);
                const auto rhs = evalTerm(*n.rhs, body, statusCode);
                if (!lhs || !rhs) {
                    return false;
                }
                return compareValues(n.op, *lhs, *rhs);
            } else if constexpr (std::is_same_v<T, TruthyNode>) {
                const auto v = evalTerm(*n.inner, body, statusCode);
                if (!v) {
                    return false;
                }
                return isTruthy(*v);
            } else {
                const auto v = evalTerm(node, body, statusCode);
                return v && isTruthy(*v);
            }
        },
        node.kind);
}

/// Builds the failure statement for `PredicateEvaluator::explain`.
class Explainer {
public:
    Explainer(const Json& body, int statusCode, const SecretRegistry* registry)
        : body_(body), statusCode_(statusCode), registry_(registry) {}

    /// Called only for a node that evaluated false.
    [[nodiscard]] std::string why(const ParsedPredicate::Node& node) const {
        return std::visit(
            [&](const auto& n) -> std::string {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, LogicNode>) {
                    return whyLogic(n);
                } else if constexpr (std::is_same_v<T, CompareNode>) {
                    return whyCompare(n);
                } else if constexpr (std::is_same_v<T, TruthyNode>) {
                    return whyFalsy(*n.inner);
                } else {
                    return whyFalsy(node);
                }
            },
            node.kind);
    }

private:
    // `&&` fails at its first false operand; a failed `||` had every operand false, so the first
    // one stands for them.
    [[nodiscard]] std::string whyLogic(const LogicNode& n) const {
        for (const auto& operand : n.operands) {
            if (n.op == LogicOp::Or || !evalNode(*operand, body_, statusCode_)) {
                return why(*operand);
            }
        }
        return {};
    }

    [[nodiscard]] std::string whyFalsy(const ParsedPredicate::Node& term) const {
        const auto v = evalTerm(term, body_, statusCode_);
        if (!v) {
            return label(term) + " was missing";
        }
        return label(term) + " was " + value(*v) + ", expected a truthy value";
    }

    [[nodiscard]] std::string whyCompare(const CompareNode& n) const {
        const auto lhs = evalTerm(*n.lhs, body_, statusCode_);
        const auto rhs = evalTerm(*n.rhs, body_, statusCode_);
        const std::string op = opText(n.op);
        if (!rhs) {
            return label(*n.rhs) + " was missing (right-hand side of " + op + ")";
        }
        const std::string expected =
            n.op == CompareOp::In && rhs->is_array() ? list(*rhs) : shown(*n.rhs, *rhs);
        if (!lhs) {
            return label(*n.lhs) + " was missing, expected " + op + " " + expected;
        }
        if (!isPath(*n.lhs)) {
            return shown(*n.lhs, *lhs) + " " + op + " " + expected + " is false";
        }
        const std::string found = label(*n.lhs) + " was " + value(*lhs);
        if (n.op != CompareOp::Matches) {
            return found + ", expected " + op + " " + expected;
        }
        if (!lhs->is_string() || !rhs->is_string()) {
            return found + ", expected a string matching " + expected;
        }
        const auto search = guardedRegexSearch(lhs->get_ref<const std::string&>(),
                                               rhs->get_ref<const std::string&>());
        if (search.outcome == RegexSearchResult::Outcome::Refused) {
            return found + "; the pattern " + expected + " was refused: " + redact(search.reason);
        }
        return found + ", which does not match " + expected;
    }

    [[nodiscard]] static bool isPath(const ParsedPredicate::Node& node) {
        return std::holds_alternative<JsonPathNode>(node.kind);
    }

    [[nodiscard]] static std::string opText(CompareOp op) {
        switch (op) {
            case CompareOp::Eq:
                return "==";
            case CompareOp::Neq:
                return "!=";
            case CompareOp::Lt:
                return "<";
            case CompareOp::Le:
                return "<=";
            case CompareOp::Gt:
                return ">";
            case CompareOp::Ge:
                return ">=";
            case CompareOp::In:
                return "in";
            case CompareOp::Matches:
                return "matches";
        }
        return "?";
    }

    /// A path as written; any other term as its value.
    [[nodiscard]] std::string label(const ParsedPredicate::Node& node) const {
        const auto* path = std::get_if<JsonPathNode>(&node.kind);
        if (path == nullptr) {
            const auto v = evalTerm(node, body_, statusCode_);
            return v ? value(*v) : std::string{"a value"};
        }
        std::string text = "$";
        for (const auto& seg : path->segments) {
            if (seg.kind == JsonPathNode::SegKind::Index) {
                text += "[" + std::to_string(seg.index) + "]";
            } else if (isIdentifier(seg.field)) {
                text += "." + seg.field;
            } else {
                text += "[" + quote(seg.field) + "]";
            }
        }
        return cut(redact(text));
    }

    /// A path followed by its value in parentheses; any other term as its value.
    [[nodiscard]] std::string shown(const ParsedPredicate::Node& node, const Json& v) const {
        return isPath(node) ? label(node) + " (" + value(v) + ")" : value(v);
    }

    [[nodiscard]] std::string value(const Json& v) const {
        if (v.is_string()) {
            return text(v.get_ref<const std::string&>());
        }
        if (v.is_array()) {
            return std::format("an array of {} item{}", v.size(), v.size() == 1 ? "" : "s");
        }
        if (v.is_object()) {
            return std::format("an object with {} key{}", v.size(), v.size() == 1 ? "" : "s");
        }
        return cut(redact(v.dump()));
    }

    [[nodiscard]] std::string list(const Json& items) const {
        std::string out = "[";
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (out.size() > kMaxExplainedValueBytes) {
                out += std::format(", … ({} items)", items.size());
                break;
            }
            out += (i == 0 ? "" : ", ") + value(items[i]);
        }
        return out + "]";
    }

    /// Quoted; redacted before the cut so no part of a secret survives it.
    [[nodiscard]] std::string text(const std::string& raw) const {
        const std::string masked = redact(raw);
        if (masked.size() <= kMaxExplainedValueBytes) {
            return quote(masked);
        }
        return quote(codecs::truncateUtf8(masked, kMaxExplainedValueBytes) + "…") +
               std::format(" ({} bytes)", masked.size());
    }

    [[nodiscard]] static std::string cut(const std::string& s) {
        if (s.size() <= kMaxExplainedValueBytes) {
            return s;
        }
        return codecs::truncateUtf8(s, kMaxExplainedValueBytes) + "…";
    }

    [[nodiscard]] static std::string quote(const std::string& s) {
        return Json(s).dump(-1, ' ', false, Json::error_handler_t::replace);
    }

    [[nodiscard]] static bool isIdentifier(const std::string& s) {
        return !s.empty() && std::ranges::all_of(s, [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
        });
    }

    [[nodiscard]] std::string redact(std::string_view s) const {
        return registry_ != nullptr ? registry_->redact(s) : std::string{s};
    }

    const Json& body_;
    int statusCode_;
    const SecretRegistry* registry_;
};

}  // namespace

ParsedPredicate::ParsedPredicate() = default;
ParsedPredicate::~ParsedPredicate() = default;
ParsedPredicate::ParsedPredicate(ParsedPredicate&&) noexcept = default;
ParsedPredicate& ParsedPredicate::operator=(ParsedPredicate&&) noexcept = default;
ParsedPredicate::ParsedPredicate(std::unique_ptr<Node> root) : root_(std::move(root)) {}

struct ParsedBody::Impl {
    Json body{Json::object()};  ///< Empty object when the source wasn't valid JSON.
};

ParsedBody::ParsedBody() = default;
ParsedBody::~ParsedBody() = default;
ParsedBody::ParsedBody(ParsedBody&&) noexcept = default;
ParsedBody& ParsedBody::operator=(ParsedBody&&) noexcept = default;
ParsedBody::ParsedBody(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

namespace {

/// A quantifier at `i`: its length (0 when none) and whether it is unbounded.
struct Quantifier {
    std::size_t length{};
    bool unbounded{false};
};

[[nodiscard]] Quantifier readQuantifier(std::string_view p, std::size_t i) {
    Quantifier q;
    if (i >= p.size()) {
        return q;
    }
    if (p[i] == '*' || p[i] == '+' || p[i] == '?') {
        q = {1, p[i] != '?'};
    } else if (p[i] == '{') {
        std::size_t j = i + 1;
        const auto digits = [&] {
            const auto from = j;
            while (j < p.size() && std::isdigit(static_cast<unsigned char>(p[j])) != 0) {
                ++j;
            }
            return j > from;
        };
        if (!digits()) {
            return q;  // a literal '{'
        }
        bool unbounded = false;
        if (j < p.size() && p[j] == ',') {
            ++j;
            unbounded = !digits();
        }
        if (j >= p.size() || p[j] != '}') {
            return q;
        }
        q = {j + 1 - i, unbounded};
    }
    if (q.length > 0 && i + q.length < p.size() && p[i + q.length] == '?') {
        ++q.length;  // lazy form repeats just as far
    }
    return q;
}

[[nodiscard]] std::size_t skipCharClass(std::string_view p, std::size_t i) {
    std::size_t j = i + 1;
    if (j < p.size() && p[j] == '^') {
        ++j;
    }
    if (j < p.size() && p[j] == ']') {
        ++j;  // a leading ']' is literal
    }
    while (j < p.size() && p[j] != ']') {
        j += (p[j] == '\\') ? 2 : 1;
    }
    return std::min(j + 1, p.size());
}

/// Index just past a group's `(` and any `?:`, `?=`, `?<name>` prefix, so its `?` is not
/// read as a quantifier.
[[nodiscard]] std::size_t skipGroupPrefix(std::string_view p, std::size_t i) {
    std::size_t j = i + 1;
    if (j >= p.size() || p[j] != '?') {
        return j;
    }
    ++j;
    if (j < p.size() && (p[j] == ':' || p[j] == '=' || p[j] == '!')) {
        return j + 1;
    }
    if (j < p.size() && p[j] == '<') {
        ++j;
        if (j < p.size() && (p[j] == '=' || p[j] == '!')) {
            return j + 1;
        }
        const auto close = p.find('>', j);
        return close == std::string_view::npos ? p.size() : close + 1;
    }
    return j;
}

/// Heuristic scan for the classic catastrophic-backtracking shape: a group whose content ends
/// in an unbounded repeat and which is itself repeated without bound (`(a+)+`, `(x|y+){2,}`).
/// Escapes, character classes and group prefixes are understood so `\(a+\)+` and `[(+]+` pass.
/// Catches the common shape only; NOT a proof of reasonable run time. Returns the offending
/// group's closing position, or npos.
[[nodiscard]] std::size_t findNestedUnboundedRepeat(std::string_view p) {
    std::vector<bool> altEndsUnbounded;  // one entry per open group
    bool lastUnbounded = false;          // previous token ends in an unbounded repeat
    std::size_t i = 0;
    while (i < p.size()) {
        const char c = p[i];
        if (c == '\\' || c == '[') {
            i = (c == '\\') ? std::min(i + 2, p.size()) : skipCharClass(p, i);
            lastUnbounded = false;
        } else if (c == '(') {
            altEndsUnbounded.push_back(false);
            i = skipGroupPrefix(p, i);
            lastUnbounded = false;
        } else if (c == '|') {
            if (!altEndsUnbounded.empty() && lastUnbounded) {
                altEndsUnbounded.back() = true;
            }
            lastUnbounded = false;
            ++i;
        } else if (c == ')' && !altEndsUnbounded.empty()) {
            const bool groupEndsUnbounded = altEndsUnbounded.back() || lastUnbounded;
            altEndsUnbounded.pop_back();
            const auto q = readQuantifier(p, i + 1);
            if (groupEndsUnbounded && q.unbounded) {
                return i;
            }
            lastUnbounded = q.length > 0 ? q.unbounded : groupEndsUnbounded;
            i += 1 + q.length;
        } else if (const auto q = readQuantifier(p, i); q.length > 0) {
            lastUnbounded = q.unbounded;
            i += q.length;
        } else {
            lastUnbounded = false;
            ++i;
        }
    }
    return std::string_view::npos;
}

/// Compiled patterns shared by every evaluation, so a poll loop or filter compiles each once.
/// Bounded: cleared when full.
class RegexCache {
public:
    std::shared_ptr<const std::regex> get(std::string_view pattern) {
        const std::scoped_lock lock{mutex_};
        if (const auto it = cache_.find(pattern); it != cache_.end()) {
            return it->second;
        }
        std::shared_ptr<const std::regex> compiled;
        try {
            compiled = std::make_shared<const std::regex>(std::string{pattern});
        } catch (const std::regex_error&) {
            compiled = nullptr;
        }
        if (cache_.size() >= kCapacity) {
            cache_.clear();
        }
        cache_.emplace(std::string{pattern}, compiled);
        return compiled;
    }

private:
    static constexpr std::size_t kCapacity = 128;
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<const std::regex>, std::less<>> cache_;
};

RegexCache& regexCache() {
    static RegexCache cache;
    return cache;
}

[[nodiscard]] std::expected<std::shared_ptr<const std::regex>, std::string> compileGuarded(
    std::string_view pattern) {
    if (pattern.size() > kMaxRegexPatternBytes) {
        return std::unexpected("regex pattern is " + std::to_string(pattern.size()) +
                               " bytes; the limit is " + std::to_string(kMaxRegexPatternBytes));
    }
    if (const auto at = findNestedUnboundedRepeat(pattern); at != std::string_view::npos) {
        return std::unexpected(
            "regex '" + std::string{pattern} + "' refused: the group closing at position " +
            std::to_string(at) +
            " ends in an unbounded repeat (+, * or {n,}) and is itself repeated without "
            "bound, which can take exponential time on some inputs; make the inner repeat "
            "bounded (e.g. {1,64}) or remove the outer repeat");
    }
    auto compiled = regexCache().get(pattern);
    if (!compiled) {
        return std::unexpected("regex '" + std::string{pattern} + "' is not a valid pattern");
    }
    return compiled;
}

}  // namespace

std::expected<void, std::string> checkRegexPattern(std::string_view pattern) {
    if (auto compiled = compileGuarded(pattern); !compiled) {
        return std::unexpected(std::move(compiled).error());
    }
    return {};
}

RegexSearchResult guardedRegexSearch(std::string_view subject, std::string_view pattern) {
    using Outcome = RegexSearchResult::Outcome;
    auto compiled = compileGuarded(pattern);
    if (!compiled) {
        return {Outcome::Refused, {}, std::move(compiled).error()};
    }
    if (subject.size() > kMaxRegexSubjectBytes) {
        return {Outcome::Refused,
                {},
                "regex subject is " + std::to_string(subject.size()) + " bytes; the limit is " +
                    std::to_string(kMaxRegexSubjectBytes) + " bytes (1 MiB)"};
    }
    try {
        std::match_results<std::string_view::const_iterator> match;
        if (!std::regex_search(subject.begin(), subject.end(), match, **compiled)) {
            return {Outcome::NoMatch, {}, {}};
        }
        const auto& group = (match.size() >= 2 && match[1].matched) ? match[1] : match[0];
        return {Outcome::Matched, group.str(), {}};
    } catch (const std::regex_error& e) {
        return {Outcome::Refused, {}, std::string{"regex search aborted: "} + e.what()};
    }
}

bool jsonNestingExceeds(std::string_view text, std::size_t limit) noexcept {
    std::size_t depth = 0;
    bool inString = false;
    bool escaped = false;
    for (const char c : text) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
        } else if (c == '"') {
            inString = true;
        } else if (c == '[' || c == '{') {
            if (++depth > limit) {
                return true;
            }
        } else if ((c == ']' || c == '}') && depth > 0) {
            --depth;
        }
    }
    return false;
}

PredicateEvaluator::PredicateEvaluator() = default;

std::expected<ParsedPredicate, ReqloomError> PredicateEvaluator::parse(
    std::string_view expression) const {
    Lexer lex{expression};
    auto toks = lex.tokenize();
    if (!toks) {
        return std::unexpected(ReqloomError{
            ErrorCode::SchemaInvalid, ErrorClass::Schema, "predicate: " + toks.error()});
    }
    Parser parser{std::move(*toks)};
    auto root = parser.parseExpr();
    if (!root) {
        return std::unexpected(ReqloomError{
            ErrorCode::SchemaInvalid, ErrorClass::Schema, "predicate: " + root.error()});
    }
    return ParsedPredicate{std::move(*root)};
}

ParsedBody PredicateEvaluator::parseBody(std::string_view jsonBody) const noexcept {
    auto impl = std::make_unique<ParsedBody::Impl>();
    // A body nested deeper than the limit is treated as non-JSON: walking it could exhaust a
    // worker thread's stack.
    if (!jsonBody.empty() && !jsonNestingExceeds(jsonBody)) {
        try {
            impl->body = Json::parse(jsonBody);
        } catch (...) {
            // text/plain or malformed bodies still let `$.status_code`-only predicates evaluate;
            // keep the default empty object.
            impl->body = Json::object();
        }
    }
    return ParsedBody{std::move(impl)};
}

PredicateValue PredicateEvaluator::evaluate(const ParsedPredicate& predicate,
                                            const ParsedBody& body,
                                            int statusCode) const noexcept {
    try {
        if (!predicate.root_ || !body.impl_) {
            return PredicateValue::False;
        }
        return evalNode(*predicate.root_, body.impl_->body, statusCode) ? PredicateValue::True
                                                                        : PredicateValue::False;
    } catch (...) {
        return PredicateValue::False;
    }
}

PredicateExplanation PredicateEvaluator::explain(const ParsedPredicate& predicate,
                                                 const ParsedBody& body,
                                                 int statusCode,
                                                 const SecretRegistry* registry) const noexcept {
    PredicateExplanation out;
    out.value = evaluate(predicate, body, statusCode);
    if (out.value == PredicateValue::True || !predicate.root_ || !body.impl_) {
        return out;
    }
    try {
        out.detail = Explainer{body.impl_->body, statusCode, registry}.why(*predicate.root_);
    } catch (...) {
        out.detail.clear();
    }
    return out;
}

PredicateValue PredicateEvaluator::evaluate(const ParsedPredicate& predicate,
                                            std::string_view jsonBody,
                                            int statusCode) const noexcept {
    return evaluate(predicate, parseBody(jsonBody), statusCode);
}

std::expected<PredicateValue, ReqloomError> PredicateEvaluator::eval(std::string_view expression,
                                                                     std::string_view jsonBody,
                                                                     int statusCode) const {
    auto p = parse(expression);
    if (!p) {
        return std::unexpected(p.error());
    }
    return evaluate(*p, jsonBody, statusCode);
}

std::expected<void, ReqloomError> validatePredicate(std::string_view expression) {
    const PredicateEvaluator evaluator;
    if (auto parsed = evaluator.parse(expression); !parsed) {
        return std::unexpected(parsed.error());
    }
    return {};
}

std::expected<void, ReqloomError> validateRegexPattern(std::string_view pattern) {
    if (auto ok = checkRegexPattern(pattern); !ok) {
        return std::unexpected(
            ReqloomError{ErrorCode::SchemaInvalid, ErrorClass::Schema, std::move(ok).error()});
    }
    return {};
}

std::expected<bool, ReqloomError> evaluatePredicate(std::string_view expression,
                                                    std::string_view responseBody,
                                                    int statusCode) {
    const PredicateEvaluator evaluator;
    auto result = evaluator.eval(expression, responseBody, statusCode);
    if (!result) {
        return std::unexpected(result.error());
    }
    return *result == PredicateValue::True;
}

}  // namespace reqloom::engine
