// NewEndpointDialog — see header.
#include "NewEndpointDialog.h"

#include <QtCore/QVariant>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <array>
#include <cstdint>
#include <utility>

namespace reqloom::desktop {

namespace {

[[nodiscard]] bool hasIdBreakingChars(const QString& name) {
    return name.contains(QLatin1Char('.')) || name.contains(QLatin1Char('/')) ||
           name.contains(QLatin1Char('\\'));
}

/// Method label → engine value. The label is what the combo shows; the value
/// is stored in the item's userData so selection round-trips without parsing.
constexpr std::array<std::pair<const char*, engine::HttpMethod>, 7> kMethods{{
    {"GET", engine::HttpMethod::Get},
    {"POST", engine::HttpMethod::Post},
    {"PUT", engine::HttpMethod::Put},
    {"PATCH", engine::HttpMethod::Patch},
    {"DELETE", engine::HttpMethod::Delete},
    {"HEAD", engine::HttpMethod::Head},
    {"OPTIONS", engine::HttpMethod::Options},
}};

}  // namespace

NewEndpointDialog::NewEndpointDialog(const QStringList& resources,
                                     const QStringList& actors,
                                     const QString& preselectedResource,
                                     QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("New Endpoint"));
    setModal(true);

    auto* outer = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    resourceCombo_ = new QComboBox(this);
    resourceCombo_->addItems(resources);
    if (!preselectedResource.isEmpty()) {
        const int idx = resourceCombo_->findText(preselectedResource);
        if (idx >= 0) {
            resourceCombo_->setCurrentIndex(idx);
        }
    }
    form->addRow(QStringLiteral("Module"), resourceCombo_);

    nameEdit_ = new QLineEdit(this);
    nameEdit_->setPlaceholderText(QStringLiteral("verify"));
    form->addRow(QStringLiteral("Name"), nameEdit_);

    methodCombo_ = new QComboBox(this);
    for (const auto& [label, value] : kMethods) {
        methodCombo_->addItem(QString::fromUtf8(label),
                              QVariant::fromValue(static_cast<int>(value)));
    }
    form->addRow(QStringLiteral("Method"), methodCombo_);

    pathEdit_ = new QLineEdit(this);
    pathEdit_->setPlaceholderText(QStringLiteral("/api/v1/admin/orgs/{{id}}/verify"));
    form->addRow(QStringLiteral("Path"), pathEdit_);

    actorCombo_ = new QComboBox(this);
    actorCombo_->addItems(actors);
    form->addRow(QStringLiteral("Actor"), actorCombo_);
    outer->addLayout(form);

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

NewEndpointDialog::~NewEndpointDialog() = default;

QString NewEndpointDialog::resourceId() const {
    return resourceCombo_->currentText();
}

QString NewEndpointDialog::endpointName() const {
    return nameEdit_->text().trimmed();
}

engine::HttpMethod NewEndpointDialog::method() const {
    return static_cast<engine::HttpMethod>(methodCombo_->currentData().toInt());
}

QString NewEndpointDialog::pathTemplate() const {
    return pathEdit_->text().trimmed();
}

QString NewEndpointDialog::actorId() const {
    return actorCombo_->currentText();
}

void NewEndpointDialog::revalidate() {
    const QString name = nameEdit_->text().trimmed();
    QString error;
    if (resourceCombo_->count() == 0) {
        error = QStringLiteral("Create a module first.");
    } else if (name.isEmpty()) {
        error = QStringLiteral("Name cannot be empty.");
    } else if (hasIdBreakingChars(name)) {
        error = QStringLiteral("Name can't contain '.', '/', or '\\'.");
    }
    errorLabel_->setText(error);
    errorLabel_->setVisible(!error.isEmpty() && (!name.isEmpty() || resourceCombo_->count() == 0));
    if (auto* ok = buttons_->button(QDialogButtonBox::Ok); ok != nullptr) {
        ok->setEnabled(error.isEmpty());
    }
}

}  // namespace reqloom::desktop
