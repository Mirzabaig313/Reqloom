// Line-level LCS diff for the response Diff tab: an ordered list of unchanged / added / removed
// lines. Pure functions.
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cstdint>
#include <vector>

namespace reqloom::desktop::widgets::diff {

struct DiffLine {
    enum class Kind : std::uint8_t {
        Context,  ///< present in both, unchanged
        Added,    ///< present only in the new text
        Removed,  ///< present only in the old text
    };

    Kind kind{Kind::Context};
    QString text;
};

/// Diff `oldText` → `newText` line by line (split on newline); removals precede the additions that
/// replace them.
[[nodiscard]] std::vector<DiffLine> lineDiff(const QString& oldText, const QString& newText);

}  // namespace reqloom::desktop::widgets::diff
