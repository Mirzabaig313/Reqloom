// AssertionEditor — edits an operation's `assert:` block as Expression +
// optional Name rows. Each row's expression is a predicate evaluated against
// the response after the request (status/body via the engine's predicate
// grammar). Backed by an EditableKeyValueModel (key = expression, value =
// optional label) with a trailing ghost row.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

ColumnLayout {
    id: root

    // An EditableKeyValueModel: key = expression, value = optional name.
    required property var assertModel

    spacing: DesignTokens.spaceXs

    Label {
        Layout.fillWidth: true
        text: qsTr("Checked against this endpoint's response after it runs. A failing assertion fails the step.")
        color: DesignTokens.textSecondary
        font.pixelSize: DesignTokens.fontCaption
        wrapMode: Text.WordWrap
    }

    component Field: TextField {
        color: DesignTokens.textPrimary
        placeholderTextColor: DesignTokens.textSecondary
        font.pixelSize: DesignTokens.fontLabel
        // Content-driven so the field grows with OS text; floored at the density
        // control height instead of a fixed 32 that clips.
        topPadding: DesignTokens.spaceXs
        bottomPadding: DesignTokens.spaceXs
        implicitHeight: Math.max(DesignTokens.controlHeight, contentHeight + topPadding + bottomPadding)
        background: Rectangle {
            radius: DesignTokens.radiusSm
            color: DesignTokens.surfaceSunken
            border.width: 1
            border.color: parent.activeFocus ? DesignTokens.accent : DesignTokens.borderSubtle
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: DesignTokens.spaceXs
        spacing: DesignTokens.spaceXs
        Label {
            Layout.fillWidth: true
            text: qsTr("Assertion")
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontCaption
            font.weight: DesignTokens.weightSemiBold
        }
        Label {
            Layout.preferredWidth: 150
            text: qsTr("Name (optional)")
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontCaption
            font.weight: DesignTokens.weightSemiBold
        }
        Label {
            Layout.preferredWidth: 64
            text: qsTr("Live")
            horizontalAlignment: Text.AlignHCenter
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontCaption
            font.weight: DesignTokens.weightSemiBold
        }
        Item {
            Layout.preferredWidth: 28
        }
    }
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: DesignTokens.borderSubtle
    }

    Repeater {
        model: root.assertModel

        delegate: RowLayout {
            id: row
            required property int index
            required property string key
            required property string value
            required property bool isGhost
            Layout.fillWidth: true
            spacing: DesignTokens.spaceXs

            Field {
                Layout.fillWidth: true
                text: row.key
                placeholderText: qsTr("$.status_code == 200")
                font.family: DesignTokens.fontMono
                onTextEdited: {
                    root.assertModel.setKey(row.index, text);
                    badge.refresh(text);
                }
            }
            Field {
                Layout.preferredWidth: 150
                text: row.value
                placeholderText: qsTr("label")
                onTextEdited: root.assertModel.setValue(row.index, text)
            }
            // Live pass/fail/syntax badge: evaluates the expression against the
            // latest response via the engine's predicate grammar. Hidden until
            // there's a response and a non-empty expression to test.
            Label {
                id: badge
                Layout.preferredWidth: 64
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: DesignTokens.fontCaption
                font.weight: DesignTokens.weightSemiBold

                property var result: ({})
                function refresh(expr) {
                    result = AppController.evaluateAssertion(expr === undefined ? row.key : expr);
                }

                visible: AppController.hasResponse && row.key.trim().length > 0
                text: !visible ? "" : (result.valid === false ? qsTr("⚠ error") : (result.passed ? qsTr("✓ pass") : qsTr("✗ fail")))
                color: result.valid === false ? DesignTokens.statusWarning : (result.passed ? DesignTokens.statusSuccess : DesignTokens.statusError)

                Component.onCompleted: refresh()

                HoverHandler {
                    id: badgeHover
                }
                ToolTip.visible: badgeHover.hovered && badge.visible && result.valid === false && (result.error || "").length > 0
                ToolTip.text: result.error || ""

                Connections {
                    target: AppController
                    // Re-test every row when a new response lands.
                    function onResponseChanged() {
                        badge.refresh();
                    }
                }
            }
            ToolButton {
                id: assertRemove
                visible: !row.isGhost
                implicitWidth: 28
                implicitHeight: 28
                text: "✕"
                onClicked: root.assertModel.removeRow(row.index)
                contentItem: Text {
                    text: assertRemove.text
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontLabel
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: assertRemove.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
            }
        }
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("Operators: == != < <= > >= in matches, joined with && / ||. Use $.path for body fields and $.status_code for the HTTP status.")
        color: DesignTokens.textSecondary
        font.pixelSize: DesignTokens.fontCaption
        wrapMode: Text.WordWrap
    }
}
