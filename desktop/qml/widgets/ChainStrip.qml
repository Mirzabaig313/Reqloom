// ChainStrip — the resolved execution chain for the open endpoint, always
// visible above the editor. Reqloom's differentiator is that it derives the
// chain; leaving that derivation behind the Chain tab hid the one thing no
// competitor does for you.
//
// Reads AppController.executionPreview() (the same resolved plan the response
// pane previews) and AppController.chainStatus for per-step run state, so a
// failed prerequisite is visible without opening the timeline.
//
// Deliberately hidden for a single-step chain: an endpoint with no dependencies
// has nothing to show, and a one-pill strip would cost vertical space to say so.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Rectangle {
    id: root

    // Resolved steps, refreshed on the events that can change the plan rather
    // than bound — executionPreview() re-resolves the plan on every call.
    property var steps: []

    readonly property bool hasChain: root.steps.length > 1

    // A one-step (or unresolvable) chain shows nothing at all.
    visible: AppController.hasOperation && root.hasChain
    implicitHeight: visible ? Math.max(32, Math.round(stripMetrics.height + DesignTokens.spaceSm * 2)) : 0
    color: DesignTokens.surfaceSunken

    function refresh() {
        root.steps = AppController.hasOperation ? AppController.executionPreview() : [];
    }

    Component.onCompleted: root.refresh()

    Connections {
        target: AppController
        function onChainChanged() {
            root.refresh();
        }
        function onOperationChanged() {
            root.refresh();
        }
    }

    FontMetrics {
        id: stripMetrics
        font.family: DesignTokens.fontSans
        font.pointSize: DesignTokens.fontLabelPointSize
    }

    Rectangle {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        color: DesignTokens.borderSubtle
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: DesignTokens.spaceMd
        anchors.rightMargin: DesignTokens.spaceSm
        spacing: DesignTokens.spaceSm

        Label {
            text: qsTr("Chain")
            color: DesignTokens.textSecondary
            font.pointSize: DesignTokens.fontCaptionPointSize
            font.weight: DesignTokens.weightSemiBold
        }

        // Horizontal scroll rather than eliding: a long chain stays readable and
        // the strip's height never changes.
        ListView {
            id: stepsView
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: ListView.Horizontal
            spacing: DesignTokens.spaceXs
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.steps

            delegate: Item {
                id: stepRoot
                required property int index
                required property var modelData

                readonly property string opId: stepRoot.modelData.operationId
                readonly property bool isTarget: stepRoot.modelData.isTarget === true
                // Run state for this step, empty before the chain has run.
                readonly property string runToken: AppController.chainStatus[stepRoot.opId] || ""

                height: stepsView.height
                width: pill.implicitWidth + (stepRoot.index > 0 ? arrow.implicitWidth + DesignTokens.spaceXs : 0)

                Label {
                    id: arrow
                    visible: stepRoot.index > 0
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: "\u2192"
                    color: DesignTokens.textSecondary
                    font.pointSize: DesignTokens.fontCaptionPointSize
                }

                Rectangle {
                    id: pill
                    anchors.left: arrow.visible ? arrow.right : parent.left
                    anchors.leftMargin: arrow.visible ? DesignTokens.spaceXs : 0
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: pillRow.implicitWidth + DesignTokens.spaceSm * 2
                    implicitHeight: Math.round(stripMetrics.height + 4)
                    radius: DesignTokens.radiusSm
                    // The target reads as the anchor of the chain; prerequisites
                    // stay quiet until a run gives them a status of their own.
                    readonly property color hue: stepRoot.runToken.length > 0 ? DesignTokens.statusColor(stepRoot.runToken) : (stepRoot.isTarget ? DesignTokens.accent : DesignTokens.textSecondary)
                    color: stepRoot.isTarget || stepRoot.runToken.length > 0 ? Qt.rgba(pill.hue.r, pill.hue.g, pill.hue.b, 0.14) : "transparent"
                    border.width: 1
                    border.color: stepHover.hovered ? pill.hue : DesignTokens.borderSubtle
                    Behavior on color {
                        ColorMotion {}
                    }

                    RowLayout {
                        id: pillRow
                        anchors.centerIn: parent
                        spacing: DesignTokens.spaceXs

                        Label {
                            text: stepRoot.modelData.number
                            color: DesignTokens.textSecondary
                            font.pointSize: DesignTokens.fontCaptionPointSize
                            font.family: DesignTokens.fontMono
                        }
                        Label {
                            text: stepRoot.opId
                            color: stepRoot.isTarget ? DesignTokens.textPrimary : DesignTokens.textSecondary
                            font.pointSize: DesignTokens.fontLabelPointSize
                            font.weight: stepRoot.isTarget ? DesignTokens.weightMedium : DesignTokens.weightRegular
                        }
                    }

                    HoverHandler {
                        id: stepHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: AppController.activateOperationInProject(AppController.projectRoot, stepRoot.opId)
                    }
                    GlassToolTip {
                        active: stepHover.hovered
                        // Colour never carries the state on its own.
                        text: {
                            const method = stepRoot.modelData.method;
                            const path = stepRoot.modelData.path;
                            const actor = stepRoot.modelData.actor;
                            let lines = [qsTr("%1 %2").arg(method).arg(path)];
                            if (actor.length > 0) {
                                lines.push(qsTr("as %1").arg(actor));
                            }
                            lines.push(stepRoot.isTarget ? qsTr("Target — the endpoint you're editing") : qsTr("Prerequisite — resolved automatically"));
                            lines.push(qsTr("Click to open"));
                            return lines.join("\n");
                        }
                    }
                }
            }
        }

        // Sessions and extractions are reused between runs, so an explicit way
        // to run the chain from scratch belongs beside the chain itself.
        Button {
            id: cleanRunButton
            Layout.alignment: Qt.AlignVCenter
            enabled: !AppController.running
            implicitHeight: Math.round(stripMetrics.height + 6)
            padding: DesignTokens.spaceSm
            text: qsTr("Run cleanly")
            contentItem: Label {
                text: cleanRunButton.text
                color: cleanRunButton.enabled ? DesignTokens.accent : DesignTokens.textSecondary
                font.pointSize: DesignTokens.fontCaptionPointSize
                font.weight: DesignTokens.weightMedium
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: DesignTokens.radiusSm
                color: cleanRunButton.down ? DesignTokens.accentMuted : "transparent"
                border.width: 1
                border.color: cleanRunButton.enabled ? DesignTokens.borderSubtle : "transparent"
            }
            onClicked: AppController.runSelected(true, false)
            GlassToolTip {
                active: cleanRunButton.hovered
                text: qsTr("Discard cached sessions and extractions, then run the whole chain again")
            }
        }
    }
}
