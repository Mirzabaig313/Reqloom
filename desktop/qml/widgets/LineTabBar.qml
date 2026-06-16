// LineTabBar — left-aligned, content-width underline tab strip driven by a
// string model. Replaces the stock TabBar's equal-width stretch so tabs sit
// tight on the left with a single full-width baseline. Reusable across the
// request editor and response panel so every tab strip looks identical.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Reqloom

Item {
    id: bar

    property var model: []
    property int currentIndex: 0

    implicitHeight: DesignTokens.controlHeight

    // Full-width baseline under the tab row.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: DesignTokens.borderSubtle
    }

    Row {
        id: tabRow
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.top: parent.top
        spacing: DesignTokens.spaceLg

        Repeater {
            id: tabRepeater
            model: bar.model

            delegate: Item {
                id: tab
                required property string modelData
                required property int index

                width: tabLabel.implicitWidth
                height: bar.height

                Text {
                    id: tabLabel
                    anchors.verticalCenter: parent.verticalCenter
                    text: tab.modelData
                    color: bar.currentIndex === tab.index ? DesignTokens.textPrimary : DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontBody
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

    // Single sliding underline that springs between tabs, instead of a static
    // per-tab rectangle. Tracks the current tab's geometry.
    Rectangle {
        id: indicator
        readonly property Item current: tabRepeater.itemAt(bar.currentIndex)
        height: 2
        color: DesignTokens.accent
        anchors.bottom: parent.bottom
        visible: indicator.current !== null
        x: indicator.current ? tabRow.x + indicator.current.x : 0
        width: indicator.current ? indicator.current.width : 0
        Behavior on x {
            SpringMotion {}
        }
        Behavior on width {
            SpringMotion {}
        }
    }
}
