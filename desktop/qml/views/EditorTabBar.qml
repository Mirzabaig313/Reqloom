// EditorTabBar — the open-tabs strip above the centre pane. Each row is one
// open endpoint (method badge + name) or actor (user glyph + name). Click to
// activate, middle-click or the × to close, right-click for "close others".
// Bound to AppController.tabModel + activeTabIndex; the per-tab editor state
// lives in AppController and is swapped in on activate.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Rectangle {
    id: bar
    implicitHeight: 40
    color: DesignTokens.surfaceSunken

    // Bottom hairline separating the strip from the editor.
    Rectangle {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        color: DesignTokens.borderSubtle
    }

    ListView {
        id: tabsView
        anchors.fill: parent
        anchors.leftMargin: DesignTokens.spaceXs
        anchors.rightMargin: DesignTokens.spaceXs
        orientation: ListView.Horizontal
        spacing: 2
        clip: true
        model: AppController.tabModel
        currentIndex: AppController.activeTabIndex
        boundsBehavior: Flickable.StopAtBounds
        // Keep the active tab scrolled into view when it changes.
        highlightRangeMode: ListView.NoHighlightRange
        onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)

        delegate: Item {
            id: tabRoot
            required property int index
            required property int kind
            required property string title
            required property string method
            required property bool dirty
            readonly property bool active: tabRoot.index === AppController.activeTabIndex
            // Visual x-offset while dragging to reorder; reset on drop.
            property real dragDx: 0

            height: tabsView.height
            width: Math.min(220, Math.max(112, titleMetrics.width + 96))
            z: dragDx !== 0 ? 2 : 0
            transform: Translate {
                x: tabRoot.dragDx
            }

            TextMetrics {
                id: titleMetrics
                font.pixelSize: DesignTokens.fontLabel
                text: tabRoot.title
            }

            Rectangle {
                id: chip
                anchors.fill: parent
                anchors.topMargin: 5
                anchors.bottomMargin: 5
                radius: DesignTokens.radiusSm
                color: tabRoot.active ? DesignTokens.surfaceRaised : (hover.hovered ? Qt.rgba(1, 1, 1, 0.05) : "transparent")
                border.width: 1
                border.color: tabRoot.active ? DesignTokens.accent : DesignTokens.borderSubtle
                Behavior on color {
                    ColorMotion {}
                }

                HoverHandler {
                    id: hover
                }
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    // ReleaseWithinBounds avoids the passive-grab overlap with
                    // the close MouseArea (which takes an exclusive grab), so
                    // clicking × can't also fire activateTab.
                    gesturePolicy: TapHandler.ReleaseWithinBounds
                    onTapped: AppController.activateTab(tabRoot.index)
                }
                TapHandler {
                    acceptedButtons: Qt.MiddleButton
                    onTapped: AppController.closeTab(tabRoot.index)
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: tabMenu.popup()
                }
                // Drag horizontally to reorder. We offset the chip visually and
                // commit the new position on drop (target resolved by the chip's
                // centre), so variable-width tabs reorder correctly.
                DragHandler {
                    id: reorder
                    target: null
                    xAxis.enabled: true
                    yAxis.enabled: false
                    cursorShape: Qt.ClosedHandCursor
                    onActiveTranslationChanged: tabRoot.dragDx = activeTranslation.x
                    onActiveChanged: {
                        if (active) {
                            tabRoot.dragDx = 0;
                            return;
                        }
                        const centreX = tabRoot.x + tabRoot.width / 2 + tabRoot.dragDx;
                        let target = tabsView.indexAt(centreX, tabsView.height / 2);
                        if (target < 0) {
                            // Landed in an inter-tab spacing gap — probe just
                            // beside the centre before treating it as an end drop.
                            target = tabsView.indexAt(centreX - tabsView.spacing, tabsView.height / 2);
                        }
                        if (target < 0) {
                            target = tabsView.indexAt(centreX + tabsView.spacing, tabsView.height / 2);
                        }
                        if (target < 0) {
                            // Genuine past-the-end drop: snap to the near end.
                            target = centreX <= tabsView.contentX ? 0 : tabsView.count - 1;
                        }
                        tabRoot.dragDx = 0;
                        if (target !== tabRoot.index) {
                            AppController.moveTab(tabRoot.index, target);
                        }
                    }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: DesignTokens.spaceSm
                    anchors.rightMargin: DesignTokens.spaceXs
                    spacing: DesignTokens.spaceXs

                    MethodBadge {
                        visible: tabRoot.kind === 0
                        method: tabRoot.kind === 0 ? tabRoot.method : "GET"
                    }
                    AppIcon {
                        visible: tabRoot.kind === 1
                        name: "user"
                        size: 14
                        color: tabRoot.active ? DesignTokens.textPrimary : DesignTokens.textSecondary
                    }
                    Label {
                        Layout.fillWidth: true
                        text: tabRoot.title
                        color: tabRoot.active ? DesignTokens.textPrimary : DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontLabel
                        font.weight: tabRoot.active ? DesignTokens.weightMedium : DesignTokens.weightRegular
                        elide: Text.ElideRight
                    }
                    // Dirty dot (unsaved); the close × replaces it on hover.
                    Item {
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        Layout.alignment: Qt.AlignVCenter
                        Rectangle {
                            anchors.centerIn: parent
                            visible: tabRoot.dirty && !hover.hovered
                            width: 7
                            height: 7
                            radius: 3.5
                            color: DesignTokens.textSecondary
                        }
                        AppIcon {
                            anchors.centerIn: parent
                            visible: hover.hovered
                            name: "x"
                            size: 13
                            color: closeArea.containsMouse ? DesignTokens.textPrimary : DesignTokens.textSecondary
                        }
                        MouseArea {
                            id: closeArea
                            anchors.fill: parent
                            anchors.margins: -3
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: AppController.closeTab(tabRoot.index)
                        }
                    }
                }

                GlassMenu {
                    id: tabMenu
                    GlassMenuItem {
                        text: qsTr("Close")
                        // Defer: closing destroys this delegate (and the menu),
                        // so don't do it from inside the menu item handler.
                        onTriggered: Qt.callLater(() => AppController.closeTab(tabRoot.index))
                    }
                    GlassMenuItem {
                        text: qsTr("Close others")
                        enabled: AppController.tabCount > 1
                        onTriggered: AppController.closeOtherTabs(tabRoot.index)
                    }
                }
            }
        }
    }
}
