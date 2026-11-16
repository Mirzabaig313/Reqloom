// LayoutSolver — QML-facing facade over the pure ConstraintLayout solver and a font-metrics helper.
// Stateless singleton; safe to call from bindings.
#pragma once

#include <QtQml/qqmlregistration.h>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantList>

namespace reqloom::desktop::qml {

class LayoutSolver : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit LayoutSolver(QObject* parent = nullptr);

    /// Widest advance of `items` in font `family` at `pixelSize`, plus `padding`; 0 for an empty
    /// set.
    Q_INVOKABLE [[nodiscard]] qreal contentWidth(const QStringList& items,
                                                 int pixelSize,
                                                 const QString& family,
                                                 qreal padding) const;

    /// Solve a row of segments (`{ minimum, preferred, maximum, stretch }`, all optional) across
    /// `available` length. Returns `{ offset, size }` in input order.
    Q_INVOKABLE [[nodiscard]] QVariantList solveLinear(const QVariantList& segments,
                                                       qreal available,
                                                       qreal spacing) const;
};

}  // namespace reqloom::desktop::qml
