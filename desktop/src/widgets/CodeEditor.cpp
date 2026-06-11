// CodeEditor — see header.
#include "CodeEditor.h"

#include "../theming/Theme.h"

#include <QtCore/QByteArray>
#include <QtGui/QColor>

#include <array>
#include <string_view>

namespace reqloom::desktop {

namespace {

// Style ids for our container styling (0 = default).
constexpr int kStyleDefault = 0;
constexpr int kStyleKeyword = 1;
constexpr int kStyleString = 2;
constexpr int kStyleComment = 3;
constexpr int kStyleNumber = 4;
constexpr int kStyleBuiltin = 5;
constexpr int kStyleLiteral = 6;

// Scintilla wants a 0x00BBGGRR integer; element colours add an alpha byte.
[[nodiscard]] sptr_t sciColor(const QColor& c) {
    return static_cast<sptr_t>(c.red()) | (static_cast<sptr_t>(c.green()) << 8) |
           (static_cast<sptr_t>(c.blue()) << 16);
}
[[nodiscard]] sptr_t sciColorAlpha(const QColor& c) {
    return sciColor(c) | (static_cast<sptr_t>(0xFF) << 24);
}

[[nodiscard]] bool isIdentStart(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '$';
}
[[nodiscard]] bool isIdentChar(char ch) {
    return isIdentStart(ch) || (ch >= '0' && ch <= '9');
}

[[nodiscard]] int classifyWord(std::string_view word, CodeEditor::Language lang) {
    static constexpr std::array kKeywords = {
        "async",    "await",    "break",  "case",   "catch", "class",      "const",   "continue",
        "debugger", "default",  "delete", "do",     "else",  "export",     "extends", "finally",
        "for",      "function", "if",     "import", "in",    "instanceof", "let",     "new",
        "of",       "return",   "super",  "switch", "this",  "throw",      "try",     "typeof",
        "var",      "void",     "while",  "with",   "yield"};
    static constexpr std::array kLiterals = {
        "true", "false", "null", "undefined", "NaN", "Infinity"};
    static constexpr std::array kBuiltins = {
        "ctx", "console", "JSON", "Math", "hmac", "jwt", "base64"};
    for (const auto* w : kLiterals) {
        if (word == w) {
            return kStyleLiteral;
        }
    }
    if (lang == CodeEditor::Language::JavaScript) {
        for (const auto* w : kKeywords) {
            if (word == w) {
                return kStyleKeyword;
            }
        }
        for (const auto* w : kBuiltins) {
            if (word == w) {
                return kStyleBuiltin;
            }
        }
    }
    return kStyleDefault;
}

}  // namespace

CodeEditor::CodeEditor(QWidget* parent) : ScintillaEditBase(parent) {
    send(SCI_SETTABWIDTH, 4);
    send(SCI_SETUSETABS, 0);
    send(SCI_SETWRAPMODE, 0);
    // Null lexer: we own styling and re-push it on every content change.
    send(SCI_SETILEXER, 0, 0);
    connect(this, &ScintillaEditBase::notifyChange, this, [this]() { styleDocument(); });
}

CodeEditor::~CodeEditor() = default;

void CodeEditor::applyTheme(const theming::Palette& palette, Language language) {
    language_ = language;

    const QByteArray mono = QByteArrayLiteral("Geist Mono");
    sends(SCI_STYLESETFONT, STYLE_DEFAULT, mono.constData());
    send(SCI_STYLESETSIZE, STYLE_DEFAULT, 12);
    send(SCI_STYLESETFORE, STYLE_DEFAULT, sciColor(palette.textPrimary));
    send(SCI_STYLESETBACK, STYLE_DEFAULT, sciColor(palette.surfaceSunken));
    send(SCI_STYLECLEARALL);  // propagate default to all styles first

    send(SCI_STYLESETFORE, kStyleKeyword, sciColor(palette.accentBase));
    send(SCI_STYLESETFORE, kStyleString, sciColor(palette.statusSuccess));
    send(SCI_STYLESETFORE, kStyleComment, sciColor(palette.textSecondary));
    send(SCI_STYLESETFORE, kStyleNumber, sciColor(palette.methodPut));
    send(SCI_STYLESETFORE, kStyleBuiltin, sciColor(palette.methodGet));
    send(SCI_STYLESETFORE, kStyleLiteral, sciColor(palette.methodPost));

    // Line-number margin.
    send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    send(SCI_SETMARGINWIDTHN, 0, 48);
    send(SCI_STYLESETFORE, STYLE_LINENUMBER, sciColor(palette.textSecondary));
    send(SCI_STYLESETBACK, STYLE_LINENUMBER, sciColor(palette.surfaceSunken));

    // Caret, current line, selection.
    send(SCI_SETCARETFORE, sciColor(palette.textPrimary));
    send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK, sciColorAlpha(palette.surfaceRaised));
    send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK, sciColorAlpha(palette.accentMuted));

    styleDocument();
}

