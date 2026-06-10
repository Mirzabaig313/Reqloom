// ChainView — the execution chain rendered as a vertical sequence of operation
// nodes (method pill + id) with ↓ connectors and a target marker (mirrors the
// old Widgets ChainView; DESIGN.md §6.3, the product's hero surface). Fed a
// list of {operationId, method, isTarget} nodes; shows a muted line when empty.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Rectangle {
    id: root

    // [{operationId: string, method: string, isTarget: bool}, …]
    property var nodes: []
    property string emptyText: ""

    radius: DesignTokens.radiusSm
    color: DesignTokens.surfaceSunken
    border.width: 1
    border.color: DesignTokens.borderSubtle
    implicitHeight: content.implicitHeight + DesignTokens.spaceMd * 2

    Label {
        anchors.fill: parent
        anchors.margins: DesignTokens.spaceMd
        visible: root.nodes.length === 0
        text: root.emptyText
        color: DesignTokens.textSecondary
        font.pixelSize: 12
        wrapMode: Text.WordWrap
        verticalAlignment: Text.AlignVCenter
    }

    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: DesignTokens.spaceMd
        spacing: 0
        visible: root.nodes.length > 0

        Repeater {
            model: root.nodes
            delegate: ColumnLayout {
                id: nodeCol
                required property int index
                required property var modelData
                Layout.fillWidth: true
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    spacing: DesignTokens.spaceSm

                    MethodBadge {
                        method: nodeCol.modelData.method
                        Layout.preferredWidth: 58
                    }
                    Label {
                        Layout.fillWidth: true
                        text: nodeCol.modelData.operationId
                        color: nodeCol.modelData.isTarget ? DesignTokens.textPrimary : DesignTokens.textSecondary
                        font.pixelSize: 12
                        font.family: "monospace"
                        font.weight: nodeCol.modelData.isTarget ? Font.DemiBold : Font.Normal
                        elide: Text.ElideRight
                    }
                    Label {
                        visible: nodeCol.modelData.isTarget
                        text: qsTr("target")
                        color: DesignTokens.accent
                        font.pixelSize: 11
                    }
                }

                // arrow connector between steps (not after the last node).
                AppIcon {
                    visible: nodeCol.index < root.nodes.length - 1
                    name: "arrow-down"
                    size: 16
                    color: DesignTokens.borderStrong
                    Layout.leftMargin: DesignTokens.spaceMd
                }
            }
        }
    }
}
