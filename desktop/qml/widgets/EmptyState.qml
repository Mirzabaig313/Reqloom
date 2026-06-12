// EmptyState — a teaching zero-state screen (no project open or no operation
// selected). Mirrors the old Widgets EmptyState (DESIGN.md §10).
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

ColumnLayout {
    id: root
    property string iconName: "zap"
    property string heading: qsTr("No project open")
    property string body: qsTr("Open a project to start building and running API chains.")
    // Optional primary action. When actionText is set, a button is shown and
    // clicking it emits actionTriggered — saves the user hunting the toolbar.
    property string actionText: ""
    signal actionTriggered

    anchors.centerIn: parent
    spacing: DesignTokens.spaceMd
    width: 320

    AppIcon {
        Layout.alignment: Qt.AlignHCenter
        name: root.iconName
        size: 40
    }
    Text {
        Layout.alignment: Qt.AlignHCenter
        text: root.heading
        color: DesignTokens.textPrimary
        font.pixelSize: DesignTokens.fontSubtitle
        font.weight: DesignTokens.weightSemiBold
        horizontalAlignment: Text.AlignHCenter
    }
    Text {
        Layout.fillWidth: true
        text: root.body
        color: DesignTokens.textSecondary
        font.pixelSize: DesignTokens.fontBody
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
    }
    Button {
        id: actionBtn
        visible: root.actionText.length > 0
        Layout.alignment: Qt.AlignHCenter
        Layout.topMargin: DesignTokens.spaceSm
        implicitHeight: 34
        leftPadding: DesignTokens.spaceLg
        rightPadding: DesignTokens.spaceLg
        onClicked: root.actionTriggered()
        background: Rectangle {
            radius: DesignTokens.radiusSm
            color: actionBtn.down ? DesignTokens.accentHover : DesignTokens.accent
        }
        contentItem: Text {
            text: root.actionText
            color: DesignTokens.textInverse
            font.pixelSize: DesignTokens.fontBody
            font.weight: DesignTokens.weightSemiBold
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }
}
