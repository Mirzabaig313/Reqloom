// Window-layout persistence: splitter sizes and density mode (DESIGN.md §5.2,
// §5.3). Stateless helpers over a caller-supplied QSettings, matching the
// EnvironmentSettings pattern so tests can pass an isolated store.
#pragma once

#include <QtCore/QList>
#include <QtCore/QString>

#include <cstdint>

class QSettings;

namespace chainapi::desktop {

/// Row density (DESIGN.md §5.3). Comfortable is the default; Compact tightens
/// list rows for users with hundreds of operations.
enum class Density : std::uint8_t { Comfortable, Compact };

class LayoutSettings {
public:
    /// Persist splitter handle sizes under a named key (e.g. "main", "right").
    static void saveSplitter(QSettings& settings, const QString& key, const QList<int>& sizes);

    /// Load splitter sizes for `key`, or an empty list when none stored (the
    /// caller then falls back to its default stretch factors).
    [[nodiscard]] static QList<int> loadSplitter(QSettings& settings, const QString& key);

    static void saveDensity(QSettings& settings, Density density);
    [[nodiscard]] static Density loadDensity(QSettings& settings);
};

}  // namespace chainapi::desktop
