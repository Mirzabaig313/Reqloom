// RequestEditorPanel — see header. Read-only preview + Override Mode editor.
#include "RequestEditorPanel.h"

#include "../application/ProjectModel.h"
#include "../application/RunController.h"  // RequestOverride
#include "../widgets/KeyValueEditor.h"
#include "Formatting.h"

#include <QtGui/QColor>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QVBoxLayout>

#include <vector>

namespace chainapi::desktop {

namespace {

// Body-kind stack indices.
constexpr int kBodyRaw = 0;
constexpr int kBodyForm = 1;

[[nodiscard]] std::vector<std::pair<QString, QString>> toPairs(
    const std::map<std::string, std::string>& m) {
    std::vector<std::pair<QString, QString>> out;
    out.reserve(m.size());
    for (const auto& [k, v] : m) {
        out.emplace_back(QString::fromStdString(k), QString::fromStdString(v));
    }
    return out;
}

}  // namespace

RequestEditorPanel::RequestEditorPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    const int gap = theming::Theme::space(theming::Space::Md);
    layout->setContentsMargins(gap, gap, gap, gap);
    layout->setSpacing(theming::Theme::space(theming::Space::Sm));

    title_ = new QLabel(QStringLiteral("No operation selected"), this);
    title_->setFont(theme_.font(theming::TextStyle::Title));
    title_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title_);

    auto* metaRow = new QHBoxLayout;
    actorLabel_ = new QLabel(this);
    actorLabel_->setFont(theme_.font(theming::TextStyle::Caption));
    actorLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    metaRow->addWidget(actorLabel_);
    metaRow->addStretch(1);
    overrideToggle_ = new QCheckBox(QStringLiteral("Override Mode"), this);
    overrideToggle_->setToolTip(QStringLiteral(
        "Edit this request (method, path, query, headers, body, etc.) for a one-shot run. The "
        "saved project is not modified; the override applies only to the next Send."));
    metaRow->addWidget(overrideToggle_);
    layout->addLayout(metaRow);

    overrideBanner_ = new QLabel(
        QStringLiteral(
            "Override active — edits apply to the next run only, not the saved project."),
        this);
    overrideBanner_->setWordWrap(true);
    overrideBanner_->setVisible(false);
    layout->addWidget(overrideBanner_);

    auto* chainBox = new QGroupBox(QStringLiteral("Declared dependencies"), this);
    auto* chainLayout = new QVBoxLayout(chainBox);
    chainList_ = new QListWidget(chainBox);
    chainList_->setMaximumHeight(100);
    chainList_->setFont(theme_.font(theming::TextStyle::Mono));
    chainLayout->addWidget(chainList_);
    layout->addWidget(chainBox);

    // The request body of the panel swaps between a read-only preview (page 0)
    // and the editable Override form (page 1).
    requestStack_ = new QStackedWidget(this);

    // ── Page 0: read-only preview ────────────────────────────────────────
    auto* previewPage = new QWidget(requestStack_);
    auto* previewLayout = new QVBoxLayout(previewPage);
    previewLayout->setContentsMargins(0, 0, 0, 0);

    auto* headersBox = new QGroupBox(QStringLiteral("Headers"), previewPage);
    auto* headersLayout = new QVBoxLayout(headersBox);
    headersView_ = new QPlainTextEdit(headersBox);
    headersView_->setReadOnly(true);
    headersView_->setMaximumHeight(120);
    headersView_->setFont(theme_.font(theming::TextStyle::Mono));
    headersLayout->addWidget(headersView_);
    previewLayout->addWidget(headersBox);

    auto* bodyBox = new QGroupBox(QStringLiteral("Body template"), previewPage);
    auto* bodyLayout = new QVBoxLayout(bodyBox);
    bodyView_ = new QPlainTextEdit(bodyBox);
    bodyView_->setReadOnly(true);
    bodyView_->setFont(theme_.font(theming::TextStyle::Mono));
    bodyLayout->addWidget(bodyView_);
    previewLayout->addWidget(bodyBox, 1);
    requestStack_->addWidget(previewPage);

    // ── Page 1: editable Override form ───────────────────────────────────
    auto* editPage = new QWidget(requestStack_);
    auto* editLayout = new QVBoxLayout(editPage);
    editLayout->setContentsMargins(0, 0, 0, 0);

    auto* topForm = new QFormLayout;
    methodCombo_ = new QComboBox(editPage);
    methodCombo_->addItems({QStringLiteral("GET"),
                            QStringLiteral("POST"),
                            QStringLiteral("PUT"),
                            QStringLiteral("PATCH"),
                            QStringLiteral("DELETE"),
                            QStringLiteral("HEAD"),
                            QStringLiteral("OPTIONS")});
    pathEdit_ = new QLineEdit(editPage);
    actorCombo_ = new QComboBox(editPage);
    expectStatusEdit_ = new QLineEdit(editPage);
    expectStatusEdit_->setPlaceholderText(QStringLiteral("e.g. 200,201"));
    timeoutSpin_ = new QSpinBox(editPage);
    timeoutSpin_->setRange(0, 600000);
    timeoutSpin_->setSingleStep(500);
    timeoutSpin_->setSuffix(QStringLiteral(" ms"));
    timeoutSpin_->setSpecialValueText(QStringLiteral("default"));
    forceCheck_ = new QCheckBox(QStringLiteral("Force re-run (ignore extraction cache)"), editPage);
    topForm->addRow(QStringLiteral("Method"), methodCombo_);
    topForm->addRow(QStringLiteral("Path"), pathEdit_);
    topForm->addRow(QStringLiteral("Actor"), actorCombo_);
    topForm->addRow(QStringLiteral("Expect status"), expectStatusEdit_);
    topForm->addRow(QStringLiteral("Timeout"), timeoutSpin_);
    topForm->addRow(QString{}, forceCheck_);
    editLayout->addLayout(topForm);

    auto* queryBox = new QGroupBox(QStringLiteral("Query parameters"), editPage);
    auto* queryLayout = new QVBoxLayout(queryBox);
    queryEditor_ = new widgets::KeyValueEditor(queryBox);
    queryLayout->addWidget(queryEditor_);
    editLayout->addWidget(queryBox);

    auto* hdrBox = new QGroupBox(QStringLiteral("Headers"), editPage);
    auto* hdrLayout = new QVBoxLayout(hdrBox);
    headersEditor_ = new widgets::KeyValueEditor(hdrBox);
    hdrLayout->addWidget(headersEditor_);
    editLayout->addWidget(hdrBox);

    auto* bodyEditBox = new QGroupBox(QStringLiteral("Body"), editPage);
    auto* bodyEditLayout = new QVBoxLayout(bodyEditBox);
    bodyKindCombo_ = new QComboBox(bodyEditBox);
    bodyKindCombo_->addItems({QStringLiteral("Raw / JSON"), QStringLiteral("Form data")});
    bodyEditLayout->addWidget(bodyKindCombo_);
    bodyStack_ = new QStackedWidget(bodyEditBox);
    bodyRawEdit_ = new QPlainTextEdit(bodyStack_);
    bodyRawEdit_->setFont(theme_.font(theming::TextStyle::Mono));
    bodyStack_->addWidget(bodyRawEdit_);  // kBodyRaw
    formEditor_ = new widgets::KeyValueEditor(bodyStack_);
    formEditor_->setMode(widgets::KeyValueEditor::Mode::FileCapable);
    bodyStack_->addWidget(formEditor_);  // kBodyForm
    bodyEditLayout->addWidget(bodyStack_);
    editLayout->addWidget(bodyEditBox, 1);
    requestStack_->addWidget(editPage);

    layout->addWidget(requestStack_, 1);

    auto* buttonRow = new QHBoxLayout;
    sendButton_ = new QPushButton(QStringLiteral("Send"), this);
    sendButton_->setObjectName(QStringLiteral("primaryAction"));
    sendButton_->setDefault(true);
    sendCleanButton_ = new QPushButton(QStringLiteral("Send Cleanly"), this);
    dryRunButton_ = new QPushButton(QStringLiteral("Dry Run"), this);
    buttonRow->addWidget(sendButton_);
    buttonRow->addWidget(sendCleanButton_);
    buttonRow->addWidget(dryRunButton_);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    connect(sendButton_, &QPushButton::clicked, this, [this]() {
        if (!currentOp_.isEmpty()) {
            emit runRequested(currentOp_, /*clean=*/false, /*dryRun=*/false);
        }
    });
    connect(sendCleanButton_, &QPushButton::clicked, this, [this]() {
        if (!currentOp_.isEmpty()) {
            emit runRequested(currentOp_, /*clean=*/true, /*dryRun=*/false);
        }
    });
    connect(dryRunButton_, &QPushButton::clicked, this, [this]() {
        if (!currentOp_.isEmpty()) {
            emit runRequested(currentOp_, /*clean=*/false, /*dryRun=*/true);
        }
    });
    connect(overrideToggle_, &QCheckBox::toggled, this, [this](bool on) { setOverrideMode(on); });
    connect(bodyKindCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        bodyStack_->setCurrentIndex(idx == 1 ? kBodyForm : kBodyRaw);
    });

    clearOperation();
}

