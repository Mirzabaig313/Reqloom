// GlassComboBox — the app's themed select. A rounded glass field with a chevron
// indicator and a rounded, elevated popup whose rows highlight on hover. Drop-in
// for QtQuick.Controls ComboBox (same model / currentIndex / textRole API).
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

ComboBox {
    id: control
    implicitHeight: DesignTokens.controlHeight
    padding: 0
    // Reserve room for the chevron via `spacing` (ComboBox lays the content out
    // to the left of the indicator) rather than padding the text, so the text
    // gets the full width up to the chevron and short values don't elide.
    spacing: DesignTokens.spaceSm
    font.pixelSize: DesignTokens.fontLabel
    font.family: DesignTokens.fontSans

    background: Rectangle {
        radius: DesignTokens.radiusSm
        color: DesignTokens.surfaceSunken
        border.width: 1
        border.color: control.activeFocus ? DesignTokens.accent : DesignTokens.borderSubtle
    }

    contentItem: Text {
        leftPadding: DesignTokens.spaceSm
        rightPadding: DesignTokens.spaceSm
        text: control.displayText
        // A leading "+" marks a placeholder row ("+ add dependency…").
        color: control.displayText.startsWith("+") ? DesignTokens.textSecondary : DesignTokens.textPrimary
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: AppIcon {
        name: "chevron-down"
        size: 14
        x: control.width - width - DesignTokens.spaceSm
        y: (control.height - height) / 2
    }

    popup: Popup {
        y: control.height + 4
        width: control.width
        implicitHeight: Math.min(popupList.contentHeight + 8, 340)
        padding: 4

        background: Rectangle {
            radius: DesignTokens.radius
            color: DesignTokens.surfaceOverlay
            border.width: 1
            border.color: DesignTokens.borderSubtle
        }

        contentItem: ListView {
            id: popupList
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollBar.vertical: ScrollBar {}
        }
    }

    delegate: ItemDelegate {
        id: row
        width: ListView.view ? ListView.view.width : control.width
        implicitHeight: DesignTokens.controlHeight
        padding: 0
        highlighted: control.highlightedIndex === index
        contentItem: Text {
            leftPadding: DesignTokens.spaceSm
            text: control.textRole && control.textRole.length > 0 ? model[control.textRole] : modelData
            color: DesignTokens.textPrimary
            font.pixelSize: DesignTokens.fontLabel
            font.family: DesignTokens.fontSans
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: DesignTokens.radiusSm
            color: row.highlighted ? DesignTokens.accentMuted : "transparent"
        }
    }
}
