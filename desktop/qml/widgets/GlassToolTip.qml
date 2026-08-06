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
    // Wrap point for long messages — keeps the tip a readable column, capped to
    // the window so a long localized message can't grow past the edge.
    property int maxWidth: Math.min(280, Overlay.overlay ? Overlay.overlay.width - DesignTokens.spaceLg * 2 : 280)

    // Gap between the anchor and the tip, both below and above.
    readonly property int anchorGap: 6
    // Room left under the anchor, in overlay coordinates. MAX_VALUE while the
    // tip has no parent/overlay yet so the default below-anchor branch wins.
    readonly property real spaceBelowAnchor: {
        // tip.visible is read on purpose: mapToItem is not reactive to ancestor
        // movement, so this re-measures every time the tip is shown rather than
        // trusting a value captured when the tip was created.
        if (!tip.visible || !tip.parent || !Overlay.overlay) {
            return Number.MAX_VALUE;
        }
        return Overlay.overlay.height - tip.parent.mapToItem(Overlay.overlay, 0, tip.parent.height).y;
    }
    readonly property bool flipAbove: tip.spaceBelowAnchor < tip.implicitHeight + tip.anchorGap + DesignTokens.spaceSm

    // Sit below the hovered control so the tip doesn't overlay the row above
    // (e.g. the Send button sitting over the Dry Run / Send Cleanly row), and
    // flip above only when the window edge leaves no room below.
    y: tip.flipAbove ? -(tip.implicitHeight + tip.anchorGap) : ((tip.parent ? tip.parent.height : 0) + tip.anchorGap)
    // The content Text wraps at maxWidth, but the popup's implicit width comes
    // from the unwrapped text, so cap the surface as well or it renders wider
    // than the message it contains.
    width: Math.min(implicitWidth, tip.maxWidth)
    margins: DesignTokens.spaceSm
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
        leftPadding: DesignTokens.spaceSm
        rightPadding: DesignTokens.spaceSm
        topPadding: DesignTokens.spaceXs
        bottomPadding: DesignTokens.spaceXs
    }
}