RequestEditorPanel::~RequestEditorPanel() = default;

void RequestEditorPanel::setOverrideMode(bool on) {
    overrideActive_ = on;
    overrideBanner_->setVisible(on);
    requestStack_->setCurrentIndex(on ? 1 : 0);
}

void RequestEditorPanel::applyTheme(const theming::Theme& theme) {
    theme_ = theme;
    title_->setFont(theme_.font(theming::TextStyle::Title));
    actorLabel_->setFont(theme_.font(theming::TextStyle::Caption));
    overrideBanner_->setFont(theme_.font(theming::TextStyle::Caption));
    overrideBanner_->setStyleSheet(
        QStringLiteral("color: %1;")
            .arg(theme_.status(theming::StatusToken::Warning).name(QColor::HexRgb)));
    const QFont mono = theme_.font(theming::TextStyle::Mono);
    chainList_->setFont(mono);
    headersView_->setFont(mono);
    bodyView_->setFont(mono);
    bodyRawEdit_->setFont(mono);
    headersEditor_->setTheme(theme);
    queryEditor_->setTheme(theme);
    formEditor_->setTheme(theme);
}

QString RequestEditorPanel::currentOperationId() const {
    return currentOp_;
}

bool RequestEditorPanel::overrideActive() const noexcept {
    return overrideActive_;
}

