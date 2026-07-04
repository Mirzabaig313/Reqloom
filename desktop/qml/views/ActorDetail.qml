// ActorDetail — actor (auth identity) detail in the centre pane. Shows a
// read-only view by default; the "Edit" button flips the same panel into an
// inline editable form (no modal) with Save/Cancel. Seeded from AppController's
// actor* properties (set by selectActor→prepareEditActor); Save persists via
// saveActorEdits. New-actor creation still uses ActorDialog (no panel to edit
// before the actor exists).
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

ScrollView {
    id: root
    clip: true
    contentWidth: availableWidth

    property bool editing: false

    // Editable working copies (seeded on beginEdit, sent back on save). The
    // key/value tables are edited directly on the shared AppController models
    // (already populated by prepareEditActor), so they need no local copy.
    property string fName: ""
    property string fDesc: ""
    property string fAuthMethod: "POST"
    property string fAuthPath: ""
    property string fAuthBody: ""
    property string fAuthExpect: ""
    property bool fRefreshEnabled: false
    property string fRefreshMethod: "POST"
    property string fRefreshPath: ""
    property string fRefreshBody: ""

    // Step-based strategies carry "login" in their label. In edit mode the live
    // combo selection drives it; in read mode the selected actor's strategy.
    readonly property bool stepBased: editing ? (strategyCombo.currentText.indexOf("login") >= 0) : (AppController.selectedActorStrategy.indexOf("login") >= 0)
    readonly property bool nameValid: AppController.isValidName(fName)

    // Switching to a different actor (or a post-save re-seed) discards any
    // in-progress inline edit — the panel always reflects the current actor.
    // ponytail: no "unsaved changes" prompt; a mis-click loses edits. Upgrade
    // path: guard with a confirm dialog if users report lost work.
    Connections {
        target: AppController
        function onActorSelectionChanged() {
            root.editing = false;
        }
    }

    function beginEdit() {
        fName = AppController.selectedActorName;
        fDesc = AppController.selectedActorDescription;
        fAuthMethod = AppController.actorAuthMethod;
        fAuthPath = AppController.actorAuthPath;
        fAuthBody = AppController.actorAuthBody;
        fAuthExpect = AppController.actorAuthExpect;
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

    function saveEdits() {
        // originalId stays the pre-edit name (== id in this project) so a rename
        // still targets the right actor; only close edit mode when accepted.
        if (AppController.saveActorEdits(AppController.selectedActorName, fName.trim(), strategyCombo.currentText, fDesc.trim(), fAuthMethod, fAuthPath, fAuthBody, fAuthExpect, fRefreshEnabled, fRefreshMethod, fRefreshPath, fRefreshBody)) {
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
                visible: !root.editing
                text: qsTr("Edit")
                primary: true
                onClicked: root.beginEdit()
            }
            GlassButton {
                visible: root.editing
                text: qsTr("Cancel")
                onClicked: root.editing = false
            }
            GlassButton {
                visible: root.editing
                text: qsTr("Save")
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

        // ── Config-based strategies ──
        ColumnLayout {
            Layout.fillWidth: true
            visible: !root.stepBased
            spacing: DesignTokens.spaceXs
            FieldLabel {
                text: qsTr("Auth config")
            }
            KeyValueEditorView {
                Layout.fillWidth: true
                enabled: root.editing
                kvModel: AppController.editActorConfig
                keyPlaceholder: qsTr("key")
                valuePlaceholder: qsTr("value  (use {{secret.NAME}})")
            }
        }

        // ── Step-based login request ──
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.stepBased
            spacing: DesignTokens.spaceXs
            FieldLabel {
                text: qsTr("Login request")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: DesignTokens.spaceSm
                MethodBadge {
                    visible: !root.editing
                    method: AppController.actorAuthMethod
                }
                GlassComboBox {
                    visible: root.editing
                    Layout.preferredWidth: 110
                    model: ["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"]
                    currentIndex: Math.max(0, find(root.fAuthMethod))
                    onActivated: root.fAuthMethod = currentText
                }
                Label {
                    visible: !root.editing
                    Layout.fillWidth: true
                    text: AppController.actorAuthPath
                    color: DesignTokens.textPrimary
                    font.pixelSize: DesignTokens.fontBody
                    font.family: DesignTokens.fontMono
                    elide: Text.ElideRight
                }
                GlassTextField {
                    visible: root.editing
                    Layout.fillWidth: true
                    text: root.fAuthPath
                    placeholderText: qsTr("/api/v1/auth/login")
                    onTextEdited: root.fAuthPath = text
                }
                Label {
                    visible: !root.editing
                    text: AppController.actorAuthExpect.length > 0 ? qsTr("expect %1").arg(AppController.actorAuthExpect) : ""
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontLabel
                    font.family: DesignTokens.fontMono
                }
                GlassTextField {
                    visible: root.editing
                    Layout.preferredWidth: 90
                    text: root.fAuthExpect
                    placeholderText: qsTr("200")
                    onTextEdited: root.fAuthExpect = text
                }
            }
            FieldLabel {
                text: qsTr("Body")
                visible: root.editing || AppController.actorAuthBody.length > 0
            }
            BodyBox {
                visible: root.editing || AppController.actorAuthBody.length > 0
                readOnly: !root.editing
                text: root.editing ? root.fAuthBody : AppController.actorAuthBody
                onTextEdited: if (text !== root.fAuthBody) {
                    root.fAuthBody = text;
                }
            }
            FieldLabel {
                text: qsTr("Extract (variable ← JSONPath)")
            }
            Label {
                visible: root.editing
                Layout.fillWidth: true
                text: qsTr("The leading $. is optional — “data.accessToken” works too.")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontCaption
                wrapMode: Text.WordWrap
            }
            KeyValueEditorView {
                Layout.fillWidth: true
                enabled: root.editing
                kvModel: AppController.actorAuthExtract
                keyPlaceholder: qsTr("token")
                valuePlaceholder: qsTr("data.accessToken")
            }

            // ── Refresh ──
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
                        model: ["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"]
                        currentIndex: Math.max(0, find(root.fRefreshMethod))
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
            onTextChanged: bodyBox.textEdited(text)
        }
    }
}
