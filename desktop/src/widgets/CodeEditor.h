// CodeEditor — the desktop hook-script editor, built on the vendored Scintilla
// engine (permissive licence; NOT QScintilla, which is GPL). It wraps
// ScintillaEditBase and configures a line-number margin, current-line
// highlight, monospace font and our own container-driven syntax styling for
// JavaScript / JSON. The public API mirrors the QPlainTextEdit it replaced
// (setPlainText / toPlainText / applyTheme) so callers are unaffected.
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

    /// Apply appearance colours (editor surface, gutter, caret, selection) and
    /// select the syntax language. Safe to call again to re-skin.
    void applyTheme(const theming::Palette& palette, Language language);

    void setPlainText(const QString& text);
    [[nodiscard]] QString toPlainText() const;

private:
    /// Re-tokenise the whole buffer and push style bytes to Scintilla. Cheap
    /// for hook-sized scripts; driven from the content-changed signal.
    void styleDocument();

    Language language_{Language::JavaScript};
};

}  // namespace reqloom::desktop
