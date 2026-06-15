// ChainDependencyTable — the whole chain as ONE table. A single header, then
// one row per step (method badge + endpoint id in the Endpoint column, that
// step's own extracted variables stacked in the Variable/Path columns), rows
// separated by dividers, and a single "+ Add dependency" + path hint at the
// bottom. Mirrors the New Endpoint dialog's single-table layout.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

ColumnLayout {
    id: root
    spacing: 0

    component CellField: TextField {
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

    // Column headers (shown once).
    RowLayout {
        Layout.fillWidth: true
        Layout.bottomMargin: DesignTokens.spaceSm
        spacing: DesignTokens.spaceSm
        Label {
            Layout.preferredWidth: 150
            text: qsTr("Endpoint")
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontCaption
            font.weight: DesignTokens.weightSemiBold
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Variable name")
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontCaption
            font.weight: DesignTokens.weightSemiBold
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Body path / Header")
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

    // One row per step in the chain.
    Repeater {
        model: AppController.chainEditor
        delegate: ColumnLayout {
            id: step
            required property int index
            required property string operationId
            required property string method
            required property bool isTarget
            required property var extractModel
            Layout.fillWidth: true
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: DesignTokens.spaceMd
                Layout.bottomMargin: DesignTokens.spaceMd
                spacing: DesignTokens.spaceSm

                // Endpoint column: method badge + id (+ target marker).
                ColumnLayout {
                    Layout.preferredWidth: 150
                    Layout.alignment: Qt.AlignTop
                    spacing: DesignTokens.spaceXs
                    MethodBadge {
                        method: step.method
                        Layout.preferredWidth: 54
                    }
                    Label {
                        Layout.fillWidth: true
                        text: step.operationId
                        color: DesignTokens.textPrimary
                        font.pixelSize: DesignTokens.fontLabel
                        font.family: DesignTokens.fontMono
                        wrapMode: Text.WrapAnywhere
                    }
                    Label {
                        visible: step.isTarget
                        text: qsTr("target")
                        color: DesignTokens.accent
                        font.pixelSize: DesignTokens.fontCaption
                    }
                }

                // This step's own extractions (editable, with a trailing ghost
                // row that grows as you type).
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: DesignTokens.spaceXs
                    Repeater {
                        model: step.extractModel
                        delegate: RowLayout {
                            id: exRow
                            required property int index
                            required property string key
                            required property string value
                            Layout.fillWidth: true
                            spacing: DesignTokens.spaceSm
                            CellField {
                                Layout.fillWidth: true
                                text: exRow.key
                                placeholderText: qsTr("variable_name")
                                onTextEdited: step.extractModel.setKey(exRow.index, text)
                            }
                            CellField {
                                Layout.fillWidth: true
                                text: exRow.value
                                placeholderText: qsTr("$.body.path / $.headers.X")
                                onTextEdited: step.extractModel.setValue(exRow.index, text)
                            }
                        }
                    }
                }

                ToolButton {
                    id: stepRemove
                    visible: !step.isTarget
                    Layout.alignment: Qt.AlignTop
                    implicitWidth: 28
                    implicitHeight: 28
                    text: "✕"
                    onClicked: {
                        AppController.chainRemoveStep(step.operationId);
                        Qt.callLater(AppController.syncChainEditorMembership);
                    }
                    contentItem: Text {
                        text: stepRemove.text
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontLabel
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: DesignTokens.radiusSm
                        color: stepRemove.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                    }
                }
            }

            // Row separator (under every step).
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: DesignTokens.borderSubtle
            }
        }
    }

    // Single add-dependency picker.
    GlassComboBox {
        id: addCombo
        Layout.fillWidth: true
        Layout.topMargin: DesignTokens.spaceMd
        readonly property var options: [qsTr("+ Add dependency")].concat(AppController.operationIds)
        model: addCombo.options
        currentIndex: 0
        onActivated: function (i) {
            if (i > 0) {
                AppController.chainAddDependency(addCombo.options[i]);
                addCombo.currentIndex = 0;
            }
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: DesignTokens.spaceSm
        text: qsTr("$.body.path · $.headers.X · $.cookies.X · $.status_code · anything else is a JSON path")
        color: DesignTokens.textSecondary
        font.pixelSize: DesignTokens.fontCaption
        wrapMode: Text.WordWrap
    }
}
