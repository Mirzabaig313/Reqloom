// Toast — stacked feedback notices at the bottom of the window. Informational
// notices dismiss themselves after `duration` ms; error notices stay until the
// user closes them (close button, or Escape while the notice has focus) and
// their text can be selected and copied. The newest `maxShown` are visible and
// the rest are counted. It lives in the overlay above modal dialogs.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Popup {
    id: toast

    property int duration: 3000
    property int maxShown: 3
    // Bounds memory if something notifies in a loop; the oldest go first.
    property int maxQueued: 50
    readonly property int count: entries.count
    readonly property int hiddenCount: Math.max(0, entries.count - toast.maxShown)

    signal dismissed

    parent: Overlay.overlay
    // Above every dialog, including the dimmer of a modal one.
    z: 1000
    modal: false
    focus: false
    closePolicy: Popup.NoAutoClose
    padding: 0
    background: null
    enter: PopupEnter {}
    exit: PopupExit {}
    width: Math.max(0, Math.min(600, (parent ? parent.width : 400) - DesignTokens.spaceLg * 2))
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? parent.height - height - DesignTokens.spaceLg : 0
    visible: entries.count > 0

    property int _nextUid: 1

    ListModel {
        id: entries
    }

    function show(msg, error, dur) {
        const text = String(msg || "");
        const isError = !!error;
        for (let i = entries.count - 1; i >= 0; --i) {
            const e = entries.get(i);
            if (e.message === text && e.isError === isError) {
                entries.remove(i);
                break;
            }
        }
        entries.append({
            "uid": toast._nextUid++,
            "message": text,
            "isError": isError,
            "timeout": dur || toast.duration
        });
        while (entries.count > toast.maxQueued) {
            entries.remove(0);
        }
    }

    function dismiss(uid) {
        for (let i = 0; i < entries.count; ++i) {
            if (entries.get(i).uid === uid) {
                entries.remove(i);
                toast.dismissed();
                return;
            }
        }
    }

    function dismissAll() {
        entries.clear();
        toast.dismissed();
    }

    contentItem: ColumnLayout {
        spacing: DesignTokens.spaceSm

        RowLayout {
            visible: toast.hiddenCount > 0
            Layout.fillWidth: true
            spacing: DesignTokens.spaceSm
            Label {
                Layout.fillWidth: true
                text: qsTr("%n more notification(s)", "", toast.hiddenCount)
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontCaption
                horizontalAlignment: Text.AlignRight
            }
            Button {
                id: clearAllBtn
                text: qsTr("Dismiss all")
                onClicked: toast.dismissAll()
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: clearAllBtn.down ? DesignTokens.surfaceSunken : DesignTokens.surfaceRaised
                    border.width: 1
                    border.color: DesignTokens.borderSubtle
                }
                contentItem: Text {
                    text: clearAllBtn.text
                    color: DesignTokens.textPrimary
                    font.pixelSize: DesignTokens.fontCaption
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        Repeater {
            model: entries
            delegate: Rectangle {
                id: entry
                required property int index
                required property int uid
                required property string message
                required property bool isError
                required property int timeout
                readonly property bool shown: entry.index >= entries.count - toast.maxShown

                visible: entry.shown
                Layout.fillWidth: true
                implicitHeight: body.implicitHeight + DesignTokens.spaceMd * 2
                radius: DesignTokens.radiusSm
                color: entry.isError ? Qt.rgba(DesignTokens.statusError.r, DesignTokens.statusError.g, DesignTokens.statusError.b, 0.15) : DesignTokens.surfaceSunken
                border.width: 1
                border.color: entry.isError ? DesignTokens.statusError : DesignTokens.borderStrong
                Accessible.role: entry.isError ? Accessible.AlertMessage : Accessible.StaticText
                Accessible.name: entry.message

                // Only a notice the user can see counts down.
                Timer {
                    interval: entry.timeout
                    running: !entry.isError && entry.shown
                    onTriggered: toast.dismiss(entry.uid)
                }

                RowLayout {
                    id: body
                    anchors.fill: parent
                    anchors.margins: DesignTokens.spaceMd
                    spacing: DesignTokens.spaceSm

                    Label {
                        visible: !entry.isError
                        Layout.fillWidth: true
                        text: entry.message
                        color: DesignTokens.textPrimary
                        font.pixelSize: DesignTokens.fontBody
                        wrapMode: Text.Wrap
                        horizontalAlignment: Text.AlignHCenter
                    }

                    ScrollView {
                        visible: entry.isError
                        Layout.fillWidth: true
                        Layout.preferredHeight: entry.isError ? Math.min(errorText.implicitHeight, 200) : 0
                        clip: true
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                        TextArea {
                            id: errorText
                            text: entry.message
                            readOnly: true
                            selectByMouse: true
                            persistentSelection: true
                            textFormat: TextEdit.PlainText
                            wrapMode: TextEdit.Wrap
                            color: DesignTokens.statusError
                            selectionColor: DesignTokens.accentMuted
                            selectedTextColor: DesignTokens.textPrimary
                            font.pixelSize: DesignTokens.fontBody
                            padding: 0
                            background: null
                            Keys.onEscapePressed: toast.dismiss(entry.uid)
                        }
                    }

                    Button {
                        id: copyBtn
                        visible: entry.isError
                        Layout.alignment: Qt.AlignTop
                        text: qsTr("Copy")
                        onClicked: {
                            errorText.selectAll();
                            errorText.copy();
                            errorText.deselect();
                        }
                        Keys.onEscapePressed: toast.dismiss(entry.uid)
                        background: Rectangle {
                            radius: DesignTokens.radiusSm
                            color: copyBtn.down ? DesignTokens.surfaceSunken : "transparent"
                            border.width: 1
                            border.color: DesignTokens.borderSubtle
                        }
                        contentItem: Text {
                            text: copyBtn.text
                            color: DesignTokens.textPrimary
                            font.pixelSize: DesignTokens.fontCaption
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }

                    ToolButton {
                        id: closeBtn
                        visible: entry.isError
                        Layout.alignment: Qt.AlignTop
                        Accessible.name: qsTr("Dismiss")
                        onClicked: toast.dismiss(entry.uid)
                        Keys.onEscapePressed: toast.dismiss(entry.uid)
                        contentItem: AppIcon {
                            name: "x"
                            size: 14
                            color: DesignTokens.statusError
                        }
                        background: Rectangle {
                            radius: DesignTokens.radiusSm
                            color: closeBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                        }
                    }
                }
            }
        }
    }
}
