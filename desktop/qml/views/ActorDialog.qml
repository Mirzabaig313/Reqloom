// ActorDialog — create or edit an actor (an auth identity) from the QML UI.
// Covers the full auth endpoint: name, description, strategy, and either a
// config table (Basic/API Key/OAuth/AWS) or a login request (method, path,
// body, expected status, extract var←JSONPath) plus an optional refresh
// request — the same shape as a resource endpoint. Persisted via
// AppController.saveActorEdits → ProjectModel.saveActor.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Dialog {
    id: dialog
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 600
    height: Math.min(720, Overlay.overlay ? Overlay.overlay.height - 80 : 720)
    padding: DesignTokens.spaceLg

    property string originalId: ""
    // Local scalar fields (seeded from AppController on open, sent back on save).
    property string authMethod: "POST"
    property string authPath: ""
    property string authBody: ""
    property string authExpect: ""
    property bool refreshEnabled: false
    property string refreshMethod: "POST"
    property string refreshPath: ""
    property string refreshBody: ""

    readonly property bool nameValid: AppController.isValidName(nameField.text)
    readonly property bool stepBased: strategyCombo.currentText.indexOf("login") >= 0

    function openFor(actorId) {
        originalId = actorId;
        nameField.text = actorId;
        descField.text = actorId.length > 0 ? AppController.actorDescription(actorId) : "";
        if (actorId.length > 0) {
            AppController.prepareEditActor(actorId);
            // Select the actor's strategy; leave the default if it's somehow
            // not in the list (don't silently snap to the first entry).
            const stratIdx = strategyCombo.find(AppController.actorAuthLabel(actorId));
            if (stratIdx >= 0) {
                strategyCombo.currentIndex = stratIdx;
            }
        } else {
            AppController.prepareNewActor();
            strategyCombo.currentIndex = 0;
        }
        // Seed the request fields from the bridge (set by prepareEdit/NewActor).
        authMethod = AppController.actorAuthMethod;
        authPath = AppController.actorAuthPath;
        authBody = AppController.actorAuthBody;
        authExpect = AppController.actorAuthExpect;
        refreshEnabled = AppController.actorHasRefresh;
        refreshMethod = AppController.actorRefreshMethod;
        refreshPath = AppController.actorRefreshPath;
        refreshBody = AppController.actorRefreshBody;
        open();
        nameField.forceActiveFocus();
    }

    function configHint(strategy) {
        if (strategy === "Basic Auth")
            return qsTr("Keys: username, password");
        if (strategy === "API Key")
            return qsTr("Keys: key, location (header|query|cookie), name");
        if (strategy === "OAuth 2.0 (Client Credentials)")
            return qsTr("Keys: token_url, client_id, client_secret, scope");
        if (strategy === "OAuth 2.0 (Password)")
            return qsTr("Keys: token_url, client_id, client_secret, username, password, scope");
        if (strategy === "OAuth 1.0 (HMAC-SHA1)")
            return qsTr("Keys: consumer_key, consumer_secret, token, token_secret");
        if (strategy === "AWS Signature v4")
            return qsTr("Keys: access_key, secret_key, region, service");
        return qsTr("Config keys (resolved at auth time).");
    }

    background: Rectangle {
        radius: DesignTokens.radiusLg
        color: DesignTokens.surfaceRaised
        border.width: 1
        border.color: DesignTokens.glassBorder
    }

    header: DialogHeader {
        title: dialog.originalId.length > 0 ? qsTr("Edit actor") : qsTr("New actor")
    }

    // A small bordered editor used for the request body fields. Grows with its
    // content (no inner ScrollView) so it doesn't nest a second Flickable
    // inside the dialog's scroll area — nested vertical Flickables cause the
    // scroll jitter/sticking.
    component BodyBox: Rectangle {
        property alias text: bodyArea.text
        Layout.fillWidth: true
        implicitHeight: Math.max(90, bodyArea.implicitHeight + DesignTokens.spaceSm)
        radius: DesignTokens.radiusSm
        color: DesignTokens.surfaceSunken
        border.width: 1
        border.color: DesignTokens.borderSubtle
        TextArea {
            id: bodyArea
            anchors.fill: parent
            anchors.margins: DesignTokens.spaceXs
            placeholderText: "{ }"
            color: DesignTokens.textPrimary
            placeholderTextColor: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontLabel
            font.family: DesignTokens.fontMono
            wrapMode: TextEdit.WrapAnywhere
            background: null
        }
    }

    contentItem: ScrollView {
        id: contentScroll
        clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: contentScroll.availableWidth
            spacing: DesignTokens.spaceMd

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
                    placeholderText: qsTr("vendor")
                }
                FieldLabel {
                    text: qsTr("Description")
                }
                GlassTextField {
                    id: descField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Marketplace seller account (optional)")
                }
                FieldLabel {
                    text: qsTr("Strategy")
                }
                GlassComboBox {
                    id: strategyCombo
                    Layout.fillWidth: true
                    model: AppController.actorStrategies
                }
            }

            // ── Config-based strategies ──
            ColumnLayout {
                Layout.fillWidth: true
                visible: !dialog.stepBased
                spacing: DesignTokens.spaceXs
                FieldLabel {
                    text: qsTr("Auth config")
                }
                Label {
                    Layout.fillWidth: true
                    text: dialog.configHint(strategyCombo.currentText)
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontCaption
                    wrapMode: Text.WordWrap
                }
                KeyValueEditorView {
                    Layout.fillWidth: true
                    kvModel: AppController.editActorConfig
                    keyPlaceholder: qsTr("key")
                    valuePlaceholder: qsTr("value  (use {{secret.NAME}})")
                }
            }

            // ── Step-based login request ──
            ColumnLayout {
                Layout.fillWidth: true
                visible: dialog.stepBased
                spacing: DesignTokens.spaceXs
                FieldLabel {
                    text: qsTr("Login request")
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: DesignTokens.spaceSm
                    GlassComboBox {
                        Layout.preferredWidth: 110
                        model: ["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"]
                        currentIndex: Math.max(0, find(dialog.authMethod))
                        onActivated: dialog.authMethod = currentText
                    }
                    GlassTextField {
                        Layout.fillWidth: true
                        text: dialog.authPath
                        placeholderText: qsTr("/api/v1/auth/login")
                        onTextEdited: dialog.authPath = text
                    }
                    GlassTextField {
                        Layout.preferredWidth: 90
                        text: dialog.authExpect
                        placeholderText: qsTr("200")
                        onTextEdited: dialog.authExpect = text
                    }
                }
                FieldLabel {
                    text: qsTr("Body")
                }
                BodyBox {
                    text: dialog.authBody
                    onTextChanged: if (text !== dialog.authBody) {
                        dialog.authBody = text;
                    }
                }
                FieldLabel {
                    text: qsTr("Extract (variable ← JSONPath)")
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("The leading $. is optional — “data.accessToken” works too.")
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontCaption
                    wrapMode: Text.WordWrap
                }
                KeyValueEditorView {
                    Layout.fillWidth: true
                    kvModel: AppController.actorAuthExtract
                    keyPlaceholder: qsTr("token")
                    valuePlaceholder: qsTr("data.accessToken")
                }

                // ── Refresh ──
                CheckBox {
                    id: refreshCheck
                    text: qsTr("Session refresh endpoint")
                    checked: dialog.refreshEnabled
                    onToggled: dialog.refreshEnabled = checked
                    contentItem: Text {
                        leftPadding: refreshCheck.indicator.width + DesignTokens.spaceSm
                        text: refreshCheck.text
                        color: DesignTokens.textPrimary
                        font.pixelSize: DesignTokens.fontLabel
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: dialog.refreshEnabled
                    spacing: DesignTokens.spaceXs
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: DesignTokens.spaceSm
                        GlassComboBox {
                            Layout.preferredWidth: 110
                            model: ["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"]
                            currentIndex: Math.max(0, find(dialog.refreshMethod))
                            onActivated: dialog.refreshMethod = currentText
                        }
                        GlassTextField {
                            Layout.fillWidth: true
                            text: dialog.refreshPath
                            placeholderText: qsTr("/api/v1/auth/refresh")
                            onTextEdited: dialog.refreshPath = text
                        }
                    }
                    BodyBox {
                        text: dialog.refreshBody
                        onTextChanged: if (text !== dialog.refreshBody) {
                            dialog.refreshBody = text;
                        }
                    }
                    FieldLabel {
                        text: qsTr("Extract (variable ← JSONPath)")
                    }
                    KeyValueEditorView {
                        Layout.fillWidth: true
                        kvModel: AppController.actorRefreshExtract
                        keyPlaceholder: qsTr("token")
                        valuePlaceholder: qsTr("data.accessToken")
                    }
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
            Item {
                Layout.fillWidth: true
            }
            GlassButton {
                text: qsTr("Cancel")
                onClicked: dialog.reject()
            }
            GlassButton {
                text: dialog.originalId.length > 0 ? qsTr("Save actor") : qsTr("Create actor")
                primary: true
                enabled: dialog.nameValid
                // Save in place; only close the dialog when the engine accepts
                // it, so a validation/duplicate rejection keeps the user's input.
                onClicked: {
                    if (AppController.saveActorEdits(dialog.originalId, nameField.text.trim(), strategyCombo.currentText, descField.text.trim(), dialog.authMethod, dialog.authPath, dialog.authBody, dialog.authExpect, dialog.refreshEnabled, dialog.refreshMethod, dialog.refreshPath, dialog.refreshBody)) {
                        dialog.close();
                    }
                }
            }
        }
    }
}
