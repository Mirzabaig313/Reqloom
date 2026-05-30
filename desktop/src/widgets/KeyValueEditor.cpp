// KeyValueEditor — see header. Postman-style growing list of key/value rows.
// Each row is real QLineEdits (editable on a single click, no double-click),
// and the widget grows with its rows so the parent scroll area handles
// overflow — no nested per-editor scrollbar.
#include "KeyValueEditor.h"

#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

namespace chainapi::desktop::widgets {

namespace {

// Object names used to locate a row's parts when reading back / removing.
constexpr auto kRowName = "kvRow";
constexpr auto kKeyName = "kvKey";
constexpr auto kValueName = "kvValue";

// Width reserved in the caption row above the per-row remove (✕) button so the
// Key/Value captions line up with the fields, not the button.
constexpr int kRowButtonColumnWidth = 28;

}  // namespace

KeyValueEditor::KeyValueEditor(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theming::Theme::space(theming::Space::Xs));

    // Column captions, aligned with the row fields below.
    auto* captions = new QHBoxLayout;
    const int hPad = theming::Theme::space(theming::Space::Sm);
    captions->setContentsMargins(hPad, 0, hPad, 0);
    auto* keyCaption = new QLabel(QStringLiteral("Key"), this);
    keyCaption->setProperty("role", QStringLiteral("sectionHeading"));
    auto* valueCaption = new QLabel(QStringLiteral("Value"), this);
    valueCaption->setProperty("role", QStringLiteral("sectionHeading"));
    captions->addWidget(keyCaption, 1);
    captions->addWidget(valueCaption, 1);
    // Room over the remove (✕) button column so captions align with fields.
    captions->addSpacing(kRowButtonColumnWidth);
    layout->addLayout(captions);

    // Rows stack vertically; the editor grows so the outer scroll area scrolls.
    rows_ = new QVBoxLayout;
    rows_->setContentsMargins(0, 0, 0, 0);
    rows_->setSpacing(theming::Theme::space(theming::Space::Xs));
    layout->addLayout(rows_);

    auto* addRowLayout = new QHBoxLayout;
    addButton_ = new QToolButton(this);
    addButton_->setText(QStringLiteral("+ Add"));
    addButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    addButton_->setAutoRaise(true);
    addRowLayout->addWidget(addButton_);
    addRowLayout->addStretch(1);
    layout->addLayout(addRowLayout);

    connect(addButton_, &QToolButton::clicked, this, [this]() {
        addRow(QString{}, QString{}, /*focusKey=*/true);
        emit changed();
    });
}

KeyValueEditor::~KeyValueEditor() = default;

void KeyValueEditor::setMode(Mode mode) {
    mode_ = mode;
}

void KeyValueEditor::setTheme(const theming::Theme& theme) {
    theme_ = theme;
    // Mono for the value/key fields so headers and JSON-ish values align.
    const QFont mono = theme_.font(theming::TextStyle::Mono);
    for (QLineEdit* field : findChildren<QLineEdit*>()) {
        field->setFont(mono);
    }
}

void KeyValueEditor::clear() {
    // Delete every row widget. Iterate a copy since we mutate as we go.
    const auto rowWidgets = findChildren<QWidget*>(QString::fromUtf8(kRowName));
    for (QWidget* row : rowWidgets) {
        delete row;
    }
}

void KeyValueEditor::setPairs(const std::vector<std::pair<QString, QString>>& pairs) {
    clear();
    for (const auto& [key, value] : pairs) {
        addRow(key, value, /*focusKey=*/false);
    }
}

void KeyValueEditor::addRow(const QString& key, const QString& value, bool focusKey) {
    auto* row = new QWidget(this);
    row->setObjectName(QString::fromUtf8(kRowName));
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(theming::Theme::space(theming::Space::Xs));

    const QFont mono = theme_.font(theming::TextStyle::Mono);

    auto* keyField = new QLineEdit(key, row);
    keyField->setObjectName(QString::fromUtf8(kKeyName));
    keyField->setPlaceholderText(QStringLiteral("key"));
    keyField->setFont(mono);
    connect(keyField, &QLineEdit::textChanged, this, [this](const QString&) { emit changed(); });
    rowLayout->addWidget(keyField, 1);

    auto* valueField = new QLineEdit(value, row);
    valueField->setObjectName(QString::fromUtf8(kValueName));
    valueField->setPlaceholderText(mode_ == Mode::FileCapable
                                       ? QStringLiteral("value, or attach a file →")
                                       : QStringLiteral("value"));
    valueField->setFont(mono);
    connect(valueField, &QLineEdit::textChanged, this, [this](const QString&) { emit changed(); });
    rowLayout->addWidget(valueField, 1);

    if (mode_ == Mode::FileCapable) {
        auto* pick = new QToolButton(row);
        pick->setText(QStringLiteral("📎"));
        pick->setToolTip(QStringLiteral("Attach a file (sent as an @/path upload)"));
        pick->setAutoRaise(true);
        connect(pick, &QToolButton::clicked, this, [this, valueField]() {
            const QString path =
                QFileDialog::getOpenFileName(this, QStringLiteral("Choose file to upload"));
            if (!path.isEmpty()) {
                // Curl-style `@` prefix — the engine treats this as an upload.
                valueField->setText(QStringLiteral("@%1").arg(path));
                emit changed();
            }
        });
        rowLayout->addWidget(pick);
    }

    auto* remove = new QToolButton(row);
    remove->setText(QStringLiteral("✕"));
    remove->setToolTip(QStringLiteral("Remove row"));
    remove->setAutoRaise(true);
    connect(remove, &QToolButton::clicked, this, [this, row]() {
        // Don't delete the row (and this button) synchronously inside its own
        // click handler. Detach it now so toStdMap()/badge counts update
        // immediately, then free it once the event returns.
        rows_->removeWidget(row);
        row->hide();
        row->deleteLater();
        emit changed();
    });
    rowLayout->addWidget(remove);

    rows_->addWidget(row);
    if (focusKey) {
        keyField->setFocus(Qt::OtherFocusReason);  // ready to type immediately
    }
}

std::map<std::string, std::string> KeyValueEditor::toStdMap() const {
    std::map<std::string, std::string> out;
    // Direct children of the rows layout, in visual order.
    for (int i = 0; i < rows_->count(); ++i) {
        auto* item = rows_->itemAt(i);
        auto* row = item != nullptr ? item->widget() : nullptr;
        if (row == nullptr) {
            continue;
        }
        auto* keyField = row->findChild<QLineEdit*>(QString::fromUtf8(kKeyName));
        auto* valueField = row->findChild<QLineEdit*>(QString::fromUtf8(kValueName));
        if (keyField == nullptr) {
            continue;
        }
        const QString key = keyField->text().trimmed();
        if (key.isEmpty()) {
            continue;
        }
        out[key.toStdString()] = valueField != nullptr ? valueField->text().toStdString() : "";
    }
    return out;
}

bool KeyValueEditor::isEmptyOfContent() const {
    return toStdMap().empty();
}

}  // namespace chainapi::desktop::widgets
