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
        anchors.margins: DesignTokens.spaceLg
        spacing: DesignTokens.spaceSm

        RowLayout {
            Layout.fillWidth: true
            SectionLabel {
                text: qsTr("TIMELINE")
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

        // Response-time sparkline (median · p95 · max) for the current run.
        // Clicking a bar scrolls the event list to that step.
        LatencySparkline {
            Layout.fillWidth: true
            bars: AppController.timeline.latencyBars
            stats: AppController.timeline.latencyStats
            sloP95Ms: AppController.latencySloP95Ms
            onSloChangeRequested: ms => AppController.setLatencySlo(ms)
            onStepActivated: stepNumber => {
                const r = AppController.timeline.rowForStep(stepNumber);
                if (r >= 0) {
                    timelineList.positionViewAtIndex(r, ListView.Center);
                    timelineList.currentIndex = r;
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
                required property string method
                required property string path
                required property string size
                required property string clock
                required property string duration
                required property string subLabel

                readonly property bool isChild: kind === "request" || kind === "response" || kind === "extraction"
                // Request / response rows render as columns (method/status · path
                // · size · clock · duration) rather than one packed detail line.
                readonly property bool isColumnar: kind === "request" || kind === "response"
                readonly property bool isHeader: kind === "runStart" || kind === "runEnd"
                readonly property bool isStep: kind === "step"
                // Status-keyed accent for the left stripe + step badge, so each
                // step group reads pass/fail at a glance (not text-only).
                readonly property color accentColor: statusToken.length > 0 ? DesignTokens.statusColor(statusToken) : DesignTokens.borderSubtle
                // A row can reveal more than fits on one line: a payload
                // (masked headers / response headers / raw error code). Those
                // rows get a chevron + click-to-open.
                readonly property bool expandable: value.length > 0
                // Auto-open a failed step (or any error row carrying detail) so
                // the failure reason shows without a click; everything else
                // starts collapsed. Tapping the row still toggles it freely.
                property bool expanded: expandable && statusToken === "error"

                // Label the revealed payload by what kind of row owns it.
                readonly property string valueLabel: statusToken === "error" ? qsTr("Failure details") : kind === "request" ? qsTr("Request") : kind === "response" ? qsTr("Response") : kind === "extraction" ? qsTr("Source") : qsTr("Details")

                width: ListView.view.width
                implicitHeight: contentCol.implicitHeight + DesignTokens.spaceSm
                radius: DesignTokens.radiusSm
                color: expanded ? DesignTokens.surfaceSunken : (isHeader || isStep) ? DesignTokens.surfaceSunken : "transparent"
                // Card outline on step / run rows so each group reads as a
                // bordered card (mockup parity); status-tinted on failures.
                border.width: (row.isStep || row.isHeader) ? 1 : 0
                border.color: row.statusToken === "error" ? Qt.rgba(DesignTokens.statusError.r, DesignTokens.statusError.g, DesignTokens.statusError.b, 0.45) : DesignTokens.borderSubtle
                Behavior on color {
                    ColorMotion {}
                }

                // Status accent stripe on step / header rows — the colored left
                // edge that lets a step group read pass/fail at a glance.
                Rectangle {
                    visible: row.isStep || row.kind === "runEnd"
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.topMargin: 2
                    anchors.bottomMargin: 2
                    width: 3
                    radius: 1.5
                    color: row.accentColor
                }

                ColumnLayout {
                    id: contentCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: row.isChild ? DesignTokens.spaceLg : DesignTokens.spaceMd
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

                        // Numbered sub-step badge ("1.1", "1.2") on request /
                        // response rows, mirroring the mockup.
                        Rectangle {
                            visible: row.subLabel.length > 0
                            Layout.alignment: Qt.AlignVCenter
                            implicitHeight: 18
                            implicitWidth: subBadge.implicitWidth + DesignTokens.spaceSm * 2
                            radius: DesignTokens.radiusSm
                            color: DesignTokens.accentMuted
                            Label {
                                id: subBadge
                                anchors.centerIn: parent
                                text: row.subLabel
                                color: DesignTokens.accent
                                font.pixelSize: DesignTokens.fontCaption
                                font.family: DesignTokens.fontMono
                                font.weight: DesignTokens.weightSemiBold
                            }
                        }

                        // Circular pass/fail glyph on step rows, mirroring the
                        // mockup's check / cross before the operation name.
                        Rectangle {
                            visible: row.isStep && (row.statusToken === "success" || row.statusToken === "error")
                            Layout.alignment: Qt.AlignVCenter
                            implicitWidth: 16
                            implicitHeight: 16
                            radius: 8
                            color: "transparent"
                            border.width: 1.5
                            border.color: row.accentColor
                            Text {
                                anchors.centerIn: parent
                                text: row.statusToken === "error" ? "\u2717" : "\u2713"
                                color: row.accentColor
                                font.pixelSize: 9
                                font.weight: DesignTokens.weightBold
                            }
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

                        // Method pill for request rows (mirrors the sidebar verb badge).
                        MethodBadge {
                            visible: row.method.length > 0
                            method: row.method.length > 0 ? row.method : "GET"
                            minWidth: 54
                        }

                        // Request path column (request rows). Mono; elides when
                        // the panel is too narrow so it can never overrun the
                        // size / clock columns. The full URL is in the row's
                        // expansion (click the chevron).
                        Label {
                            visible: row.isColumnar && row.path.length > 0
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.alignment: Qt.AlignVCenter
                            text: row.path
                            color: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontCaption
                            font.family: DesignTokens.fontMono
                            elide: Text.ElideRight
                        }

                        // Non-columnar rows keep the single packed detail line
                        // (step status, extraction value, failure summary, …).
                        Label {
                            visible: !row.isColumnar
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            text: row.detail
                            color: row.statusToken === "warning" ? DesignTokens.statusWarning : row.statusToken === "error" ? DesignTokens.statusError : row.statusToken === "success" && row.kind === "extraction" ? DesignTokens.statusSuccess : DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontCaption
                            font.family: DesignTokens.fontMono
                            // Keep the inline detail to a single elided line so a
                            // long failure message (e.g. an RFC-7807 problem body)
                            // stays a tidy summary; the chevron reveals the full
                            // text in the selectable panel below.
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            horizontalAlignment: Text.AlignRight
                        }

                        // Step-header total response time, right-aligned (the
                        // detail label above takes the slack and pushes it right).
                        Label {
                            visible: row.isStep && row.duration.length > 0
                            Layout.alignment: Qt.AlignVCenter
                            Layout.rightMargin: DesignTokens.spaceXs
                            text: row.duration
                            color: row.statusToken === "error" ? DesignTokens.statusError : row.statusToken === "warning" ? DesignTokens.statusWarning : DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontCaption
                            font.family: DesignTokens.fontMono
                            font.weight: DesignTokens.weightMedium
                            font.features: ({
                                    "tnum": 1
                                })
                        }

                        // Right-aligned metrics cluster for request / response
                        // rows: size · clock · duration. Fixed-width, right-
                        // aligned columns so the values line up cleanly down the
                        // list (clean data separation, mockup parity).
                        RowLayout {
                            visible: row.isColumnar
                            Layout.alignment: Qt.AlignVCenter
                            spacing: DesignTokens.spaceMd
                            Label {
                                visible: row.size.length > 0
                                Layout.preferredWidth: 56
                                horizontalAlignment: Text.AlignRight
                                text: row.size
                                color: DesignTokens.textSecondary
                                font.pixelSize: DesignTokens.fontCaption
                                font.family: DesignTokens.fontMono
                                font.features: ({
                                        "tnum": 1
                                    })
                            }
                            Label {
                                visible: row.clock.length > 0
                                Layout.preferredWidth: 64
                                horizontalAlignment: Text.AlignRight
                                text: row.clock
                                color: DesignTokens.textSecondary
                                opacity: 0.75
                                font.pixelSize: DesignTokens.fontCaption
                                font.family: DesignTokens.fontMono
                                font.features: ({
                                        "tnum": 1
                                    })
                            }
                            Label {
                                Layout.preferredWidth: 48
                                horizontalAlignment: Text.AlignRight
                                text: row.duration
                                // Duration carries the row's status hue so a slow
                                // / failed response reads at a glance.
                                color: row.statusToken === "error" ? DesignTokens.statusError : row.statusToken === "warning" ? DesignTokens.statusWarning : DesignTokens.statusSuccess
                                font.pixelSize: DesignTokens.fontCaption
                                font.family: DesignTokens.fontMono
                                font.weight: DesignTokens.weightSemiBold
                                font.features: ({
                                        "tnum": 1
                                    })
                            }
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
            EmptyState {
                visible: timelineList.count === 0
                iconName: "zap"
                heading: qsTr("No run yet")
                body: qsTr("Send or Dry Run an endpoint to watch each step, request, response, and extraction stream in here.")
            }
        }

        // Footer hint bar (mockup parity): explains the dot ↔ step link.
        Rectangle {
            Layout.fillWidth: true
            visible: timelineList.count > 0
            radius: DesignTokens.radiusSm
            color: DesignTokens.surfaceSunken
            border.width: 1
            border.color: DesignTokens.borderSubtle
            implicitHeight: hintLabel.implicitHeight + DesignTokens.spaceMd
            Label {
                id: hintLabel
                anchors.fill: parent
                anchors.leftMargin: DesignTokens.spaceMd
                anchors.rightMargin: DesignTokens.spaceMd
                verticalAlignment: Text.AlignVCenter
                text: qsTr("Each dot in the latency chart is a request — click one to jump to its step.")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontCaption
                wrapMode: Text.WordWrap
            }
        }
    }
}
