// ActorDetail — actor (auth identity) detail in the centre pane. Shows a
// read-only view by default; the "Edit" button flips the same panel into an
// inline editable form (no modal) with Save/Cancel. Step-based strategies edit
// a full N-step login chain (AppController.actorAuthSteps): Add/Remove/reorder
// steps, each with its own method/path/body/expect + extractions. Seeded by
// selectActor→prepareEditActor; Save persists via saveActorInline. New-actor
// creation still uses ActorDialog (no panel to edit before the actor exists).
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
                onClicked: root.cancelEdit()
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
