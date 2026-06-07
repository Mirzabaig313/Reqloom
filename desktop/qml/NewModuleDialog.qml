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
    anchors.centerIn: Overlay.overlay
    width: 460
    padding: DesignTokens.spaceLg

    function openDialog() {
        nameField.text = ""
        descField.text = ""
        revalidate()
        open()
        nameField.forceActiveFocus()
    }

    property string errorText: ""
    readonly property bool nameValid: AppController.isValidName(nameField.text)

    function revalidate() {
        const name = nameField.text.trim()
        if (name.length > 0 && !AppController.isValidName(name)) {
            errorText = qsTr("Name can't contain '.', '/', or '\\'.")
        } else {
            errorText = ""
        }
        const okBtn = footerButtons.standardButton(Dialog.Ok)
        if (okBtn) {
            okBtn.enabled = nameValid
        }
    }

    background: Rectangle {
        radius: DesignTokens.radius
        color: DesignTokens.surfaceRaised
        border.width: 1
        border.color: DesignTokens.borderSubtle
    }

    header: Label {
        text: qsTr("New module")
        color: DesignTokens.textPrimary
        font.pixelSize: 17
        font.weight: Font.DemiBold
        padding: DesignTokens.spaceLg
        bottomPadding: 0
    }

    contentItem: ColumnLayout {
        spacing: DesignTokens.spaceMd

        Label {
            Layout.fillWidth: true
            text: qsTr("A module groups related endpoints (for example admin_organization). "
                       + "It is saved as resources/<name>.yaml.")
            wrapMode: Text.WordWrap
            color: DesignTokens.textSecondary
            font.pixelSize: 12
        }

        GridLayout {
            columns: 2
            columnSpacing: DesignTokens.spaceMd
            rowSpacing: DesignTokens.spaceSm
            Layout.fillWidth: true

            Label {
                text: qsTr("Name")
                color: DesignTokens.textSecondary
                font.pixelSize: 12
            }
            TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: qsTr("admin_organization")
                color: DesignTokens.textPrimary
                placeholderTextColor: DesignTokens.textSecondary
                onTextChanged: dialog.revalidate()
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: DesignTokens.surfaceSunken
                    border.width: 1
                    border.color: nameField.activeFocus ? DesignTokens.accent
                                                         : DesignTokens.borderSubtle
                }
            }

            Label {
                text: qsTr("Description")
                color: DesignTokens.textSecondary
                font.pixelSize: 12
            }
            TextField {
                id: descField
                Layout.fillWidth: true
                placeholderText: qsTr("Org-level admin actions (optional)")
                color: DesignTokens.textPrimary
                placeholderTextColor: DesignTokens.textSecondary
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: DesignTokens.surfaceSunken
                    border.width: 1
                    border.color: descField.activeFocus ? DesignTokens.accent
                                                         : DesignTokens.borderSubtle
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: dialog.errorText.length === 0
            text: dialog.nameValid ? qsTr("Creates resources/%1.yaml").arg(nameField.text.trim())
                                    : qsTr("Enter a name to create the module.")
            color: DesignTokens.textSecondary
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.errorText.length > 0
            text: dialog.errorText
            color: DesignTokens.methodDelete
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }
    }

    footer: DialogButtonBox {
        id: footerButtons
        standardButtons: Dialog.Ok | Dialog.Cancel
    }

    onAccepted: AppController.createResource(nameField.text.trim(), descField.text.trim())
}
