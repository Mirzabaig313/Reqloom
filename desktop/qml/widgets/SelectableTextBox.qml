// SelectableTextBox — read-only, scrollable, mouse-selectable monospace text
// block in a sunken card. Used for the response Body (Raw) / Headers views and
// the request body preview so all of them are copyable (drag to select, then
// Cmd/Ctrl+C) and look identical. Reusable: restyle once, applies everywhere.
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

Rectangle {
    id: box

    property string text: ""
    property string placeholder: ""

    radius: DesignTokens.radiusSm
    color: DesignTokens.surfaceSunken
    border.width: 1
    border.color: DesignTokens.borderSubtle

    ScrollView {
        anchors.fill: parent
        anchors.margins: DesignTokens.spaceSm
        clip: true

        TextArea {
            id: area
            readOnly: true
            selectByMouse: true
            persistentSelection: true
            textFormat: TextEdit.PlainText
            wrapMode: TextArea.NoWrap
            text: box.text.length > 0 ? box.text : box.placeholder
            color: box.text.length > 0 ? DesignTokens.textPrimary : DesignTokens.textSecondary
            selectionColor: DesignTokens.accent
            selectedTextColor: DesignTokens.textInverse
            font.pixelSize: DesignTokens.fontLabel
            font.family: DesignTokens.fontMono
            padding: 0
            background: null
        }
    }
}
