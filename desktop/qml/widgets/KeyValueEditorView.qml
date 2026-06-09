// KeyValueEditorView — editable two-column (key, value) list backed by an
// EditableKeyValueModel with an always-present trailing ghost row (Apidog
// pattern; mirrors the old Widgets KeyValueEditor). Reused for an operation's
// headers, query params, and form-data body in Edit mode. C++ owns row state.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root

    // An EditableKeyValueModel.
    required property var kvModel
    property string keyPlaceholder: qsTr("key")
    property string valuePlaceholder: qsTr("value")

    spacing: DesignTokens.spaceXs

    component Field: TextField {
        color: DesignTokens.textPrimary
        placeholderTextColor: DesignTokens.textSecondary
        font.pixelSize: 12
        font.family: "monospace"
        implicitHeight: 32
        background: Rectangle {
            radius: DesignTokens.radiusSm
            color: DesignTokens.surfaceSunken
            border.width: 1
            border.color: parent.activeFocus ? DesignTokens.accent : DesignTokens.borderSubtle
        }
    }

    Repeater {
        model: root.kvModel

        delegate: RowLayout {
            id: row
            required property int index
            required property string key
            required property string value
            required property bool isGhost
            Layout.fillWidth: true
            spacing: DesignTokens.spaceXs

            Field {
                Layout.preferredWidth: 200
                text: row.key
                placeholderText: root.keyPlaceholder
                onTextEdited: root.kvModel.setKey(row.index, text)
            }
            Field {
                Layout.fillWidth: true
                text: row.value
                placeholderText: root.valuePlaceholder
                onTextEdited: root.kvModel.setValue(row.index, text)
            }
            ToolButton {
                id: kvRemove
                visible: !row.isGhost
                implicitWidth: 28
                implicitHeight: 28
                text: "\u2715"
                onClicked: root.kvModel.removeRow(row.index)
                contentItem: Text {
                    text: kvRemove.text
                    color: DesignTokens.textSecondary
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: kvRemove.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
            }
        }
    }
}
