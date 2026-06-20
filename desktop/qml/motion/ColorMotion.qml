// ColorMotion — standardised colour transition (hover, theme, status tint). A
// spring doesn't apply to colour, so this is a short eased fade tuned to the
// same DesignTokens.motionFast budget the rest of the app uses. Usage:
// `Behavior on color { ColorMotion {} }`.
import QtQuick
import Reqloom

ColorAnimation {
    duration: DesignTokens.motionFast
    easing.type: Easing.OutCubic
}
