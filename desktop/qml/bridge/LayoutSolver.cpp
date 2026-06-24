// LayoutSolver — see header. Bridges QML to ConstraintLayout + QFontMetricsF.
#include "LayoutSolver.h"

#include "../../src/widgets/ConstraintLayout.h"

#include <QtGui/QFont>
#include <QtGui/QFontMetricsF>

#include <limits>
#include <vector>

namespace reqloom::desktop::qml {

namespace layout = reqloom::desktop::layout;

LayoutSolver::LayoutSolver(QObject* parent) : QObject(parent) {}

qreal LayoutSolver::contentWidth(const QStringList& items,
                                 int pixelSize,
                                 const QString& family,
                                 qreal padding) const {
    if (items.isEmpty()) {
        return 0.0;
    }
    QFont font{family};
    if (pixelSize > 0) {
        font.setPixelSize(pixelSize);
    }
    const QFontMetricsF metrics{font};
    qreal widest = 0.0;
    for (const QString& item : items) {
        widest = std::max(widest, metrics.horizontalAdvance(item));
    }
    return widest + padding;
}

QVariantList LayoutSolver::solveLinear(const QVariantList& segments,
                                       qreal available,
                                       qreal spacing) const {
    std::vector<layout::Constraint> constraints;
    constraints.reserve(static_cast<std::size_t>(segments.size()));
    for (const QVariant& entry : segments) {
        const QVariantMap map = entry.toMap();
        layout::Constraint c;
        c.minimum = map.value(QStringLiteral("minimum"), 0.0).toDouble();
        c.preferred = map.value(QStringLiteral("preferred"), 0.0).toDouble();
        c.maximum = map.contains(QStringLiteral("maximum"))
                        ? map.value(QStringLiteral("maximum")).toDouble()
                        : std::numeric_limits<double>::max();
        c.stretch = map.value(QStringLiteral("stretch"), 0.0).toDouble();
        constraints.push_back(c);
    }

    const auto placements = layout::solveLinear(constraints, available, spacing);

    QVariantList out;
    out.reserve(static_cast<qsizetype>(placements.size()));
    for (const auto& placement : placements) {
        QVariantMap entry;
        entry.insert(QStringLiteral("offset"), placement.offset);
        entry.insert(QStringLiteral("size"), placement.size);
        out.append(entry);
    }
    return out;
}

}  // namespace reqloom::desktop::qml
