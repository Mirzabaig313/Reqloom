// ChainView — the execution chain, rendered as a vertical sequence of
// operation nodes with connectors (DESIGN.md §6.3 chain preview, the product's
// hero surface per §1.2). Each node shows its HTTP method as a colour-coded
// pill plus the operation id; the target operation is marked. This replaces the
// plain-text dependency list so ChainAPI's core value — the resolved chain — is
// shown visually, not spelled out as a label.
#pragma once

#include "../theming/Theme.h"

#include <QtWidgets/QWidget>

#include <utility>
#include <vector>

class QVBoxLayout;

namespace chainapi::desktop::widgets {

class ChainView : public QWidget {
    Q_OBJECT

public:
    /// One node in the chain: the operation id and its HTTP method verb.
    struct Node {
        QString operationId;
        QString method;  ///< uppercase verb, e.g. "POST" (empty → neutral)
        bool isTarget{false};
    };

    explicit ChainView(QWidget* parent = nullptr);
    ~ChainView() override;

    ChainView(const ChainView&) = delete;
    ChainView& operator=(const ChainView&) = delete;
    ChainView(ChainView&&) = delete;
    ChainView& operator=(ChainView&&) = delete;

    /// Replace the rendered chain. Nodes render top-to-bottom in execution
    /// order, with a connector between each; the last is normally the target.
    void setNodes(const std::vector<Node>& nodes);

    /// Show a single muted line when there's nothing to chain (no declared
    /// dependencies). Clears any rendered nodes.
    void setEmptyMessage(const QString& message);

    void setTheme(const theming::Theme& theme);

private:
    void rebuild();

    QVBoxLayout* body_{nullptr};
    std::vector<Node> nodes_;
    QString emptyMessage_;
    theming::Theme theme_{theming::Theme::resolve(theming::Appearance::Dark)};
};

}  // namespace chainapi::desktop::widgets
