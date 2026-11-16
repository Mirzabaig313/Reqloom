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
        const std::optional<int> size = storedInt(value);
        if (!size) {
            return {};
        }
        sizes.append(*size);
    }
    return sizes;
}

std::optional<int> LayoutSettings::storedInt(const QVariant& value) {
    if (value.metaType().id() == QMetaType::Int) {
        return value.toInt();
    }
    if (value.metaType().id() != QMetaType::QString) {
        return std::nullopt;
    }
    bool ok = false;
    const int parsed = value.toString().toInt(&ok, 10);
    return ok ? std::optional<int>{parsed} : std::nullopt;
}

std::optional<bool> LayoutSettings::storedBool(const QVariant& value) {
    if (value.metaType().id() == QMetaType::Bool) {
        return value.toBool();
    }
    if (value.metaType().id() != QMetaType::QString) {
        return std::nullopt;
    }
    const QString text = value.toString();
    if (text == QLatin1String("true")) {
        return true;
    }
    if (text == QLatin1String("false")) {
        return false;
    }
    return std::nullopt;
}

}  // namespace reqloom::desktop
