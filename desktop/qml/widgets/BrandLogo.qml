// BrandLogo — the Reqloom brand mark (circuit-bonsai). Picks the transparent
// mark that stays legible on the current theme: dark UI → white tree,
// light UI → dark tree. The file color names the tree, not the background.
//
// Root is an Item (not the Image directly): Image.implicitWidth/Height are
// read-only — derived from the source pixels — so the sizing contract that
// layouts depend on is exposed here and the Image just fills it.
import QtQuick
import Reqloom

Item {
    id: root
    property int size: 96

    implicitWidth: size
    implicitHeight: size

    Image {
        anchors.fill: parent
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true
        asynchronous: true
        // Decode at display size — the brand PNGs are large.
        sourceSize.width: root.size
        sourceSize.height: root.size
        source: ThemeController.isDark ? Qt.resolvedUrl("../../branding/Reqloom-mark-light.png") : Qt.resolvedUrl("../../branding/Reqloom-mark-dark.png")
    }
}
