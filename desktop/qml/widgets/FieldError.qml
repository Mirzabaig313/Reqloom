// FieldError — the canonical inline validation message shown beneath a field.
// A small error-coloured caption that hides itself when `text` is empty, so a
// form can bind it directly to a validation string. Pairs with
// GlassTextField's `error` state.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Label {
    Layout.fillWidth: true
    visible: text.length > 0
    color: DesignTokens.statusError
    font.pointSize: DesignTokens.fontLabelPointSize
    wrapMode: Text.WordWrap
}
