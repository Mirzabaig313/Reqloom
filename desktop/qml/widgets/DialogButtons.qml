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
    // When true the action button is red-filled (delete/remove confirmations).
    property bool okDestructive: false
    property bool showCancel: true
    signal accepted
    signal rejected

    // Stack the actions once both labels no longer fit one line, which happens
    // in a narrow window or at large OS text sizes. The one-line form spends
    // columnSpacing twice (gutter to Cancel, Cancel to action), so budget both
    // or the buttons compress instead of stacking in a narrow band of widths.
    readonly property bool stacked: root.showCancel && root.width > 0 && root.width - DesignTokens.spaceLg * 2 < cancelBtn.implicitWidth + okBtn.implicitWidth + DesignTokens.spaceSm * 2

    implicitHeight: Math.max(60, buttons.implicitHeight + DesignTokens.spaceLg * 2)

    // Cells are assigned explicitly rather than relying on declaration order:
    // that keeps the actions against the trailing edge (primary on top when
    // stacked) while Cancel stays first in the focus chain. On one line that
    // matches the left-to-right reading order; stacked, the primary is on top
    // by convention, so visual and focus order intentionally differ there.
    GridLayout {
        id: buttons
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: DesignTokens.spaceLg
        columns: root.stacked ? 2 : 3
        columnSpacing: DesignTokens.spaceSm
        rowSpacing: DesignTokens.spaceSm

        // Flexible gutter, so the actions sit at the trailing edge.
        Item {
            Layout.row: 0
            Layout.column: 0
            Layout.rowSpan: root.stacked ? 2 : 1
            Layout.fillWidth: true
        }
        GlassButton {
            id: cancelBtn
            visible: root.showCancel
            Layout.row: root.stacked ? 1 : 0
            Layout.column: 1
            text: root.cancelText
            onClicked: root.rejected()
            // An irreversible action is the wrong keyboard default, so Cancel
            // takes initial focus and a stray Space can't destroy anything.
            focus: root.okDestructive && root.showCancel
        }
        GlassButton {
            id: okBtn
            Layout.row: 0
            Layout.column: root.stacked ? 1 : 2
            text: root.okText
            destructive: root.okDestructive
            // Destructive takes its own fill; don't also mark it primary.
            primary: root.okPrimary && !root.okDestructive
            enabled: root.okEnabled
            onClicked: root.accepted()
        }
    }
}
