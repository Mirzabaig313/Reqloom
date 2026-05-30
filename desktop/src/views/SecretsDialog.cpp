// SecretsDialog — see header. Keychain-backed secret management UI.
#include "SecretsDialog.h"

#include "../application/ProjectModel.h"
#include "../application/SecretManager.h"

#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QVBoxLayout>

namespace chainapi::desktop {

namespace {

// Column layout of the secrets table.
constexpr int kNameColumn = 0;
constexpr int kStatusColumn = 1;

}  // namespace

SecretsDialog::SecretsDialog(SecretManager& secrets,
                             const ProjectModel& project,
                             const theming::Theme& theme,
                             QWidget* parent)
    : QDialog(parent), secrets_(secrets), project_(project), theme_(theme) {
    setWindowTitle(QStringLiteral("Manage Secrets"));
    resize(560, 420);

    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(
        QStringLiteral("Secrets referenced by this project via {{secret.NAME}}. Values are stored "
                       "in your OS keychain and never shown here."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    backendBanner_ = new QLabel(this);
    backendBanner_->setWordWrap(true);
    backendBanner_->setStyleSheet(
        QStringLiteral("color: %1; font-weight: bold;")
            .arg(theme_.status(theming::StatusToken::Error).name(QColor::HexRgb)));
    backendBanner_->setVisible(false);
    layout->addWidget(backendBanner_);

    table_ = new QTableWidget(this);
    table_->setColumnCount(2);
    table_->setHorizontalHeaderLabels({QStringLiteral("Secret"), QStringLiteral("Status")});
    table_->horizontalHeader()->setSectionResizeMode(kNameColumn, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(kStatusColumn, QHeaderView::ResizeToContents);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    layout->addWidget(table_, 1);

    auto* actionRow = new QHBoxLayout;
    setButton_ = new QPushButton(QStringLiteral("Set / Update…"), this);
    clearButton_ = new QPushButton(QStringLiteral("Clear"), this);
    actionRow->addWidget(setButton_);
    actionRow->addWidget(clearButton_);
    actionRow->addStretch(1);
    layout->addLayout(actionRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(buttons);

    connect(setButton_, &QPushButton::clicked, this, &SecretsDialog::onSetSelected);
    connect(clearButton_, &QPushButton::clicked, this, &SecretsDialog::onClearSelected);
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this]() {
        const bool hasSelection = !selectedSecretName().isEmpty();
        setButton_->setEnabled(hasSelection);
        clearButton_->setEnabled(hasSelection);
    });
    // The Close button is a reject action; map it to reject() so the dialog's
    // result reflects "dismissed" rather than "accepted".
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (!secrets_.backendAvailable()) {
        backendBanner_->setText(QStringLiteral(
            "No OS keychain backend is available in this build. Secrets cannot be stored — "
            "values you enter will not persist."));
        backendBanner_->setVisible(true);
        setButton_->setEnabled(false);
    }

    refresh();
}

SecretsDialog::~SecretsDialog() = default;

QString SecretsDialog::selectedSecretName() const {
    const auto selected = table_->selectedItems();
    if (selected.isEmpty()) {
        return {};
    }
    auto* nameItem = table_->item(selected.first()->row(), kNameColumn);
    return nameItem != nullptr ? nameItem->text() : QString{};
}

void SecretsDialog::refresh() {
    const auto entries = secrets_.referencedSecrets(project_);

    table_->setRowCount(static_cast<int>(entries.size()));
    int row = 0;
    for (const auto& entry : entries) {
        auto* nameItem = new QTableWidgetItem(entry.name);
        table_->setItem(row, kNameColumn, nameItem);

        QString statusText;
        QColor statusColor;
        switch (entry.state) {
            case SecretState::Set:
                statusText = QStringLiteral("● set");
                statusColor = theme_.status(theming::StatusToken::Success);
                break;
            case SecretState::NotSet:
                statusText = QStringLiteral("○ not set");
                statusColor = theme_.status(theming::StatusToken::Warning);
                break;
            case SecretState::ReadError:
                statusText = QStringLiteral("⚠ %1").arg(entry.detail);
                statusColor = theme_.status(theming::StatusToken::Error);
                break;
        }
        auto* statusItem = new QTableWidgetItem(statusText);
        statusItem->setForeground(QBrush(statusColor));
        table_->setItem(row, kStatusColumn, statusItem);
        ++row;
    }

    if (entries.isEmpty()) {
        table_->setRowCount(1);
        auto* empty = new QTableWidgetItem(QStringLiteral("This project references no secrets."));
        empty->setForeground(QBrush(theme_.palette().textSecondary));
        // Non-selectable / disabled so it can't be acted on as if it were a
        // real secret (selectedSecretName would otherwise return this text).
        empty->setFlags(Qt::NoItemFlags);
        table_->setItem(0, kNameColumn, empty);
        table_->setSpan(0, 0, 1, 2);
    }

    setButton_->setEnabled(false);
    clearButton_->setEnabled(false);
}

void SecretsDialog::onSetSelected() {
    const QString name = selectedSecretName();
    if (name.isEmpty()) {
        return;
    }

    bool ok = false;
    // Masked input (Password echo) so the value isn't shoulder-surfed and
    // never lands in a visible field.
    const QString value =
        QInputDialog::getText(this,
                              QStringLiteral("Set secret: %1").arg(name),
                              QStringLiteral("Value for %1 (stored in the OS keychain):").arg(name),
                              QLineEdit::Password,
                              QString{},
                              &ok);
    if (!ok) {
        return;
    }
    if (value.isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("Empty value"),
                             QStringLiteral("Secret value cannot be empty. Use Clear to remove a "
                                            "secret instead."));
        return;
    }

    QString error;
    if (!secrets_.store(name, value, error)) {
        QMessageBox::critical(this,
                              QStringLiteral("Keychain error"),
                              QStringLiteral("Could not store '%1':\n%2").arg(name, error));
    }
    refresh();
}

void SecretsDialog::onClearSelected() {
    const QString name = selectedSecretName();
    if (name.isEmpty()) {
        return;
    }

    const auto choice =
        QMessageBox::question(this,
                              QStringLiteral("Clear secret"),
                              QStringLiteral("Remove '%1' from the OS keychain?").arg(name),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No);
    if (choice != QMessageBox::Yes) {
        return;
    }

    QString error;
    if (!secrets_.clear(name, error)) {
        QMessageBox::critical(this,
                              QStringLiteral("Keychain error"),
                              QStringLiteral("Could not clear '%1':\n%2").arg(name, error));
    }
    refresh();
}

}  // namespace chainapi::desktop
