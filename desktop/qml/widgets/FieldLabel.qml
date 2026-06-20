// FieldLabel — the standard secondary caption used beside form fields and as
// section labels. Single-sources the "muted label" look (colour + size).
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

Label {
    color: DesignTokens.textSecondary
    font.pixelSize: DesignTokens.fontLabel
}
