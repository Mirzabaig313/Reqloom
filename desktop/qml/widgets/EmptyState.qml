// EmptyState — a teaching zero-state screen (no project open or no operation
// selected). Mirrors the old Widgets EmptyState (DESIGN.md §10).
import QtQuick
import QtQuick.Layouts
import Reqloom

ColumnLayout {
    id: root
    property string icon: "⚡"
    property string heading: qsTr("No project open")
    property string body: qsTr("Open a project to start building and running API chains.")

    anchors.centerIn: parent
    spacing: DesignTokens.spaceMd
    width: 320

    Text {
        Layout.alignment: Qt.AlignHCenter
        text: root.icon
        font.pixelSize: 40
        color: DesignTokens.textSecondary
    }
    Text {
        Layout.alignment: Qt.AlignHCenter
        text: root.heading
        color: DesignTokens.textPrimary
        font.pixelSize: 18
        font.weight: Font.DemiBold
        horizontalAlignment: Text.AlignHCenter
    }
    Text {
        Layout.fillWidth: true
        text: root.body
        color: DesignTokens.textSecondary
        font.pixelSize: 13
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
    }
}