void CodeEditor::setPlainText(const QString& text) {
    const QByteArray utf8 = text.toUtf8();
    sends(SCI_SETTEXT, 0, utf8.constData());
    styleDocument();
}

QString CodeEditor::toPlainText() const {
    const sptr_t len = send(SCI_GETLENGTH);
    if (len <= 0) {
        return {};
    }
    QByteArray buf(static_cast<int>(len) + 1, '\0');
    send(SCI_GETTEXT, static_cast<uptr_t>(len + 1), reinterpret_cast<sptr_t>(buf.data()));
    return QString::fromUtf8(buf.constData(), static_cast<int>(len));
}

void CodeEditor::styleDocument() {
    const sptr_t length = send(SCI_GETLENGTH);
    if (length <= 0) {
        return;
    }
    QByteArray buf(static_cast<int>(length) + 1, '\0');
    send(SCI_GETTEXT, static_cast<uptr_t>(length + 1), reinterpret_cast<sptr_t>(buf.data()));
    const std::string_view text(buf.constData(), static_cast<std::size_t>(length));

    send(SCI_STARTSTYLING, 0);
    std::size_t i = 0;
    const std::size_t n = text.size();
    const auto styleRun = [this](std::size_t runLength, int style) {
        send(SCI_SETSTYLING, static_cast<uptr_t>(runLength), style);
    };
    while (i < n) {
        const char ch = text[i];
        // Line comment.
        if (ch == '/' && i + 1 < n && text[i + 1] == '/') {
            std::size_t j = i + 2;
            while (j < n && text[j] != '\n') {
                ++j;
            }
            styleRun(j - i, kStyleComment);
            i = j;
            continue;
        }
        // Block comment.
        if (ch == '/' && i + 1 < n && text[i + 1] == '*') {
            std::size_t j = i + 2;
            while (j + 1 < n && !(text[j] == '*' && text[j + 1] == '/')) {
                ++j;
            }
            j = (j + 1 < n) ? j + 2 : n;
            styleRun(j - i, kStyleComment);
            i = j;
            continue;
        }
        // String (single / double / template); no nesting needed for hooks.
        if (ch == '"' || ch == '\'' || ch == '`') {
            std::size_t j = i + 1;
            while (j < n && text[j] != ch) {
                if (text[j] == '\\' && j + 1 < n) {
                    ++j;
                }
                ++j;
            }
            j = (j < n) ? j + 1 : n;
            styleRun(j - i, kStyleString);
            i = j;
            continue;
        }
        // Number.
        if (ch >= '0' && ch <= '9') {
            std::size_t j = i + 1;
            while (j < n && ((text[j] >= '0' && text[j] <= '9') || text[j] == '.')) {
                ++j;
            }
            styleRun(j - i, kStyleNumber);
            i = j;
            continue;
        }
        // Identifier / keyword.
        if (isIdentStart(ch)) {
            std::size_t j = i + 1;
            while (j < n && isIdentChar(text[j])) {
                ++j;
            }
            styleRun(j - i, classifyWord(text.substr(i, j - i), language_));
            i = j;
            continue;
        }
        styleRun(1, kStyleDefault);
        ++i;
    }
}

}  // namespace reqloom::desktop
