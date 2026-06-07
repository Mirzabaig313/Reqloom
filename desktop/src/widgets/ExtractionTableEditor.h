// ExtractionTableEditor — edits an operation's `extract:` block as Variable +
// Path rows. Reuses KeyValueEditor's ghost-row table and centralizes the
// path-prefix → Extraction::Source derivation the engine applies, so the
// extraction source is never a separate (and un-persistable) field.
#pragma once

#include <reqloom/engine/Operation.h>

#include "../theming/Theme.h"

#include <QtWidgets/QWidget>

#include <vector>

namespace reqloom::desktop::widgets {

class KeyValueEditor;

class ExtractionTableEditor : public QWidget {
    Q_OBJECT

public:
    explicit ExtractionTableEditor(QWidget* parent = nullptr);
    ~ExtractionTableEditor() override;

    ExtractionTableEditor(const ExtractionTableEditor&) = delete;
    ExtractionTableEditor& operator=(const ExtractionTableEditor&) = delete;
    ExtractionTableEditor(ExtractionTableEditor&&) = delete;
    ExtractionTableEditor& operator=(ExtractionTableEditor&&) = delete;

    void setTheme(const theming::Theme& theme);

    /// Replace all rows with `extractions` (variable + path; source ignored —
    /// it is derived from the path on read-back, see `extractions()`).
    void setExtractions(const std::vector<engine::Extraction>& extractions);

    /// Current rows as Extractions. The source is derived from the path prefix
    /// to match the YAML round-trip: `$.headers.X` → Header, `$.cookies.X` →
    /// Cookie, `$.status_code` → StatusCode, anything else → JsonPath.
    [[nodiscard]] std::vector<engine::Extraction> extractions() const;

signals:
    void changed();

private:
    KeyValueEditor* table_{nullptr};
};

}  // namespace reqloom::desktop::widgets
