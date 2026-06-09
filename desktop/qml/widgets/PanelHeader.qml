// PanelHeader — a consistent header strip for the workbench panels (mirrors the
// old widgets::PanelHeader): a title with an optional subtitle line and a
// trailing actions slot. Token-driven. Place trailing controls as children;
// they are laid into the right-hand actions row.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

RowLayout {
    id: root

    property string title: ""
    property string subtitle: ""
    default property alias trailing: actions.data

    spacing: DesignTokens.spaceSm

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 0

        Label {
            text: root.title
            color: DesignTokens.textPrimary
            font.pixelSize: 15
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        Label {
            text: root.subtitle
            visible: root.subtitle.length > 0
            color: DesignTokens.textSecondary
            font.pixelSize: 11
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
    }

    RowLayout {
        id: actions
        spacing: DesignTokens.spaceXs
    }
}
