// RequestEditor — request view + editor for the selected endpoint (ADR-007
// migration phase 3, WS-B). Read mode previews the request (method pill,
// {{var}}-highlighted path, execution chain, Headers / Params / Body / Chain).
// Edit mode reveals editable controls (method combo, path field, Params /
// Headers / Body raw↔form / Options / Chain) with live per-tab count badges.
// Send applies edits to a one-shot run; Save persists them to the project.
// C++ (AppController) owns all state + logic; this file is presentation only.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

ColumnLayout {
    id: editor
    spacing: DesignTokens.spaceMd

    readonly property bool editing: AppController.editing

    // Render a path template with {{variable}} segments tinted (display-only
    // string formatting, mirrors the old highlightVariables helper).
    function escapeHtml(s) {
        return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }
    function highlightPath(path) {
        const accent = "" + DesignTokens.statusWarning;
        let out = "";
        let i = 0;
        while (i < path.length) {
            const open = path.indexOf("{{", i);
            if (open < 0) {
                out += escapeHtml(path.substring(i));
                break;
            }
            out += escapeHtml(path.substring(i, open));
            const close = path.indexOf("}}", open);
            if (close < 0) {
                out += escapeHtml(path.substring(open));
                break;
            }
            const v = path.substring(open, close + 2);
            out += "<span style='color:" + accent + "; font-weight:600;'>" + escapeHtml(v) + "</span>";
            i = close + 2;
        }
        return out;
    }

    // ── Breadcrumb + actor chip ──
    RowLayout {
        Layout.fillWidth: true
        spacing: DesignTokens.spaceSm

        Button {
            text: "←"
            implicitWidth: 32
            implicitHeight: 32
            background: Rectangle {
                radius: DesignTokens.radiusSm
                color: parent.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                border.width: 1
                border.color: DesignTokens.borderSubtle
            }
            contentItem: Text {
                text: parent.text
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontSubtitle
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: AppController.closeOperation()
        }
        Label {
            text: AppController.selectedModule + " /"
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontBody
        }
        Label {
            text: AppController.opName
            color: DesignTokens.textPrimary
            font.pixelSize: DesignTokens.fontBody
            font.weight: DesignTokens.weightSemiBold
        }
        Item {
            Layout.fillWidth: true
        }
        Rectangle {
            implicitHeight: 26
            implicitWidth: actorRow.implicitWidth + DesignTokens.spaceMd * 2
            radius: DesignTokens.radiusPill
            color: DesignTokens.surfaceSunken
            border.width: 1
            border.color: DesignTokens.borderSubtle
            RowLayout {
                id: actorRow
                anchors.centerIn: parent
                spacing: DesignTokens.spaceXs
                AppIcon {
                    name: "user"
                    size: 14
                    opacity: AppController.opActor.length > 0 ? 1.0 : 0.5
                }
                Label {
                    text: AppController.opActor.length > 0 ? AppController.opActor : qsTr("No actor")
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontLabel
                }
            }
        }
    }

    // ── Address bar: method + path + Send ──
    RowLayout {
        Layout.fillWidth: true
        spacing: DesignTokens.spaceSm

        MethodBadge {
            visible: !editor.editing
            method: AppController.opMethod
            implicitHeight: 38
            Layout.preferredWidth: 64
        }
        GlassComboBox {
            id: methodCombo
            visible: editor.editing
            implicitHeight: 38
            Layout.preferredWidth: 110
            model: ["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"]
            currentIndex: Math.max(0, find(AppController.editMethod))
            onActivated: AppController.editMethod = currentText
        }

        // Path: highlighted preview (read) ↔ editable field (edit).
        Rectangle {
            visible: !editor.editing
            Layout.fillWidth: true
            height: 38
            radius: DesignTokens.radiusSm
            color: DesignTokens.surfaceSunken
            border.width: 1
            border.color: DesignTokens.borderSubtle
            clip: true
            Flickable {
                anchors.fill: parent
                anchors.leftMargin: DesignTokens.spaceMd
                anchors.rightMargin: DesignTokens.spaceMd
                contentWidth: pathPreview.implicitWidth
                contentHeight: height
                flickableDirection: Flickable.HorizontalFlick
                clip: true
                Text {
                    id: pathPreview
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    textFormat: Text.RichText
                    text: editor.highlightPath(AppController.opPath)
                    color: DesignTokens.textPrimary
                    font.pixelSize: DesignTokens.fontBody
                    font.family: DesignTokens.fontMono
                }
            }
        }
        TextField {
            id: pathField
            visible: editor.editing
            Layout.fillWidth: true
            implicitHeight: 38
            text: AppController.editPath
            placeholderText: qsTr("/api/v1/…")
            color: DesignTokens.textPrimary
            placeholderTextColor: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontBody
            font.family: DesignTokens.fontMono
            onTextEdited: AppController.editPath = text
            background: Rectangle {
                radius: DesignTokens.radiusSm
                color: DesignTokens.surfaceSunken
                border.width: 1
                border.color: pathField.activeFocus ? DesignTokens.accent : DesignTokens.borderSubtle
            }
        }

        Button {
            id: sendButton
            text: AppController.running ? qsTr("Running…") : qsTr("Send")
            enabled: !AppController.running
            implicitWidth: 96
            implicitHeight: 38
            background: Rectangle {
                radius: DesignTokens.radiusSm
                color: !sendButton.enabled ? DesignTokens.borderStrong : sendButton.down ? DesignTokens.accentHover : DesignTokens.accent
            }
            contentItem: Text {
                text: sendButton.text
                color: DesignTokens.textInverse
                font.pixelSize: DesignTokens.fontBody
                font.weight: DesignTokens.weightSemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: editor.editing ? AppController.applyAndRun(false, false) : AppController.runSelected(false, false)
        }
    }

    // ── Secondary actions: Edit toggle + Dry Run / Send Cleanly / Save ──
    RowLayout {
        Layout.fillWidth: true
        spacing: DesignTokens.spaceSm

        // Read mode: enter editing.
        Button {
            id: editButton
            visible: !editor.editing
            text: qsTr("✎  Edit")
            implicitHeight: 32
            leftPadding: DesignTokens.spaceMd
            rightPadding: DesignTokens.spaceMd
            background: Rectangle {
                radius: DesignTokens.radiusSm
                color: editButton.down ? Qt.rgba(1, 1, 1, 0.08) : "transparent"
                border.width: 1
                border.color: DesignTokens.borderStrong
            }
            contentItem: Text {
                text: editButton.text
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontBody
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: AppController.beginEdit()
        }

        // Edit mode: Save commits the edits to the project (persists + exits
        // edit mode; a chain cycle is rejected and edit mode stays open).
        Button {
            id: saveButton
            visible: editor.editing
            enabled: !AppController.running
            text: qsTr("✓  Save")
            implicitHeight: 32
            leftPadding: DesignTokens.spaceMd
            rightPadding: DesignTokens.spaceMd
            background: Rectangle {
                radius: DesignTokens.radiusSm
                color: !saveButton.enabled ? DesignTokens.borderStrong : saveButton.down ? DesignTokens.accentHover : DesignTokens.accent
            }
            contentItem: Text {
                text: saveButton.text
                color: DesignTokens.textInverse
                font.pixelSize: DesignTokens.fontBody
                font.weight: DesignTokens.weightSemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: AppController.saveOperation()
        }
        // Edit mode: Cancel discards the edits.
        Button {
            id: cancelButton
            visible: editor.editing
            text: qsTr("Cancel")
            implicitHeight: 32
            leftPadding: DesignTokens.spaceMd
            rightPadding: DesignTokens.spaceMd
            background: Rectangle {
                radius: DesignTokens.radiusSm
                color: cancelButton.down ? Qt.rgba(1, 1, 1, 0.08) : "transparent"
                border.width: 1
                border.color: DesignTokens.borderStrong
            }
            contentItem: Text {
                text: cancelButton.text
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontBody
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: AppController.cancelEdit()
        }

        Item {
            Layout.fillWidth: true
        }

        SecondaryButton {
            text: qsTr("Dry Run")
            onClicked: editor.editing ? AppController.applyAndRun(false, true) : AppController.runSelected(false, true)
        }
        SecondaryButton {
            text: qsTr("Send Cleanly")
            onClicked: editor.editing ? AppController.applyAndRun(true, false) : AppController.runSelected(true, false)
        }
    }

    component SecondaryButton: Button {
        id: secondary
        enabled: !AppController.running
        implicitHeight: 32
        leftPadding: DesignTokens.spaceMd
        rightPadding: DesignTokens.spaceMd
        background: Rectangle {
            radius: DesignTokens.radiusSm
            color: secondary.down ? Qt.rgba(1, 1, 1, 0.08) : "transparent"
            border.width: 1
            border.color: DesignTokens.borderStrong
        }
        contentItem: Text {
            text: secondary.text
            color: secondary.enabled ? DesignTokens.textSecondary : DesignTokens.borderStrong
            font.pixelSize: DesignTokens.fontBody
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    // ── Edit banner (edit mode only) ──
    Rectangle {
        Layout.fillWidth: true
        visible: editor.editing
        radius: DesignTokens.radiusSm
        color: Qt.rgba(DesignTokens.statusWarning.r, DesignTokens.statusWarning.g, DesignTokens.statusWarning.b, 0.12)
        border.width: 1
        border.color: DesignTokens.statusWarning
        implicitHeight: bannerLabel.implicitHeight + DesignTokens.spaceMd
        Label {
            id: bannerLabel
            anchors.fill: parent
            anchors.leftMargin: DesignTokens.spaceMd
            anchors.rightMargin: DesignTokens.spaceMd
            verticalAlignment: Text.AlignVCenter
            text: qsTr("Editing — Send applies changes to the next run; Save writes them to the project.")
            color: DesignTokens.statusWarning
            font.pixelSize: DesignTokens.fontLabel
            wrapMode: Text.WordWrap
        }
    }

    // ── Execution chain (visual preview; live in edit mode) ──
    ColumnLayout {
        Layout.fillWidth: true
        spacing: DesignTokens.spaceXs

        Label {
            text: qsTr("EXECUTION CHAIN")
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontCaption
            font.weight: DesignTokens.weightSemiBold
            font.letterSpacing: 1.2
        }
        ChainView {
            Layout.fillWidth: true
            Layout.maximumHeight: 160
            nodes: AppController.chainNodes
            emptyText: qsTr("No declared dependencies — run Dry Run for the full resolved chain.")
        }
    }

    // ── Read-mode tabs ──
    ColumnLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: !editor.editing
        spacing: DesignTokens.spaceSm

        LineTabBar {
            id: readTabs
            Layout.fillWidth: true
            model: [qsTr("Headers"), qsTr("Params"), qsTr("Body"), qsTr("Chain")]
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: readTabs.currentIndex

            KeyValueList {
                model: AppController.opHeaders
                emptyText: qsTr("No headers.")
            }
            KeyValueList {
                model: AppController.opQuery
                emptyText: qsTr("No query parameters.")
            }
            SelectableTextBox {
                text: AppController.opBody
                placeholder: qsTr("No request body.")
            }
            KeyValueList {
                model: AppController.opExtractions
                emptyText: qsTr("No extractions.")
            }
        }
    }

    // ── Edit-mode tabs (Params / Headers / Body / Options / Chain) ──
    ColumnLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: editor.editing
        spacing: DesignTokens.spaceSm

        LineTabBar {
            id: editTabs
            Layout.fillWidth: true
            model: [AppController.editParamsCount > 0 ? qsTr("Params  %1").arg(AppController.editParamsCount) : qsTr("Params"), AppController.editHeadersCount > 0 ? qsTr("Headers  %1").arg(AppController.editHeadersCount) : qsTr("Headers"), AppController.editBodyFilled ? qsTr("Body  ●") : qsTr("Body"), qsTr("Options"), AppController.editChainCount > 0 ? qsTr("Chain  %1").arg(AppController.editChainCount) : qsTr("Chain")]
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: editTabs.currentIndex

            // Params
            ScrollView {
                id: paramsScroll
                clip: true
                contentWidth: availableWidth
                KeyValueEditorView {
                    width: paramsScroll.availableWidth
                    kvModel: AppController.editQuery
                }
            }
            // Headers
            ScrollView {
                id: headersScroll
                clip: true
                contentWidth: availableWidth
                KeyValueEditorView {
                    width: headersScroll.availableWidth
                    kvModel: AppController.editHeaders
                }
            }
            // Body: type selector (none / form-data / x-www-form-urlencoded /
            // JSON / XML / Text / GraphQL) + the matching editor.
            ColumnLayout {
                id: bodyBox
                spacing: DesignTokens.spaceSm

                // GraphQL is stored as a JSON body {query, variables}; these
                // hold the split halves while the GraphQL editor is shown.
                property string gqlQuery: ""
                property string gqlVars: ""

                function bodyPageIndex(t) {
                    if (t === "none")
                        return 0;
                    if (t === "form-data" || t === "x-www-form-urlencoded")
                        return 1;
                    if (t === "graphql")
                        return 3;
                    return 2;
                }
                function rawPlaceholder(t) {
                    if (t === "xml")
                        return "<root>\n</root>";
                    if (t === "text")
                        return qsTr("plain text body");
                    return "{ }";
                }
                function loadGraphql() {
                    try {
                        const o = JSON.parse(AppController.editBody);
                        bodyBox.gqlQuery = o.query || "";
                        bodyBox.gqlVars = o.variables ? JSON.stringify(o.variables, null, 2) : "";
                    } catch (e) {
                        bodyBox.gqlQuery = AppController.editBody;
                        bodyBox.gqlVars = "";
                    }
                }
                function syncGraphql() {
                    let vars = {};
                    try {
                        vars = bodyBox.gqlVars.trim().length > 0 ? JSON.parse(bodyBox.gqlVars) : {};
                    } catch (e) {
                        vars = {};
                    }
                    AppController.editBody = JSON.stringify({
                        query: bodyBox.gqlQuery,
                        variables: vars
                    });
                }

                // Body-type pills.
                Flow {
                    Layout.fillWidth: true
                    spacing: DesignTokens.spaceXs
                    Repeater {
                        model: [
                            {
                                id: "none",
                                label: qsTr("none")
                            },
                            {
                                id: "form-data",
                                label: qsTr("form-data")
                            },
                            {
                                id: "x-www-form-urlencoded",
                                label: qsTr("x-www-form-urlencoded")
                            },
                            {
                                id: "json",
                                label: qsTr("JSON")
                            },
                            {
                                id: "xml",
                                label: qsTr("XML")
                            },
                            {
                                id: "text",
                                label: qsTr("Text")
                            },
                            {
                                id: "graphql",
                                label: qsTr("GraphQL")
                            }
                        ]
                        delegate: Button {
                            id: bodyPill
                            required property var modelData
                            text: bodyPill.modelData.label
                            checkable: true
                            checked: AppController.editBodyType === bodyPill.modelData.id
                            implicitHeight: 26
                            leftPadding: DesignTokens.spaceMd
                            rightPadding: DesignTokens.spaceMd
                            onClicked: {
                                AppController.editBodyType = bodyPill.modelData.id;
                                if (bodyPill.modelData.id === "graphql") {
                                    bodyBox.loadGraphql();
                                }
                            }
                            background: Rectangle {
                                radius: DesignTokens.radiusPill
                                color: bodyPill.checked ? DesignTokens.accent : (bodyPill.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent")
                                border.width: 1
                                border.color: bodyPill.checked ? DesignTokens.accent : DesignTokens.borderSubtle
                            }
                            contentItem: Text {
                                text: bodyPill.text
                                color: bodyPill.checked ? DesignTokens.textInverse : DesignTokens.textSecondary
                                font.pixelSize: DesignTokens.fontLabel
                                font.weight: bodyPill.checked ? DesignTokens.weightSemiBold : DesignTokens.weightRegular
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: bodyBox.bodyPageIndex(AppController.editBodyType)

                    // 0 — none
                    Item {
                        Label {
                            anchors.centerIn: parent
                            text: qsTr("This request does not have a body.")
                            color: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontBody
                        }
                    }

                    // 1 — form-data / x-www-form-urlencoded
                    ScrollView {
                        id: formScroll
                        clip: true
                        contentWidth: availableWidth
                        KeyValueEditorView {
                            width: formScroll.availableWidth
                            kvModel: AppController.editForm
                            allowFiles: AppController.editBodyType === "form-data"
                            keyPlaceholder: qsTr("field")
                            valuePlaceholder: AppController.editBodyType === "form-data" ? qsTr("value  (or attach a file)") : qsTr("value")
                        }
                    }

                    // 2 — raw (JSON / XML / Text)
                    Rectangle {
                        radius: DesignTokens.radiusSm
                        color: DesignTokens.surfaceSunken
                        border.width: 1
                        border.color: DesignTokens.borderSubtle
                        ScrollView {
                            anchors.fill: parent
                            anchors.margins: DesignTokens.spaceXs
                            clip: true
                            TextArea {
                                id: rawBody
                                text: AppController.editBody
                                placeholderText: bodyBox.rawPlaceholder(AppController.editBodyType)
                                color: DesignTokens.textPrimary
                                placeholderTextColor: DesignTokens.textSecondary
                                font.pixelSize: DesignTokens.fontLabel
                                font.family: DesignTokens.fontMono
                                wrapMode: TextEdit.WrapAnywhere
                                onTextChanged: if (text !== AppController.editBody) {
                                    AppController.editBody = text;
                                }
                                background: null
                            }
                        }
                    }

                    // 3 — GraphQL (query + variables → JSON body)
                    ColumnLayout {
                        spacing: DesignTokens.spaceXs
                        Component.onCompleted: bodyBox.loadGraphql()
                        FieldLabel {
                            text: qsTr("Query")
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            radius: DesignTokens.radiusSm
                            color: DesignTokens.surfaceSunken
                            border.width: 1
                            border.color: DesignTokens.borderSubtle
                            ScrollView {
                                anchors.fill: parent
                                anchors.margins: DesignTokens.spaceXs
                                clip: true
                                TextArea {
                                    text: bodyBox.gqlQuery
                                    placeholderText: qsTr("query { … }")
                                    color: DesignTokens.textPrimary
                                    placeholderTextColor: DesignTokens.textSecondary
                                    font.pixelSize: DesignTokens.fontLabel
                                    font.family: DesignTokens.fontMono
                                    wrapMode: TextEdit.WrapAnywhere
                                    onTextChanged: if (text !== bodyBox.gqlQuery) {
                                        bodyBox.gqlQuery = text;
                                        bodyBox.syncGraphql();
                                    }
                                    background: null
                                }
                            }
                        }
                        FieldLabel {
                            text: qsTr("Variables (JSON)")
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 96
                            radius: DesignTokens.radiusSm
                            color: DesignTokens.surfaceSunken
                            border.width: 1
                            border.color: DesignTokens.borderSubtle
                            ScrollView {
                                anchors.fill: parent
                                anchors.margins: DesignTokens.spaceXs
                                clip: true
                                TextArea {
                                    text: bodyBox.gqlVars
                                    placeholderText: "{ }"
                                    color: DesignTokens.textPrimary
                                    placeholderTextColor: DesignTokens.textSecondary
                                    font.pixelSize: DesignTokens.fontLabel
                                    font.family: DesignTokens.fontMono
                                    wrapMode: TextEdit.WrapAnywhere
                                    onTextChanged: if (text !== bodyBox.gqlVars) {
                                        bodyBox.gqlVars = text;
                                        bodyBox.syncGraphql();
                                    }
                                    background: null
                                }
                            }
                        }
                    }
                }
            }
            // Options
            ScrollView {
                id: optScroll
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: optScroll.availableWidth
                    spacing: DesignTokens.spaceMd

                    OptionRow {
                        label: qsTr("Actor")
                        GlassComboBox {
                            id: actorCombo
                            width: parent.width
                            model: [qsTr("(none)")].concat(AppController.actorNames)
                            currentIndex: AppController.editActor.length === 0 ? 0 : Math.max(0, find(AppController.editActor))
                            onActivated: AppController.editActor = (currentIndex === 0 ? "" : currentText)
                        }
                    }
                    OptionRow {
                        label: qsTr("Expect status")
                        TextField {
                            id: expectField
                            width: parent.width
                            implicitHeight: 34
                            text: AppController.editExpectStatus
                            placeholderText: qsTr("e.g. 200,201")
                            color: DesignTokens.textPrimary
                            placeholderTextColor: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontLabel
                            onTextEdited: AppController.editExpectStatus = text
                            background: Rectangle {
                                radius: DesignTokens.radiusSm
                                color: DesignTokens.surfaceSunken
                                border.width: 1
                                border.color: expectField.activeFocus ? DesignTokens.accent : DesignTokens.borderSubtle
                            }
                        }
                    }
                    OptionRow {
                        label: qsTr("Timeout (ms)")
                        SpinBox {
                            id: timeoutSpin
                            width: parent.width
                            height: 34
                            from: 0
                            to: 600000
                            stepSize: 500
                            value: AppController.editTimeout
                            editable: true
                            onValueModified: AppController.editTimeout = value
                            textFromValue: function (v) {
                                return v === 0 ? qsTr("default") : v + " ms";
                            }
                            valueFromText: function (t) {
                                const n = parseInt(t.replace(/[^0-9]/g, ""), 10);
                                return isNaN(n) ? 0 : n;
                            }
                            background: Rectangle {
                                radius: DesignTokens.radiusSm
                                color: DesignTokens.surfaceSunken
                                border.width: 1
                                border.color: DesignTokens.borderSubtle
                            }
                            contentItem: TextInput {
                                leftPadding: 40
                                rightPadding: 40
                                text: timeoutSpin.displayText
                                color: DesignTokens.textPrimary
                                font.pixelSize: DesignTokens.fontLabel
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                readOnly: !timeoutSpin.editable
                                validator: timeoutSpin.validator
                                onEditingFinished: timeoutSpin.value = timeoutSpin.valueFromText(text)
                            }
                            down.indicator: Rectangle {
                                x: 0
                                y: 0
                                width: 36
                                height: timeoutSpin.height
                                radius: DesignTokens.radiusSm
                                color: timeoutSpin.down.pressed ? DesignTokens.accentMuted : "transparent"
                                border.width: 1
                                border.color: DesignTokens.borderSubtle
                                Text {
                                    anchors.centerIn: parent
                                    text: "\u2212"
                                    color: DesignTokens.textSecondary
                                    font.pixelSize: DesignTokens.fontSubtitle
                                }
                            }
                            up.indicator: Rectangle {
                                x: timeoutSpin.width - width
                                y: 0
                                width: 36
                                height: timeoutSpin.height
                                radius: DesignTokens.radiusSm
                                color: timeoutSpin.up.pressed ? DesignTokens.accentMuted : "transparent"
                                border.width: 1
                                border.color: DesignTokens.borderSubtle
                                Text {
                                    anchors.centerIn: parent
                                    text: "+"
                                    color: DesignTokens.textSecondary
                                    font.pixelSize: DesignTokens.fontSubtitle
                                }
                            }
                        }
                    }
                    OptionRow {
                        label: ""
                        CheckBox {
                            id: forceCheck
                            text: qsTr("Force re-run (ignore extraction cache)")
                            checked: AppController.editForce
                            onToggled: AppController.editForce = checked
                            indicator: Rectangle {
                                implicitWidth: 18
                                implicitHeight: 18
                                radius: 4
                                x: 0
                                y: (forceCheck.height - height) / 2
                                color: forceCheck.checked ? DesignTokens.accent : DesignTokens.surfaceSunken
                                border.width: 1
                                border.color: forceCheck.checked ? DesignTokens.accent : DesignTokens.borderStrong
                                Text {
                                    anchors.centerIn: parent
                                    visible: forceCheck.checked
                                    text: "\u2713"
                                    color: DesignTokens.textInverse
                                    font.pixelSize: DesignTokens.fontLabel
                                    font.weight: DesignTokens.weightBold
                                }
                            }
                            contentItem: Text {
                                leftPadding: forceCheck.indicator.width + DesignTokens.spaceSm
                                text: forceCheck.text
                                color: DesignTokens.textPrimary
                                font.pixelSize: DesignTokens.fontLabel
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }
            }
            // Chain
            ScrollView {
                id: chainScroll
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: chainScroll.availableWidth
                    spacing: DesignTokens.spaceSm
                    Label {
                        text: qsTr("Depends on")
                        color: DesignTokens.textPrimary
                        font.pixelSize: DesignTokens.fontLabel
                        font.weight: DesignTokens.weightSemiBold
                    }
                    DependencyEditor {
                        Layout.fillWidth: true
                        depModel: AppController.editDependencies
                        candidates: AppController.editDependencyCandidates
                    }
                    Label {
                        text: qsTr("Extract")
                        color: DesignTokens.textPrimary
                        font.pixelSize: DesignTokens.fontLabel
                        font.weight: DesignTokens.weightSemiBold
                    }
                    ExtractionEditor {
                        Layout.fillWidth: true
                        extractModel: AppController.editExtractions
                    }
                }
            }
        }
    }

    component OptionRow: RowLayout {
        property string label: ""
        default property alias fieldData: holder.data
        Layout.fillWidth: true
        spacing: DesignTokens.spaceMd
        Label {
            Layout.preferredWidth: 110
            text: parent.label
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontLabel
        }
        Item {
            id: holder
            Layout.fillWidth: true
            implicitHeight: childrenRect.height
        }
    }
}
