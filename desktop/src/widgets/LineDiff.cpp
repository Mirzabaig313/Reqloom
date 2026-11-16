// LineDiff — see header. Myers' O((N+M)·D) diff with the linear-space middle-snake refinement
// (Myers 1986, §4b), over lines interned to integers.
#include "LineDiff.h"

#include <QtCore/QHash>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace reqloom::desktop::widgets::diff {

namespace {

using Kind = DiffLine::Kind;

// Inner-loop steps allowed across one diff: about a second of GUI-thread time in a debug build.
constexpr std::int64_t kMaxWork = 25'000'000;

struct Edit {
    Kind kind{Kind::Context};
    // Index into the old lines for Context/Removed, into the new lines for Added.
    int index{0};
};

[[nodiscard]] std::size_t at(int i) {
    return static_cast<std::size_t>(i);
}

class Differ {
public:
    Differ(const std::vector<int>& a, const std::vector<int>& b) : a_(a), b_(b) {}

    /// Diff the whole input; nullopt when the work budget ran out.
    [[nodiscard]] std::optional<std::vector<Edit>> run() {
        diff(0, static_cast<int>(a_.size()), 0, static_cast<int>(b_.size()));
        if (aborted_) {
            return std::nullopt;
        }
        return std::move(edits_);
    }

private:
    void diff(int aLo, int aHi, int bLo, int bHi) {
        if (aborted_) {
            return;
        }
        int prefix = 0;
        while (aLo + prefix < aHi && bLo + prefix < bHi &&
               a_[at(aLo + prefix)] == b_[at(bLo + prefix)]) {
            ++prefix;
        }
        for (int i = 0; i < prefix; ++i) {
            edits_.push_back(Edit{Kind::Context, aLo + i});
        }
        aLo += prefix;
        bLo += prefix;
        int suffix = 0;
        while (aLo < aHi - suffix && bLo < bHi - suffix &&
               a_[at(aHi - 1 - suffix)] == b_[at(bHi - 1 - suffix)]) {
            ++suffix;
        }
        work_ += prefix + suffix;
        middle(aLo, aHi - suffix, bLo, bHi - suffix);
        for (int i = 0; i < suffix; ++i) {
            edits_.push_back(Edit{Kind::Context, aHi - suffix + i});
        }
    }

    void middle(int aLo, int aHi, int bLo, int bHi) {
        if (aLo == aHi || bLo == bHi) {
            replace(aLo, aHi, bLo, bHi);
            return;
        }
        const auto split = bisect(aLo, aHi, bLo, bHi);
        if (aborted_) {
            return;
        }
        const bool progresses =
            split && *split != std::pair{aLo, bLo} && *split != std::pair{aHi, bHi};
        if (!progresses) {
            replace(aLo, aHi, bLo, bHi);
            return;
        }
        diff(aLo, split->first, bLo, split->second);
        diff(split->first, aHi, split->second, bHi);
    }

    void replace(int aLo, int aHi, int bLo, int bHi) {
        for (int i = aLo; i < aHi; ++i) {
            edits_.push_back(Edit{Kind::Removed, i});
        }
        for (int j = bLo; j < bHi; ++j) {
            edits_.push_back(Edit{Kind::Added, j});
        }
    }

