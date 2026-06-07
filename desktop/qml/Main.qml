// Main — the QML application shell (ADR-007). Floating rounded panels on a deep
// surface, a refined sidebar, and an endpoints list. Presentation only; all
// state comes from the AppController singleton (C++).
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

ApplicationWindow {
    id: window
    width: 1180
    height: 760
    visible: true
    title: qsTr("Reqloom")
    color: DesignTokens.surfaceSunken

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: DesignTokens.spaceMd
        spacing: DesignTokens.spaceMd

        // ───────────────────────── Top strip ─────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: DesignTokens.spaceMd

            Item { Layout.fillWidth: true }

            Label {
                text: qsTr("Environment")
                color: DesignTokens.textSecondary
                font.pixelSize: 12
            }
            ComboBox {
                id: envCombo
                model: AppController.environments
                implicitWidth: 160
                implicitHeight: 32
                enabled: AppController.environments.length > 0
                onActivated: AppController.environment = currentText
                Component.onCompleted: currentIndex = Math.max(0, find(AppController.environment))
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: DesignTokens.surfaceRaised
                    border.width: 1
                    border.color: DesignTokens.borderSubtle
                }
                contentItem: Text {
                    leftPadding: DesignTokens.spaceSm
                    text: envCombo.displayText
                    color: DesignTokens.textPrimary
                    font.pixelSize: 12
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
            }

            CheckBox {
                id: captureCheck
                text: qsTr("Capture bodies")
                checked: AppController.captureBodies
                onToggled: AppController.captureBodies = checked
                contentItem: Text {
                    text: captureCheck.text
                    color: DesignTokens.textSecondary
                    font.pixelSize: 12
                    leftPadding: captureCheck.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
                indicator: Rectangle {
                    implicitWidth: 16; implicitHeight: 16
                    radius: 4
                    y: (captureCheck.height - height) / 2
                    color: captureCheck.checked ? DesignTokens.accent : DesignTokens.surfaceRaised
                    border.width: 1
                    border.color: captureCheck.checked ? DesignTokens.accent
                                                        : DesignTokens.borderStrong
                    Text {
                        anchors.centerIn: parent
                        visible: captureCheck.checked
                        text: "✓"
                        color: DesignTokens.textInverse
                        font.pixelSize: 11
                        font.weight: Font.Bold
                    }
                }
            }
        }

        // ───────────────────────── Panels ─────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: DesignTokens.spaceMd

        // ───────────────────────── Explorer (project tree) ─────────────────────────
        ExplorerPanel {
            id: explorer
            Layout.preferredWidth: 300
            Layout.fillHeight: true
            visible: !collapsed
            property bool collapsed: false
            onCollapseRequested: collapsed = true
        }

        // Thin rail to restore the explorer when collapsed.
        Rectangle {
            visible: explorer.collapsed
            Layout.preferredWidth: 28
            Layout.fillHeight: true
            radius: DesignTokens.radius
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.borderSubtle
            ToolButton {
                id: railButton
                anchors.top: parent.top
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.topMargin: DesignTokens.spaceSm
                text: "›"
                onClicked: explorer.collapsed = false
                contentItem: Text {
                    text: railButton.text
                    color: DesignTokens.textSecondary
                    font.pixelSize: 16
                    horizontalAlignment: Text.AlignHCenter
                }
                background: null
            }
        }

        // ───────────────────────── Center ─────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: DesignTokens.radius
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.borderSubtle

            // Endpoint list (shown when no operation is open)
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: DesignTokens.spaceXl
                spacing: DesignTokens.spaceLg
                visible: !AppController.hasOperation

                // Header
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        text: AppController.selectedModule.length > 0
                              ? AppController.selectedModule : qsTr("No module selected")
                        color: DesignTokens.textPrimary
                        font.pixelSize: 22
                        font.weight: Font.DemiBold
                    }
                    Label {
                        text: endpointList.count === 1
                              ? qsTr("1 endpoint") : qsTr("%1 endpoints").arg(endpointList.count)
                        color: DesignTokens.textSecondary
                        font.pixelSize: 13
                    }
                }

                // Endpoints
                ListView {
                    id: endpointList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: DesignTokens.spaceXs
                    model: AppController.operations

                    delegate: ItemDelegate {
                        id: opRow
                        required property string method
                        required property string name
                        required property string path
                        width: ListView.view.width
                        height: 56

                        background: Rectangle {
                            radius: DesignTokens.radiusSm
                            color: opRow.hovered ? Qt.rgba(1, 1, 1, 0.04) : DesignTokens.surfaceSunken
                            border.width: 1
                            border.color: opRow.hovered ? DesignTokens.borderStrong
                                                        : DesignTokens.borderSubtle
                        }

                        contentItem: RowLayout {
                            anchors.fill: parent
                            spacing: DesignTokens.spaceMd
                            MethodBadge { method: opRow.method }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Label {
                                    text: opRow.name
                                    color: DesignTokens.textPrimary
                                    font.pixelSize: 14
                                    font.weight: Font.Medium
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: opRow.path
                                    color: DesignTokens.textSecondary
                                    font.pixelSize: 12
                                    font.family: "monospace"
                                    elide: Text.ElideRight
                                }
                            }
                            Label {
                                text: "›"
                                color: DesignTokens.textSecondary
                                font.pixelSize: 18
                                opacity: opRow.hovered ? 1.0 : 0.4
                            }
                        }

                        onClicked: AppController.selectOperation(AppController.selectedModule,
                                                                opRow.name)
                    }

                    // Empty state
                    Label {
                        anchors.centerIn: parent
                        visible: endpointList.count === 0
                        text: qsTr("No endpoints in this module yet.")
                        color: DesignTokens.textSecondary
                        font.pixelSize: 13
                    }
                }
            }

            // Request editor (shown when an operation is open)
            RequestEditor {
                anchors.fill: parent
                anchors.margins: DesignTokens.spaceXl
                visible: AppController.hasOperation
            }
        }

        // ───────────────────────── Response (third column) ─────────────────────────
        ResponsePanel {
            Layout.preferredWidth: 400
            Layout.fillHeight: true
            visible: AppController.hasOperation
        }
        }
    }

    // ───────────────────────── Toast (transient feedback) ─────────────────────────
    // Minimal notification surface for create/rename/delete outcomes and errors.
    // The full Toast UI lands in WS-D; this keeps WS-A actions legible meanwhile.
    Rectangle {
        id: toast
        property string message: ""
        property bool isError: false
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: DesignTokens.spaceXl
        visible: opacity > 0
        opacity: 0
        radius: DesignTokens.radiusSm
        color: DesignTokens.surfaceRaised
        border.width: 1
        border.color: toast.isError ? DesignTokens.methodDelete : DesignTokens.borderStrong
        implicitWidth: toastLabel.implicitWidth + DesignTokens.spaceLg * 2
        implicitHeight: toastLabel.implicitHeight + DesignTokens.spaceMd * 2

        Label {
            id: toastLabel
            anchors.centerIn: parent
            text: toast.message
            color: toast.isError ? DesignTokens.methodDelete : DesignTokens.textPrimary
            font.pixelSize: 13
        }

        Behavior on opacity { NumberAnimation { duration: 150 } }
        Timer {
            id: toastTimer
            interval: 2600
            onTriggered: toast.opacity = 0
        }

        Connections {
            target: AppController
            function onNotify(message, isError) {
                toast.message = message
                toast.isError = isError
                toast.opacity = 1
                toastTimer.restart()
            }
        }
    }
}
