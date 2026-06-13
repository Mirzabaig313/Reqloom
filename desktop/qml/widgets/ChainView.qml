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
    // bright, the rest dim, so you can trace what feeds what.
    property int hoveredIndex: -1
    onStatusMapChanged: edgeCanvas.requestPaint()
    onHoveredIndexChanged: edgeCanvas.requestPaint()

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

    onGraphChanged: edgeCanvas.requestPaint()
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
                        const s = root.nodes[root.edges[i].from];
                        const t = root.nodes[root.edges[i].to];
                        if (!s || !t)
                            continue;
                        // Colour the edge by the downstream node's run status so
                        // the executed flow lights up; default to a neutral line.
                        const st = root.statusMap[t.operationId] || "";
                        const col = st.length > 0 ? DesignTokens.statusColor(st) : DesignTokens.borderStrong;
                        // Dim edges not touching the hovered node.
                        const connected = root.hoveredIndex < 0 || root.edges[i].from === root.hoveredIndex || root.edges[i].to === root.hoveredIndex;
                        ctx.globalAlpha = connected ? 1.0 : 0.16;
                        ctx.strokeStyle = col;
                        ctx.fillStyle = col;
                        const sx = ox + s.x + root.nodeW / 2;
                        const sy = s.y + root.nodeH;
                        const tx = ox + t.x + root.nodeW / 2;
                        const ty = t.y;
                        const dy = ty - sy;
                        // Smooth vertical S-curve from source bottom to target top.
                        ctx.beginPath();
                        ctx.moveTo(sx, sy);
                        ctx.bezierCurveTo(sx, sy + dy * 0.5, tx, ty - dy * 0.5, tx, ty);
                        ctx.stroke();
                        // Arrowhead tucked into the target's top edge.
                        ctx.beginPath();
                        ctx.moveTo(tx, ty);
                        ctx.lineTo(tx - 4, ty - 7);
                        ctx.lineTo(tx + 4, ty - 7);
                        ctx.closePath();
                        ctx.fill();
                    }
                    ctx.globalAlpha = 1.0;
                }
            }

            Repeater {
                model: root.nodes
                delegate: Rectangle {
                    id: card
                    required property int index
                    required property var modelData
                    readonly property string nodeStatus: root.statusMap[card.modelData.operationId] || ""
                    x: plane.offsetX + card.modelData.x
                    y: card.modelData.y
                    width: root.nodeW
                    height: root.nodeH
                    radius: DesignTokens.radiusSm
                    color: card.modelData.isTarget ? DesignTokens.accentMuted : DesignTokens.surfaceRaised
                    border.width: card.nodeStatus.length > 0 ? 2 : 1
                    border.color: card.nodeStatus.length > 0 ? DesignTokens.statusColor(card.nodeStatus) : (card.modelData.isTarget ? DesignTokens.accent : DesignTokens.borderSubtle)

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: DesignTokens.spaceSm
                        anchors.rightMargin: DesignTokens.spaceSm
                        spacing: DesignTokens.spaceSm

                        MethodBadge {
                            method: card.modelData.method
                            Layout.preferredWidth: 54
                        }
                        Label {
                            Layout.fillWidth: true
                            text: card.modelData.operationId
                            color: card.modelData.isTarget ? DesignTokens.textPrimary : DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontLabel
                            font.family: DesignTokens.fontMono
                            font.weight: card.modelData.isTarget ? DesignTokens.weightSemiBold : DesignTokens.weightRegular
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
                            visible: card.modelData.isTarget && card.nodeStatus.length === 0
                            text: qsTr("target")
                            color: DesignTokens.accent
                            font.pixelSize: DesignTokens.fontCaption
                        }
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        onHoveredChanged: root.hoveredIndex = hovered ? card.index : -1
                    }
                    TapHandler {
                        onTapped: root.showDetail(card.modelData, card.x, card.y + card.height + 6)
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
