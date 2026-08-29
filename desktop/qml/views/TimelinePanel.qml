// TimelinePanel — the live run timeline . Renders
// AppController.timeline (a TimelineModel fed by every RunController signal):
// a run header, per-step rows with a status badge, and indented request /
// response / extraction child rows. Extraction outcomes are green when
// resolved and AMBER (not red) when null/missing — mirroring the old Widgets
// TimelinePanel and Presentation only.
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
    // Selection lives in TimelineModel, so it rides along in that model's
    // per-tab snapshot and can be read by the chain graph and the explorer.
    // Read-only here on purpose: this view WRITES selection only from a user
    // gesture (tap, arrow keys, sparkline click) and otherwise only READS it.
    // Binding a write back from a render-time change handler is what creates a
    // loop, so the two directions must not share a path.
    readonly property int selectedStepIndex: AppController.timeline.selectedStep
    property int revealedRootFailureRow: -1

    signal openRequestFieldRequested(string operationId, string field, string key)
    signal editSourceRequested(var action)

    function activateStep(stepIndex, rowIndex, reveal) {
        if (rowIndex >= 0) {
            timelineList.currentIndex = rowIndex;
            timelineList.currentStepIndex = Math.max(0, stepIndex);
            timelineList.followTail = rowIndex >= timelineList.count - 1;
            if (reveal) {
                timelineList.positionViewAtIndex(rowIndex, ListView.Center);
            }
        }
        if (stepIndex > 0) {
            AppController.timeline.selectedStep = stepIndex;
        }
    }

    function jumpToStep(stepIndex) {
        if (stepIndex <= 0) {
            return;
        }
        const rowIndex = AppController.timeline.rowForStep(stepIndex);
        if (rowIndex >= 0) {
            activateStep(stepIndex, rowIndex, true);
        }
    }

    function revealRootFailure() {
        const rowIndex = AppController.timeline.rootFailureRow;
        if (!panel.visible || rowIndex < 0 || rowIndex === panel.revealedRootFailureRow) {
            return;
        }
        panel.revealedRootFailureRow = rowIndex;
        timelineList.followTail = false;
        timelineList.positionViewAtIndex(rowIndex, ListView.Center);
    }

    onVisibleChanged: {
        if (visible) {
            Qt.callLater(panel.revealRootFailure);
        }
    }
    Component.onCompleted: Qt.callLater(panel.revealRootFailure)

    Connections {
        target: AppController.timeline
        function onRootFailureChanged() {
            // Row indexes are snapshot-local, so the same number can identify
            // different failures when switching tabs.
            panel.revealedRootFailureRow = -1;
            if (AppController.timeline.rootFailureRow >= 0) {
                Qt.callLater(panel.revealRootFailure);
            } else {
                // A reset/fresh run resumes streaming until another failure is revealed.
                timelineList.followTail = true;
            }
        }
    }

    function sourceActionLabel(kind) {
        if (kind === "environment") {
            return qsTr("Edit environment");
        }
        if (kind === "secret") {
            return qsTr("Set secret");
        }
        if (kind === "actor") {
            return qsTr("Edit actor");
        }
        if (kind === "extraction") {
            return qsTr("Edit extraction");
        }
        return qsTr("Edit source");
    }

    component DiagnosticAction: Button {
        id: diagnosticAction
        implicitHeight: Math.max(26, implicitContentHeight + DesignTokens.spaceXs * 2)
        leftPadding: DesignTokens.spaceSm
        rightPadding: DesignTokens.spaceSm
        activeFocusOnTab: visible
        background: Rectangle {
            radius: DesignTokens.radiusSm
            color: diagnosticAction.hovered || diagnosticAction.down || diagnosticAction.activeFocus ? DesignTokens.accentMuted : "transparent"
            border.width: 1
            border.color: diagnosticAction.activeFocus ? DesignTokens.accent : DesignTokens.borderStrong
        }
        contentItem: Text {
            text: diagnosticAction.text
            color: diagnosticAction.enabled ? DesignTokens.accent : DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontCaption
            font.weight: DesignTokens.weightMedium
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

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
            id: latencyChart
            Layout.fillWidth: true
            bars: AppController.timeline.latencyBars
            stats: AppController.timeline.latencyStats
            selectedStepIndex: panel.selectedStepIndex
            sloP95Ms: AppController.latencySloP95Ms
            onSloChangeRequested: ms => AppController.setLatencySlo(ms)
            onStepActivated: stepNumber => {
                const r = AppController.timeline.rowForStep(stepNumber);
                if (r >= 0) {
                    panel.activateStep(stepNumber, r, true);
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
            activeFocusOnTab: count > 0
            keyNavigationEnabled: true
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Execution timeline")
            Accessible.focusable: activeFocusOnTab
            Accessible.focused: activeFocus
            property int currentStepIndex: 0
            property bool followTail: true

            function activateCurrent() {
                if (timelineList.currentIndex >= 0) {
                    panel.activateStep(timelineList.currentStepIndex, timelineList.currentIndex, false);
                }
            }

            // Follow streamed rows until the user inspects older evidence.
            onCountChanged: {
                if (followTail) {
                    positionViewAtEnd();
                }
            }
            onMovementStarted: followTail = false
            onMovementEnded: {
                if (atYEnd) {
                    followTail = true;
                }
            }
            onCurrentIndexChanged: {
                // Keyboard-cursor bookkeeping only. This must NOT clear the
                // model's pinned step: a model reset parks currentIndex at -1,
                // so clearing here would wipe the selection a tab switch just
                // restored. TimelineModel::reset() owns clearing instead.
                if (currentIndex < 0) {
                    currentStepIndex = 0;
                }
            }
            onActiveFocusChanged: {
                if (activeFocus && currentIndex < 0 && count > 0) {
                    currentIndex = 0;
                }
            }
            Keys.onReturnPressed: timelineList.activateCurrent()
            Keys.onEnterPressed: timelineList.activateCurrent()
            Keys.onSpacePressed: timelineList.activateCurrent()

            delegate: Rectangle {
                id: row
                required property int index
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
                required property string op
                required property string variableName
                required property var diagnostics
                required property int blockedByStep
                required property bool rootFailure

                // Steps that needed this value. Only computed for a *missed*
                // extraction — that is the case where "what did this break?"
                // is the question, and it keeps the plan resolution off the
                // happy path.
                readonly property bool missedExtraction: row.kind === "extraction" && row.statusToken === "warning"
                property var consumers: []
                Component.onCompleted: {
                    if (row.missedExtraction) {
                        row.consumers = AppController.extractionConsumers(row.op, row.variableName);
                    }
                }

                // ponytail: duration is an internal "<integer> ms" contract.
                // Add a numeric model role before localizing or changing it.
                readonly property real responseDurationMs: kind === "response" && duration.length > 0 ? parseFloat(duration) : 0

                readonly property bool isChild: kind === "request" || kind === "response" || kind === "extraction"
                // Request / response rows render as columns (method/status · path
                // · size · clock · duration) rather than one packed detail line.
                readonly property bool isColumnar: kind === "request" || kind === "response"
                readonly property bool isHeader: kind === "runStart" || kind === "runEnd"
                readonly property bool isStep: kind === "step"
                readonly property bool selected: stepIndex > 0 && stepIndex === panel.selectedStepIndex
                readonly property bool keyboardCurrent: ListView.isCurrentItem && timelineList.activeFocus
                readonly property string accessibleName: title + (statusLabel.length > 0 ? qsTr(", %1").arg(statusLabel) : "") + (detail.length > 0 ? qsTr(", %1").arg(detail) : "") + (duration.length > 0 ? qsTr(", %1").arg(duration) : "")
                // Status-keyed accent for the execution nodes + step badge, so
                // each step group reads pass/fail at a glance (not text-only).
                readonly property color accentColor: statusToken.length > 0 ? DesignTokens.statusColor(statusToken) : DesignTokens.borderSubtle
                readonly property real threadCenterX: DesignTokens.spaceMd / 2
                readonly property real summaryCenterY: contentCol.y + rowContent.y + titleLabel.y + titleLabel.height / 2
                // A row can reveal more than fits on one line: a payload
                // (masked headers / response headers / raw error code). Those
                // rows get a chevron + click-to-open.
                readonly property bool expandable: value.length > 0 || diagnostics.length > 0
                // Only the first failure is opened automatically. Later errors
                // remain collapsed so the causal failure keeps visual priority.
                property bool expanded: expandable && rootFailure
                // Eight lines preserve timeline scanability; the toggle below
                // keeps the complete selectable value available on demand.
                readonly property int payloadPreviewLines: 8
                property bool showFullValue: false
                onExpandedChanged: {
                    if (!expanded) {
                        showFullValue = false;
                    }
                }

                // One-line rows need the fixed method/size/clock/duration columns
                // plus useful operation/path space. Below that sum, secondary
                // detail and the complete aligned metrics move to later lines.
                readonly property real inlineMinimumWidth: 54 + 56 + 64 + 48 + 160 + DesignTokens.spaceMd * 2 + DesignTokens.spaceSm * 7
                readonly property bool reflowDetails: width < inlineMinimumWidth
                readonly property bool hasStepGlyph: isStep && (statusToken === "success" || statusToken === "error")
                readonly property int reflowTitleColumn: (expandable ? 1 : 0) + ((subLabel.length > 0 || hasStepGlyph) ? 1 : 0)

                // Label the revealed payload by what kind of row owns it.
                readonly property string valueLabel: statusToken === "error" ? qsTr("Failure details") : kind === "request" ? qsTr("Request") : kind === "response" ? qsTr("Response") : kind === "extraction" ? qsTr("Source") : qsTr("Details")

                function activate() {
                    panel.activateStep(row.stepIndex, row.index, false);
                }

                width: ListView.view.width
                implicitHeight: contentCol.implicitHeight + DesignTokens.spaceSm
                radius: DesignTokens.radiusSm
                color: row.selected ? DesignTokens.accentMuted : row.expanded ? DesignTokens.surfaceSunken : (row.isHeader || (row.isStep && row.statusToken !== "blocked")) ? DesignTokens.surfaceSunken : "transparent"
                // Selection and keyboard focus use the product accent without
                // replacing the row's semantic status glyphs or text colours.
                border.width: row.keyboardCurrent ? 2 : (row.selected || row.isHeader || (row.isStep && row.statusToken !== "blocked")) ? 1 : 0
                border.color: row.keyboardCurrent || row.selected ? DesignTokens.accent : row.statusToken === "error" ? Qt.rgba(DesignTokens.statusError.r, DesignTokens.statusError.g, DesignTokens.statusError.b, 0.45) : DesignTokens.borderSubtle
                Accessible.role: Accessible.ListItem
                Accessible.name: row.accessibleName
                Accessible.selectable: row.stepIndex > 0
                Accessible.selected: row.selected
                Accessible.focusable: true
                Accessible.focused: row.keyboardCurrent
                Accessible.onPressAction: row.activate()
                ListView.onIsCurrentItemChanged: {
                    if (ListView.isCurrentItem) {
                        timelineList.currentStepIndex = row.stepIndex;
                        if (row.stepIndex > 0) {
                            // Arrow-key navigation is a gesture, so it may write.
                            AppController.timeline.selectedStep = row.stepIndex;
                        }
                        if (timelineList.activeFocus) {
                            timelineList.followTail = row.index >= timelineList.count - 1;
                        }
                    }
                }
                Behavior on color {
                    ColorMotion {}
                }

                // Keep the thread outside layouts so it cannot change row sizing.
                // Summary-line geometry keeps expansion from moving its nodes.
                Item {
                    anchors.fill: parent
                    Accessible.ignored: true

                    Rectangle {
                        x: row.threadCenterX - width / 2
                        y: row.kind === "runStart" ? row.summaryCenterY : -timelineList.spacing / 2
                        width: 1
                        height: Math.max(0, (row.kind === "runEnd" ? row.summaryCenterY : row.height + timelineList.spacing / 2) - y)
                        color: DesignTokens.borderStrong
                        Accessible.ignored: true
                    }

                    Rectangle {
                        visible: row.isStep || row.isHeader
                        x: row.threadCenterX - width / 2
                        y: row.summaryCenterY - height / 2
                        width: 6
                        height: 6
                        radius: width / 2
                        color: row.accentColor
                        Accessible.ignored: true
                    }

                    Rectangle {
                        visible: row.isChild
                        x: row.threadCenterX
                        y: row.summaryCenterY - height / 2
                        width: (row.expandable ? DesignTokens.spaceLg : DesignTokens.spaceLg - DesignTokens.spaceXs) - row.threadCenterX
                        height: 1
                        color: DesignTokens.borderStrong
                        Accessible.ignored: true
                    }

                    Rectangle {
                        visible: row.isChild && !row.expandable
                        x: DesignTokens.spaceLg - DesignTokens.spaceXs - width / 2
                        y: row.summaryCenterY - height / 2
                        width: 4
                        height: 4
                        radius: width / 2
                        color: row.accentColor
                        Accessible.ignored: true
                    }
                }

                ColumnLayout {
                    id: contentCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: row.isChild ? DesignTokens.spaceLg : DesignTokens.spaceMd
                    anchors.rightMargin: DesignTokens.spaceXs
                    spacing: DesignTokens.spaceXs

                    GridLayout {
                        id: rowContent
                        Layout.fillWidth: true
                        columns: 8
                        rowSpacing: DesignTokens.spaceXs
                        columnSpacing: DesignTokens.spaceSm

                        // Tap the summary line to expand/collapse. Scoped to the
                        // header row so selecting text in the expanded body below
                        // doesn't toggle it shut.
                        TapHandler {
                            onTapped: eventPoint => {
                                row.activate();
                                if (!row.expandable) {
                                    return;
                                }
                                const position = eventPoint.position;
                                const outsideDisclosure = position.x < disclosureButton.x || position.x > disclosureButton.x + disclosureButton.width || position.y < disclosureButton.y || position.y > disclosureButton.y + disclosureButton.height;
                                if (outsideDisclosure) {
                                    row.expanded = !row.expanded;
                                }
                            }
                        }

                        // Disclosure button — only on rows that have more to show.
                        ToolButton {
                            id: disclosureButton
                            visible: row.expandable
                            Layout.row: 0
                            Layout.column: 0
                            Layout.alignment: Qt.AlignVCenter
                            implicitWidth: 24
                            implicitHeight: 24
                            padding: 0
                            activeFocusOnTab: true
                            Accessible.name: row.expanded ? qsTr("Collapse %1 details").arg(row.title) : qsTr("Expand %1 details").arg(row.title)
                            onClicked: row.expanded = !row.expanded
                            contentItem: Label {
                                text: row.expanded ? "\u25BE" : "\u25B8"
                                color: row.isChild ? row.accentColor : DesignTokens.textSecondary
                                font.pixelSize: DesignTokens.fontCaption
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: DesignTokens.radiusSm
                                color: disclosureButton.hovered || disclosureButton.down || disclosureButton.activeFocus ? DesignTokens.accentMuted : "transparent"
                                border.width: disclosureButton.activeFocus ? 1 : 0
                                border.color: DesignTokens.accent
                            }
                        }

                        // Numbered sub-step badge ("1.1", "1.2") on request /
                        // response rows, mirroring the mockup.
                        Rectangle {
                            visible: row.subLabel.length > 0
                            Layout.row: 0
                            Layout.column: row.reflowDetails ? (row.expandable ? 1 : 0) : 1
                            Layout.alignment: Qt.AlignVCenter
                            implicitHeight: Math.max(18, subBadge.implicitHeight + DesignTokens.spaceXs)
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
                            visible: row.hasStepGlyph
                            Layout.row: 0
                            Layout.column: row.reflowDetails ? (row.expandable ? 1 : 0) : 2
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

                        TextMetrics {
                            id: titleTextMetrics
                            font: titleLabel.font
                            text: row.title
                        }

                        Label {
                            id: titleLabel
                            Layout.row: 0
                            Layout.column: row.reflowDetails ? row.reflowTitleColumn : 3
                            Layout.columnSpan: row.reflowDetails ? Math.max(1, 5 - row.reflowTitleColumn) : 1
                            Layout.fillWidth: true
                            // Measure outside the elided label so GridLayout
                            // cannot feed its assigned width back into the minimum.
                            Layout.minimumWidth: row.isColumnar ? Math.ceil(titleTextMetrics.advanceWidth) : 0
                            text: row.title
                            color: row.kind === "runEnd" ? (row.statusToken === "error" ? DesignTokens.statusError : row.statusToken === "success" ? DesignTokens.statusSuccess : row.statusToken === "cancelled" ? DesignTokens.statusWarning : DesignTokens.textPrimary) : row.isChild ? DesignTokens.textSecondary : DesignTokens.textPrimary
                            font.pixelSize: row.isHeader ? 13 : 12
                            font.weight: row.isHeader ? DesignTokens.weightSemiBold : row.kind === "step" ? DesignTokens.weightMedium : DesignTokens.weightRegular
                            font.family: row.kind === "extraction" ? DesignTokens.fontMono : DesignTokens.fontSans
                            elide: Text.ElideRight
                        }

                        StatusBadge {
                            visible: row.statusLabel.length > 0 && !row.isColumnar
                            Layout.row: 0
                            Layout.column: row.reflowDetails ? 5 : 4
                            token: row.statusToken.length > 0 ? row.statusToken : "idle"
                            label: row.statusLabel
                        }

                        // Method pill for request rows (mirrors the sidebar verb badge).
                        MethodBadge {
                            visible: row.method.length > 0
                            Layout.row: row.reflowDetails ? 1 : 0
                            Layout.column: row.reflowDetails ? 1 : 5
                            method: row.method.length > 0 ? row.method : "GET"
                            minWidth: 54
                        }

                        // Request path column (request rows). Mono; elides when
                        // the panel is too narrow so it can never overrun the
                        // size / clock columns. The full URL is in the row's
                        // expansion (click the chevron).
                        Label {
                            visible: row.isColumnar && row.path.length > 0
                            Layout.row: row.reflowDetails ? 1 : 0
                            Layout.column: row.reflowDetails ? 2 : 6
                            Layout.columnSpan: row.reflowDetails ? 6 : 1
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
                            Layout.row: row.reflowDetails ? 1 : 0
                            Layout.column: row.reflowDetails ? row.reflowTitleColumn : 5
                            Layout.columnSpan: row.reflowDetails ? 8 - row.reflowTitleColumn : 2
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
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
                            horizontalAlignment: row.reflowDetails ? Text.AlignLeft : Text.AlignRight
                        }

                        // Step-header total response time, right-aligned (the
                        // detail label above takes the slack and pushes it right).
                        Label {
                            visible: row.isStep && row.duration.length > 0
                            Layout.row: 0
                            Layout.column: row.reflowDetails ? 6 : 7
                            Layout.columnSpan: row.reflowDetails ? 2 : 1
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

                        // Compact outcome cluster for request / response rows:
                        // status · size · clock · duration stay visually together.
                        Flickable {
                            id: metricViewport
                            visible: row.isColumnar
                            Layout.row: row.reflowDetails ? ((row.method.length > 0 || row.path.length > 0) ? 2 : 1) : 0
                            Layout.column: row.reflowDetails ? 0 : 7
                            Layout.columnSpan: row.reflowDetails ? 8 : 1
                            Layout.fillWidth: row.reflowDetails
                            Layout.minimumWidth: 0
                            implicitWidth: metricColumns.implicitWidth
                            implicitHeight: metricColumns.implicitHeight
                            contentWidth: metricColumns.implicitWidth
                            contentHeight: metricColumns.implicitHeight
                            clip: row.reflowDetails
                            boundsBehavior: Flickable.StopAtBounds
                            flickableDirection: Flickable.HorizontalFlick
                            ScrollBar.horizontal: ScrollBar {
                                policy: row.reflowDetails && metricViewport.contentWidth > metricViewport.width ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                            }
                            RowLayout {
                                id: metricColumns
                                spacing: DesignTokens.spaceMd
                                StatusBadge {
                                    visible: row.statusLabel.length > 0
                                    token: row.statusToken.length > 0 ? row.statusToken : "idle"
                                    label: row.statusLabel
                                }
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
                                    color: row.kind === "response" ? latencyChart.dotColor({
                                        "ms": row.responseDurationMs,
                                        "token": row.statusToken
                                    }) : DesignTokens.textSecondary
                                    font.pixelSize: DesignTokens.fontCaption
                                    font.family: DesignTokens.fontMono
                                    font.weight: DesignTokens.weightSemiBold
                                    font.features: ({
                                            "tnum": 1
                                        })
                                }
                            }
                        }
                    }

                    // A missed extraction on its own says little; what matters is
                    // which later steps needed the value. Naming them turns two
                    // seemingly separate failures into one cause.
                    Flow {
                        Layout.fillWidth: true
                        Layout.bottomMargin: DesignTokens.spaceXs
                        visible: row.consumers.length > 0
                        spacing: DesignTokens.spaceXs

                        Label {
                            text: qsTr("needed by")
                            color: DesignTokens.statusWarning
                            font.pixelSize: DesignTokens.fontCaption
                            font.weight: DesignTokens.weightSemiBold
                        }

                        Repeater {
                            model: row.consumers
                            delegate: Rectangle {
                                id: consumerChip
                                required property string modelData
                                readonly property int targetStep: AppController.timeline.stepForOperation(consumerChip.modelData)
                                implicitWidth: chipLabel.implicitWidth + DesignTokens.spaceSm * 2
                                implicitHeight: Math.max(18, chipLabel.implicitHeight + DesignTokens.spaceXs)
                                radius: DesignTokens.radiusSm
                                color: chipArea.containsMouse && consumerChip.targetStep > 0 ? DesignTokens.accentMuted : DesignTokens.surfaceBase
                                border.width: 1
                                border.color: chipArea.containsMouse && consumerChip.targetStep > 0 ? DesignTokens.accent : DesignTokens.borderSubtle

                                Label {
                                    id: chipLabel
                                    anchors.centerIn: parent
                                    text: consumerChip.modelData
                                    color: DesignTokens.textPrimary
                                    font.pixelSize: DesignTokens.fontCaption
                                    font.family: DesignTokens.fontMono
                                }
                                MouseArea {
                                    id: chipArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    // Only offer the jump when that step actually
                                    // ran; a consumer can be blocked and absent.
                                    cursorShape: consumerChip.targetStep > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                                    onClicked: {
                                        const step = consumerChip.targetStep;
                                        if (step > 0) {
                                            const at = AppController.timeline.rowForStep(step);
                                            panel.activateStep(step, at, true);
                                        }
                                    }
                                }
                                GlassToolTip {
                                    active: chipArea.containsMouse
                                    text: consumerChip.targetStep > 0 ? qsTr("Go to this step") : qsTr("This step did not run")
                                }
                            }
                        }
                    }

                    Flow {
                        Layout.fillWidth: true
                        Layout.bottomMargin: DesignTokens.spaceXs
                        visible: row.blockedByStep > 0
                        spacing: DesignTokens.spaceXs

                        DiagnosticAction {
                            text: qsTr("Show blocking step %1").arg(row.blockedByStep)
                            onClicked: panel.jumpToStep(row.blockedByStep)
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.bottomMargin: DesignTokens.spaceXs
                        visible: row.expanded && row.diagnostics.length > 0
                        spacing: DesignTokens.spaceSm

                        Label {
                            text: qsTr("Unresolved variables")
                            color: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontCaption
                            font.weight: DesignTokens.weightSemiBold
                            font.letterSpacing: 0.6
                        }

                        Repeater {
                            model: row.diagnostics
                            delegate: ColumnLayout {
                                id: diagnostic
                                required property int index
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: DesignTokens.spaceXs

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: DesignTokens.spaceSm

                                    Label {
                                        text: qsTr("%1. %2").arg(diagnostic.index + 1).arg(diagnostic.modelData.token)
                                        color: DesignTokens.textPrimary
                                        font.pixelSize: DesignTokens.fontLabel
                                        font.family: DesignTokens.fontMono
                                        font.weight: DesignTokens.weightSemiBold
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: diagnostic.modelData.location
                                        color: DesignTokens.textSecondary
                                        font.pixelSize: DesignTokens.fontCaption
                                        elide: Text.ElideRight
                                        horizontalAlignment: Text.AlignRight
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: diagnostic.modelData.causeText
                                    color: DesignTokens.textPrimary
                                    font.pixelSize: DesignTokens.fontCaption
                                    wrapMode: Text.WordWrap
                                }

                                Flow {
                                    Layout.fillWidth: true
                                    spacing: DesignTokens.spaceXs

                                    DiagnosticAction {
                                        visible: diagnostic.modelData.canOpenRequestField === true
                                        text: qsTr("Open request field")
                                        onClicked: panel.openRequestFieldRequested(row.op, diagnostic.modelData.requestField, diagnostic.modelData.requestKey)
                                    }
                                    DiagnosticAction {
                                        visible: diagnostic.modelData.canEditSource === true
                                        text: panel.sourceActionLabel(diagnostic.modelData.editKind)
                                        onClicked: panel.editSourceRequested(diagnostic.modelData)
                                    }
                                    DiagnosticAction {
                                        visible: diagnostic.modelData.canShowProducer === true
                                        text: qsTr("Show producer step %1").arg(diagnostic.modelData.producerStep)
                                        onClicked: panel.jumpToStep(diagnostic.modelData.producerStep)
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    visible: diagnostic.index + 1 < row.diagnostics.length
                                    implicitHeight: 1
                                    color: DesignTokens.borderSubtle
                                }
                            }
                        }
                    }

                    // Expanded detail stays selectable but begins as a bounded
                    // preview so one payload cannot consume the timeline viewport.
                    Rectangle {
                        id: payloadWell
                        visible: row.expanded && row.value.length > 0
                        Layout.fillWidth: true
                        Layout.bottomMargin: DesignTokens.spaceXs
                        implicitHeight: payloadContent.implicitHeight + DesignTokens.spaceSm * 2
                        radius: DesignTokens.radiusSm
                        color: DesignTokens.surfaceBase
                        border.width: 1
                        border.color: payloadText.activeFocus ? DesignTokens.accent : DesignTokens.borderSubtle
                        readonly property real previewHeight: payloadFontMetrics.lineSpacing * row.payloadPreviewLines

                        FontMetrics {
                            id: payloadFontMetrics
                            font: payloadText.font
                        }

                        ColumnLayout {
                            id: payloadContent
                            anchors.fill: parent
                            anchors.margins: DesignTokens.spaceSm
                            spacing: DesignTokens.spaceXs

                            RowLayout {
                                Layout.fillWidth: true

                                Label {
                                    text: row.valueLabel
                                    color: DesignTokens.textSecondary
                                    font.pixelSize: DesignTokens.fontCaption
                                    font.weight: DesignTokens.weightSemiBold
                                    font.letterSpacing: 0.6
                                }

                                Item {
                                    Layout.fillWidth: true
                                }

                                ToolButton {
                                    id: payloadToggle
                                    visible: payloadText.contentHeight > payloadWell.previewHeight + 0.5
                                    text: row.showFullValue ? qsTr("Show less") : qsTr("Show full")
                                    Accessible.name: text
                                    onClicked: row.showFullValue = !row.showFullValue
                                    contentItem: Label {
                                        text: payloadToggle.text
                                        color: DesignTokens.accent
                                        font.pixelSize: DesignTokens.fontCaption
                                        font.weight: DesignTokens.weightMedium
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    background: Rectangle {
                                        radius: DesignTokens.radiusSm
                                        color: payloadToggle.hovered || payloadToggle.down || payloadToggle.activeFocus ? DesignTokens.accentMuted : "transparent"
                                        border.width: payloadToggle.activeFocus ? 1 : 0
                                        border.color: DesignTokens.accent
                                    }
                                }
                            }

                            TextEdit {
                                id: payloadText
                                Layout.fillWidth: true
                                Layout.preferredHeight: row.showFullValue ? contentHeight : Math.min(contentHeight, payloadWell.previewHeight)
                                clip: !row.showFullValue
                                text: row.value
                                readOnly: true
                                activeFocusOnTab: true
                                Accessible.name: row.valueLabel
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
            }

            Connections {
                target: AppController.timeline
                function onModelReset() {
                    // Selection is deliberately not touched here. This fires for
                    // both a fresh run and a tab-snapshot restore; the model
                    // clears its own selection in the former and restores it in
                    // the latter, so writing 0 here would break tab switching.
                    timelineList.currentIndex = -1;
                    timelineList.currentStepIndex = 0;
                    timelineList.followTail = true;
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
