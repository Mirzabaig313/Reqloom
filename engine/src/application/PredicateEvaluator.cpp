// PredicateEvaluator — recursive-descent parser with total evaluation (returns False on failure).
#include "PredicateEvaluator.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chainapi::engine {

namespace {

using Json = nlohmann::json;

// AST

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
struct LogicNode {
    LogicOp op;
    NodePtr lhs;
    NodePtr rhs;
};
struct TruthyNode {
    NodePtr inner;
};
struct ArrayNode {
    std::vector<NodePtr> items;
};

}  // namespace

// Defined in the engine namespace so std::unique_ptr<Node> in the header
// stays well-formed.
struct ParsedPredicate::Node {
    std::variant<LiteralNode, JsonPathNode, CompareNode, LogicNode, TruthyNode, ArrayNode> kind;
};

namespace {

// Tokenizer

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
            if (std::isspace(static_cast<unsigned char>(c))) {
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
            } else if (std::isdigit(static_cast<unsigned char>(c)) ||
                       (c == '-' && pos_ + 1 < src_.size() &&
                        std::isdigit(static_cast<unsigned char>(src_[pos_ + 1])))) {
                if (auto t = readNumber(); t) {
                    out.push_back(std::move(*t));
                } else {
                    return std::unexpected(std::move(t).error());
                }
            } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
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
        if (src_[pos_] == '-') ++pos_;
        bool sawDigit = false;
        bool sawDot = false;
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (std::isdigit(static_cast<unsigned char>(c))) {
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
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                ++pos_;
            } else {
                break;
            }
        }
        std::string text{src_.substr(start, pos_ - start)};
        if (text == "true") return Token{Tok::True, {}, start};
        if (text == "false") return Token{Tok::False, {}, start};
        if (text == "null") return Token{Tok::Null, {}, start};
        if (text == "in") return Token{Tok::In, {}, start};
        if (text == "matches") return Token{Tok::Matches, {}, start};
        if (text == "and") return Token{Tok::And, {}, start};
        if (text == "or") return Token{Tok::Or, {}, start};
        return Token{Tok::Ident, std::move(text), start};
    }

    std::string_view src_;
    std::size_t pos_{0};
};

// Parser

class Parser {
public:
    Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    std::expected<NodePtr, std::string> parseExpr() {
        auto root = parseLogicOr();
        if (!root) return std::unexpected(std::move(root).error());
        if (peek().kind != Tok::End) {
            return std::unexpected("unexpected trailing tokens at position " +
                                   std::to_string(peek().pos));
        }
        return root;
    }

private:
    std::expected<NodePtr, std::string> parseLogicOr() {
        auto lhs = parseLogicAnd();
        if (!lhs) return lhs;
        while (peek().kind == Tok::Or) {
            consume();
            auto rhs = parseLogicAnd();
            if (!rhs) return rhs;
            auto node = std::make_unique<ParsedPredicate::Node>();
            node->kind = LogicNode{LogicOp::Or, std::move(*lhs), std::move(*rhs)};
            lhs = std::move(node);
        }
        return lhs;
    }

    std::expected<NodePtr, std::string> parseLogicAnd() {
        auto lhs = parseCompare();
        if (!lhs) return lhs;
        while (peek().kind == Tok::And) {
            consume();
            auto rhs = parseCompare();
            if (!rhs) return rhs;
            auto node = std::make_unique<ParsedPredicate::Node>();
            node->kind = LogicNode{LogicOp::And, std::move(*lhs), std::move(*rhs)};
            lhs = std::move(node);
        }
        return lhs;
    }

    std::expected<NodePtr, std::string> parseCompare() {
        auto lhs = parseTerm();
        if (!lhs) return lhs;

        const auto k = peek().kind;
        const bool isCompareOp = k == Tok::Eq || k == Tok::Neq || k == Tok::Lt || k == Tok::Le ||
                                 k == Tok::Gt || k == Tok::Ge || k == Tok::In || k == Tok::Matches;
        if (!isCompareOp) {
            // Bare term — must be a JSONPath truthiness check.
            // Bare literals cannot stand on their own as a predicate.
            const auto* path = std::get_if<JsonPathNode>(&((*lhs)->kind));
            if (!path) {
                return std::unexpected("expected comparison operator at position " +
                                       std::to_string(peek().pos));
            }
            auto node = std::make_unique<ParsedPredicate::Node>();
            node->kind = TruthyNode{std::move(*lhs)};
            return node;
        }

        const Tok opTok = consume().kind;
        auto rhs = parseTerm();
        if (!rhs) return rhs;

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
        auto node = std::make_unique<ParsedPredicate::Node>();
        node->kind = CompareNode{op, std::move(*lhs), std::move(*rhs)};
        return node;
    }

