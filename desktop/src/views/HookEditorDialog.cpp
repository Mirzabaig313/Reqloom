// HookEditorDialog — see header.
#include "HookEditorDialog.h"

#include "../theming/Theme.h"
#include "../widgets/CodeEditor.h"

#include <reqloom/engine/Hook.h>

#include <QtGui/QColor>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <string>

namespace reqloom::desktop {

namespace {

// Build one tab: an optional "from file" hint above a themed CodeEditor seeded
// with `script`. Returns the editor so the dialog can read it back on accept.
[[nodiscard]] CodeEditor* buildTab(QTabWidget* tabs,
                                   const QString& title,
                                   const QString& script,
                                   const QString& ref,
                                   const theming::Palette& palette) {
    auto* page = new QWidget(tabs);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    if (!ref.isEmpty()) {
        auto* hint = new QLabel(QObject::tr("Saved to file: %1").arg(ref), page);
        hint->setStyleSheet(
            QStringLiteral("color: %1; padding: 4px 2px;").arg(palette.textSecondary.name()));
        layout->addWidget(hint);
    }

    auto* editor = new CodeEditor(page);
    editor->applyTheme(palette, CodeEditor::Language::JavaScript);
    editor->setPlainText(script);
    layout->addWidget(editor, 1);

    tabs->addTab(page, title);
    return editor;
}

}  // namespace

HookEditorDialog::HookEditorDialog(const QString& operationId,
                                   const QString& preScript,
                                   const QString& preRef,
                                   const QString& postScript,
                                   const QString& postRef,
                                   const theming::Palette& palette,
                                   QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Edit Hooks — %1").arg(operationId));
    resize(720, 560);

    const QString sheet = "QDialog { background: " + palette.surfaceBase.name() +
                          "; }"
                          "QLabel { color: " +
                          palette.textSecondary.name() +
                          "; }"
                          "QTabBar::tab { background: " +
                          palette.surfaceSunken.name() +
                          "; color: " + palette.textSecondary.name() +
                          "; padding: 6px 14px; }"
                          "QTabBar::tab:selected { color: " +
                          palette.textPrimary.name() +
                          "; }"
                          "QTabWidget::pane { border: 1px solid " +
                          palette.surfaceSunken.name() +
                          "; }"
                          "QPushButton { background: " +
                          palette.surfaceRaised.name() + "; color: " + palette.textPrimary.name() +
                          "; border: 1px solid " + palette.borderStrong.name() +
                          "; border-radius: 6px; padding: 6px 18px; min-width: 76px; }"
                          "QPushButton:hover { border-color: " +
                          palette.accentBase.name() +
                          "; }"
                          "QPushButton:default { background: " +
                          palette.accentBase.name() + "; color: " + palette.textInverse.name() +
                          "; border: 1px solid " + palette.accentBase.name() +
                          "; }"
                          "QPushButton:default:hover { background: " +
                          palette.accentHover.name() + "; }";
    setStyleSheet(sheet);

    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(
        tr("JavaScript runs in the QuickJS sandbox. Use ctx.request / ctx.env / ctx.actors; "
           "helpers: hmac, jwt, base64."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* tabs = new QTabWidget(this);
    preEditor_ = buildTab(tabs, tr("Pre-request"), preScript, preRef, palette);
    postEditor_ = buildTab(tabs, tr("Post-response"), postScript, postRef, palette);
    layout->addWidget(tabs, 1);

    // Status line for validation results (hidden until the author validates).
    okColor_ = palette.statusSuccess.name();
    errorColor_ = palette.statusError.name();
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_->hide();
    layout->addWidget(status_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    auto* validateButton = buttons->addButton(tr("Validate"), QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(validateButton, &QPushButton::clicked, this, &HookEditorDialog::validateHooks);
}

HookEditorDialog::~HookEditorDialog() = default;

void HookEditorDialog::validateHooks() {
    namespace ce = reqloom::engine;

    bool hadError = false;
    QStringList lines;

    // Dry-run one phase's script (skipping an empty one) against a minimal
    // sample context — enough to surface syntax and runtime errors.
    const auto check = [&](const QString& label, ce::HookPhase phase, const QString& script) {
        const QString trimmed = script.trimmed();
        if (trimmed.isEmpty()) {
            return;
        }
        ce::HookDryRunInput input;
        input.phase = phase;
        input.script = trimmed.toStdString();
        input.request.method = ce::HttpMethod::Get;
        input.request.url = "https://example.test/path";
        if (phase == ce::HookPhase::PostResponse) {
            input.response = ce::HookSampleResponse{200, {}, std::string{}};
        }
        const auto outcome = ce::dryRunHook(input);
        if (outcome) {
            lines.append(tr("%1: OK").arg(label));
        } else {
            hadError = true;
            lines.append(tr("%1: %2").arg(label, QString::fromStdString(outcome.error().detail)));
        }
    };

    check(tr("Pre-request"), ce::HookPhase::PreRequest, preEditor_->toPlainText());
    check(tr("Post-response"), ce::HookPhase::PostResponse, postEditor_->toPlainText());

    if (lines.isEmpty()) {
        lines.append(tr("No hook scripts to validate."));
    }
    status_->setStyleSheet(
        QStringLiteral("color: %1; padding: 4px 2px;").arg(hadError ? errorColor_ : okColor_));
    status_->setText(lines.join(QLatin1Char('\n')));
    status_->show();
}

QString HookEditorDialog::preScript() const {
    return preEditor_->toPlainText();
}

QString HookEditorDialog::postScript() const {
    return postEditor_->toPlainText();
}

}  // namespace reqloom::desktop
