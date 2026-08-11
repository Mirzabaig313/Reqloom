// ResponsePanel — the run result surface (QML Migration Roadmap WS-C). A top
// Response | Timeline tab bar: Response shows the status line with a semantic
// class-coloured rail, a Save-as-example action, and an examples dropdown to
// re-show a saved response, plus Body (Tree) / Body (Raw) / Headers / Diff views; Timeline
// embeds the live per-step stream. Presentation only; state comes from
// AppController.
//
// Body (Tree) is a collapsible JSONPath-addressable tree (click a container to
// expand/collapse, click a leaf to copy its JSONPath). Diff reuses the tested
// LCS widgets::diff::lineDiff via AppController. The full collapsible
// ResponseBodyModel (roadmap T-C5) can still replace the JS tree for very large
// bodies.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Rectangle {
    id: panel
    radius: 0
    color: DesignTokens.glassFill
    border.width: 1
    border.color: DesignTokens.glassBorder

    function statusColor(code) {
        if (code >= 200 && code < 300)
            return DesignTokens.statusSuccess;
        if (code >= 300 && code < 400)
            return DesignTokens.statusWarning;
        if (code >= 400)
            return DesignTokens.statusError;
        return DesignTokens.textSecondary;
    }

    /// Emitted when the user closes the response panel; Main collapses it.
    signal closeRequested

    /// Reveal the Timeline tab (used when replaying a past run from history).
    function showTimeline() {
        topTabs.currentIndex = 1;
    }

    /// Whether the editor/response are stacked (true) or side-by-side (false),
    /// and a request to flip it — driven from Main's window.responseStacked.
    property bool stacked: false
    signal toggleStackRequested
    /// Request a specific split layout: stacked (horizontal divider) when true,
    /// side-by-side (vertical divider) when false.
    signal setStackedRequested(bool value)

    /// Pretty-print the Body (Raw) view (indented JSON) vs show it verbatim.
    property bool prettyRaw: true

    // Response body display format, auto-detected from the Content-Type header
    // and overridable via the Body (Raw) format dropdown (JSON/XML/HTML/…).
    readonly property var formatLabels: ["JSON", "XML", "HTML", "YAML", "JavaScript", "Markdown", "Text"]
    readonly property var formatValues: ["json", "xml", "html", "yaml", "javascript", "markdown", "text"]
    readonly property string detectedFormat: detectFormat(AppController.respHeaders)
    property string respFormat: detectedFormat
    // Re-snap to the detected format whenever a new response arrives.
    onDetectedFormatChanged: respFormat = detectedFormat
    // Preview rendered HTML / Markdown instead of the coloured source.
    property bool previewMode: false

    readonly property real availableContentWidth: Math.max(0, width - DesignTokens.spaceLg * 2)
    readonly property bool topControlsReflow: availableContentWidth < topTabs.implicitWidth + stackBtn.implicitWidth + closeBtn.implicitWidth + DesignTokens.spaceSm * 2
    // Measured against the tray's real content width rather than a guessed
    // minimum, so the actions only drop to a second row when they genuinely
    // cannot sit beside it.
    // Budget matches the real layout: one grid gap between the tray and the
    // action cell, plus one gap between the two buttons.
    readonly property bool statusActionsReflow: availableContentWidth < statusSummary.implicitWidth + copyBodyBtn.implicitWidth + saveExampleBtn.implicitWidth + DesignTokens.spaceSm * 2
    readonly property bool examplesReflow: availableContentWidth < examplesLabel.implicitWidth + 260 + DesignTokens.spaceSm
    readonly property bool responseTabsReflow: prettyBtn.visible && availableContentWidth < respTabs.implicitWidth + prettyBtn.implicitWidth + DesignTokens.spaceSm
    readonly property bool rawControlsReflow: availableContentWidth < 150 + Math.max(previewBtn.implicitWidth, rawPrettyBtn.implicitWidth) + DesignTokens.spaceSm
    readonly property bool diffPickerReflow: availableContentWidth < compareLabel.implicitWidth + 180 + DesignTokens.spaceSm

    // Map a response's Content-Type to a display-format token. Defaults to JSON
    // (the overwhelmingly common API shape) when there's no usable hint.
    function detectFormat(headers) {
        const m = headers.toLowerCase().match(/content-type:\s*([^\n;]+)/);
        const ct = m ? m[1].trim() : "";
        if (ct.indexOf("json") >= 0)
            return "json";
        if (ct.indexOf("html") >= 0)
            return "html";
        if (ct.indexOf("xml") >= 0)
            return "xml";
        if (ct.indexOf("yaml") >= 0)
            return "yaml";
        if (ct.indexOf("javascript") >= 0)
            return "javascript";
        if (ct.indexOf("markdown") >= 0)
            return "markdown";
        if (ct.length === 0)
            return "json";
        return "text";
    }

    // The body text to display, pretty-printed when it's JSON and Pretty is on.
    function displayBody() {
        const body = AppController.respBody;
        if (body.length === 0)
            return "";
        if (respFormat === "json" && prettyRaw)
            return prettyJson(body);
        return body;
    }

    // Pretty-print a JSON body for display; returns the raw text unchanged when
    // it isn't valid JSON. Pure presentation formatting (used by Raw + Diff).
    function prettyJson(body) {
        if (body.length === 0)
            return "";
        try {
            return JSON.stringify(JSON.parse(body), null, 2);
        } catch (e) {
            return body;
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: DesignTokens.spaceLg
        spacing: DesignTokens.spaceMd

        // Indeterminate progress while a run is in flight — an at-a-glance
        // "working" signal even before the first step streams in.
        BusyBar {
            Layout.fillWidth: true
            running: AppController.running
        }

        // ── Response | Timeline switch + close button ───────────────────────
        GridLayout {
            Layout.fillWidth: true
            columns: 3
            rowSpacing: DesignTokens.spaceXs
            columnSpacing: DesignTokens.spaceSm
            TabBar {
                id: topTabs
                Layout.row: 0
                Layout.column: 0
                Layout.columnSpan: panel.topControlsReflow ? 3 : 1
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                background: Rectangle {
                    color: "transparent"
                }
                Repeater {
                    model: [qsTr("Response"), qsTr("Timeline")]
                    delegate: TabButton {
                        id: topTabButton
                        required property string modelData
                        required property int index
                        contentItem: Text {
                            text: topTabButton.modelData
                            color: topTabs.currentIndex === topTabButton.index ? DesignTokens.textPrimary : DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontBody
                            font.weight: topTabs.currentIndex === topTabButton.index ? DesignTokens.weightSemiBold : DesignTokens.weightRegular
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            color: "transparent"
                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: 2
                                color: topTabs.currentIndex === topTabButton.index ? DesignTokens.accent : "transparent"
                            }
                        }
                    }
                }
            }
            Button {
                id: stackBtn
                Layout.row: panel.topControlsReflow ? 1 : 0
                Layout.column: 1
                implicitWidth: 28
                implicitHeight: 28
                GlassToolTip {
                    active: stackBtn.hovered
                    text: qsTr("Split layout")
                }
                onClicked: splitMenu.popup(stackBtn, 0, stackBtn.height + 4)
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: stackBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
                contentItem: AppIcon {
                    name: panel.stacked ? "rows" : "columns"
                    size: 16
                    anchors.centerIn: parent
                }

                // Split-layout chooser: horizontal (stacked) vs vertical
                // (side-by-side), with a check on the active one.
                GlassMenu {
                    id: splitMenu
                    GlassMenuItem {
                        text: qsTr("Split Horizontally")
                        onTriggered: panel.setStackedRequested(true)
                        contentItem: RowLayout {
                            spacing: DesignTokens.spaceSm
                            AppIcon {
                                name: "rows"
                                size: 15
                                Layout.alignment: Qt.AlignVCenter
                                color: DesignTokens.textSecondary
                            }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Split Horizontally")
                                color: DesignTokens.textPrimary
                                font.pixelSize: DesignTokens.fontBody
                            }
                            Text {
                                visible: panel.stacked
                                text: "\u2713"
                                color: DesignTokens.accent
                                font.pixelSize: DesignTokens.fontBody
                                Layout.alignment: Qt.AlignVCenter
                            }
                        }
                    }
                    GlassMenuItem {
                        text: qsTr("Split Vertically")
                        onTriggered: panel.setStackedRequested(false)
                        contentItem: RowLayout {
                            spacing: DesignTokens.spaceSm
                            AppIcon {
                                name: "columns"
                                size: 15
                                Layout.alignment: Qt.AlignVCenter
                                color: DesignTokens.textSecondary
                            }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Split Vertically")
                                color: DesignTokens.textPrimary
                                font.pixelSize: DesignTokens.fontBody
                            }
                            Text {
                                visible: !panel.stacked
                                text: "\u2713"
                                color: DesignTokens.accent
                                font.pixelSize: DesignTokens.fontBody
                                Layout.alignment: Qt.AlignVCenter
                            }
                        }
                    }
                }
            }
            Button {
                id: closeBtn
                Layout.row: panel.topControlsReflow ? 1 : 0
                Layout.column: 2
                implicitWidth: 28
                implicitHeight: 28
                GlassToolTip {
                    active: closeBtn.hovered
                    text: qsTr("Close response panel")
                }
                onClicked: panel.closeRequested()
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: closeBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
                contentItem: AppIcon {
                    name: "x"
                    size: 16
                    anchors.centerIn: parent
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: topTabs.currentIndex

            // ── Response content ────────────────────────────────────────────
            ColumnLayout {
                spacing: DesignTokens.spaceMd

                // Status tray + Copy + Save-as-example.
                GridLayout {
                    Layout.fillWidth: true
                    visible: AppController.hasResponse
                    columns: 2
                    rowSpacing: DesignTokens.spaceXs
                    columnSpacing: DesignTokens.spaceSm

                    // Grouped status tray: code + example + timing + size read
                    // as one unit instead of four loose labels (UI/UX review §5).
                    Rectangle {
                        id: statusSummary

                        readonly property color statusHue: panel.statusColor(AppController.respStatus)

                        Layout.row: 0
                        Layout.column: 0
                        Layout.columnSpan: panel.statusActionsReflow ? 2 : 1
                        // Hug the content: a stretched tray left "HTTP 200 …"
                        // floating in a wide empty pill and starved the actions
                        // of the room they needed to stay on this row.
                        Layout.minimumWidth: 0
                        implicitWidth: statusTray.implicitWidth + DesignTokens.spaceMd * 2
                        implicitHeight: Math.max(DesignTokens.controlHeight, statusTray.implicitHeight + DesignTokens.spaceXs * 2)
                        radius: DesignTokens.radiusSm
                        color: DesignTokens.surfaceSunken
                        border.width: 1
                        border.color: DesignTokens.borderSubtle
                        // Row, not Flow: its implicit width is pure content, so
                        // the pill can size to it without the width feeding back
                        // through wrapping. Narrowness is handled by the whole
                        // row reflowing plus the example name eliding.
                        Row {
                            id: statusTray
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: DesignTokens.spaceMd
                            spacing: DesignTokens.spaceSm
                            Label {
                                text: AppController.respStatus > 0 ? ("HTTP " + AppController.respStatus) : AppController.runOutcome
                                color: DesignTokens.textPrimary
                                font.pointSize: DesignTokens.fontLabelPointSize
                                font.family: DesignTokens.fontMono
                                font.weight: DesignTokens.weightSemiBold
                            }
                            Rectangle {
                                visible: AppController.shownExample.length > 0
                                implicitWidth: 1
                                implicitHeight: 14
                                color: DesignTokens.borderSubtle
                            }
                            Label {
                                visible: AppController.shownExample.length > 0
                                width: Math.min(implicitWidth, 160)
                                text: AppController.shownExample
                                color: DesignTokens.textSecondary
                                font.pointSize: DesignTokens.fontLabelPointSize
                                elide: Text.ElideRight
                            }
                            Rectangle {
                                implicitWidth: 1
                                implicitHeight: 14
                                color: DesignTokens.borderSubtle
                            }
                            Label {
                                text: AppController.respElapsedMs + qsTr(" ms")
                                color: DesignTokens.textSecondary
                                font.pointSize: DesignTokens.fontLabelPointSize
                                font.family: DesignTokens.fontMono
                                // Tabular figures so the ms value doesn't shift
                                // width as digits change between runs.
                                font.features: ({
                                        "tnum": 1
                                    })
                            }
                            Label {
                                text: AppController.respBodySize + qsTr(" B")
                                color: DesignTokens.textSecondary
                                font.pointSize: DesignTokens.fontLabelPointSize
                                font.family: DesignTokens.fontMono
                                font.features: ({
                                        "tnum": 1
                                    })
                            }
                        }

                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottom: parent.bottom
                            width: Math.max(0, Math.min(12, parent.width - DesignTokens.spaceXs * 2))
                            height: 2
                            radius: 1
                            color: statusSummary.statusHue
                            Accessible.ignored: true
                            Behavior on color {
                                ColorMotion {}
                            }
                        }
                    }

                    // Both actions live in one cell, so the row costs one grid gap
                    // plus one gap between them instead of three separate grid
                    // gaps. The leading spacer keeps them against the trailing
                    // edge without the tray having to stretch.
                    RowLayout {
                        Layout.row: panel.statusActionsReflow ? 1 : 0
                        Layout.column: panel.statusActionsReflow ? 0 : 1
                        Layout.columnSpan: panel.statusActionsReflow ? 2 : 1
                        Layout.fillWidth: true
                        spacing: DesignTokens.spaceSm

                        Item {
                            Layout.fillWidth: true
                        }

                        Button {
                            id: copyBodyBtn
                            text: qsTr("Copy")
                            implicitHeight: Math.max(28, contentItem.implicitHeight + DesignTokens.spaceXs * 2)
                            leftPadding: DesignTokens.spaceSm
                            rightPadding: DesignTokens.spaceSm
                            enabled: AppController.respBody.length > 0
                            GlassToolTip {
                                active: copyBodyBtn.hovered
                                text: qsTr("Copy the full response body")
                            }
                            onClicked: AppController.copyToClipboard(AppController.respBody, qsTr("response body"))
                            contentItem: Text {
                                text: copyBodyBtn.text
                                color: copyBodyBtn.enabled ? DesignTokens.accent : DesignTokens.textSecondary
                                font.pixelSize: DesignTokens.fontLabel
                                font.weight: DesignTokens.weightSemiBold
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: DesignTokens.radiusSm
                                color: copyBodyBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                                border.width: 1
                                border.color: DesignTokens.borderSubtle
                            }
                        }
                        Button {
                            id: saveExampleBtn
                            // Shortened from "Save as example": the full phrase cost
                            // ~25 DIP that pushed this pair onto its own row. The
                            // meaning lives in the tooltip and the dialog title.
                            text: qsTr("Save example")
                            implicitHeight: Math.max(28, contentItem.implicitHeight + DesignTokens.spaceXs * 2)
                            leftPadding: DesignTokens.spaceSm
                            rightPadding: DesignTokens.spaceSm
                            GlassToolTip {
                                active: saveExampleBtn.hovered
                                text: qsTr("Save this response as a named example")
                            }
                            onClicked: saveExampleDialog.open()
                            contentItem: Text {
                                text: saveExampleBtn.text
                                color: DesignTokens.accent
                                font.pixelSize: DesignTokens.fontLabel
                                font.weight: DesignTokens.weightSemiBold
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: DesignTokens.radiusSm
                                color: saveExampleBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                                border.width: 1
                                border.color: DesignTokens.borderSubtle
                            }
                        }
                    }
                }

                // Examples dropdown — re-show a saved example for this op.
                GridLayout {
                    Layout.fillWidth: true
                    visible: examplesCombo.count > 0
                    columns: 2
                    rowSpacing: DesignTokens.spaceXs
                    columnSpacing: DesignTokens.spaceSm
                    Label {
                        id: examplesLabel
                        Layout.row: 0
                        Layout.column: 0
                        text: qsTr("Examples")
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontLabel
                    }
                    GlassComboBox {
                        id: examplesCombo
                        Layout.row: panel.examplesReflow ? 1 : 0
                        Layout.column: panel.examplesReflow ? 0 : 1
                        Layout.columnSpan: panel.examplesReflow ? 2 : 1
                        Layout.fillWidth: panel.examplesReflow
                        Layout.minimumWidth: 0
                        // A compact dropdown reads as a picker; full-width it
                        // looked like an empty input field (UI/UX review §5).
                        Layout.preferredWidth: Math.min(260, panel.availableContentWidth)
                        Layout.maximumWidth: Math.min(320, panel.availableContentWidth)
                        model: AppController.examples
                        textRole: "name"
                        onActivated: AppController.showExample(currentText)
                    }
                }

                // Inner Body(Tree) | Body(Raw) | Headers | Diff tabs + Pretty.
                GridLayout {
                    Layout.fillWidth: true
                    visible: AppController.hasResponse
                    columns: 2
                    rowSpacing: DesignTokens.spaceXs
                    columnSpacing: DesignTokens.spaceSm
                    TabBar {
                        id: respTabs
                        Layout.row: 0
                        Layout.column: 0
                        Layout.columnSpan: panel.responseTabsReflow ? 2 : 1
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        visible: AppController.hasResponse
                        background: Rectangle {
                            color: "transparent"
                        }
                        Repeater {
                            model: [qsTr("Body (Tree)"), qsTr("Body (Raw)"), qsTr("Headers"), qsTr("Diff")]
                            delegate: TabButton {
                                id: respTabButton
                                required property string modelData
                                required property int index
                                contentItem: Text {
                                    text: respTabButton.modelData
                                    color: respTabs.currentIndex === respTabButton.index ? DesignTokens.textPrimary : DesignTokens.textSecondary
                                    font.pixelSize: DesignTokens.fontBody
                                    font.weight: respTabs.currentIndex === respTabButton.index ? DesignTokens.weightSemiBold : DesignTokens.weightRegular
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    color: "transparent"
                                    Rectangle {
                                        anchors.bottom: parent.bottom
                                        width: parent.width
                                        height: 2
                                        color: respTabs.currentIndex === respTabButton.index ? DesignTokens.accent : "transparent"
                                    }
                                }
                            }
                        }
                    }
                    Button {
                        id: prettyBtn
                        visible: respTabs.currentIndex === 3
                        Layout.row: panel.responseTabsReflow ? 1 : 0
                        Layout.column: 1
                        Layout.alignment: Qt.AlignRight
                        implicitHeight: Math.max(26, contentItem.implicitHeight + DesignTokens.spaceXs * 2)
                        leftPadding: DesignTokens.spaceSm
                        rightPadding: DesignTokens.spaceSm
                        checkable: true
                        checked: panel.prettyRaw
                        onToggled: panel.prettyRaw = checked
                        background: Rectangle {
                            radius: DesignTokens.radiusSm
                            color: prettyBtn.checked ? DesignTokens.accentMuted : (prettyBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent")
                            border.width: 1
                            border.color: prettyBtn.checked ? DesignTokens.accent : DesignTokens.borderSubtle
                        }
                        contentItem: Text {
                            text: qsTr("Pretty")
                            color: prettyBtn.checked ? DesignTokens.accent : DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontLabel
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: AppController.hasResponse
                    currentIndex: respTabs.currentIndex

                    // Body (Tree) — flattened JSON rows (field → value), indented
                    // by depth. Non-JSON bodies render as a single "(not JSON)" row.
                    Rectangle {
                        radius: DesignTokens.radiusSm
                        color: DesignTokens.surfaceSunken
                        border.width: 1
                        border.color: DesignTokens.borderSubtle
                        ListView {
                            id: treeView
                            anchors.fill: parent
                            anchors.margins: DesignTokens.spaceSm
                            clip: true
                            // C++-parsed, virtualized JSON tree — no per-response
                            // JS parse/flatten, so large bodies don't stutter.
                            model: AppController.responseBody
                            ScrollBar.vertical: ScrollBar {}
                            delegate: Rectangle {
                                id: treeRow
                                required property int index
                                required property int depth
                                required property string field
                                required property string value
                                required property string rawValue
                                required property bool isLeaf
                                required property bool hasChildren
                                required property bool collapsed
                                required property string path
                                width: ListView.view.width
                                height: 22
                                radius: DesignTokens.radiusSm
                                color: rowMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.04) : "transparent"

                                MouseArea {
                                    id: rowMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (treeRow.hasChildren) {
                                            AppController.responseBody.toggle(treeRow.index);
                                        } else {
                                            AppController.copyToClipboard(treeRow.rawValue, qsTr("value"));
                                        }
                                    }
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: DesignTokens.spaceXs
                                    anchors.rightMargin: DesignTokens.spaceXs
                                    spacing: DesignTokens.spaceSm
                                    Item {
                                        Layout.preferredWidth: treeRow.depth * 16
                                    }
                                    Text {
                                        Layout.preferredWidth: 12
                                        text: treeRow.hasChildren ? (treeRow.collapsed ? "\u25B8" : "\u25BE") : ""
                                        color: DesignTokens.textSecondary
                                        font.pixelSize: DesignTokens.fontCaption
                                        horizontalAlignment: Text.AlignHCenter
                                    }
                                    Text {
                                        id: fieldText
                                        text: treeRow.field
                                        color: fieldMouse.containsMouse ? DesignTokens.accent : DesignTokens.textSecondary
                                        font.pixelSize: DesignTokens.fontLabel
                                        font.family: DesignTokens.fontMono
                                        // Clicking the key copies its JSONPath
                                        // (the value is copied by clicking the row).
                                        MouseArea {
                                            id: fieldMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: AppController.copyToClipboard(treeRow.path, treeRow.path)
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: treeRow.value
                                        color: treeRow.isLeaf ? DesignTokens.textPrimary : DesignTokens.textSecondary
                                        font.pixelSize: DesignTokens.fontLabel
                                        font.family: DesignTokens.fontMono
                                        font.weight: treeRow.isLeaf ? DesignTokens.weightRegular : DesignTokens.weightSemiBold
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        visible: fieldMouse.containsMouse
                                        text: qsTr("copy path")
                                        color: DesignTokens.accent
                                        font.pixelSize: DesignTokens.fontCaption
                                    }
                                    Text {
                                        visible: rowMouse.containsMouse && !fieldMouse.containsMouse && treeRow.isLeaf
                                        text: qsTr("copy value")
                                        color: DesignTokens.accent
                                        font.pixelSize: DesignTokens.fontCaption
                                    }
                                    // Save this response value as a chain variable
                                    // (extraction) on the current endpoint. The
                                    // tree already carries the JSONPath (with any
                                    // array index), so picking is one click.
                                    Text {
                                        id: extractHint
                                        visible: extractMouse.containsMouse || (rowMouse.containsMouse && treeRow.isLeaf)
                                        text: qsTr("＋ save as variable")
                                        color: extractMouse.containsMouse ? DesignTokens.accentHover : DesignTokens.accent
                                        font.pixelSize: DesignTokens.fontCaption
                                        font.weight: DesignTokens.weightSemiBold
                                        MouseArea {
                                            id: extractMouse
                                            anchors.fill: parent
                                            anchors.margins: -4
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: extractDialog.openFor(treeRow.path, treeRow.field)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Body (Raw) — syntax-highlighted source with a format
                    // dropdown + a Preview toggle (renders HTML / Markdown).
                    ColumnLayout {
                        spacing: DesignTokens.spaceSm

                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            rowSpacing: DesignTokens.spaceXs
                            columnSpacing: DesignTokens.spaceSm
                            GlassComboBox {
                                id: formatCombo
                                Layout.row: 0
                                Layout.column: 0
                                Layout.columnSpan: panel.rawControlsReflow ? 2 : 1
                                Layout.fillWidth: panel.rawControlsReflow
                                Layout.minimumWidth: 0
                                Layout.preferredWidth: Math.min(150, panel.availableContentWidth)
                                Layout.maximumWidth: Math.min(150, panel.availableContentWidth)
                                model: panel.formatLabels
                                Component.onCompleted: currentIndex = panel.formatValues.indexOf(panel.respFormat)
                                onActivated: panel.respFormat = panel.formatValues[currentIndex]
                                Connections {
                                    target: panel
                                    function onRespFormatChanged() {
                                        formatCombo.currentIndex = panel.formatValues.indexOf(panel.respFormat);
                                    }
                                }
                            }
                            Button {
                                id: previewBtn
                                visible: panel.respFormat === "html" || panel.respFormat === "markdown"
                                Layout.row: panel.rawControlsReflow ? 1 : 0
                                Layout.column: 1
                                Layout.alignment: Qt.AlignRight
                                implicitHeight: Math.max(30, contentItem.implicitHeight + DesignTokens.spaceXs * 2)
                                leftPadding: DesignTokens.spaceMd
                                rightPadding: DesignTokens.spaceMd
                                checkable: true
                                checked: panel.previewMode
                                onToggled: panel.previewMode = checked
                                background: Rectangle {
                                    radius: DesignTokens.radiusSm
                                    color: previewBtn.checked ? DesignTokens.accentMuted : (previewBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent")
                                    border.width: 1
                                    border.color: previewBtn.checked ? DesignTokens.accent : DesignTokens.borderSubtle
                                }
                                contentItem: Text {
                                    text: qsTr("Preview")
                                    color: previewBtn.checked ? DesignTokens.accent : DesignTokens.textSecondary
                                    font.pixelSize: DesignTokens.fontLabel
                                    font.weight: DesignTokens.weightSemiBold
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }
                            // JSON-only indent toggle — lives with the format
                            // dropdown so body formatting is one control cluster.
                            Button {
                                id: rawPrettyBtn
                                visible: panel.respFormat === "json"
                                Layout.row: panel.rawControlsReflow ? 1 : 0
                                Layout.column: 1
                                Layout.alignment: Qt.AlignRight
                                implicitHeight: Math.max(30, contentItem.implicitHeight + DesignTokens.spaceXs * 2)
                                leftPadding: DesignTokens.spaceMd
                                rightPadding: DesignTokens.spaceMd
                                checkable: true
                                checked: panel.prettyRaw
                                onToggled: panel.prettyRaw = checked
                                background: Rectangle {
                                    radius: DesignTokens.radiusSm
                                    color: rawPrettyBtn.checked ? DesignTokens.accentMuted : (rawPrettyBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent")
                                    border.width: 1
                                    border.color: rawPrettyBtn.checked ? DesignTokens.accent : DesignTokens.borderSubtle
                                }
                                contentItem: Text {
                                    text: qsTr("Pretty")
                                    color: rawPrettyBtn.checked ? DesignTokens.accent : DesignTokens.textSecondary
                                    font.pixelSize: DesignTokens.fontLabel
                                    font.weight: DesignTokens.weightSemiBold
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }
                        }

                        // Coloured source view.
                        CodeView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            visible: !panel.previewMode
                            language: panel.respFormat
                            text: panel.displayBody()
                            placeholder: qsTr("(body not captured — enable “Capture bodies”)")
                        }

                        // Rendered preview (HTML / Markdown).
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            visible: panel.previewMode
                            radius: DesignTokens.radiusSm
                            color: DesignTokens.surfaceSunken
                            border.width: 1
                            border.color: DesignTokens.borderSubtle
                            ScrollView {
                                anchors.fill: parent
                                anchors.margins: DesignTokens.spaceSm
                                clip: true
                                TextArea {
                                    readOnly: true
                                    selectByMouse: true
                                    wrapMode: TextArea.Wrap
                                    textFormat: panel.respFormat === "markdown" ? TextEdit.MarkdownText : TextEdit.RichText
                                    text: AppController.respBody
                                    color: DesignTokens.textPrimary
                                    selectionColor: DesignTokens.accent
                                    selectedTextColor: DesignTokens.textInverse
                                    font.pixelSize: DesignTokens.fontBody
                                    background: null
                                }
                            }
                        }
                    }

                    // Headers
                    SelectableTextBox {
                        text: AppController.respHeaders
                    }

                    // Diff — compare the live body against a saved example.
                    ColumnLayout {
                        spacing: DesignTokens.spaceSm
                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            rowSpacing: DesignTokens.spaceXs
                            columnSpacing: DesignTokens.spaceSm
                            Label {
                                id: compareLabel
                                Layout.row: 0
                                Layout.column: 0
                                text: qsTr("Compare with")
                                color: DesignTokens.textSecondary
                                font.pixelSize: DesignTokens.fontLabel
                            }
                            GlassComboBox {
                                id: diffCombo
                                Layout.row: panel.diffPickerReflow ? 1 : 0
                                Layout.column: panel.diffPickerReflow ? 0 : 1
                                Layout.columnSpan: panel.diffPickerReflow ? 2 : 1
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                model: AppController.examples
                                textRole: "name"
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            radius: DesignTokens.radiusSm
                            color: DesignTokens.surfaceSunken
                            border.width: 1
                            border.color: DesignTokens.borderSubtle
                            ListView {
                                id: diffView
                                anchors.fill: parent
                                anchors.margins: DesignTokens.spaceSm
                                clip: true
                                ScrollBar.vertical: ScrollBar {}
                                model: diffCombo.count > 0 ? AppController.lineDiff(panel.prettyRaw ? panel.prettyJson(AppController.exampleBody(diffCombo.currentText)) : AppController.exampleBody(diffCombo.currentText), panel.prettyRaw ? panel.prettyJson(AppController.respBody) : AppController.respBody) : []
                                delegate: Text {
                                    id: diffRow
                                    required property var modelData
                                    width: ListView.view.width
                                    text: diffRow.modelData.sign + " " + diffRow.modelData.text
                                    color: diffRow.modelData.sign === "+" ? DesignTokens.statusSuccess : (diffRow.modelData.sign === "-" ? DesignTokens.statusError : DesignTokens.textSecondary)
                                    font.pixelSize: DesignTokens.fontLabel
                                    font.family: DesignTokens.fontMono
                                    wrapMode: Text.WrapAnywhere
                                }
                            }
                        }
                    }
                }

                // Pre-run state: the resolved execution path, so the pane answers
                // "what happens when I press Send" instead of showing a bare icon.
                // Falls back to the generic empty state when there is no chain to
                // describe (no operation selected, or an unresolvable chain).
                Item {
                    id: preRun
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: !AppController.hasResponse

                    // Held in a property, not bound: executionPreview() resolves the
                    // plan on every call, so it is refreshed on the events that can
                    // change it rather than on every binding re-evaluation.
                    property var steps: []

                    function refresh() {
                        preRun.steps = preRun.visible ? AppController.executionPreview() : [];
                    }

                    onVisibleChanged: preRun.refresh()
                    Component.onCompleted: preRun.refresh()

                    Connections {
                        target: AppController
                        function onChainChanged() {
                            preRun.refresh();
                        }
                        function onOperationChanged() {
                            preRun.refresh();
                        }
                    }

                    EmptyState {
                        anchors.centerIn: parent
                        visible: preRun.steps.length === 0
                        iconName: "zap"
                        heading: qsTr("No response yet")
                        body: qsTr("Press Send to run this endpoint's chain and the response will appear here.")
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        visible: preRun.steps.length > 0
                        spacing: DesignTokens.spaceSm

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: DesignTokens.spaceSm
                            SectionLabel {
                                text: qsTr("EXECUTION PATH")
                            }
                            Label {
                                Layout.fillWidth: true
                                text: preRun.steps.length === 1 ? qsTr("1 step") : qsTr("%1 steps").arg(preRun.steps.length)
                                color: DesignTokens.textSecondary
                                font.pointSize: DesignTokens.fontCaptionPointSize
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Values shown as {{name}} are resolved while the chain runs.")
                            color: DesignTokens.textSecondary
                            font.pointSize: DesignTokens.fontCaptionPointSize
                            wrapMode: Text.WordWrap
                        }

                        ListView {
                            id: pathList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: DesignTokens.spaceXs
                            model: preRun.steps

                            delegate: Rectangle {
                                id: stepRow
                                required property var modelData
                                width: ListView.view.width
                                implicitHeight: stepCol.implicitHeight + DesignTokens.spaceSm * 2
                                radius: DesignTokens.radiusSm
                                color: stepRow.modelData.isTarget ? DesignTokens.accentMuted : DesignTokens.surfaceSunken
                                border.width: 1
                                border.color: stepRow.modelData.isTarget ? DesignTokens.accent : DesignTokens.borderSubtle

                                ColumnLayout {
                                    id: stepCol
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.leftMargin: DesignTokens.spaceSm
                                    anchors.rightMargin: DesignTokens.spaceSm
                                    spacing: 2

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: DesignTokens.spaceSm

                                        Label {
                                            text: stepRow.modelData.number
                                            color: DesignTokens.textSecondary
                                            font.pointSize: DesignTokens.fontCaptionPointSize
                                            font.family: DesignTokens.fontMono
                                            font.features: ({
                                                    "tnum": 1
                                                })
                                        }
                                        MethodBadge {
                                            visible: stepRow.modelData.method.length > 0
                                            method: stepRow.modelData.method.length > 0 ? stepRow.modelData.method : "GET"
                                            minWidth: 54
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: stepRow.modelData.path.length > 0 ? stepRow.modelData.path : qsTr("operation not found in this project")
                                            color: stepRow.modelData.path.length > 0 ? DesignTokens.textPrimary : DesignTokens.statusError
                                            font.pointSize: DesignTokens.fontLabelPointSize
                                            font.family: DesignTokens.fontMono
                                            elide: Text.ElideMiddle
                                        }
                                        Label {
                                            visible: stepRow.modelData.isTarget
                                            text: qsTr("target")
                                            color: DesignTokens.accent
                                            font.pointSize: DesignTokens.fontCaptionPointSize
                                            font.weight: DesignTokens.weightSemiBold
                                        }
                                    }

                                    // The values this step hands downstream — the
                                    // knots that make the chain a chain.
                                    Label {
                                        Layout.fillWidth: true
                                        visible: stepRow.modelData.produces.length > 0
                                        text: {
                                            const names = [];
                                            for (let i = 0; i < stepRow.modelData.produces.length; ++i) {
                                                names.push(stepRow.modelData.produces[i].variable);
                                            }
                                            return qsTr("produces %1").arg(names.join(", "));
                                        }
                                        color: DesignTokens.textSecondary
                                        font.pointSize: DesignTokens.fontCaptionPointSize
                                        font.family: DesignTokens.fontMono
                                        elide: Text.ElideRight
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        visible: stepRow.modelData.actor.length > 0 || stepRow.modelData.expectStatus.length > 0
                                        text: {
                                            const parts = [];
                                            if (stepRow.modelData.actor.length > 0) {
                                                parts.push(qsTr("as %1").arg(stepRow.modelData.actor));
                                            }
                                            if (stepRow.modelData.expectStatus.length > 0) {
                                                parts.push(qsTr("expects %1").arg(stepRow.modelData.expectStatus.join(", ")));
                                            }
                                            return parts.join("  ·  ");
                                        }
                                        color: DesignTokens.textSecondary
                                        font.pointSize: DesignTokens.fontCaptionPointSize
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ── Timeline content ────────────────────────────────────────────
            TimelinePanel {
                // Embedded as a borderless surface inside the panel.
                color: "transparent"
                border.width: 0
            }
        }
    }

    // Save-as-example prompt.
    Dialog {
        id: saveExampleDialog
        modal: true
        enter: PopupEnter {}
        exit: PopupExit {}
        anchors.centerIn: Overlay.overlay
        width: 380
        padding: DesignTokens.spaceLg
        title: qsTr("Save response as example")
        header: DialogHeader {
            title: qsTr("Save response as example")
        }

        onOpened: {
            exampleNameField.text = "HTTP " + AppController.respStatus;
            exampleNameField.forceActiveFocus();
            exampleNameField.selectAll();
        }

        background: Rectangle {
            radius: DesignTokens.radiusLg
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.glassBorder
        }

        contentItem: ColumnLayout {
            spacing: DesignTokens.spaceSm
            FieldLabel {
                text: qsTr("Example name")
            }
            GlassTextField {
                id: exampleNameField
                Layout.fillWidth: true
            }
        }

        footer: DialogButtons {
            okText: qsTr("Save Response")
            okEnabled: exampleNameField.text.trim().length > 0
            onAccepted: saveExampleDialog.accept()
            onRejected: saveExampleDialog.reject()
        }

        onAccepted: AppController.saveResponse(exampleNameField.text.trim())
    }

    // Save-as-variable prompt (response-driven extract picker). The JSONPath
    // comes from the clicked tree node; the user just names the variable.
    Dialog {
        id: extractDialog
        modal: true
        enter: PopupEnter {}
        exit: PopupExit {}
        anchors.centerIn: Overlay.overlay
        width: 440
        padding: DesignTokens.spaceLg
        title: qsTr("Save as variable")
        header: DialogHeader {
            title: qsTr("Save as variable")
        }

        property string sourcePath: ""

        function openFor(path, suggestedName) {
            sourcePath = path;
            extractNameField.text = suggestedName;
            open();
            extractNameField.forceActiveFocus();
            extractNameField.selectAll();
        }

        background: Rectangle {
            radius: DesignTokens.radiusLg
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.glassBorder
        }

        contentItem: ColumnLayout {
            spacing: DesignTokens.spaceSm
            FieldLabel {
                text: qsTr("Variable name")
            }
            GlassTextField {
                id: extractNameField
                Layout.fillWidth: true
            }
            FieldLabel {
                text: qsTr("From this response path")
            }
            Label {
                Layout.fillWidth: true
                text: extractDialog.sourcePath
                color: DesignTokens.textPrimary
                font.pixelSize: DesignTokens.fontLabel
                font.family: DesignTokens.fontMono
                wrapMode: Text.WrapAnywhere
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Saved on this endpoint. Later steps reference it as {{%1.%2}}.").arg(AppController.selectedModule).arg(extractNameField.text.trim().length > 0 ? extractNameField.text.trim() : "<variable>")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontCaption
                wrapMode: Text.WordWrap
            }
        }

        footer: DialogButtons {
            okText: qsTr("Save variable")
            okEnabled: extractNameField.text.trim().length > 0
            onAccepted: extractDialog.accept()
            onRejected: extractDialog.reject()
        }

        onAccepted: AppController.addExtraction(extractNameField.text.trim(), extractDialog.sourcePath)
    }
}
