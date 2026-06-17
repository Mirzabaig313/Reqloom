// KeyValueList — renders a KeyValueModel (headers / params / extractions) as a
// two-column read-only list with an empty state.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Rectangle {
    id: root
    property alias model: list.model
    property string emptyText: qsTr("Nothing here yet.")

    radius: DesignTokens.radiusSm
    color: DesignTokens.surfaceSunken
    border.width: 1
    border.color: DesignTokens.borderSubtle

    ListView {
        id: list
        anchors.fill: parent
        anchors.margins: DesignTokens.spaceXs
        clip: true
        spacing: 0

        delegate: Rectangle {
            id: kvRow
            required property string key
            required property string value
            width: ListView.view.width
            height: 34
            color: "transparent"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: DesignTokens.spaceSm
                anchors.rightMargin: DesignTokens.spaceSm
                spacing: DesignTokens.spaceMd
                Label {
                    Layout.preferredWidth: parent.width * 0.34
                    text: kvRow.key
                    color: DesignTokens.textPrimary
                    font.pixelSize: DesignTokens.fontLabel
                    font.family: DesignTokens.fontMono
                    elide: Text.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    text: kvRow.value
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontLabel
                    font.family: DesignTokens.fontMono
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: DesignTokens.borderSubtle
                opacity: 0.5
            }
        }

        Label {
            anchors.centerIn: parent
            visible: list.count === 0
            text: root.emptyText
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontLabel
        }
    }
}
