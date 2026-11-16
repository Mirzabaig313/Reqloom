// Named splitter persistence for desktop shell layout.
#pragma once

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QVariant>

#include <optional>

class QSettings;

namespace reqloom::desktop {

class LayoutSettings {
public:
    /// Persists splitter handle sizes under a named key.
    static void saveSplitter(QSettings& settings, const QString& key, const QList<int>& sizes);

    /// Loads splitter sizes, or an empty list when none are stored.
    [[nodiscard]] static QList<int> loadSplitter(QSettings& settings, const QString& key);

    /// A stored integer: an int, or its decimal string form (INI files return strings).
    [[nodiscard]] static std::optional<int> storedInt(const QVariant& value);

    /// A stored bool: a bool, or exactly "true" / "false" (INI files return strings).
    [[nodiscard]] static std::optional<bool> storedBool(const QVariant& value);
};

}  // namespace reqloom::desktop
