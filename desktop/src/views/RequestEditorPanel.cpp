// RequestEditorPanel — see header. Address-bar request editor with a teaching
// zero-state (DESIGN.md §10), a top method+path+Send bar (Postman/Apidog
// convention), and tabbed Params/Headers/Body/Options below.
#include "RequestEditorPanel.h"

#include "../application/ProjectModel.h"
#include "../application/RunController.h"  // RequestOverride
#include "../widgets/ChainView.h"
#include "../widgets/EmptyState.h"
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
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>

#include <cstddef>
#include <vector>

namespace chainapi::desktop {

namespace {

// Body-kind stack indices.
constexpr int kBodyRaw = 0;
constexpr int kBodyForm = 1;

// Root stack pages: teaching empty-state vs. the live editor.
constexpr int kPageEmpty = 0;
constexpr int kPageContent = 1;

// Method-stack pages: read-only pill vs. editable combo.
constexpr int kMethodPill = 0;
constexpr int kMethodCombo = 1;

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
/// §15 bans nested cards; sections are signalled by a heading + spacing.
[[nodiscard]] QLabel* sectionHeading(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("role", QStringLiteral("sectionHeading"));
    return label;
}

}  // namespace

RequestEditorPanel::RequestEditorPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("workspacePanel"));
    setAttribute(Qt::WA_StyledBackground, true);

    // Teaching empty-state until an operation is selected (DESIGN.md §10).
    emptyState_ = new widgets::EmptyState(this);
    emptyState_->setTitle(QStringLiteral("No operation selected"));
    emptyState_->setMessage(QStringLiteral(
        "Select an endpoint from the Explorer to preview its request and run it with the full "
        "dependency chain resolved. Press Cmd+P to search operations."));

    rootStack_ = new QStackedWidget(this);
    rootStack_->addWidget(emptyState_);     // kPageEmpty
    rootStack_->addWidget(buildContent());  // kPageContent

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(rootStack_);

    wireConnections();
    clearOperation();
}

QWidget* RequestEditorPanel::buildContent() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    const int gap = theming::Theme::space(theming::Space::Md);
    layout->setContentsMargins(gap, gap, gap, gap);
    layout->setSpacing(theming::Theme::space(theming::Space::Sm));

    layout->addWidget(buildAddressBar());
    layout->addWidget(buildSecondaryActions());

    overrideBanner_ = new QLabel(
        QStringLiteral("Editing — Send applies changes to the next run; Save writes them to the "
                       "project."),
        page);
    overrideBanner_->setWordWrap(true);
    overrideBanner_->setVisible(false);
    layout->addWidget(overrideBanner_);

    // The execution chain is the product's hero surface (DESIGN.md §1.2): show
    // it visually, not as a plain label. Capped height so it stays a preview.
    layout->addWidget(sectionHeading(QStringLiteral("Execution Chain"), page));
    chainView_ = new widgets::ChainView(page);
    chainView_->setMaximumHeight(160);
    layout->addWidget(chainView_);

    requestStack_ = new QStackedWidget(page);
    requestStack_->addWidget(buildPreviewPage());  // index 0
    requestStack_->addWidget(buildEditPage());     // index 1
    layout->addWidget(requestStack_, 1);

    return page;
}

QWidget* RequestEditorPanel::buildAddressBar() {
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("requestLineBar"));
    bar->setAttribute(Qt::WA_StyledBackground, true);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(0, 0, theming::Theme::space(theming::Space::Sm), 0);
    row->setSpacing(0);

    // Method: a coloured pill when previewing, an editable combo when editing.
    methodStack_ = new QStackedWidget(bar);
    methodStack_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    methodPill_ = new QLabel(bar);
    methodPill_->setObjectName(QStringLiteral("methodPill"));
    methodPill_->setAlignment(Qt::AlignCenter);
    methodCombo_ = new QComboBox(bar);
    methodCombo_->addItems({QStringLiteral("GET"),
                            QStringLiteral("POST"),
                            QStringLiteral("PUT"),
                            QStringLiteral("PATCH"),
                            QStringLiteral("DELETE"),
                            QStringLiteral("HEAD"),
                            QStringLiteral("OPTIONS")});
    methodCombo_->setObjectName(QStringLiteral("methodCombo"));
    methodStack_->addWidget(methodPill_);   // kMethodPill
    methodStack_->addWidget(methodCombo_);  // kMethodCombo
    row->addWidget(methodStack_);

    pathEdit_ = new QLineEdit(bar);
    pathEdit_->setPlaceholderText(QStringLiteral("/api/v1/…"));
    pathEdit_->setReadOnly(true);
    pathEdit_->setFont(theme_.font(theming::TextStyle::Mono));
    pathEdit_->setFrame(false);
    pathEdit_->setAccessibleName(QStringLiteral("Request path"));
    row->addWidget(pathEdit_, 1);

    row->addSpacing(theming::Theme::space(theming::Space::Sm));

    // The prominent primary action, in the address bar where the eye lands.
    sendButton_ = new QPushButton(QStringLiteral("Send"), bar);
    sendButton_->setObjectName(QStringLiteral("primaryAction"));
    sendButton_->setDefault(true);
    row->addWidget(sendButton_, 0, Qt::AlignVCenter);

    return bar;
}

