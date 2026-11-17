// HostApprovalDialog — asks whether a project may send credentials to a host the person has not
// approved for it (FR-15.9). Opened with AppController.hostApprovalRequested; `answered` reports
// approve = true or false exactly once per opening. Escape, a click outside and the default
// button all mean "Don't send": approving takes a deliberate click.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Dialog {
    id: root
    objectName: "hostApprovalDialog"

    property int requestId: 0
    property string projectName: ""
    property string host: ""
    property string credential: ""
    property string url: ""
    property bool pendingApprove: false

    signal answered(int requestId, bool approve)

    // Questions that arrive while one is open (a run and an OAuth2 token request) wait their turn,
    // so no answer is ever given to the wrong question.
    property var waiting: []

    function ask(id, projectName, host, credential, url) {
        if (root.visible) {
            root.waiting.push([id, projectName, host, credential, url]);
            return;
        }
        root.requestId = id;
        root.projectName = projectName;
        root.host = host;
        root.credential = credential;
        root.url = url;
        root.pendingApprove = false;
        root.open();
    }

    function askNext() {
        if (root.waiting.length > 0 && !root.visible) {
            const next = root.waiting.shift();
            root.ask(next[0], next[1], next[2], next[3], next[4]);
        }
    }

    function answer(approve) {
        root.pendingApprove = approve;
        root.close();
    }

    title: qsTr("Send credentials to this host?")
    modal: true
    enter: PopupEnter {}
    exit: PopupExit {}
    anchors.centerIn: Overlay.overlay
    width: Math.min(520, Overlay.overlay ? Overlay.overlay.width - 64 : 520)
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

    contentItem: ColumnLayout {
        spacing: DesignTokens.spaceSm

        Label {
            objectName: "hostApprovalMessage"
            Layout.fillWidth: true
            text: qsTr("Project “%1” is about to send %2 to:").arg(root.projectName).arg(root.credential)
            textFormat: Text.PlainText
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontLabel
            wrapMode: Text.WordWrap
        }
        Label {
            objectName: "hostApprovalHost"
            Layout.fillWidth: true
            text: root.host
            textFormat: Text.PlainText
            color: DesignTokens.textPrimary
            font.family: DesignTokens.fontMono
            font.bold: true
            wrapMode: Text.WrapAnywhere
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Request: %1").arg(root.url)
            textFormat: Text.PlainText
            color: DesignTokens.textSecondary
            font.family: DesignTokens.fontMono
            font.pixelSize: DesignTokens.fontLabel
            wrapMode: Text.WrapAnywhere
            maximumLineCount: 3
            elide: Text.ElideRight
        }
        Label {
            Layout.fillWidth: true
            Layout.topMargin: DesignTokens.spaceSm
            text: qsTr("This host has not been approved for this project. Approve it only if you expect this project to send credentials there: a project file edited by someone else or by an AI agent can point requests anywhere. Your answer is remembered for this project; you can revoke it in Manage Secrets.")
            textFormat: Text.PlainText
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontLabel
            wrapMode: Text.WordWrap
        }
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

            Item {
                Layout.fillWidth: true
            }
            GlassButton {
                objectName: "hostApprovalDenyButton"
                text: qsTr("Don't send")
                // Focused first, so Space answers no.
                focus: true
                onClicked: root.answer(false)
            }
            GlassButton {
                objectName: "hostApprovalApproveButton"
                text: qsTr("Approve and send")
                primary: true
                onClicked: root.answer(true)
            }
        }
    }

    onClosed: {
        root.answered(root.requestId, root.pendingApprove);
        Qt.callLater(root.askNext);
    }
}
