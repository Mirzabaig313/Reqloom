// Text encoding for machine-readable reports: valid UTF-8 for JSON, valid XML 1.0 for JUnit.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace reqloom::cli {

/// `input` with every invalid UTF-8 sequence (including encoded surrogates) replaced by U+FFFD.
[[nodiscard]] std::string toValidUtf8(std::string_view input);

/// `input` escaped for the inside of a JSON string (RFC 8259 §7), invalid UTF-8 as U+FFFD.
[[nodiscard]] std::string jsonEscape(std::string_view input);

/// `input` escaped for XML 1.0 character data; characters XML 1.0 forbids (most C0 controls,
/// U+FFFE, U+FFFF) and invalid UTF-8 become U+FFFD.
[[nodiscard]] std::string xmlText(std::string_view input);

/// As `xmlText`, also escaping quotes, and tab and line breaks so parsers keep them.
[[nodiscard]] std::string xmlAttribute(std::string_view input);

/// `input` as one or more CDATA sections: `]]>` is split across two sections.
[[nodiscard]] std::string xmlCdata(std::string_view input);

/// The first line of `text`, cut to at most `maxBytes` on a UTF-8 boundary with "…" appended
/// when shortened.
[[nodiscard]] std::string firstLine(std::string_view text, std::size_t maxBytes);

}  // namespace reqloom::cli
