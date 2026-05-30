// LayoutSettings — see header. QSettings group accessors for window layout.
#include "LayoutSettings.h"

#include <QtCore/QSettings>
#include <QtCore/QVariant>

namespace chainapi::desktop {

namespace {

constexpr const char* kSplitterGroup = "splitterSizes";
constexpr const char* kDensityKey = "layout/density";

}  // namespace

void LayoutSettings::saveSplitter(QSettings& settings,
                                  const QString& key,
                                  const QList<int>& sizes) {
    if (key.isEmpty() || sizes.isEmpty()) {
        return;
    }
    QVariantList encoded;
    encoded.reserve(sizes.size());
    for (const int size : sizes) {
        encoded.append(size);
    }
    settings.beginGroup(QString::fromUtf8(kSplitterGroup));
    settings.setValue(key, encoded);
    settings.endGroup();
}

QList<int> LayoutSettings::loadSplitter(QSettings& settings, const QString& key) {
    if (key.isEmpty()) {
        return {};
    }
    settings.beginGroup(QString::fromUtf8(kSplitterGroup));
    const QVariantList encoded = settings.value(key).toList();
    settings.endGroup();

    QList<int> sizes;
    sizes.reserve(encoded.size());
    for (const QVariant& v : encoded) {
        bool ok = false;
        const int size = v.toInt(&ok);
        // A corrupt or non-integer entry invalidates the whole record: fall
        // back to defaults rather than restore a half-broken layout.
        if (!ok) {
            return {};
        }
        sizes.append(size);
    }
    return sizes;
}

void LayoutSettings::saveDensity(QSettings& settings, Density density) {
    settings.setValue(
        QString::fromUtf8(kDensityKey),
        density == Density::Compact ? QStringLiteral("compact") : QStringLiteral("comfortable"));
}

Density LayoutSettings::loadDensity(QSettings& settings) {
    const QString value = settings.value(QString::fromUtf8(kDensityKey)).toString();
    return value == QStringLiteral("compact") ? Density::Compact : Density::Comfortable;
}

}  // namespace chainapi::desktop
