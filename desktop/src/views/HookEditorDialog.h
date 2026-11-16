// HookEditorDialog — standalone Widgets window for editing an operation's pre-request and
// post-response JavaScript hooks with the CodeEditor (a code editor is hard to rebuild in QML).
// AppController owns loading and persisting.
#pragma once

#include <QtWidgets/QDialog>

#include <QtCore/QFutureWatcher>
#include <QtCore/QString>
#include <QtCore/QStringList>

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
    /// `*Ref` is the source file path when a hook came from a `./hooks/*.js` reference (empty for
    /// inline); shown as a read-only hint.
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
    /// Dry-run both hook scripts in the engine sandbox against a sample context and report
    /// per-phase OK / error. Runs off the GUI thread (the sandbox has a 1s/phase budget).
    void validateHooks();

    /// Off-thread validation result, delivered back via `validateWatcher_`.
    struct ValidationReport {
        bool hadError{false};
        QStringList lines;
    };

    CodeEditor* preEditor_{};
    CodeEditor* postEditor_{};
    QLabel* status_{};
    QString okColor_;
    QString errorColor_;
    QFutureWatcher<ValidationReport> validateWatcher_;
};

}  // namespace reqloom::desktop
