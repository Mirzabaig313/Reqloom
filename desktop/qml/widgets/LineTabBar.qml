// LineTabBar — left-aligned, content-width underline tab strip driven by a
// string model. Replaces the stock TabBar's equal-width stretch so tabs sit
// tight on the left with a single full-width baseline. Reusable across the
// request editor and response panel so every tab strip looks identical.
pragma ComponentBehavior: Bound
import QtQuick
import Reqloom

Item {
    id: bar

    property var model: []
    property int currentIndex: 0

    implicitHeight: Math.max(DesignTokens.controlHeight, tabFontMetrics.height + DesignTokens.spaceSm * 2)

    FontMetrics {
        id: tabFontMetrics
        font.family: DesignTokens.fontSans
        font.pointSize: DesignTokens.fontBodyPointSize
    }

    // Full-width baseline under the tab row.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: DesignTokens.borderSubtle
    }

    // Horizontal scroller: when the strip overflows its width the tabs stay on
    // one line and flick/scroll instead of clipping. No visible scrollbar — the
    // strip is one control-height tall, so a bar would collide with the
    // underline; the active tab is always scrolled into view instead.
    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: tabRow.width
        contentHeight: height
        clip: true
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds

        Row {
            id: tabRow
            height: flick.height
            spacing: DesignTokens.spaceLg

            Repeater {
                id: tabRepeater
                model: bar.model

                delegate: Item {
                    id: tab
                    required property string modelData
                    required property int index

                    width: tabLabel.implicitWidth
                    height: tabRow.height

                    Text {
                        id: tabLabel
                        anchors.verticalCenter: parent.verticalCenter
                        text: tab.modelData
                        color: bar.currentIndex === tab.index ? DesignTokens.textPrimary : DesignTokens.textSecondary
                        font.pointSize: DesignTokens.fontBodyPointSize
                        font.weight: bar.currentIndex === tab.index ? DesignTokens.weightSemiBold : DesignTokens.weightRegular
                        Behavior on color {
                            ColorMotion {}
                        }
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: bar.currentIndex = tab.index
                    }
                }
            }
        }

        // Single sliding underline that springs between tabs. Inside the
        // content so it scrolls with the row and stays under its tab.
        Rectangle {
            id: indicator
            // itemAt() isn't itself reactive; gating on count makes the binding
            // re-resolve when the Repeater rebuilds, and guards an out-of-range
            // index. ponytail: a same-length model swap keeps count unchanged and
            // would not refresh this — callers pass fixed section lists, so fine.
            readonly property Item current: bar.currentIndex >= 0 && bar.currentIndex < tabRepeater.count ? tabRepeater.itemAt(bar.currentIndex) : null
            height: 2
            color: DesignTokens.accent
            y: tabRow.height - height
            visible: indicator.current !== null
            x: indicator.current ? indicator.current.x : 0
            width: indicator.current ? indicator.current.width : 0
            Behavior on x {
                SpringMotion {}
            }
            Behavior on width {
                SpringMotion {}
            }
        }
    }

    // Scroll the active tab into view when the current tab changes. Deferred so
    // the delegate exists and geometry has settled.
    function ensureCurrentVisible() {
        const it = tabRepeater.itemAt(bar.currentIndex);
        if (!it) {
            return;
        }
        if (it.x < flick.contentX) {
            flick.contentX = it.x;
        } else if (it.x + it.width > flick.contentX + flick.width) {
            flick.contentX = Math.min(it.x + it.width - flick.width, Math.max(0, tabRow.width - flick.width));
        }
    }
    onCurrentIndexChanged: Qt.callLater(bar.ensureCurrentVisible)
}
