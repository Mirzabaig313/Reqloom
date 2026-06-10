// GlassMenu — themed context/popup menu: a rounded, elevated overlay surface
// with padding so highlighted items read as inset pills. Pair with GlassMenuItem.
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

Menu {
    id: menu
    padding: 6
    background: Rectangle {
        implicitWidth: 230
        radius: DesignTokens.radius
        color: DesignTokens.surfaceOverlay
        border.width: 1
        border.color: DesignTokens.borderSubtle
    }
}
