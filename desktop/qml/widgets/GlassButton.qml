// GlassButton — the app's themed button. `primary` fills with the iridescent
// accent (teal) for the main action; `destructive` fills red for dangerous
// actions (delete/remove); otherwise it's a quiet outlined button. Disabled
// state dims rather than greys out.
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

Button {
    id: control
    property bool primary: false
    // Red-filled variant for irreversible actions so a confirm and a delete
    // never look the same. Takes precedence over `primary`.
    property bool destructive: false
    readonly property bool filled: control.primary || control.destructive
    implicitHeight: Math.max(DesignTokens.controlHeightLg,
                             contentItem.implicitHeight + topPadding + bottomPadding)
    leftPadding: DesignTokens.spaceLg
    rightPadding: DesignTokens.spaceLg
    topPadding: DesignTokens.spaceSm
    bottomPadding: DesignTokens.spaceSm
    font.pointSize: DesignTokens.fontBodyPointSize
    font.weight: DesignTokens.weightSemiBold
    font.family: DesignTokens.fontSans

    background: Rectangle {
        radius: DesignTokens.radiusSm
        color: control.destructive ? (control.down ? Qt.darker(DesignTokens.statusError, 1.25) : DesignTokens.statusError) : control.primary ? (control.down ? DesignTokens.accentHover : DesignTokens.accent) : (control.down ? DesignTokens.accentMuted : (control.hovered ? DesignTokens.surfaceSunken : "transparent"))
        border.width: control.filled ? 0 : 1
        border.color: DesignTokens.borderSubtle
        opacity: control.enabled ? 1.0 : 0.5
        Behavior on color {
            ColorMotion {}
        }
    }

    // A subtle press-in spring so every button feels physical.
    scale: control.down ? 0.97 : 1.0
    Behavior on scale {
        SpringMotion {}
    }

    contentItem: Text {
        text: control.text
        color: control.filled ? DesignTokens.textInverse : DesignTokens.textPrimary
        font: control.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        opacity: control.enabled ? 1.0 : 0.6
    }
}