    std::expected<NodePtr, std::string> parseTerm() {
        const auto& t = peek();
        switch (t.kind) {
            case Tok::Dollar:
                return parseJsonPath();
            // Use parens, not braces, on Json constructors — nlohmann's
            // initializer_list constructor wraps the value in a one-element
            // array, breaking string/bool/null comparisons silently.
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
        consume();  // '['
        ArrayNode arr;
        if (peek().kind != Tok::RBracket) {
            for (;;) {
                auto item = parseTerm();
                if (!item) return item;
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

    const Token& peek() const { return toks_[idx_]; }
    Token consume() { return toks_[idx_++]; }

    std::vector<Token> toks_;
    std::size_t idx_{0};
};

// Evaluator

std::optional<Json> walk(const Json& root, const JsonPathNode& path) {
    const Json* cur = &root;
    for (const auto& seg : path.segments) {
        if (seg.kind == JsonPathNode::SegKind::Field) {
            if (!cur->is_object()) return std::nullopt;
            auto it = cur->find(seg.field);
            if (it == cur->end()) return std::nullopt;
            cur = &(*it);
        } else {
            if (!cur->is_array()) return std::nullopt;
            if (seg.index >= cur->size()) return std::nullopt;
            cur = &((*cur)[seg.index]);
        }
    }
    return std::optional<Json>{*cur};
}

std::optional<Json> resolvePath(const JsonPathNode& path, const Json& body, int statusCode) {
    if (path.isStatusCode) {
        // Body field takes precedence if present.
        if (auto bodyHit = walk(body, path); bodyHit) return bodyHit;
        // Use parens, not braces — brace form produces a one-element array.
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
                    if (!v) return std::nullopt;
                    arr.push_back(std::move(*v));
                }
                return std::optional<Json>{std::move(arr)};
            } else {
                return std::nullopt;
            }
        },
        node.kind);
}

/// JSON-aware comparison. Type-mismatched compares return false rather than
/// erroring — keeps evaluation total.
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
            if (!rhs.is_array()) return false;
            for (const auto& el : rhs) {
                if (el == lhs) return true;
            }
            return false;
        }
        case CompareOp::Matches: {
            if (!lhs.is_string() || !rhs.is_string()) return false;
            try {
                std::regex re{rhs.get<std::string>()};
                return std::regex_search(lhs.get<std::string>(), re);
            } catch (const std::regex_error&) {
                return false;
            }
        }
    }
    return false;
}

bool isTruthy(const Json& v) {
    if (v.is_null()) return false;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0.0;
    if (v.is_string()) return !v.get<std::string>().empty();
    if (v.is_array()) return !v.empty();
    if (v.is_object()) return !v.empty();
    return false;
}

bool evalNode(const ParsedPredicate::Node& node, const Json& body, int statusCode) {
    return std::visit(
        [&](const auto& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, LogicNode>) {
                const bool lhs = evalNode(*n.lhs, body, statusCode);
                if (n.op == LogicOp::And && !lhs) return false;
                if (n.op == LogicOp::Or && lhs) return true;
                return evalNode(*n.rhs, body, statusCode);
            } else if constexpr (std::is_same_v<T, CompareNode>) {
                const auto lhs = evalTerm(*n.lhs, body, statusCode);
                const auto rhs = evalTerm(*n.rhs, body, statusCode);
                if (!lhs || !rhs) return false;
                return compareValues(n.op, *lhs, *rhs);
            } else if constexpr (std::is_same_v<T, TruthyNode>) {
                const auto v = evalTerm(*n.inner, body, statusCode);
                if (!v) return false;
                return isTruthy(*v);
            } else {
                const auto v = evalTerm(node, body, statusCode);
                return v && isTruthy(*v);
            }
        },
        node.kind);
}

}  // namespace

// ParsedPredicate

ParsedPredicate::ParsedPredicate() = default;
ParsedPredicate::~ParsedPredicate() = default;
ParsedPredicate::ParsedPredicate(ParsedPredicate&&) noexcept = default;
ParsedPredicate& ParsedPredicate::operator=(ParsedPredicate&&) noexcept = default;
ParsedPredicate::ParsedPredicate(std::unique_ptr<Node> root) : root_(std::move(root)) {}

// Public API

PredicateEvaluator::PredicateEvaluator() = default;
PredicateEvaluator::~PredicateEvaluator() = default;

std::expected<ParsedPredicate, ChainApiError> PredicateEvaluator::parse(
    std::string_view expression) const {
    Lexer lex{expression};
    auto toks = lex.tokenize();
    if (!toks) {
        return std::unexpected(ChainApiError{
            ErrorCode::SchemaInvalid, ErrorClass::Schema, "predicate: " + toks.error()});
    }
    Parser parser{std::move(*toks)};
    auto root = parser.parseExpr();
    if (!root) {
        return std::unexpected(ChainApiError{
            ErrorCode::SchemaInvalid, ErrorClass::Schema, "predicate: " + root.error()});
    }
    return ParsedPredicate{std::move(*root)};
}

PredicateValue PredicateEvaluator::evaluate(const ParsedPredicate& predicate,
                                            std::string_view jsonBody,
                                            int statusCode) const noexcept {
    try {
        Json body;
        if (!jsonBody.empty()) {
            try {
                body = Json::parse(jsonBody);
            } catch (const std::exception&) {
                // text/plain bodies still work for predicates that only touch $.status_code.
                body = Json::object();
            }
        } else {
            body = Json::object();
        }
        if (!predicate.root_) return PredicateValue::False;
        return evalNode(*predicate.root_, body, statusCode) ? PredicateValue::True
                                                            : PredicateValue::False;
    } catch (...) {
        return PredicateValue::False;
    }
}

std::expected<PredicateValue, ChainApiError> PredicateEvaluator::eval(std::string_view expression,
                                                                      std::string_view jsonBody,
                                                                      int statusCode) const {
    auto p = parse(expression);
    if (!p) return std::unexpected(p.error());
    return evaluate(*p, jsonBody, statusCode);
}

}  // namespace chainapi::engine
