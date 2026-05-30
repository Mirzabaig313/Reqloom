// View-level test for ResponseViewerPanel — confirms the response body
// renders as a JSON tree + raw text when capture is on, and that a non-empty
// body with capture off surfaces the "enable capture" guidance rather than a
// blank pane. Drives the panel's public slot directly; no engine, no network.
#include "views/ResponseViewerPanel.h"

#include <gtest/gtest.h>

#include <QtWidgets/QApplication>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>

#include <QtCore/QString>
#include <QtCore/QtGlobal>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace chainapi::desktop::tests {

namespace {

// One QApplication for the whole binary — Qt widgets require it to exist.
// GoogleTest's main creates it before any test body runs.
QApplication* g_app = nullptr;

[[nodiscard]] QTreeWidget* bodyTree(ResponseViewerPanel& panel) {
    return panel.findChild<QTreeWidget*>(QStringLiteral("bodyTree"));
}

[[nodiscard]] QPlainTextEdit* bodyRaw(ResponseViewerPanel& panel) {
    return panel.findChild<QPlainTextEdit*>(QStringLiteral("bodyRaw"));
}

/// Depth-first search for a tree row whose field column equals `field`.
[[nodiscard]] QTreeWidgetItem* findRow(QTreeWidgetItem* node, const QString& field) {
    if (node->text(0) == field) {
        return node;
    }
    for (int i = 0; i < node->childCount(); ++i) {
        if (auto* hit = findRow(node->child(i), field)) {
            return hit;
        }
    }
    return nullptr;
}

[[nodiscard]] QTreeWidgetItem* findRow(QTreeWidget* tree, const QString& field) {
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        if (auto* hit = findRow(tree->topLevelItem(i), field)) {
            return hit;
        }
    }
    return nullptr;
}

}  // namespace

TEST(ResponseViewerPanel, renders_json_object_body_as_tree_with_values) {
    // Arrange — a nested object body, capture on (non-empty body delivered).
    ResponseViewerPanel panel;
    const QString body =
        QStringLiteral(R"({"data":{"order":{"id":"ord_abc123","total":4200}},"ok":true})");

    // Act
    panel.onResponseReceived(/*index=*/1,
                             /*status=*/200,
                             /*headers=*/QStringLiteral("X: y"),
                             /*bodySize=*/static_cast<int>(body.toUtf8().size()),
                             /*elapsedMs=*/12,
                             body);

    // Assert — the leaf the importer cares about (the order id) is present
    // with its value in column 1.
    auto* idRow = findRow(bodyTree(panel), QStringLiteral("id"));
    ASSERT_NE(idRow, nullptr);
    EXPECT_EQ(idRow->text(1), QStringLiteral("ord_abc123"));

    // Integers render without a trailing ".0".
    auto* totalRow = findRow(bodyTree(panel), QStringLiteral("total"));
    ASSERT_NE(totalRow, nullptr);
    EXPECT_EQ(totalRow->text(1), QStringLiteral("4200"));

    // Raw tab shows the body verbatim.
    EXPECT_EQ(bodyRaw(panel)->toPlainText(), body);
}

TEST(ResponseViewerPanel, clicking_a_value_copies_its_jsonpath_and_emits) {
    ResponseViewerPanel panel;
    const QString body =
        QStringLiteral(R"({"data":{"order":{"id":"ord_abc123"}},"items":[{"sku":"a"}]})");
    panel.onResponseReceived(
        1, 200, QStringLiteral("X: y"), static_cast<int>(body.toUtf8().size()), 12, body);

    QString emitted;
    QObject::connect(
        &panel, &ResponseViewerPanel::jsonPathCopied, &panel, [&emitted](const QString& p) {
            emitted = p;
        });

    auto* tree = bodyTree(panel);
    auto* idRow = findRow(tree, QStringLiteral("id"));
    ASSERT_NE(idRow, nullptr);

    // Simulate the click handler's effect: emit via the same path the slot uses.
    emit tree->itemClicked(idRow, 0);

    EXPECT_EQ(emitted, QStringLiteral("$.data.order.id"));
    EXPECT_EQ(QGuiApplication::clipboard()->text(), QStringLiteral("$.data.order.id"));

    // Array element path uses bracket indexing.
    auto* skuRow = findRow(tree, QStringLiteral("sku"));
    ASSERT_NE(skuRow, nullptr);
    emit tree->itemClicked(skuRow, 0);
    EXPECT_EQ(emitted, QStringLiteral("$.items[0].sku"));
}

TEST(ResponseViewerPanel, non_json_body_shows_raw_but_flags_tree_as_unstructured) {
    ResponseViewerPanel panel;
    const QString body = QStringLiteral("<html><body>Not Found</body></html>");

    panel.onResponseReceived(2,
                             404,
                             QStringLiteral("Content-Type: text/html"),
                             static_cast<int>(body.toUtf8().size()),
                             8,
                             body);

    // Raw still shows it; the tree explains it isn't JSON.
    EXPECT_EQ(bodyRaw(panel)->toPlainText(), body);
    EXPECT_NE(findRow(bodyTree(panel), QStringLiteral("(not JSON — see Raw tab)")), nullptr);
}

TEST(ResponseViewerPanel, empty_body_with_nonzero_size_prompts_to_enable_capture) {
    // Capture off → RunController delivers an empty body string even though
    // the response had bytes. The panel must explain how to see it, not show
    // a blank pane.
    ResponseViewerPanel panel;

    panel.onResponseReceived(0, 200, QStringLiteral("X: y"), /*bodySize=*/256, 5, QString{});

    EXPECT_TRUE(bodyRaw(panel)->toPlainText().contains(QStringLiteral("Capture response bodies")));
    EXPECT_TRUE(bodyRaw(panel)->toPlainText().contains(QStringLiteral("256")));
}

TEST(ResponseViewerPanel, genuinely_empty_body_reports_empty_not_capture_prompt) {
    ResponseViewerPanel panel;

    panel.onResponseReceived(0, 204, QString{}, /*bodySize=*/0, 3, QString{});

    EXPECT_TRUE(bodyRaw(panel)->toPlainText().contains(QStringLiteral("Empty response body")));
    EXPECT_FALSE(bodyRaw(panel)->toPlainText().contains(QStringLiteral("Capture response bodies")));
}

TEST(ResponseViewerPanel, reset_clears_a_previously_rendered_body) {
    ResponseViewerPanel panel;
    panel.onResponseReceived(
        1, 200, QStringLiteral("X: y"), 10, 12, QStringLiteral(R"({"id":"x"})"));
    ASSERT_NE(findRow(bodyTree(panel), QStringLiteral("id")), nullptr);

    panel.reset();

    EXPECT_EQ(findRow(bodyTree(panel), QStringLiteral("id")), nullptr);
}

}  // namespace chainapi::desktop::tests

int main(int argc, char** argv) {
    // Force the offscreen QPA platform before constructing QApplication.
    // gtest_discover_tests runs this binary to enumerate tests, and that
    // discovery process does not inherit CTest's ENVIRONMENT property, so on
    // a headless CI runner Qt would otherwise try the xcb plugin, fail to
    // connect to a display, and abort. Only set it when the caller hasn't,
    // so a developer can still force a real platform locally.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    chainapi::desktop::tests::g_app = &app;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
