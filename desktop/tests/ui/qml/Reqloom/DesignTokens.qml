// Test stub of the DesignTokens singleton: only the tokens the widgets under
// test read, with fixed values.
pragma Singleton
import QtQuick

QtObject {
    readonly property color accent: "#2bb3c0"
    readonly property color accentHover: "#3cc4d1"
    readonly property color accentMuted: "#203a3d"
    readonly property color borderStrong: "#555555"
    readonly property color borderSubtle: "#333333"
    readonly property color glassBorder: "#444444"
    readonly property color statusError: "#e5484d"
    readonly property color statusWarning: "#f5a524"
    readonly property color surfaceRaised: "#222222"
    readonly property color surfaceSunken: "#111111"
    readonly property color textPrimary: "#eeeeee"
    readonly property color textSecondary: "#aaaaaa"
    readonly property color textInverse: "#000000"
    readonly property int controlHeightLg: 32
    readonly property real fontBodyPointSize: 13
    readonly property real fontSubtitlePointSize: 15
    readonly property string fontSans: "Helvetica"
    readonly property int weightSemiBold: Font.DemiBold
    readonly property int radiusLg: 8
    readonly property int fontBody: 13
    readonly property int fontCaption: 11
    readonly property int fontLabel: 12
    readonly property string fontMono: "Menlo"
    readonly property int iconSize: 16
    readonly property int motionFast: 0
    readonly property int radiusSm: 4
    readonly property int spaceXs: 4
    readonly property int spaceSm: 8
    readonly property int spaceMd: 12
    readonly property int spaceLg: 16
}
