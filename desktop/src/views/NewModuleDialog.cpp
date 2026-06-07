// NewModuleDialog — see header.
#include "NewModuleDialog.h"

#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace reqloom::desktop {

namespace {

/// Mirrors the engine-side guard in ProjectModel: a module name becomes a
/// resource id (dotted scheme) and a file name, so these characters would
/// break the project or escape the resources/ directory.
[[nodiscard]] bool hasIdBreakingChars(const QString& name) {
    return name.contains(QLatin1Char('.')) || name.contains(QLatin1Char('/')) ||
           name.contains(QLatin1Char('\\'));
}

}  // namespace

NewModuleDialog::NewModuleDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("New Module"));
    setModal(true);

    auto* outer = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    nameEdit_ = new QLineEdit(this);
    nameEdit_->setPlaceholderText(QStringLiteral("admin_organization"));
    form->addRow(QStringLiteral("Name"), nameEdit_);

    descriptionEdit_ = new QLineEdit(this);
    descriptionEdit_->setPlaceholderText(QStringLiteral("Org-level admin actions (optional)"));
    form->addRow(QStringLiteral("Description"), descriptionEdit_);
    outer->addLayout(form);

    hintLabel_ = new QLabel(this);
    hintLabel_->setText(QStringLiteral("Creates resources/<name>.yaml"));
    outer->addWidget(hintLabel_);

    errorLabel_ = new QLabel(this);
    errorLabel_->setVisible(false);
    errorLabel_->setWordWrap(true);
    outer->addWidget(errorLabel_);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons_->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Create"));
    outer->addWidget(buttons_);

    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(nameEdit_, &QLineEdit::textChanged, this, [this]() { revalidate(); });

    revalidate();
    nameEdit_->setFocus();
}

NewModuleDialog::~NewModuleDialog() = default;

QString NewModuleDialog::moduleName() const {
    return nameEdit_->text().trimmed();
}

QString NewModuleDialog::description() const {
    return descriptionEdit_->text().trimmed();
}

void NewModuleDialog::revalidate() {
    const QString name = nameEdit_->text().trimmed();
    QString error;
    if (name.isEmpty()) {
        error = QStringLiteral("Name cannot be empty.");
    } else if (hasIdBreakingChars(name)) {
        error = QStringLiteral("Name can't contain '.', '/', or '\\'.");
    }
    errorLabel_->setText(error);
    errorLabel_->setVisible(!error.isEmpty() && !name.isEmpty());
    if (auto* ok = buttons_->button(QDialogButtonBox::Ok); ok != nullptr) {
        ok->setEnabled(error.isEmpty());
    }
}

}  // namespace reqloom::desktop
