// DependencyListEditor — see header. A column of operation-id pickers with an
// always-present trailing blank row (Apidog ghost-row pattern), built from
// combos so a dependency is always a real, existing operation.
#include "DependencyListEditor.h"

#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <set>

namespace reqloom::desktop::widgets {

namespace {

constexpr auto kRowName = "depRow";
constexpr auto kComboName = "depCombo";
constexpr auto kRemoveName = "depRemove";

}  // namespace

DependencyListEditor::DependencyListEditor(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    rows_ = new QVBoxLayout;
    rows_->setContentsMargins(0, 0, 0, 0);
    rows_->setSpacing(0);
    layout->addLayout(rows_);
    layout->addStretch(1);

    syncRows();
}

DependencyListEditor::~DependencyListEditor() = default;

void DependencyListEditor::setTheme(const theming::Theme& theme) {
    theme_ = theme;
}

void DependencyListEditor::setCandidates(const QStringList& operationIds) {
    candidates_ = operationIds;
    // Drop every row, then re-seed the single trailing blank picker.
    const auto rowWidgets = findChildren<QWidget*>(QString::fromUtf8(kRowName));
    for (QWidget* row : rowWidgets) {
        delete row;
    }
    syncRows();
}

void DependencyListEditor::setDependencies(const std::vector<std::string>& dependencies) {
    const auto rowWidgets = findChildren<QWidget*>(QString::fromUtf8(kRowName));
    for (QWidget* row : rowWidgets) {
        delete row;
    }
    for (const auto& dep : dependencies) {
        const QString value = QString::fromStdString(dep);
        if (candidates_.contains(value)) {
            addRow(value, /*focus=*/false);
        }
    }
    syncRows();
}

std::vector<std::string> DependencyListEditor::dependencies() const {
    std::vector<std::string> out;
    std::set<QString> seen;
    for (int i = 0; i < rows_->count(); ++i) {
        auto* item = rows_->itemAt(i);
        auto* row = item != nullptr ? item->widget() : nullptr;
        if (row == nullptr) {
            continue;
        }
        auto* combo = row->findChild<QComboBox*>(QString::fromUtf8(kComboName));
        if (combo == nullptr || combo->currentIndex() <= 0) {
            continue;  // index 0 is the "add dependency…" placeholder
        }
        const QString value = combo->currentText();
        if (seen.insert(value).second) {
            out.push_back(value.toStdString());
        }
    }
    return out;
}

void DependencyListEditor::addRow(const QString& selected, bool focus) {
    auto* row = new QWidget(this);
    row->setObjectName(QString::fromUtf8(kRowName));
    row->setAttribute(Qt::WA_StyledBackground, true);
    auto* rowLayout = new QHBoxLayout(row);
    const int hPad = theming::Theme::space(theming::Space::Sm);
    const int vPad = theming::Theme::space(theming::Space::Xs);
    rowLayout->setContentsMargins(hPad, vPad, hPad, vPad);
    rowLayout->setSpacing(theming::Theme::space(theming::Space::Xs));

    auto* combo = new QComboBox(row);
    combo->setObjectName(QString::fromUtf8(kComboName));
    combo->addItem(QStringLiteral("+ add dependency…"));  // index 0 = blank
    combo->addItems(candidates_);
    if (!selected.isEmpty()) {
        const int idx = combo->findText(selected);
        if (idx > 0) {
            combo->setCurrentIndex(idx);
        }
    }
    connect(combo, &QComboBox::currentIndexChanged, this, [this](int) {
        syncRows();
        emit changed();
    });
    rowLayout->addWidget(combo, 1);

    auto* remove = new QToolButton(row);
    remove->setObjectName(QString::fromUtf8(kRemoveName));
    remove->setText(QStringLiteral("✕"));
    remove->setToolTip(QStringLiteral("Remove dependency"));
    remove->setAutoRaise(true);
    connect(remove, &QToolButton::clicked, this, [this, row]() {
        rows_->removeWidget(row);
        row->hide();
        row->deleteLater();
        syncRows();
        emit changed();
    });
    rowLayout->addWidget(remove);

    rows_->addWidget(row);
    if (focus) {
        combo->setFocus(Qt::OtherFocusReason);
    }
}

void DependencyListEditor::syncRows() {
    const auto rowIsBlank = [](QWidget* row) {
        auto* combo = row->findChild<QComboBox*>(QString::fromUtf8(kComboName));
        return combo == nullptr || combo->currentIndex() <= 0;
    };

    const int count = rows_->count();
    bool lastBlank = false;
    if (count > 0) {
        auto* lastItem = rows_->itemAt(count - 1);
        auto* lastRow = lastItem != nullptr ? lastItem->widget() : nullptr;
        lastBlank = (lastRow != nullptr) && rowIsBlank(lastRow);
    }
    if (count == 0 || !lastBlank) {
        addRow(QString{}, /*focus=*/false);
    }

    // A blank picker has nothing to remove — hide its ✕.
    for (int i = 0; i < rows_->count(); ++i) {
        auto* item = rows_->itemAt(i);
        auto* row = item != nullptr ? item->widget() : nullptr;
        if (row == nullptr) {
            continue;
        }
        if (auto* remove = row->findChild<QToolButton*>(QString::fromUtf8(kRemoveName))) {
            remove->setVisible(!rowIsBlank(row));
        }
    }
}

}  // namespace reqloom::desktop::widgets
