// BrandLogo — the Reqloom brand mark (circuit-bonsai). Picks the transparent
// mark that stays legible on the current theme: dark UI → white tree,
// light UI → dark tree. The file color names the tree, not the background.
import QtQuick
import Reqloom

Image {
    id: root
    property int size: 96

    implicitWidth: size
    implicitHeight: size
    fillMode: Image.PreserveAspectFit
    smooth: true
    mipmap: true
    asynchronous: true
    source: ThemeController.isDark ? Qt.resolvedUrl("../../branding/Reqloom-mark-light.png") : Qt.resolvedUrl("../../branding/Reqloom-mark-dark.png")
}