RequestOverride RequestEditorPanel::buildOverride() const {
    RequestOverride ov;
    ov.active = overrideActive_;
    if (!overrideActive_) {
        return ov;
    }
    ov.method = methodCombo_->currentText();
    ov.path = pathEdit_->text();
    ov.headers = headersEditor_->toStdMap();
    ov.queryParams = queryEditor_->toStdMap();
    ov.actor = actorCombo_->currentText();
    ov.expectStatus = expectStatusEdit_->text();
    ov.timeoutMs = timeoutSpin_->value();
    ov.forceReRun = forceCheck_->isChecked();

    ov.bodyIsForm = (bodyKindCombo_->currentIndex() == 1);
    if (ov.bodyIsForm) {
        ov.formFields = formEditor_->toStdMap();
    } else {
        ov.body = bodyRawEdit_->toPlainText();
    }
    return ov;
}

void RequestEditorPanel::setRunEnabled(bool enabled) {
    const bool hasOp = !currentOp_.isEmpty();
    sendButton_->setEnabled(enabled && hasOp);
    sendCleanButton_->setEnabled(enabled && hasOp);
    dryRunButton_->setEnabled(enabled && hasOp);
}

void RequestEditorPanel::clearOperation() {
    currentOp_.clear();
    overrideToggle_->setChecked(false);
    overrideToggle_->setEnabled(false);
    title_->setText(QStringLiteral("No operation selected"));
    actorLabel_->clear();
    chainList_->clear();
    headersView_->clear();
    bodyView_->clear();
    setRunEnabled(false);
}

