// PanelHeader — a consistent header strip for the workbench panels. A title
// in the `subtitle` type style with an optional trailing actions area, so the
// explorer / request / response / timeline panels read with the same rhythm
// (DESIGN.md §5, §6). Token-driven; refreshes on theme change.
#pragma once

#include "../theming/Theme.h"

#include <QtWidgets/QWidget>

class QHBoxLayout;
class QLabel;

namespace chainapi::desktop::widgets {

class PanelHeader : public QWidget {
    Q_OBJECT

public:
    explicit PanelHeader(const QString& title, QWidget* parent = nullptr);
    ~PanelHeader() override;

    PanelHeader(const PanelHeader&) = delete;
    PanelHeader& operator=(const PanelHeader&) = delete;
    PanelHeader(PanelHeader&&) = delete;
    PanelHeader& operator=(PanelHeader&&) = delete;

    void setTitle(const QString& title);

    /// Add a trailing control (button, badge) to the header's right side.
    void addTrailingWidget(QWidget* widget);

    void setTheme(const theming::Theme& theme);

private:
    QLabel* titleLabel_{nullptr};
    QHBoxLayout* layout_{nullptr};
    theming::Theme theme_{theming::Theme::resolve(theming::Appearance::Dark)};
};

}  // namespace chainapi::desktop::widgets
