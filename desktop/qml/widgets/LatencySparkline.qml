// LatencySparkline — a compact response-time chart for the current run. One bar
// per response (coloured by HTTP-class status token), a dashed p95 marker, and
// a one-line summary (median · p95 · max). Fed by TimelineModel.latencyBars /
// latencyStats; honest stats come from the pure LatencyStats helper in C++.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

ColumnLayout {
    id: root

    // { ms, token, op } per response, and the summary/histogram map.
    required property var bars
    required property var stats

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
        }
    }

    // The bars + p95 line, drawn on a Canvas so it scales to any sample count.
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
                const n = root.bars.length;
                if (n === 0) {
                    return;
                }
                const w = width;
                const h = height;
                const gap = n > 1 ? 2 : 0;
                const barW = Math.max(1, (w - gap * (n - 1)) / n);
                for (let i = 0; i < n; ++i) {
                    const ms = root.bars[i].ms;
                    const bh = Math.max(2, (ms / root.maxMs) * h);
                    const x = i * (barW + gap);
                    const y = h - bh;
                    ctx.fillStyle = DesignTokens.statusColor(root.bars[i].token);
                    ctx.globalAlpha = 0.85;
                    ctx.fillRect(x, y, barW, bh);
                }
                ctx.globalAlpha = 1.0;

                // Dashed p95 marker across the plot.
                const p95 = root.stats.p95 || 0;
                if (p95 > 0 && root.maxMs > 0) {
                    const yp = h - (p95 / root.maxMs) * h;
                    ctx.strokeStyle = DesignTokens.textSecondary;
                    ctx.globalAlpha = 0.7;
                    ctx.setLineDash([4, 3]);
                    ctx.beginPath();
                    ctx.moveTo(0, yp);
                    ctx.lineTo(w, yp);
                    ctx.stroke();
                    ctx.setLineDash([]);
                    ctx.globalAlpha = 1.0;
                }
            }
        }

        // Repaint when data, size, or theme changes.
        Connections {
            target: root
            function onBarsChanged() {
                canvas.requestPaint();
            }
            function onStatsChanged() {
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
    }
}