void RequestEditorPanel::showOperation(const ProjectModel& project, const QString& operationId) {
    const engine::OperationId target{operationId.toStdString()};
    const auto* op = project.findOperation(target);
    if (op == nullptr) {
        clearOperation();
        return;
    }

    // Switching operations cancels any pending override.
    overrideToggle_->setChecked(false);
    overrideToggle_->setEnabled(true);

    currentOp_ = operationId;
    title_->setText(QStringLiteral("%1  %2").arg(format::method(op->method),
                                                 QString::fromStdString(op->pathTemplate)));
    actorLabel_->setText(QStringLiteral("Operation: %1     Actor: %2")
                             .arg(operationId,
                                  op->actor.value.empty()
                                      ? QStringLiteral("(none)")
                                      : QString::fromStdString(op->actor.value)));

    renderChainPreview(project, target);

    QString headerText;
    for (const auto& [key, value] : op->headers) {
        headerText.append(QStringLiteral("%1: %2\n")
                              .arg(QString::fromStdString(key), QString::fromStdString(value)));
    }
    headersView_->setPlainText(headerText.trimmed());

    if (op->bodyTemplate) {
        bodyView_->setPlainText(QString::fromStdString(*op->bodyTemplate));
    } else if (op->bodyForm) {
        QString form;
        for (const auto& [key, value] : *op->bodyForm) {
            form.append(QStringLiteral("%1 = %2\n")
                            .arg(QString::fromStdString(key), QString::fromStdString(value)));
        }
        bodyView_->setPlainText(form.trimmed());
    } else {
        bodyView_->setPlainText(QStringLiteral("(no body)"));
    }

    loadOverrideFields(project, *op);
    setRunEnabled(true);
}

void RequestEditorPanel::loadOverrideFields(const ProjectModel& project,
                                            const engine::Operation& op) {
    // Seed the editable controls from the operation so a fresh Override Mode
    // starts as a faithful copy the user then tweaks.
    methodCombo_->setCurrentText(format::method(op.method));
    pathEdit_->setText(QString::fromStdString(op.pathTemplate));

    actorCombo_->clear();
    actorCombo_->addItem(QString{});  // "(none)" / unchanged
    if (project.hasProject()) {
        for (const auto& [actorId, _] : project.project().actors) {
            actorCombo_->addItem(QString::fromStdString(actorId.value));
        }
    }
    actorCombo_->setCurrentText(QString::fromStdString(op.actor.value));

    if (!op.expectStatusList.empty()) {
        QStringList codes;
        for (const int code : op.expectStatusList) {
            codes.append(QString::number(code));
        }
        expectStatusEdit_->setText(codes.join(QLatin1Char(',')));
    } else if (op.expectStatus) {
        expectStatusEdit_->setText(QString::number(*op.expectStatus));
    } else {
        expectStatusEdit_->clear();
    }

    timeoutSpin_->setValue(op.timeout ? static_cast<int>(op.timeout->count()) : 0);
    forceCheck_->setChecked(op.force);

    headersEditor_->setPairs(toPairs(op.headers));
    queryEditor_->setPairs(toPairs(op.queryParams));

    if (op.bodyForm) {
        bodyKindCombo_->setCurrentIndex(1);
        bodyStack_->setCurrentIndex(kBodyForm);
        formEditor_->setPairs(toPairs(*op.bodyForm));
        bodyRawEdit_->clear();
    } else {
        bodyKindCombo_->setCurrentIndex(0);
        bodyStack_->setCurrentIndex(kBodyRaw);
        bodyRawEdit_->setPlainText(op.bodyTemplate ? QString::fromStdString(*op.bodyTemplate)
                                                   : QString{});
        formEditor_->clear();
    }
}

void RequestEditorPanel::renderChainPreview(const ProjectModel& project,
                                            const engine::OperationId& target) {
    chainList_->clear();
    const auto* op = project.findOperation(target);
    if (op == nullptr) {
        return;
    }
    if (op->explicitDependencies.empty()) {
        auto* item = new QListWidgetItem(
            QStringLiteral("No declared dependencies — run Dry Run for the full resolved chain"),
            chainList_);
        item->setForeground(theme_.palette().textSecondary);
        return;
    }
    // Static view of the operation's own declared dependencies. Implicit
    // ({{var}}) dependencies and the full topological order are resolved by
    // the engine — surfaced in the timeline after a Dry Run (FR-2.8).
    for (const auto& dep : op->explicitDependencies) {
        chainList_->addItem(QString::fromStdString(dep.value));
    }
}

}  // namespace chainapi::desktop
