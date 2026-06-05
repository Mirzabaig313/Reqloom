// MethodItemDelegate — see header. Left method badge + operation name.
#include "MethodItemDelegate.h"

#include <QtGui/QColor>
#include <QtGui/QFontMetrics>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>

namespace reqloom::desktop::widgets {

namespace {

// Apidog/Postman-style read: the method verb sits immediately to the left of
// the operation name, with a small fixed gap between them. Variable-width
// (no aligned column gutter), so a long verb like OPTIONS still reads tight.
constexpr int kBadgeNameGap = 8;
constexpr int kRowLeftPad = 8;

}  // namespace

MethodItemDelegate::MethodItemDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

MethodItemDelegate::~MethodItemDelegate() = default;

void MethodItemDelegate::setTheme(const theming::Theme& theme) {
    theme_ = theme;
}

void MethodItemDelegate::paint(QPainter* painter,
                               const QStyleOptionViewItem& option,
                               const QModelIndex& index) const {
    const bool isOperation = index.data(roles::kIsOperation).toBool();
    if (!isOperation) {
        // Groups (Actors/Resources/resource folders) keep the default paint.
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    // Let the style draw the row background (selection, hover) but not the
    // text — we render the method badge + name ourselves.
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QString name = opt.text;
    opt.text.clear();
    const QWidget* widget = option.widget;
    QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    const QString method = index.data(roles::kMethodText).toString();
    const QColor methodColor = index.data(roles::kMethodColor).value<QColor>();

    painter->save();
    const QRect rect = option.rect;

    // Method badge — coloured, bold, uppercase. Measured to its actual width
    // so the name sits a small fixed gap to its right (Apidog-style read; no
    // aligned column gutter, so a long verb still reads tight).
    QFont badgeFont = theme_.font(theming::TextStyle::Mono);
    badgeFont.setWeight(QFont::DemiBold);
    painter->setFont(badgeFont);
    painter->setPen(methodColor);
    const QFontMetrics badgeMetrics(badgeFont);
    const int badgeWidth = badgeMetrics.horizontalAdvance(method);
    const QRect badgeRect(rect.left() + kRowLeftPad, rect.top(), badgeWidth, rect.height());
    painter->drawText(badgeRect, Qt::AlignLeft | Qt::AlignVCenter, method);

    // Operation name — primary text, follows the badge on the same line. The
    // selected row paints its text in the highlighted-text colour for contrast.
    const bool selected = (option.state & QStyle::State_Selected) != 0;
    painter->setFont(option.font);
    painter->setPen(selected ? option.palette.highlightedText().color()
                             : theme_.palette().textPrimary);
    const int nameLeft = rect.left() + kRowLeftPad + badgeWidth + kBadgeNameGap;
    const QRect nameRect(
        nameLeft, rect.top(), rect.right() - nameLeft - kRowLeftPad, rect.height());
    const QFontMetrics metrics(option.font);
    const QString elided = metrics.elidedText(name, Qt::ElideRight, nameRect.width());
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, elided);

    painter->restore();
}

QSize MethodItemDelegate::sizeHint(const QStyleOptionViewItem& option,
                                   const QModelIndex& index) const {
    QSize base = QStyledItemDelegate::sizeHint(option, index);
    if (index.data(roles::kIsOperation).toBool()) {
        // Reserve room for a typical verb plus the name beside it. Widest
        // common verb (OPTIONS) keeps the row tall enough; horizontal slack
        // is handled by the parent panel/splitter.
        QFont badgeFont = theme_.font(theming::TextStyle::Mono);
        badgeFont.setWeight(QFont::DemiBold);
        const QFontMetrics badgeMetrics(badgeFont);
        const int verbWidth = badgeMetrics.horizontalAdvance(QStringLiteral("OPTIONS"));
        base.setWidth(base.width() + verbWidth + kBadgeNameGap + (kRowLeftPad * 2));
    }
    return base;
}

}  // namespace reqloom::desktop::widgets
