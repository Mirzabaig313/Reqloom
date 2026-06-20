// EnvironmentSelector — Apidog-style environment picker: a single pill showing
// a coloured initials badge + the active environment name + a chevron. Clicking
// it opens a dropdown that lists the environments and folds create / edit /
// delete into a footer (no separate "+" button). Selection + delete go straight
// to AppController; new/edit raise signals so the host can open the dialog.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Item {
    id: sel

    signal newRequested
    signal manageRequested

    implicitHeight: DesignTokens.controlHeightLg
    implicitWidth: Math.max(180, row.implicitWidth + DesignTokens.spaceMd * 2)

    readonly property string current: AppController.environment

    function initials(s) {
        const t = (s || "").trim();
        return t.length === 0 ? "··" : t.substring(0, 2);
    }
    // Deterministic hue from the name so each env keeps a stable badge colour.
    function badgeHue(s) {
        let h = 0;
        for (let i = 0; i < s.length; ++i) {
            h = (h * 31 + s.charCodeAt(i)) >>> 0;
        }
        return Qt.hsla((h % 360) / 360, 0.55, DesignTokens.isDark ? 0.62 : 0.5, 1.0);
    }

    Rectangle {
        id: pill
        anchors.fill: parent
        radius: DesignTokens.radiusSm
        color: pillHover.hovered ? DesignTokens.surfaceOverlay : DesignTokens.surfaceSunken
        border.width: 1
        border.color: menu.visible ? DesignTokens.accent : DesignTokens.borderSubtle

        RowLayout {
            id: row
            anchors.fill: parent
            anchors.leftMargin: DesignTokens.spaceSm
            anchors.rightMargin: DesignTokens.spaceSm
            spacing: DesignTokens.spaceSm

            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                visible: sel.current.length > 0
                implicitWidth: 22
                implicitHeight: 22
                radius: DesignTokens.radiusSm
                color: Qt.rgba(sel.badgeHue(sel.current).r, sel.badgeHue(sel.current).g, sel.badgeHue(sel.current).b, 0.18)
                Text {
                    anchors.centerIn: parent
                    text: sel.initials(sel.current)
                    color: sel.badgeHue(sel.current)
                    font.pixelSize: DesignTokens.fontCaption
                    font.weight: DesignTokens.weightBold
                    font.capitalization: Font.Capitalize
                }
            }
            Label {
                Layout.fillWidth: true
                text: sel.current.length > 0 ? sel.current : qsTr("No environment")
                color: sel.current.length > 0 ? DesignTokens.textPrimary : DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontBody
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
            AppIcon {
                name: "chevron-down"
                size: 14
                color: DesignTokens.textSecondary
            }
        }

        HoverHandler {
            id: pillHover
        }
        TapHandler {
            onTapped: menu.open()
        }
    }

    GlassMenu {
        id: menu
        y: sel.height + 4
        width: Math.max(sel.width, 220)

        // Environment rows (dynamic) — inserted before the footer items.
        Instantiator {
            model: AppController.environments
            delegate: GlassMenuItem {
                required property string modelData
                text: modelData
                onTriggered: AppController.environment = modelData
            }
            onObjectAdded: (index, object) => menu.insertItem(index, object)
            onObjectRemoved: (index, object) => menu.removeItem(object)
        }

        MenuSeparator {}

        GlassMenuItem {
            text: qsTr("New Environment…")
            onTriggered: sel.newRequested()
        }
        GlassMenuItem {
            text: qsTr("Manage Environments…")
            onTriggered: sel.manageRequested()
        }
    }
}
