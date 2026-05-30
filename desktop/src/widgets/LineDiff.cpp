// LineDiff — see header. Classic LCS dynamic-programming line diff.
#include "LineDiff.h"

#include <algorithm>
#include <cstddef>

namespace chainapi::desktop::widgets::diff {

namespace {

// Split on '\n', preserving empty trailing segments so a blank final line is
// not silently dropped from the comparison.
[[nodiscard]] QStringList splitLines(const QString& text) {
    return text.split(QLatin1Char('\n'));
}

}  // namespace

std::vector<DiffLine> lineDiff(const QString& oldText, const QString& newText) {
    const QStringList oldLines = splitLines(oldText);
    const QStringList newLines = splitLines(newText);
    const int rows = static_cast<int>(oldLines.size());
    const int cols = static_cast<int>(newLines.size());

    // lcs[i][j] = length of the longest common subsequence of oldLines[i:] and
    // newLines[j:]. Built bottom-up so the forward walk below can greedily
    // follow the longest match. `at(i, j)` hides the size_t indexing so the
    // -Wconversion-clean call sites read in plain ints.
    const auto idx = [](int v) {
        return static_cast<std::size_t>(v);
    };
    std::vector<std::vector<int>> lcs(idx(rows) + 1, std::vector<int>(idx(cols) + 1, 0));
    const auto at = [&lcs, &idx](int i, int j) -> int& {
        return lcs[idx(i)][idx(j)];
    };
    for (int i = rows - 1; i >= 0; --i) {
        for (int j = cols - 1; j >= 0; --j) {
            if (oldLines[i] == newLines[j]) {
                at(i, j) = at(i + 1, j + 1) + 1;
            } else {
                at(i, j) = std::max(at(i + 1, j), at(i, j + 1));
            }
        }
    }

    std::vector<DiffLine> out;
    out.reserve(idx(rows + cols));
    int i = 0;
    int j = 0;
    while (i < rows && j < cols) {
        if (oldLines[i] == newLines[j]) {
            out.push_back(DiffLine{DiffLine::Kind::Context, oldLines[i]});
            ++i;
            ++j;
        } else if (at(i + 1, j) >= at(i, j + 1)) {
            // Dropping the old line keeps an LCS at least as long → removal.
            out.push_back(DiffLine{DiffLine::Kind::Removed, oldLines[i]});
            ++i;
        } else {
            out.push_back(DiffLine{DiffLine::Kind::Added, newLines[j]});
            ++j;
        }
    }
    while (i < rows) {
        out.push_back(DiffLine{DiffLine::Kind::Removed, oldLines[i]});
        ++i;
    }
    while (j < cols) {
        out.push_back(DiffLine{DiffLine::Kind::Added, newLines[j]});
        ++j;
    }
    return out;
}

}  // namespace chainapi::desktop::widgets::diff
