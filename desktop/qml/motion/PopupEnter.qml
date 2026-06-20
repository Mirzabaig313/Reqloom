// PopupEnter — the app's standard popup/dialog entrance: a quick fade + a
// small scale-up so overlays feel like they grow into place. Use as
// `enter: PopupEnter {}` on any Popup, Menu, or Dialog. Tuned to the shared
// DesignTokens.motionFast budget so every overlay opens with one feel.
import QtQuick
import Reqloom

Transition {
    NumberAnimation {
        property: "opacity"
        from: 0.0
        to: 1.0
        duration: DesignTokens.motionFast
        easing.type: Easing.OutCubic
    }
    NumberAnimation {
        property: "scale"
        from: 0.96
        to: 1.0
        duration: DesignTokens.motionFast
        easing.type: Easing.OutCubic
    }
}
