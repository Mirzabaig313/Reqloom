// EmptyState — a teaching zero-state screen (no project open or no operation
// selected). Mirrors the old Widgets EmptyState (DESIGN.md §10).
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

ColumnLayout {
    id: root
    property string iconName: "zap"
    // When true, show the Reqloom brand mark instead of the line icon — used
    // on the top-level zero state so the app's first screen is branded.
    property bool useBrandLogo: false
    property string heading: qsTr("No project open")
    property string body: qsTr("Open a project to start building and running API chains.")
    // Optional primary action. When actionText is set, a button is shown and
    // clicking it emits actionTriggered — saves the user hunting the toolbar.
    property string actionText: ""
    signal actionTriggered

    anchors.centerIn: parent
    spacing: DesignTokens.spaceMd
    // Cap at a comfortable reading width but shrink to fit narrow panels
    // (the response/timeline pane can be dragged down to ~200px).
    width: Math.min(320, (parent ? parent.width : 320) - DesignTokens.spaceXl * 2)

    AppIcon {
        Layout.alignment: Qt.AlignHCenter
        visible: !root.useBrandLogo
        name: root.iconName
        size: 40
    }
    // Loader, not a plain BrandLogo: a hidden Image still decodes its source,
    // and EmptyState is reused on several screens that show a small line icon.
    // active gating avoids decoding the multi-MB brand PNG where it isn't shown.
    Loader {
        Layout.alignment: Qt.AlignHCenter
        active: root.useBrandLogo
        sourceComponent: BrandLogo {
            size: 88
        }
    }
    Text {
        Layout.alignment: Qt.AlignHCenter
        text: root.heading
        color: DesignTokens.textPrimary
        font.pixelSize: DesignTokens.fontSubtitle
        font.weight: DesignTokens.weightSemiBold
        horizontalAlignment: Text.AlignHCenter
    }
    Text {
        Layout.fillWidth: true
        text: root.body
        color: DesignTokens.textSecondary
        font.pixelSize: DesignTokens.fontBody
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
    }
    Button {
        id: actionBtn
        visible: root.actionText.length > 0
        Layout.alignment: Qt.AlignHCenter
        Layout.topMargin: DesignTokens.spaceSm
        implicitHeight: 34
        leftPadding: DesignTokens.spaceLg
        rightPadding: DesignTokens.spaceLg
        onClicked: root.actionTriggered()
        background: Rectangle {
            radius: DesignTokens.radiusSm
            color: actionBtn.down ? DesignTokens.accentHover : DesignTokens.accent
        }
        contentItem: Text {
            text: root.actionText
            color: DesignTokens.textInverse
            font.pixelSize: DesignTokens.fontBody
            font.weight: DesignTokens.weightSemiBold
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }
}
