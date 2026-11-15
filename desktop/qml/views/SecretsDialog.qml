// SecretsDialog — keychain secrets management (mirrors the old Widgets
// SecretsDialog). Lists every {{secret.NAME}} the project references with
// its keychain state, and lets the user set or clear each one.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Dialog {
    id: dialog
    title: qsTr("Manage Secrets")
    header: DialogHeader {
        title: qsTr("Manage Secrets")
    }
    modal: true
    enter: PopupEnter {}
    exit: PopupExit {}
    anchors.centerIn: Overlay.overlay
    // Fit the window; the secrets list below absorbs any height deficit and
    // scrolls, so the intro text, banner and Close stay reachable.
    width: Math.min(560, Overlay.overlay ? Overlay.overlay.width - 64 : 560)
    height: Math.min(implicitHeight, Overlay.overlay ? Overlay.overlay.height - 64 : implicitHeight)
    padding: DesignTokens.spaceLg
    focus: true
    property string focusedSecretName: ""
    // The row whose Clear failed, so its error shows beside that button.
    property string clearErrorName: ""
    property string clearError: ""

    function openDialog() {
        focusedSecretName = "";
        clearErrorName = "";
        clearError = "";
        SecretsController.refresh();
        open();
    }

    function openSource(name) {
        SecretsController.refresh();
        const targetRow = SecretsController.secrets.rowForName(name);
        if (targetRow < 0) {
            return false;
        }
        focusedSecretName = name;
        clearErrorName = "";
        clearError = "";
        secretList.currentIndex = targetRow;
        open();
        Qt.callLater(function () {
            secretList.positionViewAtIndex(targetRow, ListView.Center);
            Qt.callLater(function () {
                const item = secretList.itemAtIndex(targetRow);
                if (item) {
                    item.focusSetAction();
                }
            });
        });
        return true;
    }

    background: Rectangle {
        radius: DesignTokens.radiusLg
        color: DesignTokens.surfaceRaised
        border.width: 1
        border.color: DesignTokens.glassBorder
    }

    contentItem: ColumnLayout {
        spacing: DesignTokens.spaceMd

        Label {
            Layout.fillWidth: true
            text: qsTr("Secrets referenced by this project via {{secret.NAME}}. Values are " + "stored in your OS keychain and never shown here.")
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontLabel
            wrapMode: Text.WordWrap
        }

        // Backend-unavailable banner.
        Rectangle {
            Layout.fillWidth: true
            visible: !SecretsController.backendAvailable
            radius: DesignTokens.radiusSm
            color: Qt.rgba(DesignTokens.statusError.r, DesignTokens.statusError.g, DesignTokens.statusError.b, 0.12)
            border.width: 1
            border.color: DesignTokens.statusError
            implicitHeight: bannerLabel.implicitHeight + DesignTokens.spaceSm * 2
            Label {
                id: bannerLabel
                anchors.fill: parent
                anchors.margins: DesignTokens.spaceSm
                text: qsTr("No OS keychain is available, so secrets can't be saved, cleared or read here, and requests that use {{secret.NAME}} will fail. Use a Reqloom build with keychain support, or pass the value through an allowed environment variable ({{$.env.NAME}}) instead.")
                color: DesignTokens.statusError
                font.pixelSize: DesignTokens.fontLabel
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
            }
        }

        // Folder-conflict banner: this project's id belongs to another folder
        // on this machine (the project was moved or copied). Secrets stay
        // blocked until the user confirms the move.
        Rectangle {
            Layout.fillWidth: true
            visible: SecretsController.folderConflict !== ""
            radius: DesignTokens.radiusSm
            color: Qt.rgba(DesignTokens.statusError.r, DesignTokens.statusError.g, DesignTokens.statusError.b, 0.12)
            border.width: 1
            border.color: DesignTokens.statusError
            implicitHeight: conflictColumn.implicitHeight + DesignTokens.spaceSm * 2
            ColumnLayout {
                id: conflictColumn
                anchors.fill: parent
                anchors.margins: DesignTokens.spaceSm
                spacing: DesignTokens.spaceXs
                Label {
                    Layout.fillWidth: true
                    text: qsTr("This project's id belongs to another folder on this machine:\n%1\nIf you moved the project here, use its secrets in this folder (the old folder loses access). If this is a copy, give it a new id in reqloom.yaml instead.").arg(SecretsController.folderConflict)
                    color: DesignTokens.statusError
                    font.pixelSize: DesignTokens.fontLabel
                    wrapMode: Text.WordWrap
                }
                Button {
                    id: confirmMoveBtn
                    text: qsTr("I moved this project — use its secrets here")
                    implicitHeight: Math.max(28, implicitContentHeight + DesignTokens.spaceXs * 2)
                    leftPadding: DesignTokens.spaceSm
                    rightPadding: DesignTokens.spaceSm
                    background: Rectangle {
                        radius: DesignTokens.radiusSm
                        color: confirmMoveBtn.down ? DesignTokens.accentMuted : "transparent"
                        border.width: 1
                        border.color: DesignTokens.borderStrong
                    }
                    contentItem: Text {
                        text: confirmMoveBtn.text
                        color: DesignTokens.textPrimary
                        font.pixelSize: DesignTokens.fontLabel
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: {
                        moveDialog.error = "";
                        moveDialog.open();
                    }
                }
                FieldError {
                    Layout.fillWidth: true
                    text: moveDialog.error
                }
            }
        }

        // Secrets table.
        ListView {
            id: secretList
            Layout.fillWidth: true
            // Shrink-wrap a short list, cap a long one, and take the deficit
            // here (rather than from the prose above) when the dialog is
            // clamped to the window.
            Layout.fillHeight: true
            Layout.preferredHeight: Math.min(contentHeight, 360)
            Layout.minimumHeight: DesignTokens.controlHeight * 2
            clip: true
            ScrollBar.vertical: ScrollBar {}
            model: SecretsController.secrets

            delegate: Rectangle {
                id: secretRow
                required property int index
                required property string name
                required property string status
                readonly property bool sourceFocused: name === dialog.focusedSecretName
                width: ListView.view.width
                // Row grows with its tallest child (reference label or the
                // Set…/Clear actions) at large OS text sizes.
                height: rowContent.height + (rowError.visible ? rowError.implicitHeight + DesignTokens.spaceXs : 0)
                color: sourceFocused ? DesignTokens.accentMuted : "transparent"

                function focusSetAction() {
                    setSecretBtn.forceActiveFocus();
                }

                RowLayout {
                    id: rowContent
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: Math.max(44, implicitHeight + DesignTokens.spaceSm * 2)
                    anchors.leftMargin: DesignTokens.spaceSm
                    anchors.rightMargin: DesignTokens.spaceSm
                    spacing: DesignTokens.spaceMd

                    Label {
                        id: secretName
                        Layout.fillWidth: true
                        text: "{{secret." + secretRow.name + "}}"
                        color: DesignTokens.textPrimary
                        font.pixelSize: DesignTokens.fontBody
                        font.family: DesignTokens.fontMono
                        elide: Text.ElideRight
                    }
                    Label {
                        text: secretRow.status === "set" ? "✓  set" : secretRow.status
                        color: secretRow.status === "set" ? DesignTokens.statusSuccess : DesignTokens.statusError
                        font.pixelSize: DesignTokens.fontLabel
                    }

                    Button {
                        id: setSecretBtn
                        text: qsTr("Set…")
                        implicitHeight: Math.max(28, implicitContentHeight + DesignTokens.spaceXs * 2)
                        leftPadding: DesignTokens.spaceSm
                        rightPadding: DesignTokens.spaceSm
                        background: Rectangle {
                            radius: DesignTokens.radiusSm
                            color: setSecretBtn.down ? DesignTokens.accentMuted : "transparent"
                            border.width: 1
                            border.color: DesignTokens.borderStrong
                        }
                        contentItem: Text {
                            text: setSecretBtn.text
                            color: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontLabel
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        onClicked: setDialog.openFor(secretRow.name)
                    }
                    Button {
                        id: clearSecretBtn
                        text: qsTr("Clear")
                        visible: secretRow.status === "set"
                        implicitHeight: Math.max(28, implicitContentHeight + DesignTokens.spaceXs * 2)
                        leftPadding: DesignTokens.spaceSm
                        rightPadding: DesignTokens.spaceSm
                        background: Rectangle {
                            radius: DesignTokens.radiusSm
                            color: clearSecretBtn.down ? Qt.rgba(DesignTokens.statusError.r, DesignTokens.statusError.g, DesignTokens.statusError.b, 0.12) : "transparent"
                            border.width: 1
                            border.color: DesignTokens.borderStrong
                        }
                        contentItem: Text {
                            text: clearSecretBtn.text
                            color: DesignTokens.statusError
                            font.pixelSize: DesignTokens.fontLabel
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        onClicked: {
                            const ok = SecretsController.clear(secretRow.name);
                            dialog.clearErrorName = ok ? "" : secretRow.name;
                            dialog.clearError = ok ? "" : SecretsController.lastError;
                        }
                    }
                }

                FieldError {
                    id: rowError
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: rowContent.bottom
                    anchors.leftMargin: DesignTokens.spaceSm
                    anchors.rightMargin: DesignTokens.spaceSm
                    text: secretRow.name === dialog.clearErrorName ? dialog.clearError : ""
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: DesignTokens.borderSubtle
                    opacity: 0.5
                }
            }
        }
    }

    footer: DialogButtons {
        showCancel: false
        okText: qsTr("Close")
        okPrimary: false
        onAccepted: dialog.close()
    }

    // Set-value sub-dialog. Closes only when the value was stored.
    Dialog {
        id: setDialog
        modal: true
        enter: PopupEnter {}
        exit: PopupExit {}
        anchors.centerIn: Overlay.overlay
        width: Math.min(380, Overlay.overlay ? Overlay.overlay.width - 64 : 380)
        height: Math.min(implicitHeight, Overlay.overlay ? Overlay.overlay.height - 64 : implicitHeight)
        padding: DesignTokens.spaceLg
        focus: true
        property string secretName: ""
        property string error: ""
        readonly property bool canSave: valueField.text.length > 0
        header: DialogHeader {
            title: qsTr("Set secret")
        }

        function openFor(name) {
            secretName = name;
            error = "";
            valueField.text = "";
            open();
            valueField.forceActiveFocus();
        }

        function save() {
            if (!canSave) {
                return;
            }
            if (SecretsController.store(secretName, valueField.text)) {
                close();
            } else {
                error = SecretsController.lastError;
            }
        }

        // The secret must not linger in the field once the dialog is gone.
        onAboutToHide: {
            valueField.text = "";
            revealToggle.checked = false;
        }

        background: Rectangle {
            radius: DesignTokens.radiusLg
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.glassBorder
        }

        contentItem: ColumnLayout {
            spacing: DesignTokens.spaceSm
            FieldLabel {
                Layout.fillWidth: true
                text: qsTr("Value for {{secret.%1}} (stored in the OS keychain):").arg(setDialog.secretName)
                wrapMode: Text.WordWrap
            }
            GlassTextField {
                id: valueField
                Layout.fillWidth: true
                echoMode: revealToggle.checked ? TextInput.Normal : TextInput.Password
                placeholderText: qsTr("value (not shown)")
                onTextEdited: setDialog.error = ""
                onAccepted: setDialog.save()
            }
            CheckBox {
                id: revealToggle
                text: qsTr("Show value")
                checked: false
                contentItem: Text {
                    text: revealToggle.text
                    color: DesignTokens.textSecondary
                    font.pointSize: DesignTokens.fontLabelPointSize
                    leftPadding: revealToggle.indicator.width + DesignTokens.spaceSm
                    verticalAlignment: Text.AlignVCenter
                }
                indicator: Rectangle {
                    implicitWidth: 16
                    implicitHeight: 16
                    radius: 4
                    y: (revealToggle.height - height) / 2
                    color: revealToggle.checked ? DesignTokens.accent : DesignTokens.surfaceRaised
                    border.width: 1
                    border.color: revealToggle.checked ? DesignTokens.accent : DesignTokens.borderStrong
                    Text {
                        anchors.centerIn: parent
                        visible: revealToggle.checked
                        text: "✓"
                        color: DesignTokens.textInverse
                        font.pointSize: DesignTokens.fontCaptionPointSize
                        font.weight: DesignTokens.weightBold
                    }
                }
            }
            FieldError {
                Layout.fillWidth: true
                text: setDialog.error
            }
        }
        footer: DialogButtons {
            okText: qsTr("Save")
            okEnabled: setDialog.canSave
            onAccepted: setDialog.save()
            onRejected: setDialog.reject()
        }
    }

    // Rebinding the project id moves secret access away from the old folder,
    // so it needs both folders in front of the user before it happens.
    Dialog {
        id: moveDialog
        modal: true
        enter: PopupEnter {}
        exit: PopupExit {}
        anchors.centerIn: Overlay.overlay
        width: Math.min(460, Overlay.overlay ? Overlay.overlay.width - 64 : 460)
        height: Math.min(implicitHeight, Overlay.overlay ? Overlay.overlay.height - 64 : implicitHeight)
        padding: DesignTokens.spaceLg
        property string error: ""
        header: DialogHeader {
            title: qsTr("Use this project's secrets here?")
        }

        background: Rectangle {
            radius: DesignTokens.radiusLg
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.glassBorder
        }

        contentItem: Label {
            text: qsTr("Secrets for this project id are recorded for:\n%1\n\nThey will be used from this folder instead:\n%2\n\nThe old folder loses access to them. This can't be undone from here.").arg(SecretsController.folderConflict).arg(SecretsController.currentFolder)
            color: DesignTokens.textPrimary
            font.pixelSize: DesignTokens.fontLabel
            wrapMode: Text.WrapAnywhere
        }

        footer: DialogButtons {
            okText: qsTr("Use secrets here")
            okDestructive: true
            onAccepted: {
                moveDialog.error = SecretsController.confirmMove() ? "" : SecretsController.lastError;
                moveDialog.close();
            }
            onRejected: moveDialog.reject()
        }
    }
}
