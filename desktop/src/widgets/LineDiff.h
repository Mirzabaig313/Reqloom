// Line-level diff for the response Diff tab: an ordered list of unchanged / added / removed
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

/// Most lines either side may keep after the shared prefix and suffix are trimmed.
inline constexpr int kMaxDiffLines = 20000;

struct LineDiffResult {
    std::vector<DiffLine> lines;
    /// The inputs exceeded kMaxDiffLines or the work budget; `lines` is empty.
    bool tooLarge{false};
};

/// Minimal line diff of `oldText` → `newText` (split on newline); removals precede the additions
/// that replace them. Memory is linear in the input; time is O((N+M)·D) and is capped, so very
/// different large inputs give `tooLarge` instead of a diff.
[[nodiscard]] LineDiffResult boundedLineDiff(const QString& oldText, const QString& newText);

/// As boundedLineDiff, but inputs that are too large come back as every old line removed and
/// every new line added.
[[nodiscard]] std::vector<DiffLine> lineDiff(const QString& oldText, const QString& newText);

}  // namespace reqloom::desktop::widgets::diff
