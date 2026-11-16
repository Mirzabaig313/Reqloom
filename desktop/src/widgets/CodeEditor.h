// CodeEditor — the desktop hook-script editor on the vendored Scintilla (permissive licence; NOT
// QScintilla, which is GPL), with container-driven JavaScript / JSON styling. API mirrors
// QPlainTextEdit (setPlainText / toPlainText / applyTheme).
#pragma once

#include <ScintillaEditBase.h>

#include <QtCore/QString>

#include <cstdint>

class QWidget;

namespace reqloom::desktop {

namespace theming {
struct Palette;
}  // namespace theming

class CodeEditor : public ScintillaEditBase {
    Q_OBJECT

public:
    enum class Language : std::uint8_t { PlainText, JavaScript, Json };

    explicit CodeEditor(QWidget* parent = nullptr);
    ~CodeEditor() override;

    /// Apply appearance colours and select the syntax language; safe to call again to re-skin.
    void applyTheme(const theming::Palette& palette, Language language);

    void setPlainText(const QString& text);
    [[nodiscard]] QString toPlainText() const;

private:
    /// Re-tokenise the whole buffer and push style bytes to Scintilla; cheap for hook-sized
    /// scripts.
    void styleDocument();
    /// Defer styling to the next event-loop tick: it must not run inside Scintilla's modification
    /// notification (re-entrancy crash), so post it instead of calling styleDocument() from a
    /// notify handler or after SCI_SETTEXT.
    void scheduleStyling();

    Language language_{Language::JavaScript};
    bool stylingScheduled_{false};
};

}  // namespace reqloom::desktop
