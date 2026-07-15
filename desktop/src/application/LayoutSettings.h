// Named splitter persistence for desktop shell layout.
#pragma once

#include <QtCore/QList>
#include <QtCore/QString>

class QSettings;

namespace reqloom::desktop {

class LayoutSettings {
public:
    /// Persists splitter handle sizes under a named key.
    static void saveSplitter(QSettings& settings, const QString& key, const QList<int>& sizes);

    /// Loads splitter sizes, or an empty list when none are stored.
    [[nodiscard]] static QList<int> loadSplitter(QSettings& settings, const QString& key);
};

}  // namespace reqloom::desktop
