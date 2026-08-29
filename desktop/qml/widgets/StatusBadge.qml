// Compact run-status instrument label. QML Migration Roadmap WS-C.
// Semantic color is paired with a glyph and optional text, never used alone.
import QtQuick
import Reqloom

Rectangle {
    id: badge

    // Status vocabulary token: "running", "success", "warning", "error",
    // "skipped", "cancelled", "blocked", "idle", "neutral".
    required property string token
    // Optional trailing label (e.g. "HTTP 200"). Empty means glyph-only.
    property string label: ""
    readonly property color hue: ({
            "running": DesignTokens.statusRunning,
            "success": DesignTokens.statusSuccess,
            "warning": DesignTokens.statusWarning,
            "error": DesignTokens.statusError,
            "cancelled": DesignTokens.statusCancelled,
            "blocked": DesignTokens.statusBlocked,
            "skipped": DesignTokens.statusSkipped,
            "idle": DesignTokens.statusIdle,
            "neutral": DesignTokens.statusIdle
        })[token] || DesignTokens.statusIdle
    readonly property string statusName: ({
            "running": qsTr("Running"),
            "success": qsTr("Success"),
            "warning": qsTr("Warning"),
            "error": qsTr("Error"),
            "cancelled": qsTr("Cancelled"),
            "blocked": qsTr("Blocked"),
            "skipped": qsTr("Skipped"),
            "idle": qsTr("Idle"),
            "neutral": qsTr("Neutral")
        })[token] || qsTr("Idle")

    implicitWidth: row.implicitWidth + DesignTokens.spaceXs * 2
    implicitHeight: Math.max(20, row.implicitHeight + DesignTokens.spaceXs * 2)
    radius: DesignTokens.radiusSm
    color: DesignTokens.surfaceSunken
    border.width: 1
    border.color: DesignTokens.borderSubtle

    Accessible.role: Accessible.StaticText
    Accessible.name: label.trim().length > 0 ? label.trim() : statusName
    Accessible.description: {
        const detail = label.trim();
        return detail.length > 0 && detail.toLocaleLowerCase() !== statusName.toLocaleLowerCase() ? statusName : "";
    }

    Row {
        id: row

        anchors.centerIn: parent
        spacing: DesignTokens.spaceXs

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: DesignTokens.statusGlyph(badge.token)
            color: badge.hue
            font.pointSize: DesignTokens.fontCaptionPointSize
            font.weight: DesignTokens.weightBold
            Accessible.ignored: true
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: badge.label.length > 0
            text: badge.label
            color: DesignTokens.textPrimary
            font.pointSize: DesignTokens.fontCaptionPointSize
            font.family: DesignTokens.fontMono
            font.weight: DesignTokens.weightSemiBold
            Accessible.ignored: true
        }
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        width: Math.max(0, Math.min(12, parent.width - DesignTokens.spaceXs * 2))
        height: 2
        radius: 1
        color: badge.hue
        Accessible.ignored: true
        Behavior on color {
            ColorMotion {}
        }
    }
}
