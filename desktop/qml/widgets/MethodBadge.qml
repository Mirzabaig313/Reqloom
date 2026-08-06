// MethodBadge — a small color-coded HTTP method pill .
// The hue comes from DesignTokens; the fill is a low-emphasis tint of it.
import QtQuick
import Reqloom

Rectangle {
    id: badge
    required property string method
    // When > 0, the pill is at least this wide so a column of badges
    // (e.g. the Explorer) keeps every following name on the same x-axis,
    // regardless of method length (GET vs OPTIONS).
    property int minWidth: 0

    function hue(m) {
        return DesignTokens.methodColor(m);
    }

    implicitWidth: Math.max(label.implicitWidth + DesignTokens.spaceSm * 2, minWidth)
    implicitHeight: Math.max(22, label.implicitHeight + DesignTokens.spaceXs * 2)
    radius: DesignTokens.radiusSm
    color: Qt.rgba(hue(method).r, hue(method).g, hue(method).b, 0.16)
    Behavior on color {
        ColorMotion {}
    }

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
