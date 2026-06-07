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
        AppController.prepareNewEndpoint(preselectedResource)
        nameField.text = ""
        pathField.text = ""
        methodCombo.currentIndex = 0
        chainToggle.checked = false
        const idx = moduleCombo.find(preselectedResource)
        moduleCombo.currentIndex = idx >= 0 ? idx : 0
        actorCombo.currentIndex = 0
        revalidate()
        open()
        nameField.forceActiveFocus()
    }

    property string errorText: ""

    function revalidate() {
        const name = nameField.text.trim()
        if (AppController.moduleNames.length === 0) {
            errorText = qsTr("Create a module first.")
        } else if (name.length === 0) {
            errorText = qsTr("Name cannot be empty.")
        } else if (!AppController.isValidName(name)) {
            errorText = qsTr("Name can't contain '.', '/', or '\\'.")
        } else {
            errorText = ""
        }
        const okBtn = footerButtons.standardButton(Dialog.Ok)
        if (okBtn) {
            okBtn.enabled = errorText.length === 0
        }
    }

    background: Rectangle {
        radius: DesignTokens.radius
        color: DesignTokens.surfaceRaised
        border.width: 1
        border.color: DesignTokens.borderSubtle
    }

    header: Label {
        text: qsTr("New endpoint")
        color: DesignTokens.textPrimary
        font.pixelSize: 17
        font.weight: Font.DemiBold
        padding: DesignTokens.spaceLg
        bottomPadding: 0
    }

    component FieldBox: Rectangle {
        radius: DesignTokens.radiusSm
        color: DesignTokens.surfaceSunken
        border.width: 1
        border.color: DesignTokens.borderSubtle
        implicitHeight: 32
    }

    contentItem: ColumnLayout {
        spacing: DesignTokens.spaceMd

        Label {
            Layout.fillWidth: true
            text: qsTr("Define the request. You can wire its dependency chain now or later in "
                       + "the editor.")
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
                text: qsTr("Module")
                color: DesignTokens.textSecondary
                font.pixelSize: 12
            }
            ComboBox {
                id: moduleCombo
                Layout.fillWidth: true
                implicitHeight: 32
                model: AppController.moduleNames
                background: FieldBox {
                    border.color: moduleCombo.activeFocus ? DesignTokens.accent
                                                          : DesignTokens.borderSubtle
                }
                contentItem: Text {
                    leftPadding: DesignTokens.spaceSm
                    text: moduleCombo.displayText
                    color: DesignTokens.textPrimary
                    font.pixelSize: 12
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
            }

            Label {
                text: qsTr("Name")
                color: DesignTokens.textSecondary
                font.pixelSize: 12
            }
            TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: qsTr("verify")
                color: DesignTokens.textPrimary
                placeholderTextColor: DesignTokens.textSecondary
                onTextChanged: dialog.revalidate()
                background: FieldBox {
                    border.color: nameField.activeFocus ? DesignTokens.accent
                                                        : DesignTokens.borderSubtle
                }
            }

            Label {
                text: qsTr("Method")
                color: DesignTokens.textSecondary
                font.pixelSize: 12
            }
            ComboBox {
                id: methodCombo
                Layout.fillWidth: true
                implicitHeight: 32
                model: ["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"]
                background: FieldBox {
                    border.color: methodCombo.activeFocus ? DesignTokens.accent
                                                          : DesignTokens.borderSubtle
                }
                contentItem: Text {
                    leftPadding: DesignTokens.spaceSm
                    text: methodCombo.displayText
                    color: DesignTokens.textPrimary
                    font.pixelSize: 12
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Label {
                text: qsTr("Path")
                color: DesignTokens.textSecondary
                font.pixelSize: 12
            }
            TextField {
                id: pathField
                Layout.fillWidth: true
                placeholderText: qsTr("/api/v1/admin/orgs/{{id}}/verify")
                color: DesignTokens.textPrimary
                placeholderTextColor: DesignTokens.textSecondary
                font.family: "monospace"
                background: FieldBox {
                    border.color: pathField.activeFocus ? DesignTokens.accent
                                                        : DesignTokens.borderSubtle
                }
            }

            Label {
                text: qsTr("Actor")
                color: DesignTokens.textSecondary
                font.pixelSize: 12
            }
            ComboBox {
                id: actorCombo
                Layout.fillWidth: true
                implicitHeight: 32
                model: AppController.actorNames
                background: FieldBox {
                    border.color: actorCombo.activeFocus ? DesignTokens.accent
                                                         : DesignTokens.borderSubtle
                }
                contentItem: Text {
                    leftPadding: DesignTokens.spaceSm
                    text: actorCombo.displayText
                    color: DesignTokens.textPrimary
                    font.pixelSize: 12
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: dialog.errorText.length > 0
            text: dialog.errorText
            color: DesignTokens.methodDelete
            font.pixelSize: 12
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
            contentItem: Label {
                text: (chainToggle.checked ? "▾  " : "▸  ") + qsTr("Chain (optional)")
                color: DesignTokens.textSecondary
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: chainToggle.checked
            spacing: DesignTokens.spaceSm

            Label {
                text: qsTr("Depends on")
                color: DesignTokens.textPrimary
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }
            DependencyEditor {
                Layout.fillWidth: true
                depModel: AppController.newEndpointDependencies
                candidates: AppController.operationIds
            }
            Label {
                text: qsTr("Extract")
                color: DesignTokens.textPrimary
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }
            ExtractionEditor {
                Layout.fillWidth: true
                extractModel: AppController.newEndpointExtractions
            }
        }
    }

    footer: DialogButtonBox {
        id: footerButtons
        standardButtons: Dialog.Ok | Dialog.Cancel
    }

    onAccepted: AppController.createOperation(moduleCombo.currentText,
                                              nameField.text.trim(),
                                              methodCombo.currentText,
                                              pathField.text.trim(),
                                              actorCombo.currentText)
}
