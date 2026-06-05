// MethodItemDelegate — paints explorer operation rows as a left-aligned,
// colour-coded HTTP method badge followed by the operation name (the
// Postman/Apidog read), so the verb and name sit on one line and move together
// with tree indentation. Group/folder rows fall through to the default paint.
// DESIGN.md §14.1 item 1: theme-reactive custom painting via QStyledItemDelegate
// is the explorer's highest-leverage treatment.
#pragma once

#include "../theming/Theme.h"

#include <QtCore/Qt>
#include <QtWidgets/QStyledItemDelegate>

namespace reqloom::desktop::widgets {

/// Item-data roles shared between the explorer (which writes them) and the
/// delegate (which reads them). Kept in one place so the two never drift.
namespace roles {
constexpr int kOperationId = Qt::UserRole + 1;  ///< QString fully-qualified op id
constexpr int kIsOperation = Qt::UserRole + 2;  ///< bool: operation row vs. group
constexpr int kMethodText = Qt::UserRole + 3;   ///< QString verb, e.g. "POST"
constexpr int kMethodColor = Qt::UserRole + 4;  ///< QColor resolved for the theme
}  // namespace roles

class MethodItemDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit MethodItemDelegate(QObject* parent = nullptr);
    ~MethodItemDelegate() override;

    MethodItemDelegate(const MethodItemDelegate&) = delete;
    MethodItemDelegate& operator=(const MethodItemDelegate&) = delete;
    MethodItemDelegate(MethodItemDelegate&&) = delete;
    MethodItemDelegate& operator=(MethodItemDelegate&&) = delete;

    void setTheme(const theming::Theme& theme);

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override;

private:
    theming::Theme theme_{theming::Theme::resolve(theming::Appearance::Dark)};
};

}  // namespace reqloom::desktop::widgets
