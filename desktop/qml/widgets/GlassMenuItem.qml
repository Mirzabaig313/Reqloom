// GlassMenuItem — a themed menu row: highlights as an accent-tinted pill on
// hover/keyboard focus. Used inside GlassMenu.
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

MenuItem {
    id: item
    // Density floor that still grows with the label at large OS text sizes.
    implicitHeight: Math.max(DesignTokens.controlHeight, label.implicitHeight + DesignTokens.spaceSm * 2)
    horizontalPadding: DesignTokens.spaceMd
    contentItem: Text {
        id: label
        text: item.text
        color: item.enabled ? DesignTokens.textPrimary : DesignTokens.textSecondary
        opacity: item.enabled ? 1.0 : 0.6
        font.pixelSize: DesignTokens.fontBody
        font.family: DesignTokens.fontSans
        verticalAlignment: Text.AlignVCenter
        // A long localized label elides instead of painting past the surface.
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: DesignTokens.radiusSm
        color: item.highlighted ? DesignTokens.accentMuted : "transparent"
        Behavior on color {
            ColorMotion {}
        }
    }
}
