// GlassMenu — themed context/popup menu: a rounded, elevated overlay surface
// with padding so highlighted items read as inset pills. Pair with GlassMenuItem.
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

Menu {
    id: menu
    padding: 6
    // Keep the menu inside the window instead of assuming an English-label
    // width fits: margins let Qt shift it into view, and the width cap stops a
    // long localized entry from growing the surface past the window edge.
    margins: DesignTokens.spaceSm
    width: Math.min(implicitWidth, Overlay.overlay ? Overlay.overlay.width - DesignTokens.spaceLg * 2 : implicitWidth)
    // Cap the height too, otherwise a long menu at large text sizes runs past
    // the window and its last entries can't be reached; capping makes the
    // menu's own view scroll instead.
    height: Math.min(implicitHeight, Overlay.overlay ? Overlay.overlay.height - DesignTokens.spaceLg * 2 : implicitHeight)

    enter: PopupEnter {}
    exit: PopupExit {}

    background: Rectangle {
        implicitWidth: 230
        radius: DesignTokens.radius
        color: DesignTokens.surfaceOverlay
        border.width: 1
        border.color: DesignTokens.borderSubtle
    }
}
