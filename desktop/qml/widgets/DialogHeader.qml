// DialogHeader — the themed title strip for dialogs, replacing the default
// (black) Dialog header bar. Use as `header: DialogHeader { title: "…" }`.
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

Label {
    property string title: ""
    text: title
    color: DesignTokens.textPrimary
    font.pixelSize: DesignTokens.fontSubtitle
    font.weight: DesignTokens.weightSemiBold
    padding: DesignTokens.spaceLg
    bottomPadding: 0
}
