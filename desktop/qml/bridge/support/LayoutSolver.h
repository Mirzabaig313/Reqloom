// LayoutSolver — QML-facing facade over the pure ConstraintLayout solver and a
// font-metrics measurement helper. Lets QML express layout intent directly:
// `contentWidth` measures the widest of a set of strings in the theme font
// (so a combo can size to its widest item instead of a magic constant), and
// `solveLinear` distributes a row's available width across declared segments
// (fixed / filling). Stateless singleton; safe to call from bindings.
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

    /// Widest advance of `items` in font `family` at `pixelSize`, plus
    /// `padding` (room for chevrons / button padding). Returns 0 for an empty
    /// set. Used to size a control to its content rather than a fixed width.
    Q_INVOKABLE [[nodiscard]] qreal contentWidth(const QStringList& items,
                                                 int pixelSize,
                                                 const QString& family,
                                                 qreal padding) const;

    /// Solve a row of segments into placements spanning `available` length.
    /// Each segment is a JS object: `{ minimum, preferred, maximum, stretch }`
    /// (all optional). Returns a list of `{ offset, size }` in input order.
    Q_INVOKABLE [[nodiscard]] QVariantList solveLinear(const QVariantList& segments,
                                                       qreal available,
                                                       qreal spacing) const;
};

}  // namespace reqloom::desktop::qml
