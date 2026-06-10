// EmptyState — a teaching zero-state screen (no project open or no operation
// selected). Mirrors the old Widgets EmptyState (DESIGN.md §10).
import QtQuick
import QtQuick.Layouts
import Reqloom

ColumnLayout {
    id: root
    property string iconName: "zap"
    property string heading: qsTr("No project open")
    property string body: qsTr("Open a project to start building and running API chains.")

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
}
