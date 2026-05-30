// RequestEditorPanel — see header. Read-only preview + in-place editor.
#include "RequestEditorPanel.h"

#include "../application/ProjectModel.h"
#include "../application/RunController.h"  // RequestOverride
#include "../widgets/KeyValueEditor.h"
#include "Formatting.h"

#include <QtGui/QColor>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>

#include <cstddef>
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

/// A lightweight section heading — a label, not a framed group box. DESIGN.md
/// §5 bans nested cards; sections are signalled by a heading + spacing.
[[nodiscard]] QLabel* sectionHeading(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("role", QStringLiteral("sectionHeading"));
    return label;
}

}  // namespace

RequestEditorPanel::RequestEditorPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    const int gap = theming::Theme::space(theming::Space::Md);
    outer->setContentsMargins(gap, gap, gap, gap);
    outer->setSpacing(theming::Theme::space(theming::Space::Sm));

    outer->addWidget(buildHeaderRow());

    // Scrollable detail (so the panel can shrink without clipping).
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* scrollBody = new QWidget(scroll);
    auto* detail = new QVBoxLayout(scrollBody);
    detail->setContentsMargins(0, 0, 0, 0);
    detail->setSpacing(theming::Theme::space(theming::Space::Md));

    detail->addWidget(sectionHeading(QStringLiteral("Dependencies"), scrollBody));
    chainList_ = new QListWidget(scrollBody);
    chainList_->setMaximumHeight(90);
    chainList_->setFont(theme_.font(theming::TextStyle::Mono));
    detail->addWidget(chainList_);

    requestStack_ = new QStackedWidget(scrollBody);
    requestStack_->addWidget(buildPreviewPage());  // index 0
    requestStack_->addWidget(buildEditPage());     // index 1
    detail->addWidget(requestStack_, 1);

    scroll->setWidget(scrollBody);
    outer->addWidget(scroll, 1);

    outer->addWidget(buildActionRow());

    wireConnections();
    clearOperation();
}

QWidget* RequestEditorPanel::buildHeaderRow() {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theming::Theme::space(theming::Space::Xs));

    title_ = new QLabel(QStringLiteral("No operation selected"), container);
    title_->setFont(theme_.font(theming::TextStyle::Title));
    title_->setWordWrap(true);
    title_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title_);

    auto* metaRow = new QHBoxLayout;
    actorLabel_ = new QLabel(container);
    actorLabel_->setFont(theme_.font(theming::TextStyle::Caption));
    actorLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    metaRow->addWidget(actorLabel_);
    metaRow->addStretch(1);
    overrideToggle_ = new QCheckBox(QStringLiteral("Edit"), container);
    overrideToggle_->setToolTip(QStringLiteral(
        "Edit this request (method, path, query, headers, body, etc.). Send applies the edits to "
        "the next run; Save to Project writes them to disk."));
    metaRow->addWidget(overrideToggle_);
    layout->addLayout(metaRow);

    overrideBanner_ = new QLabel(
        QStringLiteral("Editing — Send applies changes to the next run; Save writes them to the "
                       "project."),
        container);
    overrideBanner_->setWordWrap(true);
    overrideBanner_->setVisible(false);
    layout->addWidget(overrideBanner_);
    return container;
}

QWidget* RequestEditorPanel::buildPreviewPage() {
    auto* previewPage = new QWidget(this);
    auto* previewLayout = new QVBoxLayout(previewPage);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(theming::Theme::space(theming::Space::Md));
    previewLayout->addWidget(sectionHeading(QStringLiteral("Headers"), previewPage));
    headersView_ = new QPlainTextEdit(previewPage);
    headersView_->setReadOnly(true);
    headersView_->setMaximumHeight(120);
    headersView_->setFont(theme_.font(theming::TextStyle::Mono));
    previewLayout->addWidget(headersView_);
    previewLayout->addWidget(sectionHeading(QStringLiteral("Body"), previewPage));
    bodyView_ = new QPlainTextEdit(previewPage);
    bodyView_->setReadOnly(true);
    bodyView_->setFont(theme_.font(theming::TextStyle::Mono));
    previewLayout->addWidget(bodyView_, 1);
    return previewPage;
}

