// SectionLabel — the canonical "overline" section heading (tracked, muted,
// semibold caption). Single-sources the recipe that was duplicated inline
// across panels so every section header reads identically. See
// doc/local/UI_improment.md §1 (Visual Hierarchy & Spacing).
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

Label {
    color: DesignTokens.textSecondary
    font.pointSize: DesignTokens.fontCaptionPointSize
    font.weight: DesignTokens.weightSemiBold
    font.letterSpacing: 1.2
    elide: Text.ElideRight
}
