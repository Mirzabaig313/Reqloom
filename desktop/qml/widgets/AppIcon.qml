// AppIcon — a themeable line icon. Renders a monochrome SVG from qml/icons/ and
// recolours it to `color` (defaults to secondary text) via MultiEffect, so all
// iconography follows DesignTokens. Usage: AppIcon { name: "chevron-left" }.
import QtQuick
import QtQuick.Effects
import Reqloom

Item {
    id: root
    property string name: ""
    property color color: DesignTokens.textSecondary
    property int size: DesignTokens.iconSize

    implicitWidth: size
    implicitHeight: size

    Image {
        id: src
        anchors.fill: parent
        source: root.name.length > 0 ? Qt.resolvedUrl("../icons/" + root.name + ".svg") : ""
        sourceSize.width: root.size * 2
        sourceSize.height: root.size * 2
        fillMode: Image.PreserveAspectFit
        smooth: true
        visible: false
    }

    MultiEffect {
        anchors.fill: parent
        source: src
        colorization: 1.0
        colorizationColor: root.color
    }
}
