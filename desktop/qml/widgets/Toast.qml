// Toast — a transient feedback overlay (mirrors the old Widgets Toast widget).
// Shown at the bottom of the window; auto-dismisses after `duration` ms.
// Error toasts tint red; regular toasts use the overlay surface.
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

Rectangle {
    id: toast

    property string message: ""
    property bool isError: false
    property int duration: 3000

    signal dismissed()

    visible: false
    width: Math.min(600, parent ? parent.width * 0.6 : 400)
    height: label.implicitHeight + DesignTokens.spaceMd * 2
    radius: DesignTokens.radiusSm
    color: toast.isError
           ? Qt.rgba(DesignTokens.statusError.r, DesignTokens.statusError.g,
                     DesignTokens.statusError.b, 0.15)
           : DesignTokens.surfaceSunken
    border.width: 1
    border.color: toast.isError ? DesignTokens.statusError : DesignTokens.borderStrong

    Text {
        id: label
        anchors.fill: parent
        anchors.margins: DesignTokens.spaceMd
        text: toast.message
        color: toast.isError ? DesignTokens.statusError : DesignTokens.textPrimary
        font.pixelSize: DesignTokens.fontBody
        wrapMode: Text.WordWrap
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: Text.AlignHCenter
    }

    function show(msg, error, dur) {
        message = msg
        isError = !!error
        duration = dur || 3000
        visible = true
        hideTimer.restart()
    }

    Timer {
        id: hideTimer
        interval: toast.duration
        onTriggered: {
            toast.visible = false
            toast.dismissed()
        }
    }
}
