// JsonWriter — a small streaming writer for the CLI's JSON documents. The CLI links only the engine
// and Qt6::Core, and hand-concatenated JSON is easy to get wrong once documents nest.
//
// Output is compact for an agent's context: members of the top-level object and of the objects and
// arrays directly inside it go on their own lines; anything nested deeper is written inline.
#pragma once

#include "TextEncoding.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::cli {

class JsonWriter {
public:
    JsonWriter& beginObject() { return open('{'); }
    JsonWriter& endObject() { return close('}'); }
    JsonWriter& beginArray() { return open('['); }
    JsonWriter& endArray() { return close(']'); }

    /// The next member's name; must be followed by exactly one value or container.
    JsonWriter& key(std::string_view name) {
        separate();
        out_ += '"';
        out_ += jsonEscape(name);
        out_ += "\": ";
        afterKey_ = true;
        return *this;
    }

    JsonWriter& string(std::string_view value) {
        separate();
        out_ += '"';
        out_ += jsonEscape(value);
        out_ += '"';
        return *this;
    }
    JsonWriter& stringOrNull(std::string_view value) {
        return value.empty() ? null() : string(value);
    }
    JsonWriter& number(std::int64_t value) {
        separate();
        out_ += std::to_string(value);
        return *this;
    }
    JsonWriter& boolean(bool value) {
        separate();
        out_ += value ? "true" : "false";
        return *this;
    }
    JsonWriter& null() {
        separate();
        out_ += "null";
        return *this;
    }
    JsonWriter& strings(const std::vector<std::string>& values) {
        beginArray();
        for (const auto& value : values) {
            string(value);
        }
        return endArray();
    }

    // Shorthands for one member.
    JsonWriter& field(std::string_view name, std::string_view value) {
        return key(name).string(value);
    }
    JsonWriter& field(std::string_view name, const char* value) { return key(name).string(value); }
    JsonWriter& field(std::string_view name, std::int64_t value) { return key(name).number(value); }
    JsonWriter& field(std::string_view name, int value) { return key(name).number(value); }
    JsonWriter& field(std::string_view name, std::size_t value) {
        return key(name).number(static_cast<std::int64_t>(value));
    }
    JsonWriter& field(std::string_view name, bool value) { return key(name).boolean(value); }
    JsonWriter& fieldOrNull(std::string_view name, std::string_view value) {
        return key(name).stringOrNull(value);
    }
    JsonWriter& fieldOrNull(std::string_view name, std::optional<std::int64_t> value) {
        key(name);
        return value ? number(*value) : null();
    }
    JsonWriter& field(std::string_view name, const std::vector<std::string>& values) {
        return key(name).strings(values);
    }

    /// The document, ending in a newline.
    [[nodiscard]] std::string str() const { return out_ + "\n"; }

private:
    /// Members of containers at depth 1 and 2 go on their own lines.
    static constexpr std::size_t kPrettyDepth = 2;

    struct Level {
        bool empty{true};
    };

    void separate() {
        if (afterKey_) {
            afterKey_ = false;
            return;
        }
        if (levels_.empty()) {
            return;
        }
        auto& level = levels_.back();
        if (!level.empty) {
            out_ += ',';
        }
        level.empty = false;
        if (levels_.size() <= kPrettyDepth) {
            out_ += '\n';
            out_.append(levels_.size() * 2, ' ');
        } else if (out_.back() == ',') {
            out_ += ' ';
        }
    }

    JsonWriter& open(char bracket) {
        separate();
        out_ += bracket;
        levels_.push_back(Level{});
        return *this;
    }

    JsonWriter& close(char bracket) {
        const bool wasEmpty = levels_.back().empty;
        levels_.pop_back();
        if (!wasEmpty && levels_.size() < kPrettyDepth) {
            out_ += '\n';
            out_.append(levels_.size() * 2, ' ');
        }
        out_ += bracket;
        return *this;
    }

    std::string out_;
    std::vector<Level> levels_;
    bool afterKey_{false};
};

}  // namespace reqloom::cli
