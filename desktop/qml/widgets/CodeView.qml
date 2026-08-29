// CodeView — read-only, scrollable, mouse-selectable code block with syntax
// highlighting driven by a BodyHighlighter. The `language` token (json, xml,
// html, yaml, javascript, markdown, text) selects the colouring rules; colours
// come from DesignTokens so they track the theme. Reusable wherever a body or
// snippet should be shown coloured + copyable (response body, request preview).
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

Rectangle {
    id: view

    property string text: ""
    property string language: "text"
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
            text: view.text.length > 0 ? view.text : view.placeholder
            color: view.text.length > 0 ? DesignTokens.textPrimary : DesignTokens.textSecondary
            selectionColor: DesignTokens.accent
            selectedTextColor: DesignTokens.textInverse
            font.pixelSize: DesignTokens.fontLabel
            font.family: DesignTokens.fontMono
            padding: 0
            background: null

            // Plain placeholder text must not be syntax-coloured, so the
            // highlighter only engages once there's real content.
            BodyHighlighter {
                document: area.textDocument
                language: view.text.length > 0 ? view.language : "text"
                propertyColor: DesignTokens.accent
                stringColor: DesignTokens.statusSuccess
                numberColor: DesignTokens.statusWarning
                keywordColor: DesignTokens.methodDelete
                commentColor: DesignTokens.textSecondary
                punctuationColor: DesignTokens.textSecondary
                tagColor: DesignTokens.accent
            }
        }
    }
}
