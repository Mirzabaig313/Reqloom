// DialogButtons — a themed dialog footer: a right-aligned Cancel + primary
// action built from GlassButton. Replaces the default grey DialogButtonBox.
import QtQuick
import QtQuick.Layouts
import Reqloom

Item {
    id: root
    property string cancelText: qsTr("Cancel")
    property string okText: qsTr("OK")
    property bool okEnabled: true
    property bool okPrimary: true
    property bool showCancel: true
    signal accepted
    signal rejected

    implicitHeight: 60

    RowLayout {
        anchors.fill: parent
        anchors.margins: DesignTokens.spaceLg
        spacing: DesignTokens.spaceSm
        Item {
            Layout.fillWidth: true
        }
        GlassButton {
            visible: root.showCancel
            text: root.cancelText
            onClicked: root.rejected()
        }
        GlassButton {
            text: root.okText
            primary: root.okPrimary
            enabled: root.okEnabled
            onClicked: root.accepted()
        }
    }
}
