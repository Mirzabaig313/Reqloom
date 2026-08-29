// BusyBar — a thin indeterminate progress strip for long-running operations
// (a request/chain run): a faint track with an accent segment sweeping across.
// Token-driven; fades in/out with `running`.
import QtQuick
import Reqloom

Item {
    id: root
    property bool running: false

    implicitHeight: 2
    clip: true
    visible: opacity > 0
    opacity: running ? 1 : 0
    Behavior on opacity {
        FadeMotion {}
    }

    // Faint track so the strip reads as a channel even between sweeps.
    Rectangle {
        anchors.fill: parent
        color: DesignTokens.accentMuted
    }

    // The sweeping segment.
    Rectangle {
        id: blip
        height: parent.height
        width: Math.max(48, parent.width * 0.3)
        radius: height / 2
        color: DesignTokens.accent
        x: -width

        NumberAnimation on x {
            running: root.running
            loops: Animation.Infinite
            from: -blip.width
            to: root.width
            duration: 1100
            easing.type: Easing.InOutQuad
        }
    }
}
