// ActorDetail — actor (auth identity) detail in the centre pane. Shows a
// read-only view by default; the "Edit" button flips the same panel into an
// inline editable form (no modal) with Save/Cancel. Step-based strategies edit
// a full N-step login chain (AppController.actorAuthSteps): Add/Remove/reorder
// steps, each with its own method/path/body/expect + extractions. Seeded by
// selectActor→prepareEditActor; Save persists via saveActorInline. New-actor
// creation reuses this same panel: AppController.newActor() shows it with an
// empty selection, which opens straight in edit mode (no separate dialog).
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

ScrollView {
    id: root
    clip: true
    contentWidth: availableWidth

    property bool editing: false

    // Editable working copies for the scalar fields (seeded on beginEdit). The
    // login steps live in AppController.actorAuthSteps and the refresh/config
    // key-value tables are the shared AppController models, edited directly.
    property string fName: ""
    property string fDesc: ""
    property bool fRefreshEnabled: false
    property string fRefreshMethod: "POST"
    property string fRefreshPath: ""
    property string fRefreshBody: ""

    // HTTP verbs for the method pickers. Shared so the combos can resolve their
    // current index with a plain indexOf (ComboBox.find() returns -1 while the
    // combo's internal model is still initialising, which defaulted every step
    // to GET even when the model held POST).
    readonly property var methodOptions: ["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"]

    // Step-based strategies carry "login" in their label. In edit mode the live
    // combo selection drives it; in read mode the selected actor's strategy.
    readonly property bool stepBased: editing ? (strategyCombo.currentText.indexOf("login") >= 0) : (AppController.selectedActorStrategy.indexOf("login") >= 0)
    readonly property string strategy: editing ? strategyCombo.currentText : AppController.selectedActorStrategy
    readonly property bool nameValid: AppController.isValidName(fName)
    // PKCS#12 vs PEM for the mTLS strategy's conditional fields.
    readonly property bool mtlsIsP12: AppController.actorConfig["format"] === "p12"

    // Switching to a different actor (or a post-save re-seed) discards any
    // in-progress inline edit — the panel always reflects the current actor.
    //: no "unsaved changes" prompt; a mis-click loses edits. Upgrade
    // path: guard with a confirm dialog if users report lost work.
    Connections {
        target: AppController
        function onActorSelectionChanged() {
            // A brand-new actor (no name yet) opens straight in edit mode; any
            // existing selection returns to the read view.
            if (AppController.hasActor && AppController.selectedActorName.length === 0) {
                root.beginEdit();
            } else {
                root.editing = false;
            }
        }
        function onActorEditRequested() {
            root.beginEdit();
        }
    }

    function beginEdit() {
        fName = AppController.selectedActorName;
        fDesc = AppController.selectedActorDescription;
        fRefreshEnabled = AppController.actorHasRefresh;
        fRefreshMethod = AppController.actorRefreshMethod;
        fRefreshPath = AppController.actorRefreshPath;
        fRefreshBody = AppController.actorRefreshBody;
        const idx = strategyCombo.find(AppController.selectedActorStrategy);
        if (idx >= 0) {
            strategyCombo.currentIndex = idx;
        }
        editing = true;
    }

    function cancelEdit() {
        // A never-saved draft has no persisted actor to fall back to — close
        // the panel entirely instead of leaving a blank read view.
        if (AppController.selectedActorName.length === 0) {
            AppController.cancelActorDraft();
            return;
        }
        // Discard edits to the shared step/config/refresh models by re-seeding
        // from the persisted actor (name == id for the selected actor).
        AppController.prepareEditActor(AppController.selectedActorName);
        editing = false;
    }

    function saveEdits() {
        // originalId stays the pre-edit name (== id in this project) so a rename
        // still targets the right actor; only close edit mode when accepted.
        if (AppController.saveActorInline(AppController.selectedActorName, fName.trim(), strategyCombo.currentText, fDesc.trim(), fRefreshEnabled, fRefreshMethod, fRefreshPath, fRefreshBody)) {
            editing = false;
        }
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: DesignTokens.spaceLg

        // ── Header: name + Edit / Save+Cancel ──
        RowLayout {
            Layout.fillWidth: true
            spacing: DesignTokens.spaceMd
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    visible: !root.editing
                    text: AppController.selectedActorName
                    color: DesignTokens.textPrimary
                    font.pixelSize: DesignTokens.fontTitle
                    font.weight: DesignTokens.weightSemiBold
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                // A never-saved draft has no name yet — label the panel so the
                // empty name field reads as "New actor" rather than blank.
                Label {
                    visible: root.editing && AppController.selectedActorName.length === 0
                    text: qsTr("New actor")
                    color: DesignTokens.textPrimary
                    font.pixelSize: DesignTokens.fontTitle
                    font.weight: DesignTokens.weightSemiBold
                    Layout.fillWidth: true
                }
                GlassTextField {
                    visible: root.editing
                    Layout.fillWidth: true
                    text: root.fName
                    placeholderText: qsTr("vendor")
                    onTextEdited: root.fName = text
                }
                Label {
                    visible: !root.editing && AppController.selectedActorStrategy.length > 0
                    text: AppController.selectedActorStrategy
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontBody
                }
            }
            GlassButton {
                Layout.alignment: Qt.AlignTop
                visible: !root.editing
                text: qsTr("Edit")
                primary: true
                onClicked: root.beginEdit()
            }
            GlassButton {
                Layout.alignment: Qt.AlignTop
                visible: root.editing
                text: qsTr("Cancel")
                onClicked: root.cancelEdit()
            }
            GlassButton {
                Layout.alignment: Qt.AlignTop
                visible: root.editing
                text: AppController.selectedActorName.length === 0 ? qsTr("Create actor") : qsTr("Save")
                primary: true
                enabled: root.nameValid
                onClicked: root.saveEdits()
            }
        }

        // Description — read Label / edit field.
        Label {
            visible: !root.editing && AppController.selectedActorDescription.length > 0
            Layout.fillWidth: true
            text: AppController.selectedActorDescription
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontBody
            wrapMode: Text.WordWrap
        }
        ColumnLayout {
            visible: root.editing
            Layout.fillWidth: true
            spacing: DesignTokens.spaceXs
            FieldLabel {
                text: qsTr("Description")
            }
            GlassTextField {
                Layout.fillWidth: true
                text: root.fDesc
                placeholderText: qsTr("Marketplace seller account (optional)")
                onTextEdited: root.fDesc = text
            }
        }

        // Strategy picker (edit only; read mode shows it in the header subtitle).
        ColumnLayout {
            visible: root.editing
            Layout.fillWidth: true
            spacing: DesignTokens.spaceXs
            FieldLabel {
                text: qsTr("Strategy")
            }
            GlassComboBox {
                id: strategyCombo
                Layout.fillWidth: true
                model: AppController.actorStrategies
            }
        }

        // ── Config-based strategies: typed, labeled fields per strategy ──
        ColumnLayout {
            Layout.fillWidth: true
            visible: !root.stepBased
            enabled: root.editing
            spacing: DesignTokens.spaceSm
            FieldLabel {
                text: qsTr("Auth config")
            }
            Label {
                visible: root.editing
                Layout.fillWidth: true
                text: qsTr("Values may use {{secret.NAME}} to reference a stored secret.")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontCaption
                wrapMode: Text.WordWrap
            }

            // Basic Auth
            ConfigField {
                visible: root.strategy === "Basic Auth"
                label: qsTr("Username")
                cfgKey: "username"
            }
            ConfigField {
                visible: root.strategy === "Basic Auth"
                label: qsTr("Password")
                cfgKey: "password"
                secret: true
            }

            // API Key
            ConfigField {
                visible: root.strategy === "API Key"
                label: qsTr("Name")
                cfgKey: "name"
                placeholder: qsTr("e.g. X-API-Key")
            }
            ConfigField {
                visible: root.strategy === "API Key"
                label: qsTr("Key")
                cfgKey: "key"
                secret: true
            }
            ConfigCombo {
                visible: root.strategy === "API Key"
                label: qsTr("Location")
                cfgKey: "location"
                values: ["header", "query", "cookie"]
                labels: [qsTr("Header"), qsTr("Query Param"), qsTr("Cookie")]
                defaultValue: "header"
            }

            // OAuth 2.0 (Client Credentials / Password)
            ConfigField {
                visible: root.strategy.indexOf("OAuth 2.0") === 0
                label: qsTr("Token URL")
                cfgKey: "token_url"
                placeholder: qsTr("https://id.example.com/oauth/token")
            }
            ConfigField {
                visible: root.strategy.indexOf("OAuth 2.0") === 0
                label: qsTr("Client ID")
                cfgKey: "client_id"
            }
            ConfigField {
                visible: root.strategy.indexOf("OAuth 2.0") === 0
                label: qsTr("Client Secret")
                cfgKey: "client_secret"
                secret: true
            }
            ConfigField {
                visible: root.strategy === "OAuth 2.0 (Password)"
                label: qsTr("Username")
                cfgKey: "username"
            }
            ConfigField {
                visible: root.strategy === "OAuth 2.0 (Password)"
                label: qsTr("Password")
                cfgKey: "password"
                secret: true
            }
            ConfigField {
                visible: root.strategy.indexOf("OAuth 2.0") === 0
                label: qsTr("Scope")
                cfgKey: "scope"
                placeholder: qsTr("optional — e.g. read write profile")
            }
            ConfigCombo {
                visible: root.strategy.indexOf("OAuth 2.0") === 0
                label: qsTr("Client Auth")
                cfgKey: "client_auth"
                values: ["basic", "body", "none"]
                labels: [qsTr("Send as Basic Auth header"), qsTr("Send in request body"), qsTr("None (Public Client)")]
                defaultValue: "basic"
            }

            // OAuth 1.0 (HMAC-SHA1)
            ConfigField {
                visible: root.strategy === "OAuth 1.0 (HMAC-SHA1)"
                label: qsTr("Consumer Key")
                cfgKey: "consumer_key"
            }
            ConfigField {
                visible: root.strategy === "OAuth 1.0 (HMAC-SHA1)"
                label: qsTr("Consumer Secret")
                cfgKey: "consumer_secret"
                secret: true
            }
            ConfigField {
                visible: root.strategy === "OAuth 1.0 (HMAC-SHA1)"
                label: qsTr("Token")
                cfgKey: "token"
            }
            ConfigField {
                visible: root.strategy === "OAuth 1.0 (HMAC-SHA1)"
                label: qsTr("Token Secret")
                cfgKey: "token_secret"
                secret: true
            }

            // AWS Signature v4
            ConfigField {
                visible: root.strategy === "AWS Signature v4"
                label: qsTr("Access Key")
                cfgKey: "access_key"
            }
            ConfigField {
                visible: root.strategy === "AWS Signature v4"
                label: qsTr("Secret Key")
                cfgKey: "secret_key"
                secret: true
            }
            ConfigField {
                visible: root.strategy === "AWS Signature v4"
                label: qsTr("Region")
                cfgKey: "region"
                placeholder: qsTr("e.g. us-east-1")
            }
            ConfigField {
                visible: root.strategy === "AWS Signature v4"
                label: qsTr("Service")
                cfgKey: "service"
                placeholder: qsTr("e.g. execute-api, s3")
            }
            ConfigField {
                visible: root.strategy === "AWS Signature v4"
                label: qsTr("Session Token")
                cfgKey: "session_token"
                secret: true
                placeholder: qsTr("optional (STS)")
            }

            // Bearer Token
            ConfigField {
                visible: root.strategy === "Bearer Token"
                label: qsTr("Token")
                cfgKey: "token"
                secret: true
                placeholder: qsTr("or {{secret.API_TOKEN}}")
            }

            // JWT Bearer
            ConfigCombo {
                visible: root.strategy === "JWT Bearer"
                label: qsTr("Algorithm")
                cfgKey: "algorithm"
                values: ["HS256", "HS512"]
                labels: ["HS256", "HS512"]
                defaultValue: "HS256"
            }
            ConfigField {
                visible: root.strategy === "JWT Bearer"
                label: qsTr("Secret")
                cfgKey: "secret"
                secret: true
            }
            ConfigField {
                visible: root.strategy === "JWT Bearer"
                label: qsTr("Payload (JSON)")
                cfgKey: "payload"
                placeholder: qsTr("{ \"sub\": \"1234\", \"role\": \"admin\" }")
            }

            // mTLS (Client Cert) — Certificate Format then conditional fields.
            ConfigCombo {
                visible: root.strategy === "mTLS (Client Cert)"
                label: qsTr("Certificate Format")
                cfgKey: "format"
                values: ["pem", "p12"]
                labels: [qsTr("PEM Certificate + Key"), qsTr("PKCS#12 (.p12/.pfx)")]
                defaultValue: "pem"
            }
            // PEM fields
            ConfigFileField {
                visible: root.strategy === "mTLS (Client Cert)" && !root.mtlsIsP12
                label: qsTr("Client Certificate")
                cfgKey: "cert_path"
                placeholder: qsTr("client.pem / client.crt")
                dialogTitle: qsTr("Choose client certificate")
                nameFilter: qsTr("Certificates (*.pem *.crt *.cer)")
            }
            ConfigFileField {
                visible: root.strategy === "mTLS (Client Cert)" && !root.mtlsIsP12
                label: qsTr("Private Key")
                cfgKey: "key_path"
                placeholder: qsTr("client.key")
                dialogTitle: qsTr("Choose private key")
                nameFilter: qsTr("Keys (*.key *.pem)")
            }
            ConfigField {
                visible: root.strategy === "mTLS (Client Cert)" && !root.mtlsIsP12
                label: qsTr("Private Key Passphrase")
                cfgKey: "key_password"
                secret: true
                placeholder: qsTr("optional")
            }
            // PKCS#12 fields
            ConfigFileField {
                visible: root.strategy === "mTLS (Client Cert)" && root.mtlsIsP12
                label: qsTr("PKCS#12 File")
                cfgKey: "cert_path"
                placeholder: qsTr("client.p12 / client.pfx")
                dialogTitle: qsTr("Choose PKCS#12 bundle")
                nameFilter: qsTr("PKCS#12 (*.p12 *.pfx)")
            }
            ConfigField {
                visible: root.strategy === "mTLS (Client Cert)" && root.mtlsIsP12
                label: qsTr("PKCS#12 Password")
                cfgKey: "key_password"
                secret: true
            }
            // Shared optional CA cert
            ConfigFileField {
                visible: root.strategy === "mTLS (Client Cert)"
                label: qsTr("CA Certificate")
                cfgKey: "ca_cert_path"
                placeholder: qsTr("optional — custom CA to trust the server")
                dialogTitle: qsTr("Choose CA certificate")
                nameFilter: qsTr("Certificates (*.pem *.crt *.cer)")
            }
        }

        // ── Step-based login chain (N steps) ──
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.stepBased
            spacing: DesignTokens.spaceMd
            FieldLabel {
                text: qsTr("Login chain")
            }
            Label {
                visible: root.editing
                Layout.fillWidth: true
                text: qsTr("Steps run top to bottom. Each step can extract variables the next steps reference. The last step usually returns the access token.")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontCaption
                wrapMode: Text.WordWrap
            }

            Repeater {
                id: stepsRepeater
                model: AppController.actorAuthSteps
                delegate: ColumnLayout {
                    id: stepRow
                    required property int index
                    required property string method
                    required property string path
                    required property string body
                    required property string expect
                    required property var extractModel
                    Layout.fillWidth: true
                    spacing: DesignTokens.spaceXs

                    // Step header: "Step N" + reorder / remove (edit only).
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: DesignTokens.spaceSm
                        FieldLabel {
                            text: qsTr("Step %1").arg(stepRow.index + 1)
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                        GlassButton {
                            visible: root.editing
                            text: "↑"
                            enabled: stepRow.index > 0
                            onClicked: AppController.actorAuthSteps.moveStep(stepRow.index, -1)
                        }
                        GlassButton {
                            visible: root.editing
                            text: "↓"
                            enabled: stepRow.index < stepsRepeater.count - 1
                            onClicked: AppController.actorAuthSteps.moveStep(stepRow.index, 1)
                        }
                        GlassButton {
                            visible: root.editing
                            text: qsTr("Remove")
                            enabled: stepsRepeater.count > 1
                            onClicked: AppController.actorAuthSteps.removeStep(stepRow.index)
                        }
                    }

                    // Method + path + expect.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: DesignTokens.spaceSm
                        MethodBadge {
                            visible: !root.editing
                            method: stepRow.method
                        }
                        GlassComboBox {
                            visible: root.editing
                            Layout.preferredWidth: 110
                            model: root.methodOptions
                            currentIndex: Math.max(0, root.methodOptions.indexOf(stepRow.method))
                            onActivated: AppController.actorAuthSteps.setMethod(stepRow.index, currentText)
                        }
                        Label {
                            visible: !root.editing
                            Layout.fillWidth: true
                            text: stepRow.path
                            color: DesignTokens.textPrimary
                            font.pixelSize: DesignTokens.fontBody
                            font.family: DesignTokens.fontMono
                            elide: Text.ElideRight
                        }
                        GlassTextField {
                            visible: root.editing
                            Layout.fillWidth: true
                            text: stepRow.path
                            placeholderText: qsTr("/api/v1/auth/login")
                            onTextEdited: AppController.actorAuthSteps.setPath(stepRow.index, text)
                        }
                        Label {
                            visible: !root.editing
                            text: stepRow.expect.length > 0 ? qsTr("expect %1").arg(stepRow.expect) : ""
                            color: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontLabel
                            font.family: DesignTokens.fontMono
                        }
                        GlassTextField {
                            visible: root.editing
                            Layout.preferredWidth: 90
                            text: stepRow.expect
                            placeholderText: qsTr("200")
                            onTextEdited: AppController.actorAuthSteps.setExpect(stepRow.index, text)
                        }
                    }

                    // Body (JSON / form template).
                    FieldLabel {
                        text: qsTr("Body")
                        visible: root.editing || stepRow.body.length > 0
                    }
                    BodyBox {
                        visible: root.editing || stepRow.body.length > 0
                        readOnly: !root.editing
                        text: stepRow.body
                        onTextEdited: AppController.actorAuthSteps.setBody(stepRow.index, text)
                    }

                    // Per-step extractions (variable ← JSONPath).
                    FieldLabel {
                        text: qsTr("Extract (variable ← JSONPath)")
                    }
                    KeyValueEditorView {
                        Layout.fillWidth: true
                        enabled: root.editing
                        kvModel: stepRow.extractModel
                        keyPlaceholder: qsTr("token")
                        valuePlaceholder: qsTr("data.accessToken")
                    }

                    // Divider between steps.
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: DesignTokens.spaceXs
                        implicitHeight: 1
                        color: DesignTokens.borderSubtle
                    }
                }
            }

            GlassButton {
                visible: root.editing
                text: qsTr("Add step")
                onClicked: AppController.actorAuthSteps.addStep()
            }
        }

        // ── Session refresh (single request) ──
        // Read mode shows the block only when a refresh exists; edit mode always
        // offers the toggle so it can be added or removed.
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.stepBased && (root.editing || AppController.actorHasRefresh)
            spacing: DesignTokens.spaceXs
            CheckBox {
                id: refreshCheck
                visible: root.editing
                text: qsTr("Session refresh endpoint")
                checked: root.fRefreshEnabled
                onToggled: root.fRefreshEnabled = checked
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
                visible: root.editing ? root.fRefreshEnabled : AppController.actorHasRefresh
                spacing: DesignTokens.spaceXs
                FieldLabel {
                    text: qsTr("Session refresh")
                    visible: !root.editing
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: DesignTokens.spaceSm
                    MethodBadge {
                        visible: !root.editing
                        method: AppController.actorRefreshMethod
                    }
                    GlassComboBox {
                        visible: root.editing
                        Layout.preferredWidth: 110
                        model: root.methodOptions
                        currentIndex: Math.max(0, root.methodOptions.indexOf(root.fRefreshMethod))
                        onActivated: root.fRefreshMethod = currentText
                    }
                    Label {
                        visible: !root.editing
                        Layout.fillWidth: true
                        text: AppController.actorRefreshPath
                        color: DesignTokens.textPrimary
                        font.pixelSize: DesignTokens.fontBody
                        font.family: DesignTokens.fontMono
                        elide: Text.ElideRight
                    }
                    GlassTextField {
                        visible: root.editing
                        Layout.fillWidth: true
                        text: root.fRefreshPath
                        placeholderText: qsTr("/api/v1/auth/refresh")
                        onTextEdited: root.fRefreshPath = text
                    }
                }
                BodyBox {
                    visible: root.editing || AppController.actorRefreshBody.length > 0
                    readOnly: !root.editing
                    text: root.editing ? root.fRefreshBody : AppController.actorRefreshBody
                    onTextEdited: if (text !== root.fRefreshBody) {
                        root.fRefreshBody = text;
                    }
                }
                FieldLabel {
                    text: qsTr("Extract (variable ← JSONPath)")
                }
                KeyValueEditorView {
                    Layout.fillWidth: true
                    enabled: root.editing
                    kvModel: AppController.actorRefreshExtract
                    keyPlaceholder: qsTr("token")
                    valuePlaceholder: qsTr("data.accessToken")
                }
            }
        }
    }

    // A labeled config field bound to one authConfig key (shared actorConfig
    // model). Disabled state comes from the enclosing ColumnLayout.
    component ConfigField: RowLayout {
        id: cfgFieldRoot
        property string label: ""
        property string cfgKey: ""
        property bool secret: false
        property string placeholder: ""
        Layout.fillWidth: true
        spacing: DesignTokens.spaceMd
        FieldLabel {
            text: cfgFieldRoot.label
            Layout.preferredWidth: 140
        }
        GlassTextField {
            Layout.fillWidth: true
            echoMode: cfgFieldRoot.secret ? TextInput.Password : TextInput.Normal
            placeholderText: cfgFieldRoot.placeholder
            text: AppController.actorConfig[cfgFieldRoot.cfgKey] !== undefined ? AppController.actorConfig[cfgFieldRoot.cfgKey] : ""
            onTextEdited: AppController.setActorConfigValue(cfgFieldRoot.cfgKey, text)
        }
    }

    // A labeled config field with a native file picker (for cert/key paths).
    component ConfigFileField: RowLayout {
        id: cfgFileRoot
        property string label: ""
        property string cfgKey: ""
        property string placeholder: ""
        property string dialogTitle: qsTr("Choose file")
        property string nameFilter: qsTr("All files (*)")
        Layout.fillWidth: true
        spacing: DesignTokens.spaceMd
        FieldLabel {
            text: cfgFileRoot.label
            Layout.preferredWidth: 140
        }
        GlassTextField {
            Layout.fillWidth: true
            placeholderText: cfgFileRoot.placeholder
            text: AppController.actorConfig[cfgFileRoot.cfgKey] !== undefined ? AppController.actorConfig[cfgFileRoot.cfgKey] : ""
            onTextEdited: AppController.setActorConfigValue(cfgFileRoot.cfgKey, text)
        }
        GlassButton {
            text: qsTr("Browse…")
            onClicked: {
                const picked = AppController.pickFile(cfgFileRoot.dialogTitle, cfgFileRoot.nameFilter);
                if (picked.length > 0) {
                    AppController.setActorConfigValue(cfgFileRoot.cfgKey, picked);
                }
            }
        }
    }

    // A labeled config dropdown bound to one authConfig key.
    component ConfigCombo: RowLayout {
        id: cfgComboRoot
        property string label: ""
        property string cfgKey: ""
        property var values: []
        property var labels: []
        property string defaultValue: ""
        Layout.fillWidth: true
        spacing: DesignTokens.spaceMd
        FieldLabel {
            text: cfgComboRoot.label
            Layout.preferredWidth: 140
        }
        GlassComboBox {
            Layout.fillWidth: true
            model: cfgComboRoot.labels
            currentIndex: {
                var v = AppController.actorConfig[cfgComboRoot.cfgKey];
                if (v === undefined || v === "")
                    v = cfgComboRoot.defaultValue;
                return Math.max(0, cfgComboRoot.values.indexOf(v));
            }
            onActivated: AppController.setActorConfigValue(cfgComboRoot.cfgKey, cfgComboRoot.values[currentIndex])
        }
    }

    // Bordered body editor: read-only preview or editable, gated by readOnly.
    component BodyBox: Rectangle {
        id: bodyBox
        property alias text: bodyArea.text
        property alias readOnly: bodyArea.readOnly
        signal textEdited(string text)
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
            // Fire only on user edits; onTextChanged also fires on binding-
            // driven updates (rebuild/reorder), which would spuriously re-invoke
            // the setter and, if a transform were added, risk a binding loop.
            onTextChanged: if (bodyArea.activeFocus) {
                bodyBox.textEdited(text);
            }
        }
    }
}
