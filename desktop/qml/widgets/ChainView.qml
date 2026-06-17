// ChainView — the execution chain rendered as a layered dependency graph
// (DESIGN.md §6.3, the product's hero surface). Fed a laid-out graph from
// AppController.chainGraph ({nodes:[{operationId,method,isTarget,x,y}], edges:
// [{from,to}], width, height, nodeWidth, nodeHeight}); prerequisites sit above
// their dependents, edges curve between them (Bézier on a Canvas), and the
// target node is highlighted. Scrolls when the graph outgrows the panel.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Rectangle {
    id: root

    // Laid-out graph from AppController.chainGraph (empty map → empty state).
    property var graph: ({})
    property string emptyText: ""
    // operationId → live run status token (running/success/error/…), for colour.
    property var statusMap: ({})
    // Emitted when a node is activated ("Open endpoint" in its detail popover).
    signal nodeActivated(string operationId)
    // Emitted to open a node's endpoint directly in Edit mode ("Edit chain").
    signal nodeEditRequested(string operationId)

    // Index of the node currently hovered (−1 = none): connected edges stay
    // bright, the rest dim, so you can trace what feeds what. Tracked by
    // operationId (not row index) because the stable node model below keeps
    // delegates across relayouts, so model order need not match graph order.
    property string hoveredOp: ""
    onStatusMapChanged: edgeCanvas.requestPaint()
    onHoveredOpChanged: edgeCanvas.requestPaint()

    // Stable node identity across relayouts. `graph.nodes` is a fresh array on
    // every rebuild, so binding the Repeater straight to it would destroy and
    // recreate every delegate — nodes would pop to new spots. Instead we keep a
    // ListModel keyed by operationId and mutate each row's target x/y in place,
    // so delegates persist and their `Behavior on x/y` springs glide them.
    // `nodeOps` maps an edge's node index to its operationId; `livePos` holds
    // each node's current (animating) logical position so edges follow along.
    property var nodeOps: []
    property var livePos: ({})

    ListModel {
        id: nodeModel
    }

    // Look up the full graph node (with detail fields) by operationId, for the
    // detail popover — the ListModel itself carries only render scalars.
    function nodeByOp(op) {
        const list = root.graph.nodes || [];
        for (let i = 0; i < list.length; ++i) {
            if (list[i].operationId === op) {
                return list[i];
            }
        }
        return {};
    }

    // Reconcile the ListModel with the latest graph: update existing rows'
    // target position in place, append new nodes, drop removed ones.
    function syncNodes() {
        const incoming = root.graph.nodes || [];
        const ops = [];
        const seen = {};
        for (let i = 0; i < incoming.length; ++i) {
            ops.push(incoming[i].operationId);
            seen[incoming[i].operationId] = incoming[i];
        }
        root.nodeOps = ops;

        // Drop rows whose node no longer exists.
        for (let r = nodeModel.count - 1; r >= 0; --r) {
            const op = nodeModel.get(r).operationId;
            if (!seen.hasOwnProperty(op)) {
                delete root.livePos[op];
                nodeModel.remove(r);
            }
        }

        // Upsert each incoming node.
        for (let i = 0; i < incoming.length; ++i) {
            const n = incoming[i];
            let idx = -1;
            for (let r = 0; r < nodeModel.count; ++r) {
                if (nodeModel.get(r).operationId === n.operationId) {
                    idx = r;
                    break;
                }
            }
            if (idx < 0) {
                // New node: seed its live position so the first edge paint and
                // the delegate both start at the target spot (no glide-in from 0,0).
                root.livePos[n.operationId] = {
                    "x": n.x,
                    "y": n.y
                };
                nodeModel.append({
                    "operationId": n.operationId,
                    "method": n.method || "",
                    "isTarget": n.isTarget || false,
                    "tx": n.x,
                    "ty": n.y
                });
            } else {
                nodeModel.setProperty(idx, "method", n.method || "");
                nodeModel.setProperty(idx, "isTarget", n.isTarget || false);
                nodeModel.setProperty(idx, "tx", n.x);
                nodeModel.setProperty(idx, "ty", n.y);
            }
        }
        edgeCanvas.requestPaint();
    }

    // Zoom factor for the graph plane (pan is the Flickable itself).
    property real zoom: 1.0
    readonly property real minZoom: 0.5
    readonly property real maxZoom: 2.0

    readonly property var nodes: graph.nodes || []
    readonly property var edges: graph.edges || []
    readonly property real nodeW: graph.nodeWidth || 200
    readonly property real nodeH: graph.nodeHeight || 38
    readonly property real graphW: graph.width || 0
    readonly property real graphH: graph.height || 0

    radius: DesignTokens.radiusSm
    color: DesignTokens.surfaceSunken
    border.width: 1
    border.color: DesignTokens.borderSubtle
    implicitHeight: root.nodes.length === 0 ? 56 : Math.min(graphH + DesignTokens.spaceLg * 2, 320)

    onGraphChanged: root.syncNodes()
    Component.onCompleted: root.syncNodes()
    Connections {
        target: DesignTokens
        function onTokensChanged() {
            edgeCanvas.requestPaint();
        }
    }

    Label {
        anchors.fill: parent
        anchors.margins: DesignTokens.spaceMd
        visible: root.nodes.length === 0
        text: root.emptyText
        color: DesignTokens.textSecondary
        font.pixelSize: DesignTokens.fontLabel
        wrapMode: Text.WordWrap
        verticalAlignment: Text.AlignVCenter
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: DesignTokens.spaceMd
        visible: root.nodes.length > 0
        clip: true
        contentWidth: plane.width * root.zoom
        contentHeight: plane.height * root.zoom
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        ScrollBar.horizontal: ScrollBar {}

        // Ctrl + wheel zooms; plain wheel scrolls (Flickable default).
        WheelHandler {
            acceptedModifiers: Qt.ControlModifier
            onWheel: event => {
                const step = event.angleDelta.y > 0 ? 0.1 : -0.1;
                root.zoom = Math.max(root.minZoom, Math.min(root.maxZoom, root.zoom + step));
            }
        }

        // The graph plane, centred horizontally when narrower than the view,
        // scaled by `zoom` from its top-left so the Flickable scroll maps 1:1.
        Item {
            id: plane
            width: Math.max(root.graphW, flick.width / root.zoom)
            height: root.graphH
            transformOrigin: Item.TopLeft
            scale: root.zoom
            // Zoom settles with the app's canonical spring instead of
            // snapping in 10% steps.
            Behavior on scale {
                SpringMotion {}
            }
            // Offset that centres the graph within the available width.
            readonly property real offsetX: Math.max(0, (width - root.graphW) / 2)

            // Edges first so node cards paint on top of the curves.
            Canvas {
                id: edgeCanvas
                anchors.fill: parent
                onPaint: {
                    const ctx = getContext("2d");
                    ctx.reset();
                    ctx.lineWidth = 1.5;
                    const ox = plane.offsetX;
                    for (let i = 0; i < root.edges.length; ++i) {
                        // Resolve endpoints by operationId so coordinates track
                        // the live (animating) node positions, letting edges
                        // glide with the cards during a relayout.
                        const sOp = root.nodeOps[root.edges[i].from];
                        const tOp = root.nodeOps[root.edges[i].to];
                        const s = root.livePos[sOp];
                        const t = root.livePos[tOp];
                        if (!s || !t)
                            continue;
                        // Colour the edge by the downstream node's run status so
                        // the executed flow lights up; default to a neutral line.
                        const st = root.statusMap[tOp] || "";
                        const col = st.length > 0 ? DesignTokens.statusColor(st) : DesignTokens.borderStrong;
                        // Dim edges not touching the hovered node.
                        const connected = root.hoveredOp.length === 0 || sOp === root.hoveredOp || tOp === root.hoveredOp;
                        ctx.globalAlpha = connected ? 1.0 : 0.16;
                        ctx.strokeStyle = col;
                        ctx.fillStyle = col;
                        const sx = ox + s.x + root.nodeW / 2;
                        const sy = s.y + root.nodeH;
                        const tx = ox + t.x + root.nodeW / 2;
                        const ty = t.y;
                        const dy = ty - sy;
                        // Smooth vertical S-curve from source bottom to target top.
                        // Derived (auto, from {{}} usage) edges are dashed;
                        // explicit depends_on edges are solid.
                        ctx.setLineDash(root.edges[i].explicit ? [] : [5, 4]);
                        ctx.beginPath();
                        ctx.moveTo(sx, sy);
                        ctx.bezierCurveTo(sx, sy + dy * 0.5, tx, ty - dy * 0.5, tx, ty);
                        ctx.stroke();
                        ctx.setLineDash([]);
                        // Arrowhead tucked into the target's top edge.
                        ctx.beginPath();
                        ctx.moveTo(tx, ty);
                        ctx.lineTo(tx - 4, ty - 7);
                        ctx.lineTo(tx + 4, ty - 7);
                        ctx.closePath();
                        ctx.fill();
                        // Label the edge with the variable(s) that flow along it.
                        const label = root.edges[i].label || "";
                        if (label.length > 0) {
                            const mx = (sx + tx) / 2;
                            const my = (sy + ty) / 2;
                            ctx.font = "10px monospace";
                            ctx.textAlign = "center";
                            ctx.textBaseline = "middle";
                            const tw = ctx.measureText(label).width;
                            ctx.fillStyle = DesignTokens.surfaceSunken;
                            ctx.fillRect(mx - tw / 2 - 5, my - 8, tw + 10, 16);
                            ctx.fillStyle = col;
                            ctx.fillText(label, mx, my);
                        }
                    }
                    ctx.globalAlpha = 1.0;
                }
            }

            Repeater {
                model: nodeModel
                delegate: Rectangle {
                    id: card
                    required property int index
                    required property string operationId
                    required property string method
                    required property bool isTarget
                    required property real tx
                    required property real ty
                    readonly property string nodeStatus: root.statusMap[card.operationId] || ""
                    x: plane.offsetX + card.tx
                    y: card.ty
                    width: root.nodeW
                    height: root.nodeH
                    // A gentle physical lift when hovered, and a spring on
                    // position so a relayout glides rather than jumps. The
                    // node model is mutated in place (see syncNodes), so these
                    // Behaviors actually fire on relayout instead of the
                    // delegate being recreated at the new spot.
                    scale: cardHover.hovered ? 1.03 : 1.0
                    Behavior on scale {
                        SpringMotion {}
                    }
                    Behavior on x {
                        SpringMotion {}
                    }
                    Behavior on y {
                        SpringMotion {}
                    }
                    // Publish the live (logical, pre-offset) position as the
                    // spring animates so the edge Canvas redraws its curves to
                    // follow the moving cards.
                    onXChanged: {
                        root.livePos[card.operationId] = {
                            "x": card.x - plane.offsetX,
                            "y": card.y
                        };
                        edgeCanvas.requestPaint();
                    }
                    onYChanged: {
                        root.livePos[card.operationId] = {
                            "x": card.x - plane.offsetX,
                            "y": card.y
                        };
                        edgeCanvas.requestPaint();
                    }
                    radius: DesignTokens.radiusSm
                    color: card.isTarget ? DesignTokens.accentMuted : DesignTokens.surfaceRaised
                    border.width: card.nodeStatus.length > 0 ? 2 : 1
                    border.color: card.nodeStatus.length > 0 ? DesignTokens.statusColor(card.nodeStatus) : (card.isTarget ? DesignTokens.accent : DesignTokens.borderSubtle)

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: DesignTokens.spaceSm
                        anchors.rightMargin: DesignTokens.spaceSm
                        spacing: DesignTokens.spaceSm

                        MethodBadge {
                            method: card.method
                            Layout.preferredWidth: 54
                        }
                        Label {
                            Layout.fillWidth: true
                            text: card.operationId
                            color: card.isTarget ? DesignTokens.textPrimary : DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontLabel
                            font.family: DesignTokens.fontMono
                            font.weight: card.isTarget ? DesignTokens.weightSemiBold : DesignTokens.weightRegular
                            elide: Text.ElideMiddle
                        }
                        // Live run-status glyph (colour + glyph, never colour
                        // alone — DESIGN.md §6.1) once a run touches this node.
                        StatusBadge {
                            visible: card.nodeStatus.length > 0
                            token: card.nodeStatus
                            label: ""
                        }
                        Label {
                            visible: card.isTarget && card.nodeStatus.length === 0
                            text: qsTr("target")
                            color: DesignTokens.accent
                            font.pixelSize: DesignTokens.fontCaption
                        }
                    }

                    HoverHandler {
                        id: cardHover
                        cursorShape: Qt.PointingHandCursor
                        onHoveredChanged: root.hoveredOp = hovered ? card.operationId : ""
                    }
                    TapHandler {
                        onTapped: root.showDetail(root.nodeByOp(card.operationId), card.x, card.y + card.height + 6)
                    }
                }
            }
        }
    }

    // Zoom control (Ctrl+wheel also zooms). Overlays the top-right corner.
    Rectangle {
        visible: root.nodes.length > 1
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: DesignTokens.spaceSm
        radius: DesignTokens.radiusSm
        color: DesignTokens.surfaceOverlay
        border.width: 1
        border.color: DesignTokens.borderSubtle
        implicitWidth: zoomRow.implicitWidth + DesignTokens.spaceSm * 2
        implicitHeight: 24

        Row {
            id: zoomRow
            anchors.centerIn: parent
            spacing: DesignTokens.spaceXs

            ZoomButton {
                text: "\u2212"  // minus
                TapHandler {
                    onTapped: root.zoom = Math.max(root.minZoom, root.zoom - 0.1)
                }
            }
            Label {
                width: 34
                horizontalAlignment: Text.AlignHCenter
                text: Math.round(root.zoom * 100) + "%"
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontCaption
                TapHandler {
                    onTapped: root.zoom = 1.0
                }
            }
            ZoomButton {
                text: "+"
                TapHandler {
                    onTapped: root.zoom = Math.min(root.maxZoom, root.zoom + 0.1)
                }
            }
        }
    }

    component ZoomButton: Label {
        width: 16
        horizontalAlignment: Text.AlignHCenter
        color: hover.hovered ? DesignTokens.accent : DesignTokens.textSecondary
        font.pixelSize: DesignTokens.fontBody
        font.weight: DesignTokens.weightSemiBold
        HoverHandler {
            id: hover
            cursorShape: Qt.PointingHandCursor
        }
    }

    // Per-node detail popover, opened by clicking a card. Shows the full path,
    // auth actor, extracted variables, and direct dependencies, with an action
    // to open that endpoint in the editor.
    property var detailNode: ({})
    function showDetail(node, px, py) {
        root.detailNode = node;
        detailPopup.x = Math.max(0, Math.min(px, root.width - detailPopup.width));
        detailPopup.y = py;
        detailPopup.open();
    }

    Popup {
        id: detailPopup
        width: 300
        padding: DesignTokens.spaceMd
        modal: false
        focus: true
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

            RowLayout {
                Layout.fillWidth: true
                spacing: DesignTokens.spaceSm
                MethodBadge {
                    method: root.detailNode.method || ""
                    Layout.preferredWidth: 54
                }
                Label {
                    Layout.fillWidth: true
                    text: root.detailNode.operationId || ""
                    color: DesignTokens.textPrimary
                    font.pixelSize: DesignTokens.fontBody
                    font.family: DesignTokens.fontMono
                    font.weight: DesignTokens.weightSemiBold
                    elide: Text.ElideMiddle
                }
            }

            Label {
                Layout.fillWidth: true
                text: root.detailNode.path || qsTr("(no path)")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontLabel
                font.family: DesignTokens.fontMono
                wrapMode: Text.WrapAnywhere
            }

            // Auth actor.
            RowLayout {
                Layout.fillWidth: true
                spacing: DesignTokens.spaceXs
                Label {
                    text: qsTr("Auth")
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontCaption
                    font.weight: DesignTokens.weightSemiBold
                    font.letterSpacing: 0.6
                }
                Label {
                    Layout.fillWidth: true
                    text: (root.detailNode.actor && root.detailNode.actor.length > 0) ? root.detailNode.actor : qsTr("none")
                    color: DesignTokens.textPrimary
                    font.pixelSize: DesignTokens.fontLabel
                    elide: Text.ElideRight
                }
            }

            // Extracted variables.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                visible: (root.detailNode.extracts || []).length > 0
                Label {
                    text: qsTr("EXTRACTS")
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontCaption
                    font.weight: DesignTokens.weightSemiBold
                    font.letterSpacing: 0.6
                }
                Label {
                    Layout.fillWidth: true
                    text: (root.detailNode.extracts || []).join(", ")
                    color: DesignTokens.statusSuccess
                    font.pixelSize: DesignTokens.fontLabel
                    font.family: DesignTokens.fontMono
                    wrapMode: Text.WrapAnywhere
                }
            }

            // Direct dependencies.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                visible: (root.detailNode.deps || []).length > 0
                Label {
                    text: qsTr("DEPENDS ON")
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontCaption
                    font.weight: DesignTokens.weightSemiBold
                    font.letterSpacing: 0.6
                }
                Label {
                    Layout.fillWidth: true
                    text: (root.detailNode.deps || []).join(", ")
                    color: DesignTokens.textPrimary
                    font.pixelSize: DesignTokens.fontLabel
                    font.family: DesignTokens.fontMono
                    wrapMode: Text.WrapAnywhere
                }
            }

            Button {
                id: openBtn
                visible: !(root.detailNode.isTarget || false)
                Layout.fillWidth: true
                Layout.topMargin: DesignTokens.spaceXs
                implicitHeight: 30
                onClicked: {
                    root.nodeActivated(root.detailNode.operationId);
                    detailPopup.close();
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: openBtn.down ? DesignTokens.accentHover : DesignTokens.accent
                }
                contentItem: Text {
                    text: qsTr("Open endpoint")
                    color: DesignTokens.textInverse
                    font.pixelSize: DesignTokens.fontLabel
                    font.weight: DesignTokens.weightSemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
            // Jump straight into editing this endpoint's chain (no YAML).
            Button {
                id: editChainBtn
                Layout.fillWidth: true
                implicitHeight: 28
                onClicked: {
                    root.nodeEditRequested(root.detailNode.operationId);
                    detailPopup.close();
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: editChainBtn.down ? Qt.rgba(1, 1, 1, 0.08) : "transparent"
                    border.width: 1
                    border.color: DesignTokens.borderStrong
                }
                contentItem: Text {
                    text: qsTr("Edit chain")
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontLabel
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }
}
