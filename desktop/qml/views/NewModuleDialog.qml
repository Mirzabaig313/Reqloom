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
    // Fit the window instead of a fixed 460x(content): shrink to the available
    // space and let the body scroll, so header and actions always stay visible.
    width: Math.min(460, Overlay.overlay ? Overlay.overlay.width - 64 : 460)
    height: Math.min(implicitHeight, Overlay.overlay ? Overlay.overlay.height - 64 : implicitHeight)
    padding: DesignTokens.spaceLg
    focus: true

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

    contentItem: ScrollView {
        id: bodyScroll
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: bodyScroll.availableWidth
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
                    // Enter submits when the name is valid.
                    onAccepted: {
                        if (dialog.nameValid) {
                            dialog.accept();
                        }
                    }
                }

                FieldLabel {
                    text: qsTr("Description")
                }
                GlassTextField {
                    id: descField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Org-level admin actions (optional)")
                    onAccepted: {
                        if (dialog.nameValid) {
                            dialog.accept();
                        }
                    }
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
    }

    footer: DialogButtons {
        okText: qsTr("Create module")
        okEnabled: dialog.nameValid
        onAccepted: dialog.accept()
        onRejected: dialog.reject()
    }

    onAccepted: AppController.createResource(nameField.text.trim(), descField.text.trim())
}
