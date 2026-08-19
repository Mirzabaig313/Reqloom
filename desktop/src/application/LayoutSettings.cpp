// LayoutSettings — see header. QSettings group accessors for window layout.
#include "LayoutSettings.h"

#include <QtCore/QMetaType>
#include <QtCore/QSettings>
#include <QtCore/QVariant>

namespace reqloom::desktop {

namespace {

constexpr const char* kSplitterGroup = "splitterSizes";

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
    for (const QVariant& value : encoded) {
        if (value.metaType().id() != QMetaType::Int) {
            return {};
        }
        sizes.append(value.toInt());
    }
    return sizes;
}

}  // namespace reqloom::desktop
