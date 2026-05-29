// RequestEditorPanel — see header. Read-only request view + run controls.
#include "RequestEditorPanel.h"

#include "../application/ProjectModel.h"
#include "Formatting.h"

#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace chainapi::desktop {

RequestEditorPanel::RequestEditorPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    title_ = new QLabel(QStringLiteral("No operation selected"), this);
    auto titleFont = title_->font();
    titleFont.setPointSize(titleFont.pointSize() + 3);
    titleFont.setBold(true);
    title_->setFont(titleFont);
    title_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title_);

    actorLabel_ = new QLabel(this);
    actorLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(actorLabel_);

    auto* chainBox = new QGroupBox(QStringLiteral("Declared dependencies"), this);
    auto* chainLayout = new QVBoxLayout(chainBox);
    chainList_ = new QListWidget(chainBox);
    chainList_->setMaximumHeight(120);
    chainLayout->addWidget(chainList_);
    layout->addWidget(chainBox);

    auto* headersBox = new QGroupBox(QStringLiteral("Headers"), this);
    auto* headersLayout = new QVBoxLayout(headersBox);
    headersView_ = new QPlainTextEdit(headersBox);
    headersView_->setReadOnly(true);
    headersView_->setMaximumHeight(120);
    headersLayout->addWidget(headersView_);
    layout->addWidget(headersBox);

    auto* bodyBox = new QGroupBox(QStringLiteral("Body template"), this);
    auto* bodyLayout = new QVBoxLayout(bodyBox);
    bodyView_ = new QPlainTextEdit(bodyBox);
    bodyView_->setReadOnly(true);
    bodyLayout->addWidget(bodyView_);
    layout->addWidget(bodyBox, 1);

    auto* buttonRow = new QHBoxLayout;
    sendButton_ = new QPushButton(QStringLiteral("Send"), this);
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

    clearOperation();
}

RequestEditorPanel::~RequestEditorPanel() = default;

QString RequestEditorPanel::currentOperationId() const {
    return currentOp_;
}

void RequestEditorPanel::setRunEnabled(bool enabled) {
    const bool hasOp = !currentOp_.isEmpty();
    sendButton_->setEnabled(enabled && hasOp);
    sendCleanButton_->setEnabled(enabled && hasOp);
    dryRunButton_->setEnabled(enabled && hasOp);
}

void RequestEditorPanel::clearOperation() {
    currentOp_.clear();
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

    setRunEnabled(true);
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
        item->setForeground(Qt::gray);
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
