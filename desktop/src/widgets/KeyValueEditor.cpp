// KeyValueEditor — see header. Apidog-style auto-growing key/value table.
// Real QLineEdits (single-click editable, no double-click), a captions header
// with a divider, and an always-present trailing blank row that spawns the
// next one as soon as the user types — so there's no "+ Add" button and the
// empty state is itself an editable affordance. The widget grows with its
// rows; the parent scroll area handles overflow (no nested scrollbar).
#include "KeyValueEditor.h"

#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

namespace reqloom::desktop::widgets {

namespace {

// Object names used to locate a row's parts when reading back / removing.
constexpr auto kRowName = "kvRow";
constexpr auto kKeyName = "kvKey";
constexpr auto kValueName = "kvValue";
constexpr auto kRemoveName = "kvRemove";

}  // namespace

KeyValueEditor::KeyValueEditor(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Captions header with a divider underneath (Apidog "Name / Value" row).
    auto* captionsHost = new QWidget(this);
    captionsHost->setObjectName(QStringLiteral("kvCaptions"));
    captionsHost->setAttribute(Qt::WA_StyledBackground, true);
    auto* captions = new QHBoxLayout(captionsHost);
    const int hPad = theming::Theme::space(theming::Space::Sm);
    const int vPad = theming::Theme::space(theming::Space::Xs);
    captions->setContentsMargins(hPad, vPad, hPad, vPad);
    auto* keyCaption = new QLabel(QStringLiteral("Key"), captionsHost);
    keyCaption->setProperty("role", QStringLiteral("sectionHeading"));
    auto* valueCaption = new QLabel(QStringLiteral("Value"), captionsHost);
    valueCaption->setProperty("role", QStringLiteral("sectionHeading"));
    keyCaption_ = keyCaption;
    valueCaption_ = valueCaption;
    captions->addWidget(keyCaption, 1);
    captions->addWidget(valueCaption, 1);

    captionSpacer_ = new QWidget(captionsHost);
    captionSpacer_->setFixedWidth(24);  // default for Plain Mode: remove button (20) + spacing (4)
    captions->addWidget(captionSpacer_);

    layout->addWidget(captionsHost);

    // Rows stack vertically; the editor grows so the outer scroll area scrolls.
    rows_ = new QVBoxLayout;
    rows_->setContentsMargins(0, 0, 0, 0);
    rows_->setSpacing(0);
    layout->addLayout(rows_);
    layout->addStretch(1);

    // Start with the single trailing ghost row.
    syncRows();
}

KeyValueEditor::~KeyValueEditor() = default;

void KeyValueEditor::setMode(Mode mode) {
    mode_ = mode;
    if (captionSpacer_ != nullptr) {
        captionSpacer_->setFixedWidth(mode == Mode::FileCapable ? 48 : 24);
    }
}

void KeyValueEditor::setCaptions(const QString& keyCaption, const QString& valueCaption) {
    if (keyCaption_ != nullptr) {
        keyCaption_->setText(keyCaption);
    }
    if (valueCaption_ != nullptr) {
        valueCaption_->setText(valueCaption);
    }
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
    // Append the trailing ghost row so the user can always add another.
    syncRows();
}

void KeyValueEditor::addRow(const QString& key, const QString& value, bool focusKey) {
    auto* row = new QWidget(this);
    row->setObjectName(QString::fromUtf8(kRowName));
    row->setAttribute(Qt::WA_StyledBackground, true);
    auto* rowLayout = new QHBoxLayout(row);
    const int hPad = theming::Theme::space(theming::Space::Sm);
    const int vPad = theming::Theme::space(theming::Space::Xs);
    rowLayout->setContentsMargins(hPad, vPad, hPad, vPad);
    rowLayout->setSpacing(theming::Theme::space(theming::Space::Xs));

    const QFont mono = theme_.font(theming::TextStyle::Mono);

    auto* keyField = new QLineEdit(key, row);
    keyField->setObjectName(QString::fromUtf8(kKeyName));
    keyField->setPlaceholderText(QStringLiteral("Add key"));
    keyField->setFont(mono);
    keyField->setFrame(false);
    connect(keyField, &QLineEdit::textChanged, this, [this](const QString&) {
        syncRows();
        emit changed();
    });
    rowLayout->addWidget(keyField, 1);

    auto* valueField = new QLineEdit(value, row);
    valueField->setObjectName(QString::fromUtf8(kValueName));
    valueField->setPlaceholderText(mode_ == Mode::FileCapable
                                       ? QStringLiteral("value, or attach a file →")
                                       : QStringLiteral("value"));
    valueField->setFont(mono);
    valueField->setFrame(false);
    connect(valueField, &QLineEdit::textChanged, this, [this](const QString&) {
        syncRows();
        emit changed();
    });
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
    remove->setObjectName(QString::fromUtf8(kRemoveName));
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
        syncRows();
        emit changed();
    });
    rowLayout->addWidget(remove);

    rows_->addWidget(row);
    if (focusKey) {
        keyField->setFocus(Qt::OtherFocusReason);  // ready to type immediately
    }
}

void KeyValueEditor::syncRows() {
    const auto rowIsBlank = [](QWidget* row) {
        auto* k = row->findChild<QLineEdit*>(QString::fromUtf8(kKeyName));
        auto* v = row->findChild<QLineEdit*>(QString::fromUtf8(kValueName));
        const bool keyEmpty = (k == nullptr) || k->text().trimmed().isEmpty();
        const bool valEmpty = (v == nullptr) || v->text().trimmed().isEmpty();
        return keyEmpty && valEmpty;
    };

    // Ensure exactly one trailing blank row so there's always somewhere to
    // type the next pair (the Apidog ghost row).
    const int count = rows_->count();
    bool lastBlank = false;
    if (count > 0) {
        auto* lastItem = rows_->itemAt(count - 1);
        auto* lastRow = lastItem != nullptr ? lastItem->widget() : nullptr;
        lastBlank = (lastRow != nullptr) && rowIsBlank(lastRow);
    }
    if (count == 0 || !lastBlank) {
        addRow(QString{}, QString{}, /*focusKey=*/false);
    }

    // A blank row offers nothing to remove, so hide its ✕ — only populated
    // rows carry the affordance.
    for (int i = 0; i < rows_->count(); ++i) {
        auto* item = rows_->itemAt(i);
        auto* row = item != nullptr ? item->widget() : nullptr;
        if (row == nullptr) {
            continue;
        }
        if (auto* remove = row->findChild<QToolButton*>(QString::fromUtf8(kRemoveName))) {
            remove->setVisible(!rowIsBlank(row));
        }
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

}  // namespace reqloom::desktop::widgets
