// ManageEnvironmentDialog — Apidog-style environment manager: a left sidebar
// lists every environment (coloured initials badge + name) with a "New
// Environment" action; the right pane edits the selected environment's name and
// variable table. Reuses AppController's env editor state (prepareEditEnvironment
// / editEnvVars / saveEnvironmentEdits / deleteEnvironment). Selecting a row in
// the list loads it (unsaved edits in the pane are discarded — click Save first).
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Dialog {
    id: dialog
    modal: true
    enter: PopupEnter {}
    exit: PopupExit {}
    anchors.centerIn: Overlay.overlay
    width: Math.min(920, Overlay.overlay ? Overlay.overlay.width - 64 : 920)
    height: Math.min(640, Overlay.overlay ? Overlay.overlay.height - 64 : 640)
    padding: 0

    // Original name of the env being edited ("" = a new, unsaved environment).
    property string editingOriginal: ""

    function initials(s) {
        const t = (s || "").trim();
        return t.length === 0 ? "··" : t.substring(0, 2);
    }
    function badgeHue(s) {
        let h = 0;
        for (let i = 0; i < s.length; ++i) {
            h = (h * 31 + s.charCodeAt(i)) >>> 0;
        }
        return Qt.hsla((h % 360) / 360, 0.55, DesignTokens.isDark ? 0.62 : 0.5, 1.0);
    }

    function selectEnv(name) {
        editingOriginal = name;
        nameField.text = name;
        if (name.length > 0) {
            AppController.prepareEditEnvironment(name);
        } else {
            AppController.prepareNewEnvironment();
        }
    }
    function startNew() {
        selectEnv("");
        nameField.forceActiveFocus();
    }
    function openManager(focusName) {
        const target = focusName && focusName.length > 0 ? focusName : (AppController.environment.length > 0 ? AppController.environment : (AppController.environments.length > 0 ? AppController.environments[0] : ""));
        selectEnv(target);
        open();
    }
    function persist(thenClose) {
        const ok = AppController.saveEnvironmentEdits(dialog.editingOriginal, nameField.text.trim());
        if (ok) {
            dialog.editingOriginal = nameField.text.trim();  // survive a rename
            if (thenClose) {
                dialog.close();
            }
        }
    }

    readonly property bool nameValid: AppController.isValidName(nameField.text)

    background: Rectangle {
        radius: DesignTokens.radiusLg
        color: DesignTokens.surfaceBase
        border.width: 1
        border.color: DesignTokens.glassBorder
    }

    header: Item {
        implicitHeight: 44
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: DesignTokens.spaceLg
            anchors.rightMargin: DesignTokens.spaceSm
            Label {
                Layout.fillWidth: true
                text: qsTr("Manage Environment")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontLabel
            }
            Button {
                id: closeBtn
                implicitWidth: 28
                implicitHeight: 28
                onClicked: dialog.reject()
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: closeBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
                contentItem: AppIcon {
                    name: "x"
                    size: 16
                    anchors.centerIn: parent
                }
            }
        }
    }

    contentItem: RowLayout {
        spacing: 0

        // ── Sidebar ──
        Rectangle {
            Layout.preferredWidth: 248
            Layout.fillHeight: true
            color: DesignTokens.surfaceSunken
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: DesignTokens.spaceSm
                spacing: DesignTokens.spaceXs

                Label {
                    Layout.leftMargin: DesignTokens.spaceSm
                    Layout.topMargin: DesignTokens.spaceXs
                    text: qsTr("ENVIRONMENTS")
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontCaption
                    font.weight: DesignTokens.weightSemiBold
                    font.letterSpacing: 1.2
                }

                ListView {
                    id: envList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: AppController.environments
                    spacing: 2
                    ScrollBar.vertical: ScrollBar {}
                    delegate: ItemDelegate {
                        id: envRow
                        required property string modelData
                        width: ListView.view ? ListView.view.width : 0
                        implicitHeight: 36
                        highlighted: modelData === dialog.editingOriginal
                        onClicked: dialog.selectEnv(modelData)
                        background: Rectangle {
                            radius: DesignTokens.radiusSm
                            color: envRow.highlighted ? DesignTokens.accentMuted : (envRow.hovered ? Qt.rgba(1, 1, 1, 0.04) : "transparent")
                        }
                        contentItem: RowLayout {
                            spacing: DesignTokens.spaceSm
                            Rectangle {
                                Layout.leftMargin: DesignTokens.spaceXs
                                implicitWidth: 20
                                implicitHeight: 20
                                radius: DesignTokens.radiusSm
                                color: Qt.rgba(dialog.badgeHue(envRow.modelData).r, dialog.badgeHue(envRow.modelData).g, dialog.badgeHue(envRow.modelData).b, 0.18)
                                Text {
                                    anchors.centerIn: parent
                                    text: dialog.initials(envRow.modelData)
                                    color: dialog.badgeHue(envRow.modelData)
                                    font.pixelSize: 9
                                    font.weight: DesignTokens.weightBold
                                    font.capitalization: Font.Capitalize
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: envRow.modelData
                                color: DesignTokens.textPrimary
                                font.pixelSize: DesignTokens.fontBody
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }

                ItemDelegate {
                    id: newEnvRow
                    Layout.fillWidth: true
                    implicitHeight: 34
                    onClicked: dialog.startNew()
                    background: Rectangle {
                        radius: DesignTokens.radiusSm
                        color: newEnvRow.hovered ? Qt.rgba(1, 1, 1, 0.04) : "transparent"
                    }
                    contentItem: RowLayout {
                        spacing: DesignTokens.spaceSm
                        AppIcon {
                            Layout.leftMargin: DesignTokens.spaceXs
                            name: "plus"
                            size: 14
                            color: DesignTokens.accent
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("New Environment")
                            color: DesignTokens.accent
                            font.pixelSize: DesignTokens.fontBody
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
            }
        }

        // ── Detail pane ──
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: DesignTokens.spaceLg
            spacing: DesignTokens.spaceMd

            RowLayout {
                Layout.fillWidth: true
                spacing: DesignTokens.spaceSm
                Rectangle {
                    implicitWidth: 26
                    implicitHeight: 26
                    radius: DesignTokens.radiusSm
                    color: Qt.rgba(dialog.badgeHue(nameField.text).r, dialog.badgeHue(nameField.text).g, dialog.badgeHue(nameField.text).b, 0.18)
                    Text {
                        anchors.centerIn: parent
                        text: dialog.initials(nameField.text)
                        color: dialog.badgeHue(nameField.text)
                        font.pixelSize: DesignTokens.fontCaption
                        font.weight: DesignTokens.weightBold
                        font.capitalization: Font.Capitalize
                    }
                }
                FieldLabel {
                    text: qsTr("Name")
                }
                GlassTextField {
                    id: nameField
                    Layout.fillWidth: true
                    placeholderText: qsTr("local")
                }
            }

            FieldLabel {
                text: qsTr("Base URL")
            }
            GlassTextField {
                Layout.fillWidth: true
                text: AppController.editEnvBaseUrl
                placeholderText: qsTr("http://localhost:3000")
                onTextEdited: AppController.editEnvBaseUrl = text
            }

            FieldLabel {
                text: qsTr("Variables")
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Referenced in requests as {{env.NAME}} (e.g. admin_email). Base URL is {{env.baseUrl}}.")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontCaption
                wrapMode: Text.WordWrap
            }
            ScrollView {
                id: varsScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth
                KeyValueEditorView {
                    width: varsScroll.availableWidth
                    kvModel: AppController.editEnvVars
                    keyPlaceholder: qsTr("admin_email")
                    valuePlaceholder: qsTr("admin@example.com")
                }
            }
        }
    }

    footer: Item {
        implicitHeight: 60
        RowLayout {
            anchors.fill: parent
            anchors.margins: DesignTokens.spaceLg
            spacing: DesignTokens.spaceSm

            GlassButton {
                text: qsTr("Delete")
                enabled: dialog.editingOriginal.length > 0
                onClicked: {
                    AppController.deleteEnvironment(dialog.editingOriginal);
                    dialog.selectEnv(AppController.environments.length > 0 ? AppController.environments[0] : "");
                }
            }
            Item {
                Layout.fillWidth: true
            }
            GlassButton {
                text: qsTr("Cancel")
                onClicked: dialog.reject()
            }
            GlassButton {
                text: qsTr("Save")
                enabled: dialog.nameValid
                onClicked: dialog.persist(false)
            }
            GlassButton {
                text: qsTr("Save & Close")
                primary: true
                enabled: dialog.nameValid
                onClicked: dialog.persist(true)
            }
        }
    }
}
