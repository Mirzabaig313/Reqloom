// ExtractionEditor — edits an operation's extract block as Variable + Path
// rows (mirrors the old ExtractionTableEditor). The source kind is derived from
// the path prefix in C++ on read-back, so it is never a separate field. Backed
// by an EditableKeyValueModel with a trailing ghost row.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root

    // An EditableKeyValueModel.
    required property var extractModel

    spacing: DesignTokens.spaceXs

    component Field: TextField {
        color: DesignTokens.textPrimary
        placeholderTextColor: DesignTokens.textSecondary
        font.pixelSize: DesignTokens.fontLabel
        implicitHeight: 32
        background: Rectangle {
            radius: DesignTokens.radiusSm
            color: DesignTokens.surfaceSunken
            border.width: 1
            border.color: parent.activeFocus ? DesignTokens.accent : DesignTokens.borderSubtle
        }
    }

    Repeater {
        model: root.extractModel

        delegate: RowLayout {
            id: row
            required property int index
            required property string key
            required property string value
            required property bool isGhost
            Layout.fillWidth: true
            spacing: DesignTokens.spaceXs

            Field {
                Layout.preferredWidth: 140
                text: row.key
                placeholderText: qsTr("variable")
                onTextEdited: root.extractModel.setKey(row.index, text)
            }
            Field {
                Layout.fillWidth: true
                text: row.value
                placeholderText: qsTr("$.body.path / $.headers.X")
                onTextEdited: root.extractModel.setValue(row.index, text)
            }
            ToolButton {
                id: extRemove
                visible: !row.isGhost
                implicitWidth: 28
                implicitHeight: 28
                text: "✕"
                onClicked: root.extractModel.removeRow(row.index)
                contentItem: Text {
                    text: extRemove.text
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontLabel
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: extRemove.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
            }
        }
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("$.headers.X · $.cookies.X · $.status_code · anything else is a JSON path")
        color: DesignTokens.textSecondary
        font.pixelSize: DesignTokens.fontCaption
        wrapMode: Text.WordWrap
    }
}
