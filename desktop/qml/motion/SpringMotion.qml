// SpringMotion — the app's one canonical spring, for use inside a Behavior on
// a position/size property (x, y, scale, width…). Reads its tuning from
// DesignTokens so every settling motion shares one stiffness/damping ratio and
// feels physically coherent. Usage: `Behavior on y { SpringMotion {} }`.
import QtQuick
import Reqloom

SpringAnimation {
    spring: DesignTokens.motionSpring
    damping: DesignTokens.motionDamping
    mass: DesignTokens.motionMass
    epsilon: DesignTokens.motionEpsilon
}
