// GlassTextField — the app's themed text input: rounded sunken field with a
// teal focus ring. Set `mono: true` for code/path inputs. All sizing/colour
// comes from DesignTokens, so the whole app's fields restyle from one place.
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

TextField {
    id: control
    property bool mono: false

    implicitHeight: DesignTokens.controlHeight
    color: DesignTokens.textPrimary
    placeholderTextColor: DesignTokens.textSecondary
    font.pixelSize: DesignTokens.fontLabel
    font.family: control.mono ? DesignTokens.fontMono : DesignTokens.fontSans
    leftPadding: DesignTokens.spaceMd
    rightPadding: DesignTokens.spaceMd

    background: Rectangle {
        radius: DesignTokens.radiusSm
        color: DesignTokens.surfaceSunken
        border.width: 1
        border.color: control.activeFocus ? DesignTokens.accent : DesignTokens.borderSubtle
    }
}
