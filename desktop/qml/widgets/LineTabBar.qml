// LineTabBar — left-aligned, content-width underline tab strip driven by a
// string model. Replaces the stock TabBar's equal-width stretch so tabs sit
// tight on the left with a single full-width baseline. Reusable across the
// request editor and response panel so every tab strip looks identical.
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
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.top: parent.top
        spacing: DesignTokens.spaceLg

        Repeater {
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
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 2
                    color: bar.currentIndex === tab.index ? DesignTokens.accent : "transparent"
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
}
