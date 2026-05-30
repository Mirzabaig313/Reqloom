// KeyValueEditor — see header. Add/remove key/value rows with optional file
// references for form-data uploads.
#include "KeyValueEditor.h"

#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

namespace chainapi::desktop::widgets {

namespace {

constexpr int kKeyColumn = 0;
constexpr int kValueColumn = 1;
constexpr int kActionColumn = 2;

}  // namespace

KeyValueEditor::KeyValueEditor(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theming::Theme::space(theming::Space::Xs));

    table_ = new QTableWidget(this);
    table_->setColumnCount(3);
    table_->setHorizontalHeaderLabels({QStringLiteral("Key"), QStringLiteral("Value"), QString{}});
    table_->horizontalHeader()->setSectionResizeMode(kKeyColumn, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(kValueColumn, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(kActionColumn, QHeaderView::ResizeToContents);
    table_->verticalHeader()->setVisible(false);
    layout->addWidget(table_);

    auto* row = new QHBoxLayout;
    addButton_ = new QToolButton(this);
    addButton_->setText(QStringLiteral("+ Add row"));
    addButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    row->addWidget(addButton_);
    row->addStretch(1);
    layout->addLayout(row);

    connect(addButton_, &QToolButton::clicked, this, [this]() {
        addRow(QString{}, QString{});
        emit changed();
    });
    connect(table_, &QTableWidget::cellChanged, this, [this](int, int) { emit changed(); });
}

KeyValueEditor::~KeyValueEditor() = default;

void KeyValueEditor::setMode(Mode mode) {
    mode_ = mode;
}

void KeyValueEditor::setTheme(const theming::Theme& theme) {
    theme_ = theme;
    const QFont mono = theme_.font(theming::TextStyle::Mono);
    table_->setFont(mono);
}

void KeyValueEditor::clear() {
    table_->setRowCount(0);
}

void KeyValueEditor::setPairs(const std::vector<std::pair<QString, QString>>& pairs) {
    table_->setRowCount(0);
    for (const auto& [key, value] : pairs) {
        addRow(key, value);
    }
}

void KeyValueEditor::addRow(const QString& key, const QString& value) {
    const int r = table_->rowCount();
    table_->insertRow(r);
    table_->setItem(r, kKeyColumn, new QTableWidgetItem(key));
    installValueWidget(r, value);

    // Trailing remove button. Looked up by row at click time via the button's
    // position, since row indices shift as rows are removed.
    auto* remove = new QToolButton(table_);
    remove->setText(QStringLiteral("✕"));
    remove->setToolTip(QStringLiteral("Remove row"));
    remove->setAutoRaise(true);
    connect(remove, &QToolButton::clicked, this, [this, remove]() {
        const int idx = table_->indexAt(remove->pos()).row();
        if (idx >= 0) {
            table_->removeRow(idx);
            emit changed();
        }
    });
    table_->setCellWidget(r, kActionColumn, remove);
}

void KeyValueEditor::installValueWidget(int row, const QString& value) {
    if (mode_ == Mode::Plain) {
        table_->setItem(row, kValueColumn, new QTableWidgetItem(value));
        return;
    }

    // FileCapable: an inline value field plus a "file" button. The field holds
    // the literal value (or `@/path` after a pick) so toStdMap reads it back.
    auto* cell = new QWidget(table_);
    auto* cellLayout = new QHBoxLayout(cell);
    cellLayout->setContentsMargins(0, 0, 0, 0);
    cellLayout->setSpacing(theming::Theme::space(theming::Space::Xs));

    auto* field = new QLineEdit(value, cell);
    field->setObjectName(QStringLiteral("kvValueField"));
    field->setPlaceholderText(QStringLiteral("value, or pick a file →"));
    connect(field, &QLineEdit::textChanged, this, [this](const QString&) { emit changed(); });
    cellLayout->addWidget(field, 1);

    auto* pick = new QToolButton(cell);
    pick->setText(QStringLiteral("📎"));
    pick->setToolTip(QStringLiteral("Attach a file (sends as @/path upload)"));
    pick->setAutoRaise(true);
    connect(pick, &QToolButton::clicked, this, [this, row]() { chooseFileForRow(row); });
    cellLayout->addWidget(pick);

    table_->setCellWidget(row, kValueColumn, cell);
}

void KeyValueEditor::chooseFileForRow(int row) {
    const QString path =
        QFileDialog::getOpenFileName(this, QStringLiteral("Choose file to upload"));
    if (path.isEmpty()) {
        return;
    }
    auto* cell = table_->cellWidget(row, kValueColumn);
    if (cell == nullptr) {
        return;
    }
    if (auto* field = cell->findChild<QLineEdit*>(QStringLiteral("kvValueField"))) {
        // Curl-style `@` prefix — the engine's multipart builder treats this as
        // a file upload.
        field->setText(QStringLiteral("@%1").arg(path));
        emit changed();
    }
}

std::map<std::string, std::string> KeyValueEditor::toStdMap() const {
    std::map<std::string, std::string> out;
    for (int r = 0; r < table_->rowCount(); ++r) {
        auto* keyItem = table_->item(r, kKeyColumn);
        const QString key = keyItem != nullptr ? keyItem->text().trimmed() : QString{};
        if (key.isEmpty()) {
            continue;
        }
        QString value;
        if (auto* cell = table_->cellWidget(r, kValueColumn)) {
            if (auto* field = cell->findChild<QLineEdit*>(QStringLiteral("kvValueField"))) {
                value = field->text();
            }
        } else if (auto* valueItem = table_->item(r, kValueColumn)) {
            value = valueItem->text();
        }
        out[key.toStdString()] = value.toStdString();
    }
    return out;
}

bool KeyValueEditor::isEmptyOfContent() const {
    return toStdMap().empty();
}

}  // namespace chainapi::desktop::widgets
