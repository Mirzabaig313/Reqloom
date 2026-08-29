// Compact HTTP method instrument label. Desktop design system.
import QtQuick
import Reqloom

Rectangle {
    id: badge

    required property string method
    // When > 0, badges share a column width regardless of method length.
    property int minWidth: 0
    readonly property color methodHue: ({
            "GET": DesignTokens.methodGet,
            "POST": DesignTokens.methodPost,
            "PUT": DesignTokens.methodPut,
            "PATCH": DesignTokens.methodPatch,
            "DELETE": DesignTokens.methodDelete
        })[method] || DesignTokens.textSecondary

    implicitWidth: Math.max(label.implicitWidth + DesignTokens.spaceXs * 2, minWidth)
    implicitHeight: Math.max(22, label.implicitHeight + DesignTokens.spaceXs * 2)
    radius: DesignTokens.radiusSm
    color: DesignTokens.surfaceSunken
    border.width: 1
    border.color: DesignTokens.borderSubtle

    Text {
        id: label

        anchors.centerIn: parent
        text: badge.method
        color: DesignTokens.textPrimary
        font.pointSize: DesignTokens.fontCaptionPointSize
        font.family: DesignTokens.fontMono
        font.weight: DesignTokens.weightSemiBold
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        width: Math.max(0, Math.min(12, parent.width - DesignTokens.spaceXs * 2))
        height: 2
        radius: 1
        color: badge.methodHue
        Behavior on color {
            ColorMotion {}
        }
    }
}
