// NewModuleDialog — create a new module (engine resource) from the QML UI
// (mirrors the old NewModuleDialog). Name + optional description with live
// validation (non-empty, no id-breaking '.', '/', '\\'); OK disabled until
// valid; shows the resources/<name>.yaml file hint. Authoritative validation
// (and duplicate rejection) happens in AppController.createResource.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Dialog {
    id: dialog
    title: qsTr("New Module")
    modal: true
    enter: PopupEnter {}
    exit: PopupExit {}
    anchors.centerIn: Overlay.overlay
    width: 460
    padding: DesignTokens.spaceLg

    function openDialog() {
        nameField.text = "";
        descField.text = "";
        revalidate();
        open();
        nameField.forceActiveFocus();
    }

    property string errorText: ""
    readonly property bool nameValid: AppController.isValidName(nameField.text)

    function revalidate() {
        const name = nameField.text.trim();
        if (name.length > 0 && !AppController.isValidName(name)) {
            errorText = qsTr("Name can't contain '.', '/', or '\\'.");
        } else {
            errorText = "";
        }
    }

    background: Rectangle {
        radius: DesignTokens.radiusLg
        color: DesignTokens.surfaceRaised
        border.width: 1
        border.color: DesignTokens.glassBorder
    }

    header: DialogHeader {
        title: qsTr("New module")
    }

    contentItem: ColumnLayout {
        spacing: DesignTokens.spaceMd

        Label {
            Layout.fillWidth: true
            text: qsTr("A module groups related endpoints (for example admin_organization). " + "It is saved as resources/<name>.yaml.")
            wrapMode: Text.WordWrap
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontLabel
        }

        GridLayout {
            columns: 2
            columnSpacing: DesignTokens.spaceMd
            rowSpacing: DesignTokens.spaceSm
            Layout.fillWidth: true

            FieldLabel {
                text: qsTr("Name")
            }
            GlassTextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: qsTr("admin_organization")
                error: dialog.errorText.length > 0
                onTextChanged: dialog.revalidate()
            }

            FieldLabel {
                text: qsTr("Description")
            }
            GlassTextField {
                id: descField
                Layout.fillWidth: true
                placeholderText: qsTr("Org-level admin actions (optional)")
            }
        }

        Label {
            Layout.fillWidth: true
            visible: dialog.errorText.length === 0
            text: dialog.nameValid ? qsTr("Creates resources/%1.yaml").arg(nameField.text.trim()) : qsTr("Enter a name to create the module.")
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontLabel
            wrapMode: Text.WordWrap
        }
        FieldError {
            text: dialog.errorText
        }
    }

    footer: Item {
        implicitHeight: 60
        RowLayout {
            anchors.fill: parent
            anchors.margins: DesignTokens.spaceLg
            spacing: DesignTokens.spaceSm
            Item {
                Layout.fillWidth: true
            }
            GlassButton {
                text: qsTr("Cancel")
                onClicked: dialog.reject()
            }
            GlassButton {
                text: qsTr("Create module")
                primary: true
                enabled: dialog.nameValid
                onClicked: dialog.accept()
            }
        }
    }

    onAccepted: AppController.createResource(nameField.text.trim(), descField.text.trim())
}
