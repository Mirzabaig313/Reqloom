// FuzzyMatch — see header. Greedy subsequence match with a contiguity/word-
// boundary bonus. Good enough for a few hundred operations; not a full
// Smith-Waterman.
#include "FuzzyMatch.h"

namespace chainapi::desktop::widgets::fuzzy {

namespace {

[[nodiscard]] bool isWordBoundary(const QString& s, int index) {
    if (index == 0) {
        return true;
    }
    const QChar prev = s.at(index - 1);
    return prev == QLatin1Char('.') || prev == QLatin1Char('_') || prev == QLatin1Char('-') ||
           prev == QLatin1Char('/') || prev == QLatin1Char(' ');
}

}  // namespace

bool matches(const QString& query, const QString& candidate) {
    if (query.isEmpty()) {
        return true;
    }
    int qi = 0;
    for (int ci = 0; ci < candidate.size() && qi < query.size(); ++ci) {
        if (candidate.at(ci).toLower() == query.at(qi).toLower()) {
            ++qi;
        }
    }
    return qi == query.size();
}

int score(const QString& query, const QString& candidate) {
    if (query.isEmpty()) {
        return 0;
    }

    constexpr int kBaseHit = 8;
    constexpr int kContiguousBonus = 6;
    constexpr int kWordStartBonus = 10;
    constexpr int kLeadingGapPenalty = 1;

    int total = 0;
    int qi = 0;
    int lastMatch = -2;
    int firstMatch = -1;

    for (int ci = 0; ci < candidate.size() && qi < query.size(); ++ci) {
        if (candidate.at(ci).toLower() != query.at(qi).toLower()) {
            continue;
        }
        if (firstMatch < 0) {
            firstMatch = ci;
        }
        total += kBaseHit;
        if (ci == lastMatch + 1) {
            total += kContiguousBonus;
        }
        if (isWordBoundary(candidate, ci)) {
            total += kWordStartBonus;
        }
        lastMatch = ci;
        ++qi;
    }

    if (qi != query.size()) {
        return -1;  // not a full subsequence match
    }

    // Prefer matches that start nearer the front of the candidate.
    total -= firstMatch * kLeadingGapPenalty;
    // Mild preference for shorter candidates on equal hits.
    total -= static_cast<int>(candidate.size()) / 8;
    return total;
}

}  // namespace chainapi::desktop::widgets::fuzzy
