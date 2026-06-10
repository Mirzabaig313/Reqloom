// GlassButton — the app's themed button. `primary` fills with the iridescent
// accent (teal) for the main action; otherwise it's a quiet outlined button.
// Disabled state dims rather than greys out.
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

Button {
    id: control
    property bool primary: false
    implicitHeight: DesignTokens.controlHeightLg
    leftPadding: DesignTokens.spaceLg
    rightPadding: DesignTokens.spaceLg
    font.pixelSize: DesignTokens.fontBody
    font.weight: DesignTokens.weightSemiBold
    font.family: DesignTokens.fontSans

    background: Rectangle {
        radius: DesignTokens.radiusSm
        color: control.primary ? (control.down ? DesignTokens.accentHover : DesignTokens.accent) : (control.down ? DesignTokens.accentMuted : (control.hovered ? DesignTokens.surfaceSunken : "transparent"))
        border.width: control.primary ? 0 : 1
        border.color: DesignTokens.borderSubtle
        opacity: control.enabled ? 1.0 : 0.5
    }

    contentItem: Text {
        text: control.text
        color: control.primary ? DesignTokens.textInverse : DesignTokens.textPrimary
        font: control.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        opacity: control.enabled ? 1.0 : 0.6
    }
}
