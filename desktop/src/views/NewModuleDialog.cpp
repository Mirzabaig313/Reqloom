// NewModuleDialog — see header.
#include "NewModuleDialog.h"

#include <QtGui/QColor>
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

NewModuleDialog::NewModuleDialog(const theming::Theme& theme, QWidget* parent)
    : QDialog(parent), theme_(theme) {
    setWindowTitle(QStringLiteral("New Module"));
    setModal(true);
    setMinimumWidth(460);

    const int lg = theming::Theme::space(theming::Space::Lg);
    const int md = theming::Theme::space(theming::Space::Md);
    const int sm = theming::Theme::space(theming::Space::Sm);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(lg, lg, lg, lg);
    outer->setSpacing(md);

    auto* heading = new QLabel(QStringLiteral("New module"), this);
    heading->setFont(theme_.font(theming::TextStyle::Subtitle));
    outer->addWidget(heading);

    auto* intro = new QLabel(
        QStringLiteral("A module groups related endpoints (for example admin_organization). "
                       "It is saved as resources/<name>.yaml."),
        this);
    intro->setWordWrap(true);
    intro->setFont(theme_.font(theming::TextStyle::Caption));
    intro->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme_.palette().textSecondary.name(QColor::HexRgb)));
    outer->addWidget(intro);

    auto* form = new QFormLayout();
    form->setHorizontalSpacing(md);
    form->setVerticalSpacing(sm);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft);

    nameEdit_ = new QLineEdit(this);
    nameEdit_->setPlaceholderText(QStringLiteral("admin_organization"));
    form->addRow(QStringLiteral("Name"), nameEdit_);

    descriptionEdit_ = new QLineEdit(this);
    descriptionEdit_->setPlaceholderText(QStringLiteral("Org-level admin actions (optional)"));
    form->addRow(QStringLiteral("Description"), descriptionEdit_);
    outer->addLayout(form);

    hintLabel_ = new QLabel(this);
    hintLabel_->setFont(theme_.font(theming::TextStyle::Caption));
    hintLabel_->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme_.palette().textSecondary.name(QColor::HexRgb)));
    outer->addWidget(hintLabel_);

    errorLabel_ = new QLabel(this);
    errorLabel_->setVisible(false);
    errorLabel_->setWordWrap(true);
    errorLabel_->setFont(theme_.font(theming::TextStyle::Caption));
    errorLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(
        theme_.status(theming::StatusToken::Error).name(QColor::HexRgb)));
    outer->addWidget(errorLabel_);

    outer->addStretch(1);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons_->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Create module"));
    buttons_->button(QDialogButtonBox::Ok)->setDefault(true);
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
    if (!name.isEmpty() && hasIdBreakingChars(name)) {
        error = QStringLiteral("Name can't contain '.', '/', or '\\'.");
    }

    // Live preview of the file that will be created, or the validation error.
    const bool valid = !name.isEmpty() && error.isEmpty();
    hintLabel_->setVisible(error.isEmpty());
    hintLabel_->setText(valid ? QStringLiteral("Creates resources/%1.yaml").arg(name)
                              : QStringLiteral("Enter a name to create the module."));
    errorLabel_->setText(error);
    errorLabel_->setVisible(!error.isEmpty());
    if (auto* ok = buttons_->button(QDialogButtonBox::Ok); ok != nullptr) {
        ok->setEnabled(valid);
    }
}

}  // namespace reqloom::desktop
