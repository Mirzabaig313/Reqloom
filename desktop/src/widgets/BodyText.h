// Text transforms for showing request and response bodies: token-preserving JSON
// pretty-printing and an inert HTML rendering for the rich-text preview.
#pragma once

#include <QtCore/QString>

namespace reqloom::desktop::widgets::body {

/// Re-indent JSON two spaces per level, copying every string and number token exactly as written
/// (no round trip through double). Text that is not JSON comes back unchanged.
///
/// @param unwrapEncoded  When the whole document is a JSON string that itself holds a JSON object
///                       or array, format that inner document instead.
[[nodiscard]] QString prettyJson(const QString& text, bool unwrapEncoded = false);

/// Render HTML (or Markdown when `markdown` is true) to HTML that refers to no external resource:
/// images become a "[image]" placeholder and background images, style sheets and other loadable
/// references are dropped. Nothing is fetched or read while converting.
[[nodiscard]] QString inertRichText(const QString& source, bool markdown);

}  // namespace reqloom::desktop::widgets::body
