// UnsavedChangesDialog — asks whether to save the unsaved edits an action is
// about to drop (closing tabs, leaving a project, quitting). Opened with the
// texts of AppController.unsavedChangesPrompt; `choice` reports 0 = save,
// 1 = discard, 2 = cancel exactly once per opening.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Dialog {
    id: root
    objectName: "unsavedChangesDialog"

    property string message: ""
    property string saveText: qsTr("Save")
    property string discardText: qsTr("Discard")
    property string cancelText: qsTr("Cancel")
    // Closing by Escape or a click outside counts as Cancel.
    property int pendingChoice: 2

    signal choice(int value)

    function ask(title, message, saveText, discardText, cancelText) {
        root.title = title;
        root.message = message;
        root.saveText = saveText;
        root.discardText = discardText;
        root.cancelText = cancelText;
        root.pendingChoice = 2;
        root.open();
    }

    function answer(value) {
        root.pendingChoice = value;
        root.close();
    }

    modal: true
    enter: PopupEnter {}
    exit: PopupExit {}
    anchors.centerIn: Overlay.overlay
    width: Math.min(480, Overlay.overlay ? Overlay.overlay.width - 64 : 480)
    height: Math.min(implicitHeight, Overlay.overlay ? Overlay.overlay.height - 64 : implicitHeight)
    padding: DesignTokens.spaceLg
    focus: true

    header: DialogHeader {
        title: root.title
    }

    background: Rectangle {
        radius: DesignTokens.radiusLg
        color: DesignTokens.surfaceRaised
        border.width: 1
        border.color: DesignTokens.glassBorder
    }

    contentItem: Label {
        objectName: "unsavedChangesMessage"
        text: root.message
        textFormat: Text.PlainText
        color: DesignTokens.textSecondary
        font.pixelSize: DesignTokens.fontLabel
        wrapMode: Text.WordWrap
    }

    footer: Item {
        implicitHeight: Math.max(60, buttons.implicitHeight + DesignTokens.spaceLg * 2)

        RowLayout {
            id: buttons
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: DesignTokens.spaceLg
            spacing: DesignTokens.spaceSm

            GlassButton {
                objectName: "unsavedDiscardButton"
                text: root.discardText
                destructive: true
                onClicked: root.answer(1)
            }
            Item {
                Layout.fillWidth: true
            }
            GlassButton {
                objectName: "unsavedCancelButton"
                text: root.cancelText
                onClicked: root.answer(2)
            }
            GlassButton {
                objectName: "unsavedSaveButton"
                text: root.saveText
                primary: true
                // Save is the safe default for Enter/Space.
                focus: true
                onClicked: root.answer(0)
            }
        }
    }

    onClosed: root.choice(root.pendingChoice)
}
