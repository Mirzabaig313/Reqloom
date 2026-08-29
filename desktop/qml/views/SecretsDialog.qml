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

    function openDialog() {
        focusedSecretName = "";
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
                text: qsTr("Keychain backend unavailable — secret writes are not persisted.")
                color: DesignTokens.statusError
                font.pixelSize: DesignTokens.fontLabel
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
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
                height: Math.max(44, rowContent.implicitHeight + DesignTokens.spaceSm * 2)
                color: sourceFocused ? DesignTokens.accentMuted : "transparent"

                function focusSetAction() {
                    setSecretBtn.forceActiveFocus();
                }

                RowLayout {
                    id: rowContent
                    anchors.fill: parent
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
                        onClicked: SecretsController.clear(secretRow.name)
                    }
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

    // Set-value sub-dialog.
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
        header: DialogHeader {
            title: qsTr("Set secret")
        }

        function openFor(name) {
            secretName = name;
            valueField.text = "";
            open();
            valueField.forceActiveFocus();
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
                echoMode: TextInput.Password
                placeholderText: qsTr("value (not shown)")
                onAccepted: setDialog.accept()
            }
        }
        footer: DialogButtons {
            okText: qsTr("Save")
            onAccepted: setDialog.accept()
            onRejected: setDialog.reject()
        }
        onAccepted: SecretsController.store(secretName, valueField.text)
    }
}
