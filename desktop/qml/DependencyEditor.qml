// DependencyEditor — edits an operation's depends_on list as a column of
// combo pickers over existing operation ids (mirrors the old
// DependencyListEditor). A dependency can never name something undefined, and
// an always-present trailing blank row grows the list. Backed by a
// DependencyEditModel (C++ owns state).
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root

    // A DependencyEditModel and the pickable operation ids.
    required property var depModel
    required property var candidates

    spacing: DesignTokens.spaceXs

    Repeater {
        model: root.depModel

        delegate: RowLayout {
            id: row
            required property int index
            required property string value
            required property bool isGhost
            Layout.fillWidth: true
            spacing: DesignTokens.spaceXs

            ComboBox {
                id: combo
                Layout.fillWidth: true
                implicitHeight: 32
                model: [qsTr("+ add dependency…")].concat(root.candidates)
                currentIndex: row.value.length === 0 ? 0 : Math.max(0, find(row.value))
                onActivated: function (i) {
                    root.depModel.setSelection(row.index, i === 0 ? "" : textAt(i))
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: DesignTokens.surfaceSunken
                    border.width: 1
                    border.color: combo.activeFocus ? DesignTokens.accent
                                                     : DesignTokens.borderSubtle
                }
                contentItem: Text {
                    leftPadding: DesignTokens.spaceSm
                    text: combo.displayText
                    color: row.isGhost ? DesignTokens.textSecondary : DesignTokens.textPrimary
                    font.pixelSize: 12
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
            }

            ToolButton {
                id: depRemove
                visible: !row.isGhost
                implicitWidth: 28
                implicitHeight: 28
                text: "✕"
                onClicked: root.depModel.removeRow(row.index)
                contentItem: Text {
                    text: depRemove.text
                    color: DesignTokens.textSecondary
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: depRemove.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
            }
        }
    }
}