QWidget* RequestEditorPanel::buildEditPage() {
    auto* editPage = new QWidget(this);
    auto* editLayout = new QVBoxLayout(editPage);
    editLayout->setContentsMargins(0, 0, 0, 0);
    editLayout->setSpacing(theming::Theme::space(theming::Space::Sm));

    // Method + path bar (the Postman "request line").
    auto* lineRow = new QHBoxLayout;
    lineRow->setSpacing(theming::Theme::space(theming::Space::Xs));
    methodCombo_ = new QComboBox(editPage);
    methodCombo_->addItems({QStringLiteral("GET"),
                            QStringLiteral("POST"),
                            QStringLiteral("PUT"),
                            QStringLiteral("PATCH"),
                            QStringLiteral("DELETE"),
                            QStringLiteral("HEAD"),
                            QStringLiteral("OPTIONS")});
    methodCombo_->setObjectName(QStringLiteral("methodCombo"));
    pathEdit_ = new QLineEdit(editPage);
    pathEdit_->setPlaceholderText(QStringLiteral("/api/v1/…"));
    lineRow->addWidget(methodCombo_);
    lineRow->addWidget(pathEdit_, 1);
    editLayout->addLayout(lineRow);

    editTabs_ = new QTabWidget(editPage);
    editTabs_->setDocumentMode(true);

    queryEditor_ = new widgets::KeyValueEditor(editTabs_);
    editTabs_->addTab(queryEditor_, QStringLiteral("Params"));

    headersEditor_ = new widgets::KeyValueEditor(editTabs_);
    editTabs_->addTab(headersEditor_, QStringLiteral("Headers"));

    // Body tab: kind selector + raw/form stack.
    auto* bodyTab = new QWidget(editTabs_);
    auto* bodyTabLayout = new QVBoxLayout(bodyTab);
    bodyTabLayout->setContentsMargins(0, theming::Theme::space(theming::Space::Sm), 0, 0);
    auto* bodyKindRow = new QHBoxLayout;
    bodyKindCombo_ = new QComboBox(bodyTab);
    bodyKindCombo_->addItems({QStringLiteral("Raw / JSON"), QStringLiteral("Form data")});
    bodyKindRow->addWidget(bodyKindCombo_);
    bodyKindRow->addStretch(1);
    bodyTabLayout->addLayout(bodyKindRow);
    bodyStack_ = new QStackedWidget(bodyTab);
    bodyRawEdit_ = new QPlainTextEdit(bodyStack_);
    bodyRawEdit_->setFont(theme_.font(theming::TextStyle::Mono));
    bodyRawEdit_->setPlaceholderText(QStringLiteral("{ }"));
    bodyStack_->addWidget(bodyRawEdit_);  // kBodyRaw
    formEditor_ = new widgets::KeyValueEditor(bodyStack_);
    formEditor_->setMode(widgets::KeyValueEditor::Mode::FileCapable);
    bodyStack_->addWidget(formEditor_);  // kBodyForm
    bodyTabLayout->addWidget(bodyStack_, 1);
    editTabs_->addTab(bodyTab, QStringLiteral("Body"));

    // Options tab: actor / expect status / timeout / force.
    auto* optionsTab = new QWidget(editTabs_);
    auto* optionsForm = new QFormLayout(optionsTab);
    optionsForm->setContentsMargins(theming::Theme::space(theming::Space::Sm),
                                    theming::Theme::space(theming::Space::Md),
                                    theming::Theme::space(theming::Space::Sm),
                                    theming::Theme::space(theming::Space::Sm));
    optionsForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    optionsForm->setLabelAlignment(Qt::AlignLeft);
    actorCombo_ = new QComboBox(optionsTab);
    expectStatusEdit_ = new QLineEdit(optionsTab);
    expectStatusEdit_->setPlaceholderText(QStringLiteral("e.g. 200,201"));
    timeoutSpin_ = new QSpinBox(optionsTab);
    timeoutSpin_->setRange(0, 600000);
    timeoutSpin_->setSingleStep(500);
    timeoutSpin_->setSuffix(QStringLiteral(" ms"));
    timeoutSpin_->setSpecialValueText(QStringLiteral("default"));
    forceCheck_ =
        new QCheckBox(QStringLiteral("Force re-run (ignore extraction cache)"), optionsTab);
    optionsForm->addRow(QStringLiteral("Actor"), actorCombo_);
    optionsForm->addRow(QStringLiteral("Expect status"), expectStatusEdit_);
    optionsForm->addRow(QStringLiteral("Timeout"), timeoutSpin_);
    optionsForm->addRow(QString{}, forceCheck_);
    editTabs_->addTab(optionsTab, QStringLiteral("Options"));

    editLayout->addWidget(editTabs_, 1);
    return editPage;
}

