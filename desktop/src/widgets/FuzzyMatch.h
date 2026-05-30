// Subsequence fuzzy matching for the command palette. Pure functions, no Qt
// widgets — unit-tested. A query matches a candidate when its characters
// appear in order (not necessarily adjacent); the score rewards contiguous
// runs and word-boundary starts so "ordcr" ranks "order.create" highly.
#pragma once

#include <QtCore/QString>

namespace chainapi::desktop::widgets::fuzzy {

/// Whether `query` is a case-insensitive subsequence of `candidate`. An empty
/// query matches everything.
[[nodiscard]] bool matches(const QString& query, const QString& candidate);

/// Match quality, higher is better. Returns a negative value when there is no
/// match. Contiguous and word-start matches score higher.
[[nodiscard]] int score(const QString& query, const QString& candidate);

}  // namespace chainapi::desktop::widgets::fuzzy
