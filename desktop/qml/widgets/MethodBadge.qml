// MethodBadge — a small color-coded HTTP method pill (DESIGN.md §6.2a).
// The hue comes from DesignTokens; the fill is a low-emphasis tint of it.
import QtQuick
import QtQuick.Layouts
import Reqloom

Rectangle {
    id: badge
    required property string method

    function hue(m) {
        return DesignTokens.methodColor(m)
    }

    implicitWidth: label.implicitWidth + DesignTokens.spaceSm * 2
    implicitHeight: 22
    radius: DesignTokens.radiusSm
    color: Qt.rgba(hue(method).r, hue(method).g, hue(method).b, 0.16)

    Text {
        id: label
        anchors.centerIn: parent
        text: badge.method
        color: badge.hue(badge.method)
        font.pixelSize: DesignTokens.fontCaption
        font.weight: DesignTokens.weightBold
        font.letterSpacing: 0.5
    }
}