QWidget* RequestEditorPanel::buildSecondaryActions() {
    auto* container = new QWidget(this);
    auto* row = new QHBoxLayout(container);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(theming::Theme::space(theming::Space::Sm));

    actorCaption_ = new QLabel(container);
    actorCaption_->setObjectName(QStringLiteral("actorChip"));
    actorCaption_->setFont(theme_.font(theming::TextStyle::Label));
    actorCaption_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(actorCaption_);
    row->addStretch(1);

    overrideToggle_ = new QCheckBox(QStringLiteral("Edit"), container);
    overrideToggle_->setToolTip(QStringLiteral(
        "Edit this request (method, path, query, headers, body, etc.). Send applies the edits to "
        "the next run; Save to Project writes them to disk."));
    row->addWidget(overrideToggle_);

    // Button hierarchy (DESIGN.md §7.2: one primary + at most two secondary):
    // Send (primary) lives in the address bar; Dry Run / Send Cleanly are
    // plain secondary; Save to Project is a ghost action (edit-mode only).
    dryRunButton_ = new QPushButton(QStringLiteral("Dry Run"), container);
    sendCleanButton_ = new QPushButton(QStringLiteral("Send Cleanly"), container);
    saveButton_ = new QPushButton(QStringLiteral("Save to Project"), container);
    saveButton_->setObjectName(QStringLiteral("ghostAction"));
    saveButton_->setVisible(false);  // only meaningful in edit mode
    row->addWidget(dryRunButton_);
    row->addWidget(sendCleanButton_);
    row->addWidget(saveButton_);

    return container;
}

QWidget* RequestEditorPanel::buildPreviewPage() {
    // Tabbed read-only preview (Headers / Body) so the panel recovers the
    // vertical space the old stacked text areas wasted.
    auto* tabs = new QTabWidget(this);
    tabs->setDocumentMode(true);

    headersView_ = new QPlainTextEdit(tabs);
    headersView_->setReadOnly(true);
    headersView_->setFrameShape(QFrame::NoFrame);
    headersView_->setFont(theme_.font(theming::TextStyle::Mono));
    tabs->addTab(headersView_, QStringLiteral("Headers"));

    bodyView_ = new QPlainTextEdit(tabs);
    bodyView_->setReadOnly(true);
    bodyView_->setFrameShape(QFrame::NoFrame);
    bodyView_->setFont(theme_.font(theming::TextStyle::Mono));
    tabs->addTab(bodyView_, QStringLiteral("Body"));

    return tabs;
}

QWidget* RequestEditorPanel::buildEditPage() {
    // Editable controls only — method + path live in the shared address bar.
    editTabs_ = new QTabWidget(this);
    editTabs_->setDocumentMode(true);

    // The KeyValueEditor grows with its rows, so a long list must scroll inside
    // the (non-scrolling) tab. Wrap each in a frameless QScrollArea.
    const auto scrollWrap = [this](QWidget* inner) -> QScrollArea* {
        auto* scroll = new QScrollArea(editTabs_);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidget(inner);
        return scroll;
    };

    queryEditor_ = new widgets::KeyValueEditor;
    editTabs_->addTab(scrollWrap(queryEditor_), QStringLiteral("Params"));

    headersEditor_ = new widgets::KeyValueEditor;
    editTabs_->addTab(scrollWrap(headersEditor_), QStringLiteral("Headers"));

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
    bodyRawEdit_->setFrameShape(QFrame::NoFrame);
    bodyRawEdit_->setPlaceholderText(QStringLiteral("{ }"));
    bodyStack_->addWidget(bodyRawEdit_);  // kBodyRaw
    formEditor_ = new widgets::KeyValueEditor;
    formEditor_->setMode(widgets::KeyValueEditor::Mode::FileCapable);
    bodyStack_->addWidget(scrollWrap(formEditor_));  // kBodyForm
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

    return editTabs_;
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
    methodStack_->setCurrentIndex(on ? kMethodCombo : kMethodPill);
    pathEdit_->setReadOnly(!on);
    requestStack_->setCurrentIndex(on ? 1 : 0);
}

