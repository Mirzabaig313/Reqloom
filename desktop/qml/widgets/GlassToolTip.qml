// GlassToolTip — the app's themed tooltip. A rounded, translucent surface
// popover that matches the glass design system, wraps long copy, and tracks
// the theme. Drive it with `active` (bound to a control's `hovered`): it waits
// `showDelay` ms before appearing so a casual mouse pass doesn't flash it, and
// sits just below the control so it never covers the controls above it.
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

ToolTip {
    id: tip

    // Bind to the host control's `hovered`. The tip shows only after showDelay.
    property bool active: false
    property int showDelay: 500
    // Wrap point for long messages — keeps the tip a readable column.
    property int maxWidth: 280

    // Sit below the hovered control so the tip doesn't overlay the row above
    // (e.g. the Send button sitting over the Dry Run / Send Cleanly row).
    y: (parent ? parent.height : 0) + 6
    padding: 0

    onActiveChanged: {
        if (active) {
            showTimer.restart();
        } else {
            showTimer.stop();
            visible = false;
        }
    }

    Timer {
        id: showTimer
        interval: tip.showDelay
        onTriggered: tip.visible = true
    }

    enter: PopupEnter {}
    exit: PopupExit {}

    background: Rectangle {
        radius: DesignTokens.radiusSm
        // Translucent fill so it reads as a light overlay, not a solid block.
        color: Qt.rgba(DesignTokens.surfaceOverlay.r, DesignTokens.surfaceOverlay.g, DesignTokens.surfaceOverlay.b, 0.86)
        border.width: 1
        border.color: Qt.rgba(DesignTokens.borderSubtle.r, DesignTokens.borderSubtle.g, DesignTokens.borderSubtle.b, 0.7)
    }

    contentItem: Text {
        text: tip.text
        color: DesignTokens.textPrimary
        font.pointSize: DesignTokens.fontLabelPointSize
        wrapMode: Text.WordWrap
        width: Math.min(implicitWidth, tip.maxWidth)
        leftPadding: DesignTokens.spaceSm
        rightPadding: DesignTokens.spaceSm
        topPadding: DesignTokens.spaceXs
        bottomPadding: DesignTokens.spaceXs
    }
}
