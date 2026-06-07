// ExtractionTableEditor — see header.
#include "ExtractionTableEditor.h"

#include "KeyValueEditor.h"

#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

#include <string>
#include <utility>

namespace reqloom::desktop::widgets {

namespace {

/// Derive the extraction source from the path prefix, mirroring the engine's
/// YamlSchemaParser. Keeps the in-memory Extraction consistent with what a
/// reload would produce (the YAML stores only the path, not the source).
[[nodiscard]] engine::Extraction::Source sourceForPath(const std::string& path) {
    if (path.starts_with("$.headers.")) {
        return engine::Extraction::Source::Header;
    }
    if (path.starts_with("$.cookies.")) {
        return engine::Extraction::Source::Cookie;
    }
    if (path == "$.status_code") {
        return engine::Extraction::Source::StatusCode;
    }
    return engine::Extraction::Source::JsonPath;
}

}  // namespace

ExtractionTableEditor::ExtractionTableEditor(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theming::Theme::space(theming::Space::Xs));

    table_ = new KeyValueEditor(this);
    table_->setCaptions(QStringLiteral("Variable"), QStringLiteral("Path / expression"));
    layout->addWidget(table_);

    auto* hint = new QLabel(this);
    hint->setText(
        QStringLiteral("$.headers.X · $.cookies.X · $.status_code · anything else is a JSON path"));
    hint->setProperty("role", QStringLiteral("caption"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    connect(table_, &KeyValueEditor::changed, this, [this]() { emit changed(); });
}

ExtractionTableEditor::~ExtractionTableEditor() = default;

void ExtractionTableEditor::setTheme(const theming::Theme& theme) {
    table_->setTheme(theme);
}

void ExtractionTableEditor::setExtractions(const std::vector<engine::Extraction>& extractions) {
    std::vector<std::pair<QString, QString>> pairs;
    pairs.reserve(extractions.size());
    for (const auto& ext : extractions) {
        pairs.emplace_back(QString::fromStdString(ext.variableName),
                           QString::fromStdString(ext.sourcePath));
    }
    table_->setPairs(pairs);
}

std::vector<engine::Extraction> ExtractionTableEditor::extractions() const {
    std::vector<engine::Extraction> result;
    for (const auto& [variable, path] : table_->toStdMap()) {
        if (path.empty()) {
            continue;
        }
        engine::Extraction ext;
        ext.variableName = variable;
        ext.sourcePath = path;
        ext.source = sourceForPath(path);
        result.push_back(std::move(ext));
    }
    return result;
}

}  // namespace reqloom::desktop::widgets