    // Find where a shortest edit path crosses the middle by running forward and reverse searches
    // until they overlap; returns that point in absolute indices, nullopt when nothing is shared.
    [[nodiscard]] std::optional<std::pair<int, int>> bisect(int aLo, int aHi, int bLo, int bHi) {
        const int n = aHi - aLo;
        const int m = bHi - bLo;
        const int maxD = (n + m + 1) / 2;
        const int vOffset = maxD;
        const int vLength = 2 * maxD + 2;
        std::vector<int> v1(at(vLength), -1);
        std::vector<int> v2(at(vLength), -1);
        v1[at(vOffset + 1)] = 0;
        v2[at(vOffset + 1)] = 0;
        const int delta = n - m;
        const bool front = delta % 2 != 0;
        int k1start = 0;
        int k1end = 0;
        int k2start = 0;
        int k2end = 0;
        for (int d = 0; d < maxD; ++d) {
            for (int k1 = -d + k1start; k1 <= d - k1end; k1 += 2) {
                const int k1Offset = vOffset + k1;
                int x1 = (k1 == -d || (k1 != d && v1[at(k1Offset - 1)] < v1[at(k1Offset + 1)]))
                             ? v1[at(k1Offset + 1)]
                             : v1[at(k1Offset - 1)] + 1;
                int y1 = x1 - k1;
                while (x1 < n && y1 < m && a_[at(aLo + x1)] == b_[at(bLo + y1)]) {
                    ++x1;
                    ++y1;
                    ++work_;
                }
                ++work_;
                v1[at(k1Offset)] = x1;
                if (x1 > n) {
                    k1end += 2;
                } else if (y1 > m) {
                    k1start += 2;
                } else if (front) {
                    const int k2Offset = vOffset + delta - k1;
                    if (k2Offset >= 0 && k2Offset < vLength && v2[at(k2Offset)] != -1 &&
                        x1 >= n - v2[at(k2Offset)]) {
                        return std::pair{aLo + x1, bLo + y1};
                    }
                }
            }
            for (int k2 = -d + k2start; k2 <= d - k2end; k2 += 2) {
                const int k2Offset = vOffset + k2;
                int x2 = (k2 == -d || (k2 != d && v2[at(k2Offset - 1)] < v2[at(k2Offset + 1)]))
                             ? v2[at(k2Offset + 1)]
                             : v2[at(k2Offset - 1)] + 1;
                int y2 = x2 - k2;
                while (x2 < n && y2 < m && a_[at(aLo + n - x2 - 1)] == b_[at(bLo + m - y2 - 1)]) {
                    ++x2;
                    ++y2;
                    ++work_;
                }
                ++work_;
                v2[at(k2Offset)] = x2;
                if (x2 > n) {
                    k2end += 2;
                } else if (y2 > m) {
                    k2start += 2;
                } else if (!front) {
                    const int k1Offset = vOffset + delta - k2;
                    if (k1Offset >= 0 && k1Offset < vLength && v1[at(k1Offset)] != -1) {
                        const int x1 = v1[at(k1Offset)];
                        const int y1 = vOffset + x1 - k1Offset;
                        if (x1 >= n - x2) {
                            return std::pair{aLo + x1, bLo + y1};
                        }
                    }
                }
            }
            if (work_ > kMaxWork) {
                aborted_ = true;
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    const std::vector<int>& a_;
    const std::vector<int>& b_;
    std::vector<Edit> edits_;
    std::int64_t work_{0};
    bool aborted_{false};
};

[[nodiscard]] std::vector<int> intern(const QStringList& lines, QHash<QString, int>& ids) {
    std::vector<int> out;
    out.reserve(static_cast<std::size_t>(lines.size()));
    for (const QString& line : lines) {
        const auto it = ids.constFind(line);
        if (it != ids.constEnd()) {
            out.push_back(it.value());
        } else {
            const int id = static_cast<int>(ids.size());
            ids.insert(line, id);
            out.push_back(id);
        }
    }
    return out;
}

// Lines left on the longer side once the shared prefix and suffix are removed.
[[nodiscard]] std::size_t changedSpan(const std::vector<int>& a, const std::vector<int>& b) {
    std::size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) {
        ++prefix;
    }
    std::size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
           a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) {
        ++suffix;
    }
    return std::max(a.size(), b.size()) - prefix - suffix;
}

// Within each run of changed lines, put every removal before the additions.
void removalsFirst(std::vector<DiffLine>& lines) {
    auto runStart = lines.begin();
    while (runStart != lines.end()) {
        runStart = std::find_if(
            runStart, lines.end(), [](const DiffLine& line) { return line.kind != Kind::Context; });
        const auto runEnd = std::find_if(
            runStart, lines.end(), [](const DiffLine& line) { return line.kind == Kind::Context; });
        std::stable_partition(
            runStart, runEnd, [](const DiffLine& line) { return line.kind == Kind::Removed; });
        runStart = runEnd;
    }
}

}  // namespace

LineDiffResult boundedLineDiff(const QString& oldText, const QString& newText) {
    // Split on newline, keeping empty trailing segments so a blank final line isn't dropped.
    const QStringList oldLines = oldText.split(QLatin1Char('\n'));
    const QStringList newLines = newText.split(QLatin1Char('\n'));
    QHash<QString, int> ids;
    const std::vector<int> a = intern(oldLines, ids);
    const std::vector<int> b = intern(newLines, ids);

    LineDiffResult result;
    if (changedSpan(a, b) > static_cast<std::size_t>(kMaxDiffLines)) {
        result.tooLarge = true;
        return result;
    }
    auto edits = Differ{a, b}.run();
    if (!edits) {
        result.tooLarge = true;
        return result;
    }
    result.lines.reserve(edits->size());
    for (const Edit& edit : *edits) {
        const QString& text =
            edit.kind == Kind::Added ? newLines[edit.index] : oldLines[edit.index];
        result.lines.push_back(DiffLine{edit.kind, text});
    }
    removalsFirst(result.lines);
    return result;
}

std::vector<DiffLine> lineDiff(const QString& oldText, const QString& newText) {
    LineDiffResult result = boundedLineDiff(oldText, newText);
    if (!result.tooLarge) {
        return std::move(result.lines);
    }
    std::vector<DiffLine> out;
    for (const QString& line : oldText.split(QLatin1Char('\n'))) {
        out.push_back(DiffLine{Kind::Removed, line});
    }
    for (const QString& line : newText.split(QLatin1Char('\n'))) {
        out.push_back(DiffLine{Kind::Added, line});
    }
    return out;
}

}  // namespace reqloom::desktop::widgets::diff
