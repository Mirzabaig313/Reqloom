// Subsequence fuzzy matching for the command palette: a query matches when its characters appear in
// order, and the score rewards contiguous runs and word-boundary starts.
#pragma once

#include <QtCore/QString>

namespace reqloom::desktop::widgets::fuzzy {

/// Whether `query` is a case-insensitive subsequence of `candidate`. An empty
/// query matches everything.
[[nodiscard]] bool matches(const QString& query, const QString& candidate);

/// Match quality, higher is better. Returns a negative value when there is no
/// match. Contiguous and word-start matches score higher.
[[nodiscard]] int score(const QString& query, const QString& candidate);

}  // namespace reqloom::desktop::widgets::fuzzy
