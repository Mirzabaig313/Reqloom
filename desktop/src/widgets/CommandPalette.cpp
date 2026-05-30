// CommandPalette — see header. Top-level fuzzy launcher popup.
#include "CommandPalette.h"

#include "FuzzyMatch.h"

#include <QtCore/QEvent>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QGraphicsDropShadowEffect>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cstddef>

namespace chainapi::desktop::widgets {

namespace {

constexpr int kMaxResults = 50;
constexpr int kMaxRecent = 8;
constexpr int kPaletteWidth = 600;

// Stash the chosen item on the list row so accept can recover it.
constexpr int kItemIdRole = Qt::UserRole + 1;
constexpr int kItemKindRole = Qt::UserRole + 2;

}  // namespace

CommandPalette::CommandPalette(QWidget* parent) : QWidget(parent, Qt::Popup) {
    setObjectName(QStringLiteral("commandPalette"));
    setFixedWidth(kPaletteWidth);

    auto* layout = new QVBoxLayout(this);
    const int pad = theming::Theme::space(theming::Space::Sm);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(pad);

    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("paletteSearch"));
    search_->setPlaceholderText(QStringLiteral("Search operations…  (prefix > for commands)"));
    search_->setClearButtonEnabled(true);
    search_->installEventFilter(this);  // route arrows/enter from the field
    layout->addWidget(search_);

    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("paletteList"));
    list_->setUniformItemSizes(true);
    layout->addWidget(list_, 1);

    connect(search_, &QLineEdit::textChanged, this, &CommandPalette::refilter);
    connect(
        list_, &QListWidget::itemActivated, this, [this](QListWidgetItem*) { acceptCurrent(); });
}

CommandPalette::~CommandPalette() = default;

void CommandPalette::setItems(std::vector<PaletteItem> operations,
                              std::vector<PaletteItem> globalCommands) {
    operations_ = std::move(operations);
    globalCommands_ = std::move(globalCommands);
}

void CommandPalette::setTheme(const theming::Theme& theme) {
    theme_ = theme;
    search_->setFont(theme_.font(theming::TextStyle::Body));
    list_->setFont(theme_.font(theming::TextStyle::Body));
}

void CommandPalette::popUp(QWidget* anchor) {
    search_->clear();
    refilter(QString{});

    // Center horizontally over the anchor, near the top (DESIGN.md §6.7).
    const int height = 360;
    setFixedHeight(height);
    if (anchor != nullptr) {
        const QRect a = anchor->rect();
        const QPoint topCenter =
            anchor->mapToGlobal(QPoint(a.center().x() - (kPaletteWidth / 2), a.top() + 80));
        move(topCenter);
    }
    show();
    raise();
    search_->setFocus(Qt::ShortcutFocusReason);
}

void CommandPalette::refilter(const QString& query) {
    list_->clear();

    const bool commandMode = query.startsWith(QLatin1Char('>'));
    const QString needle = commandMode ? query.mid(1).trimmed() : query.trimmed();
    const std::vector<PaletteItem>& pool = commandMode ? globalCommands_ : operations_;

    struct Ranked {
        int score{0};
        const PaletteItem* item{nullptr};
    };
    std::vector<Ranked> ranked;
    ranked.reserve(pool.size());

    for (const auto& item : pool) {
        const int s = fuzzy::score(needle, item.label);
        if (s < 0 && !needle.isEmpty()) {
            continue;
        }
        int boosted = s;
        // Recent operations float up when the query is empty or weak (FR-14.3).
        if (!commandMode) {
            const auto it = std::find(recentOps_.begin(), recentOps_.end(), item.id);
            if (it != recentOps_.end()) {
                const auto rank = static_cast<int>(std::distance(recentOps_.begin(), it));
                boosted += (kMaxRecent - rank) * 4;
            }
        }
        ranked.push_back(Ranked{boosted, &item});
    }

    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        return a.score > b.score;
    });

    int shown = 0;
    for (const auto& r : ranked) {
        if (shown >= kMaxResults) {
            break;
        }
        auto* row = new QListWidgetItem(list_);
        const QString text = r.item->detail.isEmpty()
                                 ? r.item->label
                                 : QStringLiteral("%1    %2").arg(r.item->label, r.item->detail);
        row->setText(text);
        row->setData(kItemIdRole, r.item->id);
        row->setData(kItemKindRole, static_cast<int>(r.item->kind));
        ++shown;
    }

    if (list_->count() > 0) {
        list_->setCurrentRow(0);
    }
}

void CommandPalette::moveSelection(int delta) {
    const int count = list_->count();
    if (count == 0) {
        return;
    }
    int row = list_->currentRow() + delta;
    // Wrap so a held arrow cycles rather than dead-ends.
    row = ((row % count) + count) % count;
    list_->setCurrentRow(row);
}

void CommandPalette::acceptCurrent() {
    auto* row = list_->currentItem();
    if (row == nullptr) {
        return;
    }
    PaletteItem item;
    item.kind = static_cast<PaletteItem::Kind>(row->data(kItemKindRole).toInt());
    item.id = row->data(kItemIdRole).toString();
    item.label = row->text();

    if (item.kind == PaletteItem::Kind::Operation) {
        rememberRecent(item.id);
    }
    close();
    emit itemChosen(item);
}

void CommandPalette::rememberRecent(const QString& operationId) {
    std::erase(recentOps_, operationId);
    recentOps_.insert(recentOps_.begin(), operationId);
    if (recentOps_.size() > static_cast<std::size_t>(kMaxRecent)) {
        recentOps_.resize(static_cast<std::size_t>(kMaxRecent));
    }
}

void CommandPalette::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_Escape:
            close();
            return;
        case Qt::Key_Down:
            moveSelection(1);
            return;
        case Qt::Key_Up:
            moveSelection(-1);
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            acceptCurrent();
            return;
        default:
            break;
    }
    QWidget::keyPressEvent(event);
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event) {
    // The search field has focus, so arrow/enter keys land on it first. Route
    // navigation keys to our handler; let everything else type normally.
    if (watched == search_ && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        switch (keyEvent->key()) {
            case Qt::Key_Down:
                moveSelection(1);
                return true;
            case Qt::Key_Up:
                moveSelection(-1);
                return true;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                acceptCurrent();
                return true;
            case Qt::Key_Escape:
                close();
                return true;
            default:
                break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

}  // namespace chainapi::desktop::widgets
