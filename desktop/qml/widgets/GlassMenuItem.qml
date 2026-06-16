// GlassMenuItem — a themed menu row: highlights as an accent-tinted pill on
// hover/keyboard focus. Used inside GlassMenu.
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

MenuItem {
    id: item
    implicitHeight: DesignTokens.controlHeight
    horizontalPadding: DesignTokens.spaceMd
    contentItem: Text {
        text: item.text
        color: DesignTokens.textPrimary
        font.pixelSize: DesignTokens.fontBody
        font.family: DesignTokens.fontSans
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        radius: DesignTokens.radiusSm
        color: item.highlighted ? DesignTokens.accentMuted : "transparent"
        Behavior on color {
            ColorMotion {}
        }
    }
}
