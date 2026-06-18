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

    // Bars view (default) vs Freedman–Diaconis histogram view.
    property bool showHistogram: false
    // Bar index under the cursor in bars view (−1 = none).
    property int hoveredBar: -1

    readonly property real maxMs: {
        let m = 1;
        for (let i = 0; i < root.bars.length; ++i) {
            m = Math.max(m, root.bars[i].ms);
        }
        return m;
    }

    visible: root.bars.length > 0
    spacing: DesignTokens.spaceXs

    RowLayout {
        Layout.fillWidth: true
        spacing: DesignTokens.spaceSm
        Label {
            text: qsTr("LATENCY")
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontCaption
            font.weight: DesignTokens.weightSemiBold
            font.letterSpacing: 1.2
        }
        Item {
            Layout.fillWidth: true
        }
        Label {
            // median · p95 · max, rounded to whole ms.
            text: qsTr("med %1 · p95 %2 · max %3 ms").arg(Math.round(root.stats.median || 0)).arg(Math.round(root.stats.p95 || 0)).arg(Math.round(root.stats.max || 0))
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontCaption
            font.family: DesignTokens.fontMono
            // Tabular figures so the three stats stay column-stable as they update.
            font.features: ({
                    "tnum": 1
                })
        }
        // SLO verdict pill: green when within budget, red when breached.
        // Click to edit; click "+ SLO" (below) to set one when unset.
        Rectangle {
            id: sloPill
            visible: root.sloP95Ms > 0
            implicitWidth: sloRow.implicitWidth + DesignTokens.spaceSm * 2
            implicitHeight: 18
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
        // View toggle: bars ↔ histogram.
        Label {
            id: viewToggle
            text: root.showHistogram ? qsTr("Bars") : qsTr("Histogram")
            color: toggleHover.hovered ? DesignTokens.accent : DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontCaption
            font.weight: DesignTokens.weightSemiBold
            Behavior on color {
                ColorMotion {}
            }
            HoverHandler {
                id: toggleHover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                onTapped: {
                    root.showHistogram = !root.showHistogram;
                    root.hoveredBar = -1;
                }
            }
            GlassToolTip {
                active: toggleHover.hovered
                text: root.showHistogram ? qsTr("Show per-response bars") : qsTr("Show latency distribution (Freedman–Diaconis bins)")
            }
        }
    }

    // The bars/histogram plot, drawn on a Canvas so it scales to any count.
    Item {
        id: plot
        Layout.fillWidth: true
        Layout.preferredHeight: 48

        Canvas {
            id: canvas
            anchors.fill: parent
            onPaint: {
                const ctx = getContext("2d");
                ctx.reset();
                const w = width;
                const h = height;
                if (root.showHistogram) {
                    root.paintHistogram(ctx, w, h);
                } else {
                    root.paintBars(ctx, w, h);
                }
            }
        }

        // Hover + click interaction (bars view only). Maps cursor x → bar index.
        HoverHandler {
            id: plotHover
            enabled: !root.showHistogram
            onPointChanged: root.hoveredBar = root.barAt(point.position.x, plot.width)
            onHoveredChanged: {
                if (!hovered) {
                    root.hoveredBar = -1;
                }
            }
        }
        TapHandler {
            enabled: !root.showHistogram
            onTapped: eventPoint => {
                const i = root.barAt(eventPoint.position.x, plot.width);
                if (i >= 0 && i < root.bars.length) {
                    root.stepActivated(root.bars[i].stepIndex);
                }
            }
        }
        MouseArea {
            anchors.fill: parent
            enabled: !root.showHistogram
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
            function onShowHistogramChanged() {
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

    // Bar index at plot-relative x, or −1 outside the bars.
    function barAt(px, plotW) {
        const n = root.bars.length;
        if (n === 0) {
            return -1;
        }
        const gap = n > 1 ? 2 : 0;
        const barW = Math.max(1, (plotW - gap * (n - 1)) / n);
        const i = Math.floor(px / (barW + gap));
        return (i >= 0 && i < n) ? i : -1;
    }

    function paintBars(ctx, w, h) {
        const n = root.bars.length;
        if (n === 0) {
            return;
        }
        const gap = n > 1 ? 2 : 0;
        const barW = Math.max(1, (w - gap * (n - 1)) / n);
        for (let i = 0; i < n; ++i) {
            const ms = root.bars[i].ms;
            const bh = Math.max(2, (ms / root.maxMs) * h);
            const x = i * (barW + gap);
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
