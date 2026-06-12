// TimelinePanel — the live run timeline (QML Migration Roadmap WS-C). Renders
// AppController.timeline (a TimelineModel fed by every RunController signal):
// a run header, per-step rows with a status badge, and indented request /
// response / extraction child rows. Extraction outcomes are green when
// resolved and AMBER (not red) when null/missing — mirroring the old Widgets
// TimelinePanel and DESIGN.md §2.5. Presentation only.
pragma ComponentBehavior: Bound
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
            Item {
                Layout.fillWidth: true
            }
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
                GlassToolTip {
                    active: resetBtn.hovered
                    text: qsTr("Clear session + extraction caches before the next run")
                }
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

                readonly property bool isChild: kind === "request" || kind === "response" || kind === "extraction"
                readonly property bool isHeader: kind === "runStart" || kind === "runEnd"
                // A row can reveal more than fits on one line: a payload
                // (masked headers / response headers / raw error code). Those
                // rows get a chevron + click-to-open.
                readonly property bool expandable: value.length > 0
                property bool expanded: false

                // Label the revealed payload by what kind of row owns it.
                readonly property string valueLabel: statusToken === "error" ? qsTr("Failure details") : kind === "request" ? qsTr("Request") : kind === "response" ? qsTr("Response") : kind === "extraction" ? qsTr("Source") : qsTr("Details")

                width: ListView.view.width
                implicitHeight: contentCol.implicitHeight + DesignTokens.spaceSm
                radius: DesignTokens.radiusSm
                color: expanded ? DesignTokens.surfaceSunken : isHeader ? DesignTokens.surfaceSunken : "transparent"

                ColumnLayout {
                    id: contentCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: row.isChild ? DesignTokens.spaceLg : DesignTokens.spaceXs
                    anchors.rightMargin: DesignTokens.spaceXs
                    spacing: DesignTokens.spaceXs

                    RowLayout {
                        id: rowContent
                        Layout.fillWidth: true
                        spacing: DesignTokens.spaceSm

                        // Tap the summary line to expand/collapse. Scoped to the
                        // header row so selecting text in the expanded body below
                        // doesn't toggle it shut.
                        TapHandler {
                            enabled: row.expandable
                            onTapped: row.expanded = !row.expanded
                        }

                        // Disclosure chevron — only on rows that have more to show.
                        Label {
                            visible: row.expandable
                            text: row.expanded ? "\u25BE" : "\u25B8"
                            color: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontCaption
                        }

                        Label {
                            text: row.title
                            color: row.kind === "runEnd" ? (row.statusToken === "error" ? DesignTokens.statusError : row.statusToken === "success" ? DesignTokens.statusSuccess : row.statusToken === "cancelled" ? DesignTokens.statusWarning : DesignTokens.textPrimary) : row.isChild ? DesignTokens.textSecondary : DesignTokens.textPrimary
                            font.pixelSize: row.isHeader ? 13 : 12
                            font.weight: row.isHeader ? DesignTokens.weightSemiBold : row.kind === "step" ? DesignTokens.weightMedium : DesignTokens.weightRegular
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
                            color: row.statusToken === "warning" ? DesignTokens.statusWarning : row.statusToken === "error" ? DesignTokens.statusError : row.statusToken === "success" && row.kind === "extraction" ? DesignTokens.statusSuccess : DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontCaption
                            font.family: DesignTokens.fontMono
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    // Expanded full detail — inline, selectable, wraps to its
                    // full height. No nested scroll box: the timeline list
                    // itself scrolls, so there's only ever one scrollbar.
                    ColumnLayout {
                        visible: row.expanded
                        Layout.fillWidth: true
                        Layout.bottomMargin: DesignTokens.spaceXs
                        spacing: 2

                        Label {
                            text: row.valueLabel
                            color: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontCaption
                            font.weight: DesignTokens.weightSemiBold
                            font.letterSpacing: 0.6
                        }
                        TextEdit {
                            Layout.fillWidth: true
                            text: row.value
                            readOnly: true
                            selectByMouse: true
                            persistentSelection: true
                            wrapMode: TextEdit.Wrap
                            textFormat: TextEdit.PlainText
                            color: DesignTokens.textPrimary
                            selectionColor: DesignTokens.accent
                            selectedTextColor: DesignTokens.textInverse
                            font.pixelSize: DesignTokens.fontLabel
                            font.family: DesignTokens.fontMono
                        }
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
