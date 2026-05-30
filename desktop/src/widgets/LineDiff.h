// Line-level diff for the response Diff tab (FR-7.5). A standard LCS diff over
// lines, producing an ordered list of unchanged / added / removed lines.
// Pure functions, no Qt widgets — unit-tested.
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cstdint>
#include <vector>

namespace chainapi::desktop::widgets::diff {

struct DiffLine {
    enum class Kind : std::uint8_t {
        Context,  ///< present in both, unchanged
        Added,    ///< present only in the new text
        Removed,  ///< present only in the old text
    };

    Kind kind{Kind::Context};
    QString text;
};

/// Diff `oldText` → `newText` line by line. Lines are split on '\n'. The
/// result is ordered for display: removals appear before the additions that
/// replace them, context lines in place.
[[nodiscard]] std::vector<DiffLine> lineDiff(const QString& oldText, const QString& newText);

}  // namespace chainapi::desktop::widgets::diff
