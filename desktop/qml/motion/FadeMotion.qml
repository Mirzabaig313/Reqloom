// FadeMotion — standardised opacity transition. Used where a settle spring
// would be wrong (a fade in/out is not a physical displacement). Tuned to the
// shared DesignTokens.motionFast budget. Usage: `Behavior on opacity { FadeMotion {} }`.
import QtQuick
import Reqloom

NumberAnimation {
    duration: DesignTokens.motionFast
    easing.type: Easing.OutCubic
}
