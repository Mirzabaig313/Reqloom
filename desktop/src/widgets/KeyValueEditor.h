// KeyValueEditor — a reusable Postman-style list of key/value rows, used for
// Override Mode's headers, query params, and form-data fields (DESIGN.md §6.3).
// Each row is a pair of immediately-editable QLineEdits (single click, no
// double-click cell editing), and the widget grows with its rows so the parent
// scroll area handles overflow — there's no nested per-editor scrollbar. In
// FileCapable mode each row gains a 📎 button that writes the curl-style
// `@/path` reference the engine's multipart builder understands.
#pragma once

#include "../theming/Theme.h"

#include <QtWidgets/QWidget>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

class QToolButton;
class QVBoxLayout;

namespace chainapi::desktop::widgets {

class KeyValueEditor : public QWidget {
    Q_OBJECT

public:
    /// Plain: value cells are free text. FileCapable: each value row also
    /// offers a file picker that writes `@<path>` (form-data uploads).
    enum class Mode : std::uint8_t { Plain, FileCapable };

    explicit KeyValueEditor(QWidget* parent = nullptr);
    ~KeyValueEditor() override;

    KeyValueEditor(const KeyValueEditor&) = delete;
    KeyValueEditor& operator=(const KeyValueEditor&) = delete;
    KeyValueEditor(KeyValueEditor&&) = delete;
    KeyValueEditor& operator=(KeyValueEditor&&) = delete;

    void setMode(Mode mode);
    void setTheme(const theming::Theme& theme);

    /// Replace all rows with `pairs` (insertion order preserved).
    void setPairs(const std::vector<std::pair<QString, QString>>& pairs);

    /// Current rows as an ordered key→value map. Blank-key rows are dropped;
    /// on duplicate keys the last row wins (mirrors the engine's map).
    [[nodiscard]] std::map<std::string, std::string> toStdMap() const;

    /// Whether there are no non-empty rows.
    [[nodiscard]] bool isEmptyOfContent() const;

    void clear();

signals:
    void changed();

private:
    void addRow(const QString& key, const QString& value, bool focusKey);

    QVBoxLayout* rows_{nullptr};
    QToolButton* addButton_{nullptr};
    Mode mode_{Mode::Plain};
    theming::Theme theme_{theming::Theme::resolve(theming::Appearance::Dark)};
};

}  // namespace chainapi::desktop::widgets
