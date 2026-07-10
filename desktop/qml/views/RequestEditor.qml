// RequestEditor — request view + editor for the selected endpoint (ADR-007
// migration phase 3, WS-B). Read mode previews the request (method pill,
// {{var}}-highlighted path, execution chain, Headers / Params / Body / Chain).
// Edit mode reveals editable controls (method combo, path field, Params /
// Headers / Body raw↔form / Options / Chain) with live per-tab count badges.
// Send applies edits to a one-shot run; Save persists them to the project.
// C++ (AppController) owns all state + logic; this file is presentation only.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

ColumnLayout {
    id: editor
    spacing: DesignTokens.spaceMd

    readonly property bool editing: AppController.editing

    // Pick a syntax-highlight language for a request body by sniffing its first
    // non-space char (JSON object/array, or XML/HTML tag); defaults to JSON.
    function bodyLanguage(body) {
        const t = ("" + body).trim();
        if (t.length === 0)
            return "json";
        const c = t.charAt(0);
        if (c === "{" || c === "[")
            return "json";
        if (c === "<")
            return t.toLowerCase().indexOf("<html") >= 0 || t.indexOf("<!") === 0 ? "html" : "xml";
        return "json";
    }

    // ── Breadcrumb + actor chip ──
    RowLayout {
        Layout.fillWidth: true
        spacing: DesignTokens.spaceSm

        Button {
            id: backBtn
            text: "←"
            implicitWidth: 32
            implicitHeight: 32
            background: Rectangle {
                radius: DesignTokens.radiusSm
                color: backBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                border.width: 1
                border.color: DesignTokens.borderSubtle
            }
            contentItem: Text {
                text: backBtn.text
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontSubtitle
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: AppController.closeOperation()
        }
        Label {
            id: moduleCrumb
            text: AppController.selectedModule
            color: moduleCrumbHover.hovered ? DesignTokens.accent : DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontBody
            font.underline: moduleCrumbHover.hovered
            Behavior on color {
                ColorMotion {}
            }
            // Clickable breadcrumb segment: jump back to this module's endpoint
            // list (same as the back arrow), so the trail is navigable, not decor.
            HoverHandler {
                id: moduleCrumbHover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                onTapped: AppController.closeOperation()
            }
            GlassToolTip {
                active: moduleCrumbHover.hovered
                text: qsTr("Back to %1").arg(AppController.selectedModule)
            }
        }
        Label {
            text: "/"
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
    // Constraint-solved row (LayoutSolver) instead of magic pixel widths: the
    // method slot sizes to its widest item, Send to its widest label, and the
    // path fills the remainder. See doc/local/UI_improment.md §3.
    Item {
        id: addressBar
        Layout.fillWidth: true
        implicitHeight: 38

        readonly property var methodItems: ["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"]
        // Method slot = widest method label + chevron + field padding.
        readonly property real methodSlot: Math.max(72, LayoutSolver.contentWidth(addressBar.methodItems, DesignTokens.fontLabel, DesignTokens.fontSans, 40))
        // Send slot = widest of its labels + button padding.
        readonly property real sendSlot: Math.max(90, LayoutSolver.contentWidth([qsTr("Running…"), qsTr("Send")], DesignTokens.fontBody, DesignTokens.fontSans, 44))

        readonly property var solved: LayoutSolver.solveLinear([({
                    "minimum": addressBar.methodSlot,
                    "preferred": addressBar.methodSlot,
                    "maximum": addressBar.methodSlot,
                    "stretch": 0
                }), ({
                    "minimum": 140,
                    "preferred": 140,
                    "stretch": 1
                }), ({
                    "minimum": addressBar.sendSlot,
                    "preferred": addressBar.sendSlot,
                    "maximum": addressBar.sendSlot,
                    "stretch": 0
                })], addressBar.width, DesignTokens.spaceSm)

        readonly property var methodGeom: addressBar.solved[0] || ({
                "offset": 0,
                "size": addressBar.methodSlot
            })
        readonly property var pathGeom: addressBar.solved[1] || ({
                "offset": 0,
                "size": 0
            })
        readonly property var sendGeom: addressBar.solved[2] || ({
                "offset": 0,
                "size": addressBar.sendSlot
            })

        MethodBadge {
            visible: !editor.editing
            x: addressBar.methodGeom.offset
            width: addressBar.methodGeom.size
            height: 38
            method: AppController.opMethod
        }
        GlassComboBox {
            id: methodCombo
            visible: editor.editing
            x: addressBar.methodGeom.offset
            width: addressBar.methodGeom.size
            height: 38
            model: ["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"]
            currentIndex: Math.max(0, find(AppController.editMethod))
            onActivated: AppController.editMethod = currentText
        }

        // Path: highlighted preview (read) ↔ editable field (edit).
        Rectangle {
            visible: !editor.editing
            x: addressBar.pathGeom.offset
            width: addressBar.pathGeom.size
            height: 38
            radius: DesignTokens.radiusSm
            color: DesignTokens.surfaceSunken
            border.width: 1
            border.color: DesignTokens.borderSubtle
            clip: true
            // Read-mode path with clickable `{{token}}` chips (value picker).
            PathTokenBar {
                anchors.fill: parent
                path: AppController.opPath
            }
        }
        TextField {
            id: pathField
            visible: editor.editing
            x: addressBar.pathGeom.offset
            width: addressBar.pathGeom.size
            height: 38
            text: AppController.editPath
            placeholderText: qsTr("/api/v1/…")
            color: DesignTokens.textPrimary
            placeholderTextColor: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontBody
            font.family: DesignTokens.fontMono
            // onTextChanged (not onTextEdited) so programmatic inserts from the
            // {{ autocomplete also write back to the controller.
            onTextChanged: if (text !== AppController.editPath) {
                AppController.editPath = text;
            }
            Keys.forwardTo: [pathAutocomplete.keyTarget]
            background: Rectangle {
                radius: DesignTokens.radiusSm
                color: DesignTokens.surfaceSunken
                border.width: 1
                border.color: pathField.activeFocus ? DesignTokens.accent : DesignTokens.borderSubtle
            }
            VariableAutocomplete {
                id: pathAutocomplete
                field: pathField
                operationId: AppController.selectedModule + "." + AppController.opName
            }
            ValuePicker {
                id: pathValuePicker
                field: pathField
            }
        }

        Button {
            id: sendButton
            x: addressBar.sendGeom.offset
            width: addressBar.sendGeom.size
            height: 38
            text: AppController.running ? qsTr("Running…") : qsTr("Send")
            enabled: !AppController.running
            GlassToolTip {
                active: sendButton.hovered
                text: editor.editing ? qsTr("Apply your edits and send, resolving the dependency chain  (⌘↵)") : qsTr("Send this request, resolving its dependency chain first  (⌘↵)")
            }
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

    // ── Secondary actions: Edit / Save / Cancel + unsaved cue + More menu ──
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
            GlassToolTip {
                active: editButton.hovered
                text: qsTr("Edit this request — method, path, headers, body, and dependency chain")
            }
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
            // Unsaved-dot + label: a slim, always-true-in-edit-mode cue that the
            // live edits have not yet been written to the project (replaces the
            // old full-width amber banner per UI/UX review §3).
            contentItem: RowLayout {
                spacing: DesignTokens.spaceXs
                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    implicitWidth: 6
                    implicitHeight: 6
                    radius: 3
                    color: DesignTokens.textInverse
                }
                Text {
                    text: saveButton.text
                    color: DesignTokens.textInverse
                    font.pixelSize: DesignTokens.fontBody
                    font.weight: DesignTokens.weightSemiBold
                    verticalAlignment: Text.AlignVCenter
                }
            }
            GlassToolTip {
                active: saveButton.hovered
                text: qsTr("Write these edits to the project. Until you save, Send only applies them to the next run.")
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

        // Slim inline cue (edit mode): replaces the persistent amber banner.
        // The Save button carries the unsaved-dot; this one-liner spells out the
        // Send-vs-Save model without pushing the editor content down.
        Label {
            visible: editor.editing
            Layout.alignment: Qt.AlignVCenter
            verticalAlignment: Text.AlignVCenter
            text: qsTr("Unsaved edits — Send runs them once; Save commits to the project.")
            color: DesignTokens.statusWarning
            font.pixelSize: DesignTokens.fontLabel
            elide: Text.ElideRight
        }

        Item {
            Layout.fillWidth: true
        }

        // Hierarchy (UI/UX review §3): Send is the single primary action. Hooks
        // stays a discrete button (a distinct task, not a run-variant); the
        // run-variants live in the overflow menu so the row reads primary +
        // secondary, not four equal buttons.
        SecondaryButton {
            text: qsTr("Hooks…")
            tip: qsTr("Edit the pre-request and post-response scripts for this endpoint")
            enabled: true
            onClicked: AppController.openHookEditor()
        }
        SecondaryButton {
            id: moreButton
            text: qsTr("More  ▾")
            tip: qsTr("Other ways to run this request")
            onClicked: overflowMenu.popup(moreButton, 0, moreButton.height + DesignTokens.spaceXs)
        }
    }

    Menu {
        id: overflowMenu
        padding: DesignTokens.spaceXs
        background: Rectangle {
            implicitWidth: 240
            radius: DesignTokens.radiusSm
            color: DesignTokens.surfaceSunken
            border.width: 1
            border.color: DesignTokens.borderStrong
        }

        OverflowItem {
            text: qsTr("Dry Run — preview without sending")
            onTriggered: editor.editing ? AppController.applyAndRun(false, true) : AppController.runSelected(false, true)
        }
        OverflowItem {
            text: qsTr("Send (fresh session)")
            onTriggered: editor.editing ? AppController.applyAndRun(true, false) : AppController.runSelected(true, false)
        }
    }

    component OverflowItem: MenuItem {
        id: overflowItem
        enabled: !AppController.running
        implicitHeight: 32
        contentItem: Text {
            leftPadding: DesignTokens.spaceSm
            text: overflowItem.text
            color: !overflowItem.enabled ? DesignTokens.borderStrong : (overflowItem.highlighted ? DesignTokens.textPrimary : DesignTokens.textSecondary)
            font.pixelSize: DesignTokens.fontBody
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: DesignTokens.radiusSm
            color: overflowItem.highlighted ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
        }
    }

    component SecondaryButton: Button {
        id: secondary
        property string tip: ""
        enabled: !AppController.running
        implicitHeight: 32
        leftPadding: DesignTokens.spaceMd
        rightPadding: DesignTokens.spaceMd
        GlassToolTip {
            active: secondary.hovered && secondary.tip.length > 0
            text: secondary.tip
        }
        background: Rectangle {
            radius: DesignTokens.radiusSm
            color: secondary.down ? DesignTokens.accentMuted : (secondary.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent")
            border.width: 1
            // Illuminate the boundary on hover/press so clickability stays
            // obvious on low-contrast / anti-glare displays.
            border.color: (secondary.hovered || secondary.down) ? DesignTokens.accent : DesignTokens.borderStrong
        }
        contentItem: Text {
            text: secondary.text
            color: !secondary.enabled ? DesignTokens.borderStrong : (secondary.hovered ? DesignTokens.textPrimary : DesignTokens.textSecondary)
            font.pixelSize: DesignTokens.fontBody
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    // ── Execution chain (visual preview; live in edit mode) ──
    ColumnLayout {
        id: chainSection
        Layout.fillWidth: true
        spacing: DesignTokens.spaceXs

        // Collapsed by default — the chain is reference, not the primary task,
        // so it shouldn't dominate the editor. Click the header to expand.
        property bool expanded: false
        // User-resizable height for the expanded graph (drag the grip below it).
        property int chainHeight: 240
        readonly property int nodeCount: (AppController.chainGraph.nodes || []).length

        // Clickable header: chevron + caption + a count summary when collapsed.
        RowLayout {
            Layout.fillWidth: true
            spacing: DesignTokens.spaceXs

            AppIcon {
                name: chainSection.expanded ? "chevron-down" : "chevron-right"
                size: 14
                color: DesignTokens.textSecondary
            }
            SectionLabel {
                text: qsTr("EXECUTION CHAIN")
            }
            Label {
                visible: chainSection.nodeCount > 0
                text: chainSection.nodeCount === 1 ? qsTr("1 step") : qsTr("%1 steps").arg(chainSection.nodeCount)
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontCaption
            }
            Item {
                Layout.fillWidth: true
            }

            TapHandler {
                onTapped: chainSection.expanded = !chainSection.expanded
            }
        }

        ChainView {
            Layout.fillWidth: true
            Layout.preferredHeight: chainSection.chainHeight
            Layout.minimumHeight: 80
            visible: chainSection.expanded
            graph: AppController.chainGraph
            statusMap: AppController.chainStatus
            emptyText: qsTr("No declared dependencies — run Dry Run for the full resolved chain.")
            onNodeActivated: opId => AppController.selectOperationById(opId)
            onNodeEditRequested: opId => AppController.editOperationById(opId)
        }

        // Drag grip to resize the graph height.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 10
            visible: chainSection.expanded
            color: "transparent"

            Rectangle {
                anchors.centerIn: parent
                width: 36
                height: 3
                radius: 1.5
                color: resizeHover.hovered || resizeDrag.active ? DesignTokens.accent : DesignTokens.borderStrong
            }

            HoverHandler {
                id: resizeHover
                cursorShape: Qt.SizeVerCursor
            }
            DragHandler {
                id: resizeDrag
                target: null
                xAxis.enabled: false
                yAxis.enabled: true
                property real startScene: 0
                property int startH: 0
                onActiveChanged: {
                    if (active) {
                        startScene = centroid.scenePosition.y;
                        startH = chainSection.chainHeight;
                    }
                }
                onCentroidChanged: {
                    if (active) {
                        const delta = centroid.scenePosition.y - startScene;
                        chainSection.chainHeight = Math.max(80, Math.min(640, startH + delta));
                    }
                }
            }
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
            model: [qsTr("Params"), qsTr("Headers"), qsTr("Body"), qsTr("Auth"), qsTr("Chain"), qsTr("Assertions")]
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: readTabs.currentIndex

            KeyValueList {
                model: AppController.opQuery
                emptyText: qsTr("No query parameters.")
                actionText: qsTr("＋ Add parameter")
                onActionTriggered: {
                    AppController.beginEdit();
                    editTabs.currentIndex = 0;
                }
            }
            KeyValueList {
                model: AppController.opHeaders
                emptyText: qsTr("No headers.")
                actionText: qsTr("＋ Add header")
                onActionTriggered: {
                    AppController.beginEdit();
                    editTabs.currentIndex = 1;
                }
            }
            CodeView {
                text: AppController.opBody
                language: editor.bodyLanguage(AppController.opBody)
                placeholder: qsTr("No request body.")
            }
            // Auth (read): the actor carries the auth strategy.
            Rectangle {
                radius: DesignTokens.radiusSm
                color: DesignTokens.surfaceSunken
                border.width: 1
                border.color: DesignTokens.borderSubtle
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: DesignTokens.spaceMd
                    spacing: DesignTokens.spaceXs
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: DesignTokens.spaceSm
                        AppIcon {
                            name: "user"
                            size: 15
                            color: AppController.opActor.length > 0 ? DesignTokens.accent : DesignTokens.textSecondary
                        }
                        Label {
                            text: AppController.opActor.length > 0 ? AppController.opActor : qsTr("No authentication")
                            color: DesignTokens.textPrimary
                            font.pixelSize: DesignTokens.fontBody
                            font.weight: DesignTokens.weightSemiBold
                        }
                    }
                    Label {
                        visible: AppController.opActor.length > 0
                        text: AppController.actorAuthLabel(AppController.opActor)
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontLabel
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Authentication is defined on the actor (actors/*.yaml). Pick the actor in Edit mode.")
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontCaption
                        wrapMode: Text.WordWrap
                    }
                    Item {
                        Layout.fillHeight: true
                    }
                }
            }
            KeyValueList {
                model: AppController.opExtractions
                emptyText: qsTr("No extractions.")
                actionText: qsTr("＋ Add variable")
                onActionTriggered: {
                    AppController.beginEdit();
                    editTabs.currentIndex = 5;
                }
            }
            KeyValueList {
                model: AppController.opAssertions
                emptyText: qsTr("No assertions.")
                actionText: qsTr("＋ Add assertion")
                onActionTriggered: {
                    AppController.beginEdit();
                    editTabs.currentIndex = 6;
                }
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
            model: [AppController.editParamsCount > 0 ? qsTr("Params  %1").arg(AppController.editParamsCount) : qsTr("Params"), AppController.editHeadersCount > 0 ? qsTr("Headers  %1").arg(AppController.editHeadersCount) : qsTr("Headers"), AppController.editBodyFilled ? qsTr("Body  ●") : qsTr("Body"), qsTr("Auth"), qsTr("Options"), AppController.editChainCount > 0 ? qsTr("Chain  %1").arg(AppController.editChainCount) : qsTr("Chain"), AppController.editAssertionsCount > 0 ? qsTr("Assertions  %1").arg(AppController.editAssertionsCount) : qsTr("Assertions")]
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
                ColumnLayout {
                    id: headersCol
                    width: headersScroll.availableWidth
                    spacing: DesignTokens.spaceSm
                    // Auto-generated headers section collapsed by default.
                    property bool showAuto: false

                    // Auto-generated headers first (read-only, added by the HTTP
                    // client — values from engine::HttpDefaults), then the
                    // editable rows below, mirroring Postman's layout.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: DesignTokens.spaceXs

                        Label {
                            text: qsTr("Auto-generated headers")
                            color: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontLabel
                            font.weight: DesignTokens.weightMedium
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                        Label {
                            id: autoToggle
                            text: headersCol.showAuto ? qsTr("Hide auto-generated headers") : qsTr("Show auto-generated headers")
                            color: toggleHover.hovered ? DesignTokens.accent : DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontLabel
                            HoverHandler {
                                id: toggleHover
                                cursorShape: Qt.PointingHandCursor
                            }
                            TapHandler {
                                onTapped: headersCol.showAuto = !headersCol.showAuto
                            }
                        }
                    }

                    Repeater {
                        model: headersCol.showAuto ? AppController.autoGeneratedHeaders : []
                        delegate: RowLayout {
                            id: autoRow
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: DesignTokens.spaceXs

                            AutoCell {
                                Layout.preferredWidth: 200
                                text: autoRow.modelData.name
                            }
                            AutoCell {
                                Layout.fillWidth: true
                                text: autoRow.modelData.value
                            }
                        }
                    }

                    // Divider between auto-generated and editable headers.
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: DesignTokens.spaceXs
                        implicitHeight: 1
                        color: DesignTokens.borderSubtle
                    }

                    KeyValueEditorView {
                        Layout.fillWidth: true
                        kvModel: AppController.editHeaders
                        suggestHeaderNames: true
                    }
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
                // Pretty-print the JSON body. Handles the common import artefact
                // where the body is a JSON *string* that itself contains JSON
                // (e.g. "{\"a\":1}") by unwrapping one extra layer before
                // re-indenting. Leaves invalid JSON untouched.
                function beautify() {
                    const t = AppController.editBody.trim();
                    if (t.length === 0)
                        return;
                    try {
                        let v = JSON.parse(t);
                        if (typeof v === "string") {
                            try {
                                v = JSON.parse(v);
                            } catch (inner) {
                                // It was a plain string, not double-encoded JSON.
                            }
                        }
                        AppController.editBody = JSON.stringify(v, null, 2);
                    } catch (e) {
                        // Not valid JSON — leave the user's text as-is.
                    }
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
                    ColumnLayout {
                        spacing: DesignTokens.spaceXs

                        ScrollView {
                            id: formScroll
                            Layout.fillWidth: true
                            Layout.fillHeight: true
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

                        // Upload preview (form-data only). On-demand because it
                        // reads referenced files to validate size/existence.
                        RowLayout {
                            Layout.fillWidth: true
                            visible: AppController.editBodyType === "form-data"
                            spacing: DesignTokens.spaceSm

                            Button {
                                id: checkUploadBtn
                                text: qsTr("Check upload")
                                implicitHeight: 26
                                leftPadding: DesignTokens.spaceMd
                                rightPadding: DesignTokens.spaceMd
                                onClicked: uploadSummary.refresh()
                                background: Rectangle {
                                    radius: DesignTokens.radiusSm
                                    color: checkUploadBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                                    border.width: 1
                                    border.color: DesignTokens.borderSubtle
                                }
                                contentItem: Text {
                                    text: checkUploadBtn.text
                                    color: DesignTokens.textSecondary
                                    font.pixelSize: DesignTokens.fontLabel
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }
                            Label {
                                id: uploadSummary
                                Layout.fillWidth: true
                                property var result: ({})
                                function refresh() {
                                    result = AppController.previewFormBody();
                                }
                                function summarise() {
                                    if (result.valid === undefined)
                                        return "";
                                    if (result.valid === false)
                                        return qsTr("⚠ %1").arg(result.error);
                                    if (!result.multipart)
                                        return qsTr("Sends as form-urlencoded (%1 bytes)").arg(result.totalBytes);
                                    let files = 0;
                                    let fields = 0;
                                    for (const p of result.parts) {
                                        if (p.isFile)
                                            files += 1;
                                        else
                                            fields += 1;
                                    }
                                    return qsTr("multipart/form-data — %1 file(s), %2 field(s), %3 bytes").arg(files).arg(fields).arg(result.totalBytes);
                                }
                                text: summarise()
                                color: result.valid === false ? DesignTokens.statusError : DesignTokens.textSecondary
                                font.pixelSize: DesignTokens.fontCaption
                                wrapMode: Text.WordWrap
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }

                    // 2 — raw (JSON / XML / Text), syntax-highlighted + Beautify
                    ColumnLayout {
                        spacing: DesignTokens.spaceXs

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: DesignTokens.spaceSm
                            Item {
                                Layout.fillWidth: true
                            }
                            Button {
                                id: beautifyBtn
                                visible: AppController.editBodyType === "json"
                                implicitHeight: 26
                                leftPadding: DesignTokens.spaceMd
                                rightPadding: DesignTokens.spaceMd
                                onClicked: bodyBox.beautify()
                                background: Rectangle {
                                    radius: DesignTokens.radiusSm
                                    color: beautifyBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                                    border.width: 1
                                    border.color: DesignTokens.borderSubtle
                                }
                                contentItem: Text {
                                    text: qsTr("Beautify")
                                    color: DesignTokens.accent
                                    font.pixelSize: DesignTokens.fontLabel
                                    font.weight: DesignTokens.weightSemiBold
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }
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
                                    Keys.forwardTo: [bodyAutocomplete.keyTarget]
                                    background: null

                                    VariableAutocomplete {
                                        id: bodyAutocomplete
                                        field: rawBody
                                        operationId: AppController.selectedModule + "." + AppController.opName
                                    }
                                    ValuePicker {
                                        id: bodyValuePicker
                                        field: rawBody
                                    }

                                    // Live syntax colouring for the editable body.
                                    BodyHighlighter {
                                        document: rawBody.textDocument
                                        language: AppController.editBodyType === "xml" ? "xml" : (AppController.editBodyType === "text" ? "text" : "json")
                                        propertyColor: DesignTokens.accent
                                        stringColor: DesignTokens.statusSuccess
                                        numberColor: DesignTokens.statusWarning
                                        keywordColor: DesignTokens.methodDelete
                                        commentColor: DesignTokens.textSecondary
                                        punctuationColor: DesignTokens.textSecondary
                                        tagColor: DesignTokens.accent
                                    }
                                }
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
            // Auth — Reqloom auth is actor-based; pick the actor (or No Auth).
            ScrollView {
                id: authScroll
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: authScroll.availableWidth
                    spacing: DesignTokens.spaceMd

                    // Auth choice persists with the request (saved as the
                    // operation's auth: block). No Apply step — it saves with
                    // everything else on the editor's Save.

                    OptionRow {
                        label: qsTr("Actor")
                        GlassComboBox {
                            id: actorCombo
                            width: parent.width
                            // Actor and inline Auth Type are mutually exclusive:
                            // disabled while an inline type is selected.
                            enabled: AppController.editAuthType === "none"
                            opacity: enabled ? 1.0 : 0.5
                            model: [qsTr("(No Auth)")].concat(AppController.actorNames)
                            currentIndex: AppController.editActor.length === 0 ? 0 : Math.max(0, find(AppController.editActor))
                            onActivated: AppController.editActor = (currentIndex === 0 ? "" : currentText)
                        }
                    }
                    OptionRow {
                        label: qsTr("Strategy")
                        Label {
                            width: parent.width
                            text: AppController.actorAuthLabel(AppController.editActor)
                            color: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontLabel
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Use an Actor OR a prebuilt Auth Type — picking one disables the other. Actors (login chains, OAuth, AWS SigV4) live in actors/*.yaml; an inline Auth Type is a quick per-request credential with no actor.")
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontCaption
                        wrapMode: Text.WordWrap
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: DesignTokens.borderSubtle
                    }
                    OptionRow {
                        label: qsTr("Auth Type")
                        GlassComboBox {
                            id: authTypeCombo
                            width: parent.width
                            // Disabled while an Actor is selected (see above).
                            enabled: AppController.editActor.length === 0
                            opacity: enabled ? 1.0 : 0.5
                            property var types: ["inherit", "none", authTypeCombo.separatorToken, "apikey", "bearer", "basic", "oauth2", authTypeCombo.separatorToken, "jwt", "aws_sigv4", "mtls", authTypeCombo.separatorToken, "oauth1"]
                            model: [qsTr("Inherit from parent"), qsTr("No Auth"), authTypeCombo.separatorToken, qsTr("API Key"), qsTr("Bearer Token"), qsTr("Basic Auth"), qsTr("OAuth 2.0"), authTypeCombo.separatorToken, qsTr("JWT Bearer"), qsTr("AWS Signature"), qsTr("mTLS (Client Cert)"), authTypeCombo.separatorToken, qsTr("OAuth 1.0 (Legacy)")]
                            currentIndex: Math.max(0, types.indexOf(AppController.editAuthType))
                            onActivated: {
                                // Keyboard Up/Down can land currentIndex on a divider row.
                                // Commit only real types; otherwise snap the index back to
                                // the current selection so the field never shows a divider.
                                if (types[currentIndex] !== authTypeCombo.separatorToken) {
                                    AppController.editAuthType = types[currentIndex];
                                } else {
                                    currentIndex = Qt.binding(() => Math.max(0, authTypeCombo.types.indexOf(AppController.editAuthType)));
                                }
                            }
                        }
                    }
                    // Bearer
                    OptionRow {
                        visible: AppController.editAuthType === "bearer"
                        label: qsTr("Token")
                        GlassTextField {
                            id: bearerToken
                            width: parent.width
                            placeholderText: qsTr("token or {{variable}}")
                            text: AppController.editAuthToken
                            onTextEdited: AppController.editAuthToken = text
                        }
                    }
                    // Basic
                    OptionRow {
                        visible: AppController.editAuthType === "basic"
                        label: qsTr("Username")
                        GlassTextField {
                            id: basicUser
                            width: parent.width
                            text: AppController.editAuthUsername
                            onTextEdited: AppController.editAuthUsername = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "basic"
                        label: qsTr("Password")
                        GlassTextField {
                            id: basicPass
                            width: parent.width
                            echoMode: TextInput.Password
                            text: AppController.editAuthPassword
                            onTextEdited: AppController.editAuthPassword = text
                        }
                    }
                    // API Key
                    OptionRow {
                        visible: AppController.editAuthType === "apikey"
                        label: qsTr("Key")
                        GlassTextField {
                            id: apiKeyName
                            width: parent.width
                            placeholderText: qsTr("e.g. X-API-Key")
                            text: AppController.editAuthApiKeyName
                            onTextEdited: AppController.editAuthApiKeyName = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "apikey"
                        label: qsTr("Value")
                        GlassTextField {
                            id: apiKeyValue
                            width: parent.width
                            text: AppController.editAuthApiKeyValue
                            onTextEdited: AppController.editAuthApiKeyValue = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "apikey"
                        label: qsTr("Add to")
                        GlassComboBox {
                            width: parent.width
                            model: [qsTr("Header"), qsTr("Query Param")]
                            currentIndex: AppController.editAuthApiKeyInQuery ? 1 : 0
                            onActivated: AppController.editAuthApiKeyInQuery = (currentIndex === 1)
                        }
                    }
                    // AWS Signature v4
                    OptionRow {
                        visible: AppController.editAuthType === "aws_sigv4"
                        label: qsTr("Access Key")
                        GlassTextField {
                            width: parent.width
                            text: AppController.editAuthAwsAccessKey
                            onTextEdited: AppController.editAuthAwsAccessKey = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "aws_sigv4"
                        label: qsTr("Secret Key")
                        GlassTextField {
                            width: parent.width
                            echoMode: TextInput.Password
                            text: AppController.editAuthAwsSecretKey
                            onTextEdited: AppController.editAuthAwsSecretKey = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "aws_sigv4"
                        label: qsTr("Region")
                        GlassTextField {
                            width: parent.width
                            placeholderText: qsTr("e.g. us-east-1")
                            text: AppController.editAuthAwsRegion
                            onTextEdited: AppController.editAuthAwsRegion = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "aws_sigv4"
                        label: qsTr("Service")
                        GlassTextField {
                            width: parent.width
                            placeholderText: qsTr("e.g. execute-api, s3")
                            text: AppController.editAuthAwsService
                            onTextEdited: AppController.editAuthAwsService = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "aws_sigv4"
                        label: qsTr("Session Token")
                        GlassTextField {
                            width: parent.width
                            placeholderText: qsTr("optional (STS)")
                            text: AppController.editAuthAwsSessionToken
                            onTextEdited: AppController.editAuthAwsSessionToken = text
                        }
                    }
                    // OAuth 1.0a
                    OptionRow {
                        visible: AppController.editAuthType === "oauth1"
                        label: qsTr("Consumer Key")
                        GlassTextField {
                            width: parent.width
                            text: AppController.editAuthOauthConsumerKey
                            onTextEdited: AppController.editAuthOauthConsumerKey = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "oauth1"
                        label: qsTr("Consumer Secret")
                        GlassTextField {
                            width: parent.width
                            echoMode: TextInput.Password
                            text: AppController.editAuthOauthConsumerSecret
                            onTextEdited: AppController.editAuthOauthConsumerSecret = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "oauth1"
                        label: qsTr("Token")
                        GlassTextField {
                            width: parent.width
                            text: AppController.editAuthOauthToken
                            onTextEdited: AppController.editAuthOauthToken = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "oauth1"
                        label: qsTr("Token Secret")
                        GlassTextField {
                            width: parent.width
                            echoMode: TextInput.Password
                            text: AppController.editAuthOauthTokenSecret
                            onTextEdited: AppController.editAuthOauthTokenSecret = text
                        }
                    }
                    // OAuth 2.0 — only the non-interactive grants (a headless
                    // engine can't drive the authorization-code browser flow).
                    OptionRow {
                        visible: AppController.editAuthType === "oauth2"
                        label: qsTr("Grant Type")
                        GlassComboBox {
                            width: parent.width
                            property var grants: ["authorization_code", "client_credentials", "password"]
                            model: [qsTr("Authorization Code (PKCE) — recommended"), qsTr("Client Credentials"), qsTr("Password Credentials (Legacy)")]
                            currentIndex: Math.max(0, grants.indexOf(AppController.editAuthOauth2GrantType))
                            onActivated: AppController.editAuthOauth2GrantType = grants[currentIndex]
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "oauth2"
                        label: qsTr("Access Token URL")
                        GlassTextField {
                            width: parent.width
                            placeholderText: qsTr("https://id.example.com/oauth/token")
                            text: AppController.editAuthOauth2TokenUrl
                            onTextEdited: AppController.editAuthOauth2TokenUrl = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "oauth2"
                        label: qsTr("Client ID")
                        GlassTextField {
                            width: parent.width
                            text: AppController.editAuthOauth2ClientId
                            onTextEdited: AppController.editAuthOauth2ClientId = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "oauth2"
                        label: qsTr("Client Secret")
                        GlassTextField {
                            width: parent.width
                            echoMode: TextInput.Password
                            text: AppController.editAuthOauth2ClientSecret
                            onTextEdited: AppController.editAuthOauth2ClientSecret = text
                        }
                    }
                    // Password grant reuses the generic username/password fields.
                    OptionRow {
                        visible: AppController.editAuthType === "oauth2" && AppController.editAuthOauth2GrantType === "password"
                        label: qsTr("Username")
                        GlassTextField {
                            width: parent.width
                            text: AppController.editAuthUsername
                            onTextEdited: AppController.editAuthUsername = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "oauth2" && AppController.editAuthOauth2GrantType === "password"
                        label: qsTr("Password")
                        GlassTextField {
                            width: parent.width
                            echoMode: TextInput.Password
                            text: AppController.editAuthPassword
                            onTextEdited: AppController.editAuthPassword = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "oauth2"
                        label: qsTr("Scope")
                        GlassTextField {
                            width: parent.width
                            placeholderText: qsTr("optional — e.g. read write profile")
                            text: AppController.editAuthOauth2Scope
                            onTextEdited: AppController.editAuthOauth2Scope = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "oauth2"
                        label: qsTr("Client Auth")
                        GlassComboBox {
                            width: parent.width
                            property var modes: ["basic", "body", "none"]
                            model: [qsTr("Send as Basic Auth header"), qsTr("Send in request body"), qsTr("None (Public Client)")]
                            currentIndex: Math.max(0, modes.indexOf(AppController.editAuthOauth2ClientAuth))
                            onActivated: AppController.editAuthOauth2ClientAuth = modes[currentIndex]
                        }
                    }
                    // Authorization Code (PKCE) — interactive.
                    OptionRow {
                        visible: AppController.editAuthType === "oauth2" && AppController.editAuthOauth2GrantType === "authorization_code"
                        label: qsTr("Auth URL")
                        GlassTextField {
                            width: parent.width
                            placeholderText: qsTr("https://id.example.com/oauth/authorize")
                            text: AppController.editAuthOauth2AuthUrl
                            onTextEdited: AppController.editAuthOauth2AuthUrl = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "oauth2" && AppController.editAuthOauth2GrantType === "authorization_code"
                        label: qsTr("Callback URL")
                        GlassTextField {
                            width: parent.width
                            placeholderText: qsTr("http://127.0.0.1:8080/callback")
                            text: AppController.editAuthOauth2CallbackUrl
                            onTextEdited: AppController.editAuthOauth2CallbackUrl = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "oauth2" && AppController.editAuthOauth2GrantType === "authorization_code"
                        label: qsTr("PKCE Method")
                        GlassComboBox {
                            width: parent.width
                            property var methods: ["S256", "plain"]
                            model: [qsTr("S256"), qsTr("Plain")]
                            currentIndex: Math.max(0, methods.indexOf(AppController.editAuthOauth2PkceMethod))
                            onActivated: AppController.editAuthOauth2PkceMethod = methods[currentIndex]
                        }
                    }
                    Label {
                        visible: AppController.editAuthType === "oauth2" && AppController.editAuthOauth2GrantType === "authorization_code"
                        Layout.fillWidth: true
                        text: qsTr("State is generated automatically for each authorization (CSRF protection).")
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontCaption
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        visible: AppController.editAuthType === "oauth2" && AppController.editAuthOauth2GrantType === "authorization_code"
                        Layout.fillWidth: true
                        spacing: DesignTokens.spaceMd
                        GlassButton {
                            primary: true
                            text: qsTr("Get New Token")
                            onClicked: AppController.oauth2GetNewToken()
                        }
                        Label {
                            Layout.fillWidth: true
                            text: AppController.editAuthOauth2HasToken ? qsTr("● Token acquired") : qsTr("No token yet — authorize in your browser")
                            color: AppController.editAuthOauth2HasToken ? DesignTokens.statusSuccess : DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontCaption
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    // JWT Bearer
                    OptionRow {
                        visible: AppController.editAuthType === "jwt"
                        label: qsTr("Algorithm")
                        GlassComboBox {
                            width: parent.width
                            property var algos: ["HS256", "HS512"]
                            model: algos
                            currentIndex: Math.max(0, algos.indexOf(AppController.editAuthJwtAlgorithm))
                            onActivated: AppController.editAuthJwtAlgorithm = algos[currentIndex]
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "jwt"
                        label: qsTr("Secret")
                        GlassTextField {
                            width: parent.width
                            echoMode: TextInput.Password
                            text: AppController.editAuthJwtSecret
                            onTextEdited: AppController.editAuthJwtSecret = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "jwt"
                        label: qsTr("Payload (JSON)")
                        GlassTextField {
                            width: parent.width
                            placeholderText: qsTr('{"sub":"1234","name":"Jane"}')
                            text: AppController.editAuthJwtPayload
                            onTextEdited: AppController.editAuthJwtPayload = text
                        }
                    }
                    // Mutual TLS
                    OptionRow {
                        visible: AppController.editAuthType === "mtls"
                        label: qsTr("Certificate Format")
                        GlassComboBox {
                            width: parent.width
                            property var formats: ["pem", "p12"]
                            model: [qsTr("PEM Certificate + Key"), qsTr("PKCS#12 (.p12/.pfx)")]
                            currentIndex: Math.max(0, formats.indexOf(AppController.editAuthMtlsFormat))
                            onActivated: AppController.editAuthMtlsFormat = formats[currentIndex]
                        }
                    }
                    // PEM: certificate + private key.
                    OptionRow {
                        visible: AppController.editAuthType === "mtls" && AppController.editAuthMtlsFormat !== "p12"
                        label: qsTr("Client Certificate")
                        RowLayout {
                            width: parent.width
                            spacing: DesignTokens.spaceSm
                            GlassTextField {
                                Layout.fillWidth: true
                                placeholderText: qsTr("path to client.pem / .crt")
                                text: AppController.editAuthMtlsCertPath
                                onTextEdited: AppController.editAuthMtlsCertPath = text
                            }
                            GlassButton {
                                text: qsTr("Choose…")
                                onClicked: {
                                    var f = AppController.pickFile(qsTr("Select client certificate"), qsTr("Certificates (*.pem *.crt *.cer)"));
                                    if (f.length > 0)
                                        AppController.editAuthMtlsCertPath = f;
                                }
                            }
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "mtls" && AppController.editAuthMtlsFormat !== "p12"
                        label: qsTr("Private Key")
                        RowLayout {
                            width: parent.width
                            spacing: DesignTokens.spaceSm
                            GlassTextField {
                                Layout.fillWidth: true
                                placeholderText: qsTr("path to client.key")
                                text: AppController.editAuthMtlsKeyPath
                                onTextEdited: AppController.editAuthMtlsKeyPath = text
                            }
                            GlassButton {
                                text: qsTr("Choose…")
                                onClicked: {
                                    var f = AppController.pickFile(qsTr("Select private key"), qsTr("Keys (*.key *.pem)"));
                                    if (f.length > 0)
                                        AppController.editAuthMtlsKeyPath = f;
                                }
                            }
                        }
                    }
                    // PKCS#12: single bundle file.
                    OptionRow {
                        visible: AppController.editAuthType === "mtls" && AppController.editAuthMtlsFormat === "p12"
                        label: qsTr("PKCS#12 File")
                        RowLayout {
                            width: parent.width
                            spacing: DesignTokens.spaceSm
                            GlassTextField {
                                Layout.fillWidth: true
                                placeholderText: qsTr("path to client.p12 / .pfx")
                                text: AppController.editAuthMtlsCertPath
                                onTextEdited: AppController.editAuthMtlsCertPath = text
                            }
                            GlassButton {
                                text: qsTr("Choose…")
                                onClicked: {
                                    var f = AppController.pickFile(qsTr("Select PKCS#12 bundle"), qsTr("PKCS#12 (*.p12 *.pfx)"));
                                    if (f.length > 0)
                                        AppController.editAuthMtlsCertPath = f;
                                }
                            }
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "mtls"
                        label: AppController.editAuthMtlsFormat === "p12" ? qsTr("PKCS#12 Password") : qsTr("Private Key Passphrase")
                        GlassTextField {
                            width: parent.width
                            echoMode: TextInput.Password
                            placeholderText: AppController.editAuthMtlsFormat === "p12" ? qsTr("PKCS#12 password") : qsTr("optional (encrypted key)")
                            text: AppController.editAuthMtlsKeyPassword
                            onTextEdited: AppController.editAuthMtlsKeyPassword = text
                        }
                    }
                    OptionRow {
                        visible: AppController.editAuthType === "mtls"
                        label: qsTr("CA Certificate")
                        RowLayout {
                            width: parent.width
                            spacing: DesignTokens.spaceSm
                            GlassTextField {
                                Layout.fillWidth: true
                                placeholderText: qsTr("optional — trust a private/self-signed CA")
                                text: AppController.editAuthMtlsCaCertPath
                                onTextEdited: AppController.editAuthMtlsCaCertPath = text
                            }
                            GlassButton {
                                text: qsTr("Choose…")
                                onClicked: {
                                    var f = AppController.pickFile(qsTr("Select CA certificate"), qsTr("Certificates (*.pem *.crt *.cer)"));
                                    if (f.length > 0)
                                        AppController.editAuthMtlsCaCertPath = f;
                                }
                            }
                        }
                    }
                    // Inherit — no fields; uses the project default auth.
                    Label {
                        visible: AppController.editAuthType === "inherit"
                        Layout.fillWidth: true
                        text: qsTr("This endpoint uses the project's default auth (currently: %1). Configure a concrete Auth Type on any endpoint and click \u201CSave as project default\u201D to set it.").arg(AppController.projectDefaultAuthLabel)
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontCaption
                        wrapMode: Text.WordWrap
                    }
                    // Promote the current concrete auth to the project default,
                    // which "Inherit from parent" endpoints then resolve to.
                    GlassButton {
                        visible: AppController.editAuthType !== "none" && AppController.editAuthType !== "inherit"
                        text: qsTr("Save as project default")
                        onClicked: AppController.saveProjectDefaultAuth()
                    }
                    Label {
                        visible: AppController.editAuthType !== "none" && AppController.editAuthType !== "inherit"
                        Layout.fillWidth: true
                        text: qsTr("Credential values may use {{variables}}; prefer {{secret.X}} over a plaintext token — it round-trips into the endpoint's YAML.")
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontCaption
                        wrapMode: Text.WordWrap
                    }
                    Item {
                        Layout.fillHeight: true
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
                    // Whole-chain editor: every step in the target's chain,
                    // each with its own depends_on + extract. Edit any of them
                    // here and persist the lot with one Save — no YAML.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: DesignTokens.spaceSm
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Every step in the chain. Edit any step's dependencies or extractions, then save the whole chain.")
                            color: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontCaption
                            wrapMode: Text.WordWrap
                        }
                        Button {
                            id: saveChainBtn
                            implicitHeight: 30
                            leftPadding: DesignTokens.spaceMd
                            rightPadding: DesignTokens.spaceMd
                            onClicked: AppController.saveChainEdits()
                            background: Rectangle {
                                radius: DesignTokens.radiusSm
                                color: saveChainBtn.down ? DesignTokens.accentHover : DesignTokens.accent
                            }
                            contentItem: Text {
                                text: qsTr("Save chain")
                                color: DesignTokens.textInverse
                                font.pixelSize: DesignTokens.fontLabel
                                font.weight: DesignTokens.weightSemiBold
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }

                    // One box, one table for the whole chain (header + every
                    // step as a row + a single add-dependency + hint).
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: DesignTokens.spaceSm
                        radius: DesignTokens.radiusSm
                        color: DesignTokens.surfaceRaised
                        border.width: 1
                        border.color: DesignTokens.borderSubtle
                        implicitHeight: chainTable.implicitHeight + DesignTokens.spaceMd * 2

                        ChainDependencyTable {
                            id: chainTable
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: DesignTokens.spaceMd
                        }
                    }
                }
            }
            // Assertions
            ScrollView {
                id: assertScroll
                clip: true
                contentWidth: availableWidth
                AssertionEditor {
                    width: assertScroll.availableWidth
                    assertModel: AppController.editAssertions
                }
            }
        }
    }

    // Read-only cells styled like the editable key/value
    // fields above, so auto headers read as continuous
    // table rows (Postman-style) rather than loose text.
    component AutoCell: Rectangle {
        property alias text: cellLabel.text
        implicitHeight: 32
        radius: DesignTokens.radiusSm
        color: DesignTokens.surfaceSunken
        border.width: 1
        border.color: DesignTokens.borderSubtle
        opacity: 0.7
        Label {
            id: cellLabel
            anchors.fill: parent
            anchors.leftMargin: DesignTokens.spaceSm
            anchors.rightMargin: DesignTokens.spaceSm
            verticalAlignment: Text.AlignVCenter
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontLabel
            font.family: DesignTokens.fontMono
            elide: Text.ElideRight
        }
    }

    component OptionRow: RowLayout {
        id: optionRow
        property string label: ""
        default property alias fieldData: holder.data
        Layout.fillWidth: true
        spacing: DesignTokens.spaceMd
        Label {
            Layout.preferredWidth: 110
            text: optionRow.label
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
