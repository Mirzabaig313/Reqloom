// LatencySparkline — a compact response-time chart for the current run. Two
// views, toggled by the header control:
//   • Bars  — one bar per response (coloured by HTTP-class status token), a
//             dashed p95 marker, and a one-line summary (median · p95 · max).
//             Click a bar to scroll the timeline to that step; hover highlights.
//   • Hist. — the Freedman–Diaconis histogram of the run's latencies
//             (binning computed in C++ by the LatencyStats helper).
// Fed by TimelineModel.latencyBars / latencyStats.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

ColumnLayout {
    id: root

    // { ms, token, op, stepIndex } per response, and the summary/histogram map.
    required property var bars
    required property var stats

    // Optional p95 latency budget (ms); 0 = no SLO. When the run's p95 exceeds
    // it, the p95 marker and summary turn red — latency as pass/fail.
    property int sloP95Ms: 0
    readonly property bool sloBreached: root.sloP95Ms > 0 && (root.stats.p95 || 0) > root.sloP95Ms

    // Emitted when a bar is clicked, carrying the 1-based step number so the
    // timeline can scroll to the owning step.
    signal stepActivated(int stepIndex)

    // Emitted when the user edits the p95 SLO budget inline (0 = clear).
    signal sloChangeRequested(int ms)

    // Chart view: 0 = scatter (default), 1 = bars, 2 = histogram.
    property int viewMode: 0
    readonly property bool isScatter: root.viewMode === 0
    readonly property bool isBars: root.viewMode === 1
    readonly property bool isHistogram: root.viewMode === 2
    readonly property var viewNames: [qsTr("Scatter"), qsTr("Bars"), qsTr("Histogram")]
    // Bar/dot index under the cursor (−1 = none).
    property int hoveredBar: -1

    // Threshold legend for the scatter view. With an SLO set, dots bucket
    // good / caution / slow against the budget; without one, by HTTP class.
    readonly property var legendItems: root.sloP95Ms > 0 ? [
        {
            "color": DesignTokens.statusSuccess,
            "label": qsTr("\u2264 %1 ms (Good)").arg(root.sloP95Ms)
        },
        {
            "color": DesignTokens.statusWarning,
            "label": qsTr("\u2264 %1 ms (Caution)").arg(root.sloP95Ms * 2)
        },
        {
            "color": DesignTokens.statusError,
            "label": qsTr("> %1 ms (Slow)").arg(root.sloP95Ms * 2)
        }
    ] : [
        {
            "color": DesignTokens.statusSuccess,
            "label": qsTr("2xx (Good)")
        },
        {
            "color": DesignTokens.statusWarning,
            "label": qsTr("3xx (Caution)")
        },
        {
            "color": DesignTokens.statusError,
            "label": qsTr("5xx / error (Slow)")
        }
    ]

    readonly property real maxMs: {
        let m = 1;
        for (let i = 0; i < root.bars.length; ++i) {
            m = Math.max(m, root.bars[i].ms);
        }
        return m;
    }

    visible: root.bars.length > 0
    spacing: DesignTokens.spaceXs

    Flow {
        Layout.fillWidth: true
        spacing: DesignTokens.spaceSm
        Label {
            text: qsTr("LATENCY")
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontCaption
            font.weight: DesignTokens.weightSemiBold
            font.letterSpacing: 1.2
        }
        // SLO verdict pill: green when within budget, red when breached.
        // Click to edit; click "+ SLO" (below) to set one when unset.
        Rectangle {
            id: sloPill
            visible: root.sloP95Ms > 0
            implicitWidth: sloRow.implicitWidth + DesignTokens.spaceSm * 2
            implicitHeight: Math.max(18, sloRow.implicitHeight + DesignTokens.spaceXs * 2)
            radius: DesignTokens.radiusSm
            readonly property color hue: root.sloBreached ? DesignTokens.statusError : DesignTokens.statusSuccess
            color: Qt.rgba(sloPill.hue.r, sloPill.hue.g, sloPill.hue.b, 0.16)
            Behavior on color {
                ColorMotion {}
            }
            Row {
                id: sloRow
                anchors.centerIn: parent
                spacing: 4
                Text {
                    text: root.sloBreached ? "\u2717" : "\u2713"  // ✗ / ✓
                    color: sloPill.hue
                    font.pixelSize: DesignTokens.fontCaption
                    font.weight: DesignTokens.weightBold
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: qsTr("p95 < %1 ms").arg(root.sloP95Ms)
                    color: sloPill.hue
                    font.pixelSize: DesignTokens.fontCaption
                    font.weight: DesignTokens.weightSemiBold
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            GlassToolTip {
                active: sloPillHover.hovered
                text: root.sloBreached ? qsTr("Latency SLO breached: p95 %1 ms exceeds the %2 ms budget. Click to edit.").arg(Math.round(root.stats.p95 || 0)).arg(root.sloP95Ms) : qsTr("Within latency SLO: p95 %1 ms ≤ %2 ms budget. Click to edit.").arg(Math.round(root.stats.p95 || 0)).arg(root.sloP95Ms)
            }
            HoverHandler {
                id: sloPillHover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                onTapped: root.openSloEditor()
            }
        }
        // "+ SLO" affordance shown when no budget is set.
        Label {
            visible: root.sloP95Ms === 0
            text: qsTr("+ SLO")
            color: setSloHover.hovered ? DesignTokens.accent : DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontCaption
            font.weight: DesignTokens.weightSemiBold
            Behavior on color {
                ColorMotion {}
            }
            HoverHandler {
                id: setSloHover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                onTapped: root.openSloEditor()
            }
            GlassToolTip {
                active: setSloHover.hovered
                text: qsTr("Set a p95 latency budget for this project")
            }
        }
        // View switcher — a visible segmented control (Scatter | Bars |
        // Histogram) with the active segment filled, instead of a single
        // cycling word that didn't read as a control.
        Rectangle {
            implicitHeight: Math.max(24, segRow.implicitHeight + 2)
            implicitWidth: segRow.implicitWidth + 2
            radius: DesignTokens.radiusSm
            color: DesignTokens.surfaceSunken
            border.width: 1
            border.color: DesignTokens.borderSubtle
            Row {
                id: segRow
                anchors.centerIn: parent
                spacing: 0
                Repeater {
                    model: root.viewNames
                    delegate: Rectangle {
                        id: seg
                        required property int index
                        required property string modelData
                        readonly property bool active: root.viewMode === seg.index
                        implicitHeight: Math.max(22, segText.implicitHeight + DesignTokens.spaceXs * 2)
                        implicitWidth: segText.implicitWidth + DesignTokens.spaceMd
                        radius: DesignTokens.radiusSm - 1
                        color: seg.active ? DesignTokens.accent : (segHover.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent")
                        Behavior on color {
                            ColorMotion {}
                        }
                        Label {
                            id: segText
                            anchors.centerIn: parent
                            text: seg.modelData
                            color: seg.active ? DesignTokens.textInverse : (segHover.hovered ? DesignTokens.textPrimary : DesignTokens.textSecondary)
                            font.pixelSize: DesignTokens.fontCaption
                            font.weight: seg.active ? DesignTokens.weightSemiBold : DesignTokens.weightMedium
                        }
                        HoverHandler {
                            id: segHover
                            cursorShape: Qt.PointingHandCursor
                        }
                        TapHandler {
                            onTapped: {
                                root.viewMode = seg.index;
                                root.hoveredBar = -1;
                            }
                        }
                    }
                }
            }
        }
    }

    // The bars/histogram plot, drawn on a Canvas so it scales to any count.
    // Stat chips: med · p95 · p99 · max, each a bordered pill. In a Flow so a
    // narrow / compact panel wraps them onto multiple lines instead of clipping.
    Flow {
        Layout.fillWidth: true
        spacing: DesignTokens.spaceXs
        Repeater {
            model: [
                {
                    "k": qsTr("med"),
                    "v": Math.round(root.stats.median || 0)
                },
                {
                    "k": qsTr("p95"),
                    "v": Math.round(root.stats.p95 || 0)
                },
                {
                    "k": qsTr("p99"),
                    "v": Math.round(root.stats.p99 || 0)
                },
                {
                    "k": qsTr("max"),
                    "v": Math.round(root.stats.max || 0)
                }
            ]
            delegate: Rectangle {
                id: statChip
                required property var modelData
                implicitHeight: Math.max(22, statRow.implicitHeight + DesignTokens.spaceXs * 2)
                implicitWidth: statRow.implicitWidth + DesignTokens.spaceSm * 2
                radius: DesignTokens.radiusSm
                color: "transparent"
                border.width: 1
                border.color: DesignTokens.borderSubtle
                Row {
                    id: statRow
                    anchors.centerIn: parent
                    spacing: 4
                    Text {
                        text: statChip.modelData.k
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontCaption
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: qsTr("%1 ms").arg(statChip.modelData.v)
                        color: DesignTokens.textPrimary
                        font.pixelSize: DesignTokens.fontCaption
                        font.weight: DesignTokens.weightSemiBold
                        font.family: DesignTokens.fontMono
                        font.features: ({
                                "tnum": 1
                            })
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
        }
    }

    Item {
        id: plot
        Layout.fillWidth: true
        Layout.preferredHeight: root.isScatter ? 200 : 72

        // Framed plot area for all chart views.
        Rectangle {
            anchors.fill: parent
            radius: DesignTokens.radiusSm
            color: "transparent"
            border.width: 1
            border.color: DesignTokens.borderSubtle
        }

        Canvas {
            id: canvas
            anchors.fill: parent
            onPaint: {
                const ctx = getContext("2d");
                ctx.reset();
                const w = width;
                const h = height;
                if (root.isHistogram) {
                    root.paintHistogram(ctx, w, h);
                } else if (root.isBars) {
                    root.paintBars(ctx, w, h);
                } else {
                    root.paintScatter(ctx, w, h);
                }
            }
        }

        // Hover + click interaction (scatter + bars). Maps cursor x → index.
        HoverHandler {
            id: plotHover
            enabled: !root.isHistogram
            onPointChanged: root.hoveredBar = root.barAt(point.position.x, plot.width)
            onHoveredChanged: {
                if (!hovered) {
                    root.hoveredBar = -1;
                }
            }
        }
        TapHandler {
            enabled: !root.isHistogram
            onTapped: eventPoint => {
                const i = root.barAt(eventPoint.position.x, plot.width);
                if (i >= 0 && i < root.bars.length) {
                    root.stepActivated(root.bars[i].stepIndex);
                }
            }
        }
        MouseArea {
            anchors.fill: parent
            enabled: !root.isHistogram
            acceptedButtons: Qt.NoButton
            cursorShape: root.hoveredBar >= 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
        }

        // Tooltip for the hovered bar: "step N · 123 ms".
        GlassToolTip {
            active: root.hoveredBar >= 0 && root.hoveredBar < root.bars.length
            text: root.hoveredBar >= 0 && root.hoveredBar < root.bars.length ? qsTr("step %1 · %2 ms").arg(root.bars[root.hoveredBar].stepIndex).arg(Math.round(root.bars[root.hoveredBar].ms)) : ""
        }

        Connections {
            target: root
            function onBarsChanged() {
                canvas.requestPaint();
            }
            function onStatsChanged() {
                canvas.requestPaint();
            }
            function onViewModeChanged() {
                canvas.requestPaint();
            }
            function onHoveredBarChanged() {
                canvas.requestPaint();
            }
            function onSloP95MsChanged() {
                canvas.requestPaint();
            }
        }
        Connections {
            target: DesignTokens
            function onTokensChanged() {
                canvas.requestPaint();
            }
        }
        onWidthChanged: canvas.requestPaint()
        onHeightChanged: canvas.requestPaint()

        // Inline editor for the p95 budget, parented to the window overlay so
        // it centres over the whole app. Declared inside this Item (not the
        // root ColumnLayout) so its positioning isn't flagged as layout-managed.
        Popup {
            id: sloPopup
            parent: Overlay.overlay
            x: Math.round(((parent ? parent.width : 0) - width) / 2)
            y: Math.round(((parent ? parent.height : 0) - height) / 2)
            modal: true
            focus: true
            padding: DesignTokens.spaceMd
            enter: PopupEnter {}
            exit: PopupExit {}
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

            background: Rectangle {
                radius: DesignTokens.radius
                color: DesignTokens.surfaceOverlay
                border.width: 1
                border.color: DesignTokens.borderSubtle
            }

            contentItem: ColumnLayout {
                spacing: DesignTokens.spaceSm

                Label {
                    text: qsTr("Latency SLO")
                    color: DesignTokens.textPrimary
                    font.pixelSize: DesignTokens.fontBody
                    font.weight: DesignTokens.weightSemiBold
                }
                Label {
                    text: qsTr("Fail the run's p95 response time above this budget.")
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontCaption
                    Layout.preferredWidth: 240
                    wrapMode: Text.WordWrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: DesignTokens.spaceSm
                    TextField {
                        id: sloField
                        Layout.fillWidth: true
                        placeholderText: qsTr("e.g. 800")
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator {
                            bottom: 0
                            top: 600000
                        }
                        color: DesignTokens.textPrimary
                        font.pixelSize: DesignTokens.fontBody
                        onAccepted: saveSloBtn.commit()
                        background: Rectangle {
                            radius: DesignTokens.radiusSm
                            color: DesignTokens.surfaceSunken
                            border.width: 1
                            border.color: sloField.activeFocus ? DesignTokens.accent : DesignTokens.borderSubtle
                        }
                    }
                    Label {
                        text: qsTr("ms")
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontLabel
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: DesignTokens.spaceSm
                    Button {
                        id: clearSloBtn
                        text: qsTr("Clear")
                        visible: root.sloP95Ms > 0
                        onClicked: {
                            root.sloChangeRequested(0);
                            sloPopup.close();
                        }
                        background: Rectangle {
                            radius: DesignTokens.radiusSm
                            color: "transparent"
                            border.width: 1
                            border.color: DesignTokens.borderStrong
                        }
                        contentItem: Text {
                            text: clearSloBtn.text
                            color: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontLabel
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    Button {
                        id: saveSloBtn
                        text: qsTr("Save")
                        function commit() {
                            const v = parseInt(sloField.text, 10);
                            root.sloChangeRequested(isNaN(v) ? 0 : v);
                            sloPopup.close();
                        }
                        onClicked: saveSloBtn.commit()
                        background: Rectangle {
                            radius: DesignTokens.radiusSm
                            color: saveSloBtn.down ? DesignTokens.accentHover : DesignTokens.accent
                        }
                        contentItem: Text {
                            text: saveSloBtn.text
                            color: DesignTokens.textInverse
                            font.pixelSize: DesignTokens.fontLabel
                            font.weight: DesignTokens.weightSemiBold
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
            }
        }
    }

    // Scatter legend + total request count (mockup parity). A Flow so a narrow
    // panel wraps the legend entries instead of clipping them.
    Flow {
        Layout.fillWidth: true
        visible: root.isScatter && root.bars.length > 0
        spacing: DesignTokens.spaceMd

        Repeater {
            model: root.legendItems
            delegate: Row {
                id: legendEntry
                required property var modelData
                spacing: 4
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 8
                    height: 8
                    radius: 4
                    color: legendEntry.modelData.color
                }
                Label {
                    text: legendEntry.modelData.label
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontCaption
                }
            }
        }
        Label {
            text: qsTr("Total %1 requests").arg(root.bars.length)
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontCaption
            font.features: ({
                    "tnum": 1
                })
        }
    }

    // Plot gutters for the scatter view (left = y-label axis).
    readonly property int scatterPadL: 40
    readonly property int scatterPadR: 8
    readonly property int scatterPadTop: 10
    readonly property int scatterPadBottom: 8

    // Bar/dot index at plot-relative x, or −1 outside the data.
    function barAt(px, plotW) {
        const n = root.bars.length;
        if (n === 0) {
            return -1;
        }
        if (root.isScatter) {
            const inner = Math.max(1, plotW - root.scatterPadL - root.scatterPadR);
            const rel = px - root.scatterPadL;
            if (rel < 0 || rel > inner) {
                return -1;
            }
            const i = Math.floor(rel / (inner / n));
            return (i >= 0 && i < n) ? i : -1;
        }
        const gap = n > 1 ? 3 : 0;
        const rawW = (plotW - gap * (n - 1)) / n;
        const barW = Math.max(1, Math.min(36, rawW));
        const groupW = barW * n + gap * (n - 1);
        const offsetX = Math.max(0, (plotW - groupW) / 2);
        const i = Math.floor((px - offsetX) / (barW + gap));
        return (i >= 0 && i < n) ? i : -1;
    }

    // Round a max latency up to a "nice" axis ceiling for readable gridlines.
    function niceTop(v) {
        if (v <= 0) {
            return 40;
        }
        const steps = [20, 40, 60, 80, 100, 120, 160, 200, 300, 400, 500, 800, 1000];
        for (let i = 0; i < steps.length; ++i) {
            if (v <= steps[i]) {
                return steps[i];
            }
        }
        return Math.ceil(v / 1000) * 1000;
    }

    // Dot colour for the scatter view: against the SLO budget when set
    // (good / caution / slow), otherwise by the response's HTTP-class token.
    function dotColor(bar) {
        if (root.sloP95Ms > 0) {
            if (bar.ms <= root.sloP95Ms) {
                return DesignTokens.statusSuccess;
            }
            if (bar.ms <= root.sloP95Ms * 2) {
                return DesignTokens.statusWarning;
            }
            return DesignTokens.statusError;
        }
        return DesignTokens.statusColor(bar.token);
    }

    function paintScatter(ctx, w, h) {
        const n = root.bars.length;
        if (n === 0) {
            return;
        }
        const padL = root.scatterPadL;
        const padR = root.scatterPadR;
        const padTop = root.scatterPadTop;
        const padBottom = root.scatterPadBottom;
        const plotW = Math.max(1, w - padL - padR);
        const plotH = Math.max(1, h - padTop - padBottom);
        const top = root.niceTop(root.maxMs);

        // Horizontal gridlines + y-axis labels (top → 0).
        const lines = 3;
        ctx.font = "10px sans-serif";
        ctx.textAlign = "right";
        ctx.textBaseline = "middle";
        for (let i = 0; i <= lines; ++i) {
            const val = top * (1 - i / lines);
            const y = padTop + (plotH * i / lines);
            ctx.strokeStyle = DesignTokens.borderSubtle;
            ctx.globalAlpha = (i === lines) ? 0.6 : 0.22;
            ctx.beginPath();
            ctx.moveTo(padL, y);
            ctx.lineTo(w - padR, y);
            ctx.stroke();
            ctx.globalAlpha = 0.8;
            ctx.fillStyle = DesignTokens.textSecondary;
            ctx.fillText(Math.round(val) + (i === 0 ? qsTr(" ms") : ""), padL - 6, y);
            ctx.globalAlpha = 1.0;
        }

        // Dashed threshold line: the SLO budget when set, else the run's p95.
        const thresh = root.sloP95Ms > 0 ? root.sloP95Ms : (root.stats.p95 || 0);
        if (thresh > 0) {
            const yt = padTop + plotH - Math.min(1, thresh / top) * plotH;
            ctx.strokeStyle = root.sloBreached ? DesignTokens.statusError : DesignTokens.textSecondary;
            ctx.globalAlpha = root.sloBreached ? 0.9 : 0.55;
            ctx.setLineDash([4, 3]);
            ctx.beginPath();
            ctx.moveTo(padL, yt);
            ctx.lineTo(w - padR, yt);
            ctx.stroke();
            ctx.setLineDash([]);
            ctx.globalAlpha = 1.0;
        }

        // One dot per response, spread evenly in request order.
        for (let i = 0; i < n; ++i) {
            const ms = root.bars[i].ms;
            const x = padL + (n === 1 ? plotW / 2 : ((i + 0.5) / n) * plotW);
            const y = padTop + plotH - Math.min(1, ms / top) * plotH;
            const r = (root.hoveredBar === i) ? 5 : 3.5;
            ctx.fillStyle = root.dotColor(root.bars[i]);
            ctx.globalAlpha = (root.hoveredBar < 0 || root.hoveredBar === i) ? 1.0 : 0.4;
            ctx.beginPath();
            ctx.arc(x, y, r, 0, Math.PI * 2);
            ctx.fill();
        }
        ctx.globalAlpha = 1.0;
    }

    function paintBars(ctx, w, h) {
        const n = root.bars.length;
        if (n === 0) {
            return;
        }
        // Cap the bar width so a handful of responses render as distinct bars
        // rather than one solid block; centre the group when capped.
        const gap = n > 1 ? 3 : 0;
        const rawW = (w - gap * (n - 1)) / n;
        const barW = Math.max(1, Math.min(36, rawW));
        const groupW = barW * n + gap * (n - 1);
        const offsetX = Math.max(0, (w - groupW) / 2);
        for (let i = 0; i < n; ++i) {
            const ms = root.bars[i].ms;
            const bh = Math.max(2, (ms / root.maxMs) * h);
            const x = offsetX + i * (barW + gap);
            const y = h - bh;
            ctx.fillStyle = DesignTokens.statusColor(root.bars[i].token);
            // Dim the non-hovered bars when one is hovered, for a clear target.
            ctx.globalAlpha = (root.hoveredBar < 0 || root.hoveredBar === i) ? 0.9 : 0.4;
            ctx.fillRect(x, y, barW, bh);
        }
        ctx.globalAlpha = 1.0;

        // Dashed p95 marker across the plot — red when the SLO is breached.
        const p95 = root.stats.p95 || 0;
        if (p95 > 0 && root.maxMs > 0) {
            const yp = h - (p95 / root.maxMs) * h;
            ctx.strokeStyle = root.sloBreached ? DesignTokens.statusError : DesignTokens.textSecondary;
            ctx.globalAlpha = root.sloBreached ? 0.95 : 0.7;
            ctx.setLineDash([4, 3]);
            ctx.beginPath();
            ctx.moveTo(0, yp);
            ctx.lineTo(w, yp);
            ctx.stroke();
            ctx.setLineDash([]);
            ctx.globalAlpha = 1.0;
        }

        // SLO budget line (solid, faint red) so the threshold is visible
        // relative to the bars. Clamped into view when the budget exceeds the
        // tallest bar.
        if (root.sloP95Ms > 0 && root.maxMs > 0) {
            const yb = Math.max(0, h - (root.sloP95Ms / root.maxMs) * h);
            ctx.strokeStyle = DesignTokens.statusError;
            ctx.globalAlpha = 0.35;
            ctx.beginPath();
            ctx.moveTo(0, yb);
            ctx.lineTo(w, yb);
            ctx.stroke();
            ctx.globalAlpha = 1.0;
        }
    }

    function paintHistogram(ctx, w, h) {
        const bins = root.stats.bins || [];
        const n = bins.length;
        if (n === 0) {
            return;
        }
        let maxCount = 1;
        for (let i = 0; i < n; ++i) {
            maxCount = Math.max(maxCount, bins[i]);
        }
        const gap = n > 1 ? 2 : 0;
        const barW = Math.max(1, (w - gap * (n - 1)) / n);
        ctx.fillStyle = DesignTokens.accent;
        ctx.globalAlpha = 0.8;
        for (let i = 0; i < n; ++i) {
            const bh = bins[i] > 0 ? Math.max(2, (bins[i] / maxCount) * h) : 0;
            const x = i * (barW + gap);
            ctx.fillRect(x, h - bh, barW, bh);
        }
        ctx.globalAlpha = 1.0;
    }

    function openSloEditor() {
        sloField.text = root.sloP95Ms > 0 ? String(root.sloP95Ms) : "";
        sloPopup.open();
        sloField.forceActiveFocus();
        sloField.selectAll();
    }
}
