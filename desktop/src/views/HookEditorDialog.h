// HookEditorDialog — a standalone Widgets window for editing an operation's
// pre-request and post-response JavaScript hooks with the CodeEditor. This is
// the QML Migration Roadmap's sanctioned Widgets fallback (Principle 6): a
// code editor is hard to rebuild in QML, so it lives as a separate dialog
// window launched from the QML app. Presentation only — AppController owns
// loading the scripts in and persisting the edits out.
#pragma once

#include <QtWidgets/QDialog>

#include <QtCore/QString>

class QWidget;
class QLabel;

namespace reqloom::desktop {

class CodeEditor;

namespace theming {
struct Palette;
}  // namespace theming

class HookEditorDialog : public QDialog {
    Q_OBJECT

public:
    /// `*Ref` is the source file path when a hook came from a `./hooks/*.js`
    /// reference (empty for inline hooks) — shown as a read-only hint so the
    /// author knows the edit lands in that file.
    HookEditorDialog(const QString& operationId,
                     const QString& preScript,
                     const QString& preRef,
                     const QString& postScript,
                     const QString& postRef,
                     const theming::Palette& palette,
                     QWidget* parent = nullptr);
    ~HookEditorDialog() override;

    [[nodiscard]] QString preScript() const;
    [[nodiscard]] QString postScript() const;

private:
    /// Dry-run both hook scripts in the engine sandbox against a sample
    /// context and report per-phase OK / error in the status line. Lets the
    /// author catch a syntax or runtime error before saving and running.
    void validateHooks();

    CodeEditor* preEditor_{};
    CodeEditor* postEditor_{};
    QLabel* status_{};
    QString okColor_;
    QString errorColor_;
};

}  // namespace reqloom::desktop
