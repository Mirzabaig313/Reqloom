// StatusBadge — a small status pill (glyph + optional label) coloured from the
// status vocabulary (QML Migration Roadmap WS-C). Mirrors the C++ StatusBadge
//  colour is always paired with a glyph, never colour alone,
// so colour-blind users can still distinguish states. The glyph + colour come
// from DesignTokens so QML and C++ share one vocabulary.
import QtQuick
import Reqloom

Rectangle {
    id: badge

    // Status vocabulary token: "running", "success", "warning", "error",
    // "skipped", "cancelled", "blocked", "idle", "neutral".
    required property string token
    // Optional trailing label (e.g. "HTTP 200"). Empty → glyph only.
    property string label: ""

    readonly property color hue: DesignTokens.statusColor(token)

    implicitWidth: row.implicitWidth + DesignTokens.spaceSm * 2
    implicitHeight: Math.max(20, row.implicitHeight + DesignTokens.spaceXs * 2)
    radius: DesignTokens.radiusSm
    color: Qt.rgba(hue.r, hue.g, hue.b, 0.16)
    Behavior on color {
        ColorMotion {}
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 4
        Text {
            text: DesignTokens.statusGlyph(badge.token)
            color: badge.hue
            font.pixelSize: DesignTokens.fontCaption
            font.weight: DesignTokens.weightBold
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            visible: badge.label.length > 0
            text: badge.label
            color: badge.hue
            font.pixelSize: DesignTokens.fontCaption
            font.weight: DesignTokens.weightSemiBold
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}