QWidget* RequestEditorPanel::buildActionRow() {
    auto* container = new QWidget(this);
    auto* buttonRow = new QHBoxLayout(container);
    buttonRow->setContentsMargins(0, 0, 0, 0);
    sendButton_ = new QPushButton(QStringLiteral("Send"), container);
    sendButton_->setObjectName(QStringLiteral("primaryAction"));
    sendButton_->setDefault(true);
    sendCleanButton_ = new QPushButton(QStringLiteral("Send Cleanly"), container);
    dryRunButton_ = new QPushButton(QStringLiteral("Dry Run"), container);
    saveButton_ = new QPushButton(QStringLiteral("Save to Project"), container);
    saveButton_->setVisible(false);  // only meaningful in edit mode
    buttonRow->addWidget(sendButton_);
    buttonRow->addWidget(sendCleanButton_);
    buttonRow->addWidget(dryRunButton_);
    buttonRow->addStretch(1);
    buttonRow->addWidget(saveButton_);
    return container;
}

void RequestEditorPanel::wireConnections() {
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
    connect(saveButton_, &QPushButton::clicked, this, [this]() {
        if (!currentOp_.isEmpty()) {
            emit saveRequested(currentOp_);
        }
    });
    connect(overrideToggle_, &QCheckBox::toggled, this, [this](bool on) { setOverrideMode(on); });
    connect(bodyKindCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        bodyStack_->setCurrentIndex(idx == 1 ? kBodyForm : kBodyRaw);
        refreshTabBadges();
    });
    connect(
        queryEditor_, &widgets::KeyValueEditor::changed, this, [this]() { refreshTabBadges(); });
    connect(
        headersEditor_, &widgets::KeyValueEditor::changed, this, [this]() { refreshTabBadges(); });
    connect(formEditor_, &widgets::KeyValueEditor::changed, this, [this]() { refreshTabBadges(); });
    connect(bodyRawEdit_, &QPlainTextEdit::textChanged, this, [this]() { refreshTabBadges(); });
}

RequestEditorPanel::~RequestEditorPanel() = default;

void RequestEditorPanel::setOverrideMode(bool on) {
    overrideActive_ = on;
    overrideBanner_->setVisible(on);
    saveButton_->setVisible(on);
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
    saveButton_->setEnabled(enabled && hasOp);
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

    // Switching operations cancels any pending edit.
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
    // Seed the editable controls from the operation so a fresh edit starts as a
    // faithful copy the user then tweaks.
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

    refreshTabBadges();
}

void RequestEditorPanel::refreshTabBadges() {
    if (editTabs_ == nullptr) {
        return;
    }
    // Tab order: 0 Params, 1 Headers, 2 Body, 3 Options. Show a count when a
    // section has content, Postman-style ("Headers 8"), and a dot on Body.
    const auto withCount = [](const QString& base, std::size_t n) {
        return n > 0 ? QStringLiteral("%1  %2").arg(base).arg(n) : base;
    };
    editTabs_->setTabText(0, withCount(QStringLiteral("Params"), queryEditor_->toStdMap().size()));
    editTabs_->setTabText(1,
                          withCount(QStringLiteral("Headers"), headersEditor_->toStdMap().size()));

    const bool hasBody = (bodyKindCombo_->currentIndex() == 1)
                             ? !formEditor_->toStdMap().empty()
                             : !bodyRawEdit_->toPlainText().trimmed().isEmpty();
    editTabs_->setTabText(2, hasBody ? QStringLiteral("Body  ●") : QStringLiteral("Body"));
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
