// RequestEditor — read-only request view for the selected endpoint (ADR-007
// migration phase 3). Method+path bar, actor, the execution chain, and tabbed
// Headers / Params / Body / Chain. Editing lands in a follow-up phase.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

ColumnLayout {
    id: editor
    spacing: DesignTokens.spaceLg

    // ── Breadcrumb + back ──
    RowLayout {
        Layout.fillWidth: true
        spacing: DesignTokens.spaceSm

        Button {
            text: "←"
            implicitWidth: 32; implicitHeight: 32
            background: Rectangle {
                radius: DesignTokens.radiusSm
                color: parent.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                border.width: 1
                border.color: DesignTokens.borderSubtle
            }
            contentItem: Text {
                text: parent.text
                color: DesignTokens.textSecondary
                font.pixelSize: 16
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: AppController.closeOperation()
        }
        Label {
            text: AppController.selectedModule + " /"
            color: DesignTokens.textSecondary
            font.pixelSize: 13
        }
        Label {
            text: AppController.opName
            color: DesignTokens.textPrimary
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }
        Item { Layout.fillWidth: true }
        Label {
            visible: AppController.opActor.length > 0
            text: "👤  " + AppController.opActor
            color: DesignTokens.textSecondary
            font.pixelSize: 12
            padding: 6
            leftPadding: 10; rightPadding: 10
            background: Rectangle {
                radius: 11
                color: DesignTokens.surfaceSunken
                border.width: 1
                border.color: DesignTokens.borderSubtle
            }
        }
    }

    // ── Address bar: method + path + Send ──
    RowLayout {
        Layout.fillWidth: true
        spacing: DesignTokens.spaceSm

        MethodBadge {
            method: AppController.opMethod
            implicitHeight: 38
            Layout.preferredWidth: 64
        }

        Rectangle {
            Layout.fillWidth: true
            height: 38
            radius: DesignTokens.radiusSm
            color: DesignTokens.surfaceSunken
            border.width: 1
            border.color: DesignTokens.borderSubtle
            Label {
                anchors.fill: parent
                anchors.leftMargin: DesignTokens.spaceMd
                verticalAlignment: Text.AlignVCenter
                text: AppController.opPath
                color: DesignTokens.textPrimary
                font.pixelSize: 13
                font.family: "monospace"
                elide: Text.ElideRight
            }
        }

        Button {
            text: qsTr("Dry Run")
            enabled: !AppController.running
            implicitWidth: 84; implicitHeight: 38
            background: Rectangle {
                radius: DesignTokens.radiusSm
                color: parent.down ? Qt.rgba(1,1,1,0.08) : "transparent"
                border.width: 1
                border.color: DesignTokens.borderStrong
            }
            contentItem: Text {
                text: parent.text
                color: DesignTokens.textSecondary
                font.pixelSize: 13
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: AppController.runSelected(false, true)
        }

        Button {
            text: AppController.running ? qsTr("Running…") : qsTr("Send")
            enabled: !AppController.running
            implicitWidth: 96; implicitHeight: 38
            background: Rectangle {
                radius: DesignTokens.radiusSm
                color: !parent.enabled ? DesignTokens.borderStrong
                       : parent.down ? DesignTokens.accentHover : DesignTokens.accent
            }
            contentItem: Text {
                text: parent.text
                color: DesignTokens.textInverse
                font.pixelSize: 13
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: AppController.runSelected(false, false)
        }
    }

    // ── Execution chain ──
    ColumnLayout {
        Layout.fillWidth: true
        spacing: DesignTokens.spaceXs
        visible: AppController.opDependencies.length > 0

        Label {
            text: qsTr("EXECUTION CHAIN")
            color: DesignTokens.textSecondary
            font.pixelSize: 10
            font.weight: Font.DemiBold
            font.letterSpacing: 1.2
        }
        Rectangle {
            Layout.fillWidth: true
            radius: DesignTokens.radiusSm
            color: DesignTokens.surfaceSunken
            border.width: 1
            border.color: DesignTokens.borderSubtle
            implicitHeight: chainCol.implicitHeight + DesignTokens.spaceMd * 2
            ColumnLayout {
                id: chainCol
                anchors.fill: parent
                anchors.margins: DesignTokens.spaceMd
                spacing: 2
                Repeater {
                    model: AppController.opDependencies
                    delegate: RowLayout {
                        required property string modelData
                        spacing: DesignTokens.spaceSm
                        Label { text: "↳"; color: DesignTokens.textSecondary; font.pixelSize: 12 }
                        Label {
                            text: modelData
                            color: DesignTokens.textSecondary
                            font.pixelSize: 12
                            font.family: "monospace"
                        }
                    }
                }
                RowLayout {
                    spacing: DesignTokens.spaceSm
                    Label { text: "▸"; color: DesignTokens.accent; font.pixelSize: 12 }
                    Label {
                        text: AppController.selectedModule + "." + AppController.opName
                        color: DesignTokens.textPrimary
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        font.family: "monospace"
                    }
                }
            }
        }
    }

    // ── Tabs ──
    TabBar {
        id: tabs
        Layout.fillWidth: true
        background: Rectangle { color: "transparent" }
        Repeater {
            model: [qsTr("Headers"), qsTr("Params"), qsTr("Body"), qsTr("Chain")]
            delegate: TabButton {
                required property string modelData
                required property int index
                contentItem: Text {
                    text: modelData
                    color: tabs.currentIndex === index ? DesignTokens.textPrimary
                                                       : DesignTokens.textSecondary
                    font.pixelSize: 13
                    font.weight: tabs.currentIndex === index ? Font.DemiBold : Font.Normal
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: "transparent"
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width; height: 2
                        color: tabs.currentIndex === index ? DesignTokens.accent : "transparent"
                    }
                }
            }
        }
    }

    StackLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        currentIndex: tabs.currentIndex

        KeyValueList { model: AppController.opHeaders; emptyText: qsTr("No headers.") }
        KeyValueList { model: AppController.opQuery; emptyText: qsTr("No query parameters.") }

        // Body
        Rectangle {
            radius: DesignTokens.radiusSm
            color: DesignTokens.surfaceSunken
            border.width: 1
            border.color: DesignTokens.borderSubtle
            Flickable {
                anchors.fill: parent
                anchors.margins: DesignTokens.spaceMd
                contentHeight: bodyText.implicitHeight
                clip: true
                Text {
                    id: bodyText
                    width: parent.width
                    text: AppController.opBody.length > 0 ? AppController.opBody
                                                          : qsTr("No request body.")
                    color: AppController.opBody.length > 0 ? DesignTokens.textPrimary
                                                           : DesignTokens.textSecondary
                    font.pixelSize: 12
                    font.family: "monospace"
                    wrapMode: Text.WrapAnywhere
                }
            }
        }

        KeyValueList { model: AppController.opExtractions; emptyText: qsTr("No extractions.") }
    }
}
