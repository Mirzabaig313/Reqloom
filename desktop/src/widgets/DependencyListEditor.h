// DependencyListEditor — edits an operation's `depends_on` list as a column of
// pickers. Each row is a combo of existing operation ids, so a dependency can
// never name something that doesn't exist (no undefined refs by construction).
// An always-present trailing blank row grows the list, mirroring KeyValueEditor.
#pragma once

#include "../theming/Theme.h"

#include <QtCore/QStringList>
#include <QtWidgets/QWidget>

#include <string>
#include <vector>

class QVBoxLayout;

namespace reqloom::desktop::widgets {

class DependencyListEditor : public QWidget {
    Q_OBJECT

public:
    explicit DependencyListEditor(QWidget* parent = nullptr);
    ~DependencyListEditor() override;

    DependencyListEditor(const DependencyListEditor&) = delete;
    DependencyListEditor& operator=(const DependencyListEditor&) = delete;
    DependencyListEditor(DependencyListEditor&&) = delete;
    DependencyListEditor& operator=(DependencyListEditor&&) = delete;

    void setTheme(const theming::Theme& theme);

    /// Operation ids the user may pick (typically every op except the one being
    /// edited). Resets the rows to a single blank picker.
    void setCandidates(const QStringList& operationIds);

    /// Preload selected dependencies (ignored if not in the candidate set).
    void setDependencies(const std::vector<std::string>& dependencies);

    /// Current picks in row order, de-duplicated, blanks dropped.
    [[nodiscard]] std::vector<std::string> dependencies() const;

signals:
    void changed();

private:
    void addRow(const QString& selected, bool focus);
    void syncRows();

    QVBoxLayout* rows_{nullptr};
    QStringList candidates_;
    theming::Theme theme_{theming::Theme::resolve(theming::Appearance::Dark)};
};

}  // namespace reqloom::desktop::widgets
