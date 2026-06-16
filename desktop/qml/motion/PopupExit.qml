// PopupExit — the app's standard popup/dialog dismissal: a quick fade-out. Use
// as `exit: PopupExit {}` on any Popup, Menu, or Dialog. Pairs with PopupEnter.
import QtQuick
import Reqloom

Transition {
    NumberAnimation {
        property: "opacity"
        from: 1.0
        to: 0.0
        duration: DesignTokens.motionFast
        easing.type: Easing.InCubic
    }
}
