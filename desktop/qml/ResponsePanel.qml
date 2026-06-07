// ResponsePanel — shows the latest run's response: a status line (status code
// colored by class, elapsed, size) and Body / Headers tabs. Empty until a run.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Rectangle {
    id: panel
    radius: DesignTokens.radius
    color: DesignTokens.surfaceRaised
    border.width: 1
    border.color: DesignTokens.borderSubtle

    function statusColor(code) {
        if (code >= 200 && code < 300) return DesignTokens.methodPost   // green
        if (code >= 300 && code < 400) return DesignTokens.methodPut    // orange
        if (code >= 400) return DesignTokens.methodDelete               // red
        return DesignTokens.textSecondary
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: DesignTokens.spaceLg
        spacing: DesignTokens.spaceMd

        Label {
            text: qsTr("RESPONSE")
            color: DesignTokens.textSecondary
            font.pixelSize: 10
            font.weight: Font.DemiBold
            font.letterSpacing: 1.2
        }

        // Status line
        RowLayout {
            Layout.fillWidth: true
            visible: AppController.hasResponse
            spacing: DesignTokens.spaceMd
            Label {
                text: AppController.respStatus > 0 ? ("HTTP " + AppController.respStatus)
                                                   : AppController.runOutcome
                color: panel.statusColor(AppController.respStatus)
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }
            Label {
                text: AppController.respElapsedMs + " ms"
                color: DesignTokens.textSecondary
                font.pixelSize: 12
            }
            Label {
                text: AppController.respBodySize + " B"
                color: DesignTokens.textSecondary
                font.pixelSize: 12
            }
            Item { Layout.fillWidth: true }
        }

        TabBar {
            id: respTabs
            Layout.fillWidth: true
            visible: AppController.hasResponse
            background: Rectangle { color: "transparent" }
            Repeater {
                model: [qsTr("Body"), qsTr("Headers")]
                delegate: TabButton {
                    required property string modelData
                    required property int index
                    contentItem: Text {
                        text: modelData
                        color: respTabs.currentIndex === index ? DesignTokens.textPrimary
                                                               : DesignTokens.textSecondary
                        font.pixelSize: 13
                        font.weight: respTabs.currentIndex === index ? Font.DemiBold : Font.Normal
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: "transparent"
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width; height: 2
                            color: respTabs.currentIndex === index ? DesignTokens.accent : "transparent"
                        }
                    }
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: AppController.hasResponse
            currentIndex: respTabs.currentIndex

            Rectangle {
                radius: DesignTokens.radiusSm
                color: DesignTokens.surfaceSunken
                border.width: 1
                border.color: DesignTokens.borderSubtle
                Flickable {
                    anchors.fill: parent
                    anchors.margins: DesignTokens.spaceSm
                    contentHeight: respBody.implicitHeight
                    clip: true
                    Text {
                        id: respBody
                        width: parent.width
                        text: AppController.respBody.length > 0 ? AppController.respBody
                              : qsTr("(body not captured — enable “Capture bodies”)")
                        color: AppController.respBody.length > 0 ? DesignTokens.textPrimary
                                                                 : DesignTokens.textSecondary
                        font.pixelSize: 12
                        font.family: "monospace"
                        wrapMode: Text.WrapAnywhere
                    }
                }
            }

            Rectangle {
                radius: DesignTokens.radiusSm
                color: DesignTokens.surfaceSunken
                border.width: 1
                border.color: DesignTokens.borderSubtle
                Flickable {
                    anchors.fill: parent
                    anchors.margins: DesignTokens.spaceSm
                    contentHeight: respHeaders.implicitHeight
                    clip: true
                    Text {
                        id: respHeaders
                        width: parent.width
                        text: AppController.respHeaders
                        color: DesignTokens.textSecondary
                        font.pixelSize: 12
                        font.family: "monospace"
                        wrapMode: Text.WrapAnywhere
                    }
                }
            }
        }

        // Empty state
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !AppController.hasResponse
            Item { Layout.fillHeight: true }
            Label {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("No response yet")
                color: DesignTokens.textPrimary
                font.pixelSize: 14
                font.weight: Font.Medium
            }
            Label {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Press Send to run this endpoint's chain.")
                color: DesignTokens.textSecondary
                font.pixelSize: 12
            }
            Item { Layout.fillHeight: true }
        }
    }
}
