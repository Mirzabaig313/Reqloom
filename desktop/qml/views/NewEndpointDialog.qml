// NewEndpointDialog — create a new endpoint (engine operation) under a module
// (mirrors the old NewEndpointDialog). Module / name / method / path / actor,
// plus an optional collapsed Chain section (depends_on picker + extract table).
// Live validation matches the old dialog; the authoritative validation and
// cycle rejection happen in AppController.createOperation → ProjectModel →
// engine::validateProject (a dependency that forms a cycle is rejected and
// nothing is written).
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Dialog {
    id: dialog
    title: qsTr("New Endpoint")
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 540
    padding: DesignTokens.spaceLg

    function openFor(preselectedResource) {
        AppController.prepareNewEndpoint(preselectedResource);
        nameField.text = "";
        pathField.text = "";
        methodCombo.currentIndex = 0;
        chainToggle.checked = false;
        const idx = moduleCombo.find(preselectedResource);
        moduleCombo.currentIndex = idx >= 0 ? idx : 0;
        actorCombo.currentIndex = 0;
        revalidate();
        open();
        nameField.forceActiveFocus();
    }

    property string errorText: ""
    property bool canSubmit: false

    function revalidate() {
        const name = nameField.text.trim();
        if (AppController.moduleNames.length === 0) {
            errorText = qsTr("Create a module first.");
        } else if (name.length === 0) {
            errorText = qsTr("Name cannot be empty.");
        } else if (!AppController.isValidName(name)) {
            errorText = qsTr("Name can't contain '.', '/', or '\\'.");
        } else {
            errorText = "";
        }
        canSubmit = errorText.length === 0;
    }

    background: Rectangle {
        radius: DesignTokens.radiusLg
        color: DesignTokens.surfaceRaised
        border.width: 1
        border.color: DesignTokens.glassBorder
    }

    header: DialogHeader {
        title: qsTr("New endpoint")
    }

    contentItem: ColumnLayout {
        spacing: DesignTokens.spaceMd

        Label {
            Layout.fillWidth: true
            text: qsTr("Define the request. You can wire its dependency chain now or later in " + "the editor.")
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
                text: qsTr("Module")
            }
            GlassComboBox {
                id: moduleCombo
                Layout.fillWidth: true
                model: AppController.moduleNames
            }

            FieldLabel {
                text: qsTr("Name")
            }
            GlassTextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: qsTr("verify")
                onTextChanged: dialog.revalidate()
            }

            FieldLabel {
                text: qsTr("Method")
            }
            GlassComboBox {
                id: methodCombo
                Layout.fillWidth: true
                model: ["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"]
            }

            FieldLabel {
                text: qsTr("Path")
            }
            GlassTextField {
                id: pathField
                Layout.fillWidth: true
                mono: true
                placeholderText: qsTr("/api/v1/admin/orgs/{{id}}/verify")
            }

            FieldLabel {
                text: qsTr("Actor")
            }
            GlassComboBox {
                id: actorCombo
                Layout.fillWidth: true
                model: AppController.actorNames
            }
        }

        Label {
            Layout.fillWidth: true
            visible: dialog.errorText.length > 0
            text: dialog.errorText
            color: DesignTokens.statusError
            font.pixelSize: DesignTokens.fontLabel
            wrapMode: Text.WordWrap
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: DesignTokens.borderSubtle
        }

        // Optional, collapsed chain section.
        AbstractButton {
            id: chainToggle
            checkable: true
            implicitHeight: 24
            Layout.fillWidth: true
            contentItem: RowLayout {
                spacing: DesignTokens.spaceXs
                AppIcon {
                    name: chainToggle.checked ? "chevron-down" : "chevron-right"
                    size: 14
                }
                Label {
                    text: qsTr("Chain (optional)")
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontLabel
                    font.weight: DesignTokens.weightSemiBold
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: chainToggle.checked
            spacing: DesignTokens.spaceSm

            Label {
                text: qsTr("Depends on")
                color: DesignTokens.textPrimary
                font.pixelSize: DesignTokens.fontLabel
                font.weight: DesignTokens.weightSemiBold
            }
            DependencyEditor {
                Layout.fillWidth: true
                depModel: AppController.newEndpointDependencies
                candidates: AppController.operationIds
            }
            Label {
                text: qsTr("Extract")
                color: DesignTokens.textPrimary
                font.pixelSize: DesignTokens.fontLabel
                font.weight: DesignTokens.weightSemiBold
            }
            ExtractionEditor {
                Layout.fillWidth: true
                extractModel: AppController.newEndpointExtractions
            }
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
                text: qsTr("Create endpoint")
                primary: true
                enabled: dialog.canSubmit
                onClicked: dialog.accept()
            }
        }
    }

    onAccepted: AppController.createOperation(moduleCombo.currentText, nameField.text.trim(), methodCombo.currentText, pathField.text.trim(), actorCombo.currentText)
}
