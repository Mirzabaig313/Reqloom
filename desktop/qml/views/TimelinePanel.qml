// TimelinePanel — the live run timeline (QML Migration Roadmap WS-C). Renders
// AppController.timeline (a TimelineModel fed by every RunController signal):
// a run header, per-step rows with a status badge, and indented request /
// response / extraction child rows. Extraction outcomes are green when
// resolved and AMBER (not red) when null/missing — mirroring the old Widgets
// TimelinePanel and DESIGN.md §2.5. Presentation only.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

pragma ComponentBehavior: Bound

Rectangle {
    id: panel
    radius: DesignTokens.radius
    color: DesignTokens.surfaceRaised
    border.width: 1
    border.color: DesignTokens.borderSubtle

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: DesignTokens.spaceMd
        spacing: DesignTokens.spaceSm

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("TIMELINE")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontCaption
                font.weight: DesignTokens.weightSemiBold
                font.letterSpacing: 1.2
            }
            Item { Layout.fillWidth: true }
            // Cancel an in-flight run; reset caches when idle.
            ToolButton {
                id: cancelBtn
                visible: AppController.running
                text: qsTr("Cancel")
                onClicked: AppController.cancelRun()
                contentItem: Text {
                    text: cancelBtn.text
                    color: DesignTokens.statusError
                    font.pixelSize: DesignTokens.fontLabel
                    font.weight: DesignTokens.weightSemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: cancelBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
            }
            ToolButton {
                id: resetBtn
                visible: !AppController.running
                text: qsTr("Reset caches")
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Clear session + extraction caches before the next run")
                onClicked: AppController.resetCaches()
                contentItem: Text {
                    text: resetBtn.text
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontLabel
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: resetBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
            }
        }

        ListView {
            id: timelineList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: AppController.timeline

            // Keep the latest streamed row in view during a run.
            onCountChanged: positionViewAtEnd()

            delegate: Rectangle {
                id: row
                required property string kind
                required property int stepIndex
                required property string title
                required property string detail
                required property string statusToken
                required property string statusLabel
                required property string value

                readonly property bool isChild: kind === "request" || kind === "response"
                                                 || kind === "extraction"
                readonly property bool isHeader: kind === "runStart" || kind === "runEnd"

                width: ListView.view.width
                implicitHeight: rowContent.implicitHeight + DesignTokens.spaceSm
                radius: DesignTokens.radiusSm
                color: isHeader ? DesignTokens.surfaceSunken : "transparent"

                RowLayout {
                    id: rowContent
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: row.isChild ? DesignTokens.spaceLg : DesignTokens.spaceXs
                    anchors.rightMargin: DesignTokens.spaceXs
                    spacing: DesignTokens.spaceSm

                    Label {
                        text: row.title
                        color: row.isChild ? DesignTokens.textSecondary : DesignTokens.textPrimary
                        font.pixelSize: row.isHeader ? 13 : 12
                        font.weight: row.isHeader ? DesignTokens.weightSemiBold
                                     : row.kind === "step" ? DesignTokens.weightMedium : DesignTokens.weightRegular
                        font.family: row.kind === "extraction" ? DesignTokens.fontMono : DesignTokens.fontSans
                        elide: Text.ElideRight
                    }

                    StatusBadge {
                        visible: row.statusLabel.length > 0
                        token: row.statusToken.length > 0 ? row.statusToken : "idle"
                        label: row.statusLabel
                    }

                    Label {
                        Layout.fillWidth: true
                        text: row.detail
                        color: row.statusToken === "warning" ? DesignTokens.statusWarning
                               : row.statusToken === "error" ? DesignTokens.statusError
                               : row.statusToken === "success" && row.kind === "extraction"
                                 ? DesignTokens.statusSuccess
                               : DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontCaption
                        font.family: DesignTokens.fontMono
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignRight
                        ToolTip.visible: row.value.length > 0 && hover.hovered
                        ToolTip.text: row.value
                        HoverHandler { id: hover }
                    }
                }
            }

            // Empty state.
            ColumnLayout {
                anchors.centerIn: parent
                width: parent.width - DesignTokens.spaceXl * 2
                visible: timelineList.count === 0
                spacing: DesignTokens.spaceXs
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("No run yet")
                    color: DesignTokens.textPrimary
                    font.pixelSize: DesignTokens.fontBody
                    font.weight: DesignTokens.weightMedium
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("Send or Dry Run an endpoint to watch each step, request, response, and extraction stream in here.")
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontCaption
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
