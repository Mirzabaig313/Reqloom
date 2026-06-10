// Card — the canonical elevated surface: a near-white, generously rounded
// panel that lifts off the lavender canvas through contrast + radius. Children
// lay out inside as in any Rectangle. (A true drop shadow is a later pass once
// the layout leaves padding room for the blur to render without clipping.)
import QtQuick
import Reqloom

Rectangle {
    radius: DesignTokens.radiusLg
    color: DesignTokens.glassFill
    border.width: 1
    border.color: DesignTokens.glassBorder
}