void RequestEditorPanel::refreshMethodPill() {
    if (methodPill_ == nullptr) {
        return;
    }
    methodPill_->setText(currentMethod_);
    // Map the verb to its method-vocabulary class; the central sheet colours
    // each from the method palette (DESIGN.md §6.2a / §13 property selector).
    QString cls;
    switch (format::methodColor(currentMethod_)) {
        case theming::MethodColor::Get:
            cls = QStringLiteral("get");
            break;
        case theming::MethodColor::Post:
            cls = QStringLiteral("post");
            break;
        case theming::MethodColor::Put:
            cls = QStringLiteral("put");
            break;
        case theming::MethodColor::Patch:
            cls = QStringLiteral("patch");
            break;
        case theming::MethodColor::Delete:
            cls = QStringLiteral("delete");
            break;
        case theming::MethodColor::Neutral:
            cls = QString{};
            break;
    }
    methodPill_->setProperty("methodClass", cls);
    // Property changes don't restyle automatically — repolish this one label.
    if (auto* s = methodPill_->style()) {
        s->unpolish(methodPill_);
        s->polish(methodPill_);
    }
}

void RequestEditorPanel::applyTheme(const theming::Theme& theme) {
    theme_ = theme;
    emptyState_->setTheme(theme);
    // Keep the actor chip at the Label style it's built with (§8 wants the
    // actor prominent, not caption-sized).
    actorCaption_->setFont(theme_.font(theming::TextStyle::Label));
    overrideBanner_->setFont(theme_.font(theming::TextStyle::Caption));
    overrideBanner_->setStyleSheet(
        QStringLiteral("color: %1; background-color: %2; border: 1px solid %1; border-radius: 6px; "
                       "padding: 8px 12px;")
            .arg(theme_.status(theming::StatusToken::Warning).name(QColor::HexRgb),
                 theme_.statusTint(theming::StatusToken::Warning).name(QColor::HexRgb)));
    const QFont mono = theme_.font(theming::TextStyle::Mono);
    pathEdit_->setFont(mono);
    chainView_->setTheme(theme);
    headersView_->setFont(mono);
    bodyView_->setFont(mono);
    bodyRawEdit_->setFont(mono);
    headersEditor_->setTheme(theme);
    queryEditor_->setTheme(theme);
    formEditor_->setTheme(theme);
    refreshMethodPill();
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
    currentMethod_.clear();
    overrideToggle_->setChecked(false);
    overrideToggle_->setEnabled(false);
    actorCaption_->clear();
    chainView_->setEmptyMessage(QString{});
    headersView_->clear();
    bodyView_->clear();
    pathEdit_->clear();
    setRunEnabled(false);
    // Back to the teaching empty-state — no inputs, no buttons (DESIGN.md §10).
    rootStack_->setCurrentIndex(kPageEmpty);
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
    currentMethod_ = format::method(op->method);
    pathEdit_->setText(QString::fromStdString(op->pathTemplate));
    refreshMethodPill();

    // Actor is a first-class ChainAPI concept (the session identity the chain
    // runs as), so surface it as a labelled chip, not faint metadata (§8).
    if (op->actor.value.empty()) {
        actorCaption_->setText(QStringLiteral("⊘  No actor"));
        actorCaption_->setProperty("hasActor", false);
    } else {
        actorCaption_->setText(
            QStringLiteral("👤  %1").arg(QString::fromStdString(op->actor.value)));
        actorCaption_->setProperty("hasActor", true);
    }
    if (auto* s = actorCaption_->style()) {
        s->unpolish(actorCaption_);
        s->polish(actorCaption_);
    }

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
    rootStack_->setCurrentIndex(kPageContent);
}

void RequestEditorPanel::loadOverrideFields(const ProjectModel& project,
                                            const engine::Operation& op) {
    // Seed the editable controls from the operation so a fresh edit starts as a
    // faithful copy the user then tweaks.
    methodCombo_->setCurrentText(format::method(op.method));

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
    const auto* op = project.findOperation(target);
    if (op == nullptr) {
        chainView_->setEmptyMessage(QString{});
        return;
    }
    if (op->explicitDependencies.empty()) {
        chainView_->setEmptyMessage(
            QStringLiteral("No declared dependencies — run Dry Run for the full resolved chain."));
        return;
    }
    // Static view of the operation's own declared dependencies in declared
    // order, then the target itself last. Implicit ({{var}}) dependencies and
    // the full topological order are resolved by the engine — surfaced in the
    // timeline after a Dry Run (FR-2.8).
    std::vector<widgets::ChainView::Node> nodes;
    nodes.reserve(op->explicitDependencies.size() + 1);
    for (const auto& dep : op->explicitDependencies) {
        const engine::OperationId depId{dep.value};
        const auto* depOp = project.findOperation(depId);
        nodes.push_back({QString::fromStdString(dep.value),
                         depOp != nullptr ? format::method(depOp->method) : QString{},
                         /*isTarget=*/false});
    }
    nodes.push_back(
        {QString::fromStdString(target.value), format::method(op->method), /*isTarget=*/true});
    chainView_->setNodes(nodes);
}

}  // namespace chainapi::desktop
