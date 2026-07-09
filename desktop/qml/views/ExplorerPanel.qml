// ExplorerPanel — the project explorer (QML Migration Roadmap WS-A). A single
// tree mirroring the old ProjectExplorerWidget: an "Actors" group and a
// "Resources" group → resource folders → operation leaves (method badge + name)
// → saved-example child rows. Live fuzzy filter (op id + method verb, empty
// parents hidden), select-to-preview, activate-to-run (double-click/Enter),
// per-row-type context menus, truncation tooltips, and a project-name header
// with "N operations · M actors". Presentation only; logic is in AppController.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Rectangle {
    id: panel

    signal collapseRequested

    radius: 0
    color: DesignTokens.glassFill
    border.width: 1
    border.color: DesignTokens.glassBorder

    // Context target captured when a row's menu opens.
    property string ctxOperationId: ""
    property string ctxResourceId: ""
    property string ctxExampleName: ""
    property string ctxActorId: ""
    // Owning collection of the current/context row (aggregated multi-project tree).
    property string currentProjectRoot: ""
    property string ctxProjectRoot: ""

    // ── Explorer expansion persistence ───────────────────────────────────────
    // Which tree nodes are expanded, keyed by ProjectTreeFilterModel.nodeKey.
    // Persisted to QSettings so the open/closed layout survives model rebuilds
    // (open/close/save/run all reset the model) and app restarts. Empty on a
    // first run → the tree opens fully collapsed.
    property var expandedKeys: ({})

    // Build a node key that matches ProjectTreeFilterModel::nodeKey byte-for-byte
    // (kind, projectRoot, resourceId, operationId, name joined by 0x1F).
    function keyForDelegate(del) {
        const sep = String.fromCharCode(0x1f);
        return del.kind + sep + del.projectRoot + sep + del.resourceId + sep + del.operationId + sep + del.name;
    }

    function persistExpansion() {
        AppController.saveTreeExpansion(Object.keys(panel.expandedKeys));
    }

    // Toggle a row's expansion and remember the new state.
    function toggleAndRecord(del) {
        tree.toggleExpanded(del.row);
        const key = panel.keyForDelegate(del);
        if (tree.isExpanded(del.row)) {
            panel.expandedKeys[key] = true;
        } else {
            delete panel.expandedKeys[key];
        }
        panel.persistExpansion();
    }

    // Forget every expanded node (used by Collapse-all so a later model rebuild
    // doesn't re-open anything).
    function forgetExpansion() {
        panel.expandedKeys = ({});
        panel.persistExpansion();
    }

    // Re-open the remembered nodes after a model rebuild. Walks the model
    // top-down; a node is only opened if its key is remembered AND its parent
    // was opened (recursion only descends into opened nodes), so a collapsed
    // ancestor keeps its subtree closed. expandToIndex(firstChild) opens the
    // node via its child's ancestor chain — no view-row lookup needed.
    function restoreExpansion() {
        if (!tree.model) {
            return;
        }
        panel.restoreInto(undefined);
        // Any expandToIndex may have nudged the viewport; keep the top in view.
        tree.contentY = 0;
    }

    function restoreInto(parentIndex) {
        const m = tree.model;
        const n = (parentIndex === undefined) ? m.rowCount() : m.rowCount(parentIndex);
        for (let i = 0; i < n; ++i) {
            const idx = (parentIndex === undefined) ? m.index(i, 0) : m.index(i, 0, parentIndex);
            const key = m.nodeKey(idx);
            if (key.length > 0 && panel.expandedKeys[key] === true && m.rowCount(idx) > 0) {
                tree.expandToIndex(m.index(0, 0, idx));
                panel.restoreInto(idx);
            }
        }
    }

    // Open the New Endpoint dialog (optionally pre-selecting a module). Lets
    // other views (e.g. the centre endpoint-list empty state) trigger the same
    // flow without owning a second dialog.
    function openNewEndpoint(resourceId) {
        newEndpointDialog.openFor(resourceId);
    }
    // Open the New Module dialog. Exposed so the command palette (Main) can
    // trigger it without reaching into this component's internal ids.
    function openNewModule() {
        newModuleDialog.openDialog();
    }

    // Human-readable label for the trailing operation status dot, so the colour
    // isn't the only signal (accessibility, UI/UX review §4 + §7).
    function statusDotTip(token, hasChildren) {
        switch (token) {
        case "success":
            return qsTr("Last run passed");
        case "error":
        case "failed":
            return qsTr("Last run failed");
        case "running":
            return qsTr("Running…");
        case "blocked":
            return qsTr("Blocked — an upstream step failed");
        case "skipped":
            return qsTr("Skipped — satisfied from cache");
        case "cancelled":
            return qsTr("Last run cancelled");
        default:
            return hasChildren ? qsTr("Has saved responses — not run this session") : qsTr("Never run");
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: DesignTokens.spaceLg
        spacing: DesignTokens.spaceSm

        PanelHeader {
            Layout.fillWidth: true
            title: AppController.projectName.length > 0 ? AppController.projectName : qsTr("Explorer")
            subtitle: AppController.projectName.length > 0 ? qsTr("%1 operations · %2 actors").arg(AppController.operationCount).arg(AppController.actorCount) : qsTr("No project open")

            ToolButton {
                id: addBtn
                implicitWidth: 28
                implicitHeight: 28
                GlassToolTip {
                    active: addBtn.hovered
                    text: qsTr("Add module or endpoint")
                }
                onClicked: addMenu.popup()
                contentItem: AppIcon {
                    name: "plus"
                    size: 16
                    anchors.centerIn: parent
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: addBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
            }
            ToolButton {
                id: collapseAllBtn
                implicitWidth: 28
                implicitHeight: 28
                GlassToolTip {
                    active: collapseAllBtn.hovered
                    text: qsTr("Collapse all groups")
                }
                onClicked: {
                    tree.collapseRecursively();
                    panel.forgetExpansion();
                }
                contentItem: AppIcon {
                    name: "chevron-up"
                    size: 16
                    anchors.centerIn: parent
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: collapseAllBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
            }
            ToolButton {
                id: collapseBtn
                implicitWidth: 28
                implicitHeight: 28
                GlassToolTip {
                    active: collapseBtn.hovered
                    text: qsTr("Collapse sidebar")
                }
                onClicked: panel.collapseRequested()
                contentItem: AppIcon {
                    name: "chevron-left"
                    size: 16
                    anchors.centerIn: parent
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: collapseBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
            }
        }

        TreeView {
            id: tree
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: true
            model: AppController.explorerModel
            selectionModel: ItemSelectionModel {
                model: tree.model
            }

            // Restore the remembered open/closed layout after the model
            // REBUILDS (open/close/load/save/example updates reset the model,
            // collapsing the TreeView). Not bound to projectChanged: a plain
            // project switch doesn't rebuild the tree.
            Connections {
                target: tree.model
                function onModelReset() {
                    Qt.callLater(panel.restoreExpansion);
                }
            }
            Component.onCompleted: {
                const saved = AppController.loadTreeExpansion();
                const set = ({});
                for (let i = 0; i < saved.length; ++i) {
                    set[saved[i]] = true;
                }
                panel.expandedKeys = set;
                Qt.callLater(panel.restoreExpansion);
            }

            // Enter / Return activates (runs) the current operation row in its
            // owning collection.
            Keys.onReturnPressed: {
                if (panel.currentOperationId.length > 0) {
                    AppController.activateOperationInProject(panel.currentProjectRoot, panel.currentOperationId);
                }
            }
            Keys.onEnterPressed: {
                if (panel.currentOperationId.length > 0) {
                    AppController.activateOperationInProject(panel.currentProjectRoot, panel.currentOperationId);
                }
            }

            delegate: TreeViewDelegate {
                id: del
                required property string kind
                required property string name
                required property string operationId
                required property string resourceId
                required property string method
                required property string exampleName
                required property string tooltip
                required property string projectRoot
                required property int count
                required property int status
                required property string statusToken

                implicitHeight: 34
                indentation: 16

                readonly property bool isProject: kind === "project"
                readonly property bool isOperation: kind === "operation"
                readonly property bool isExample: kind === "example"
                readonly property bool isResource: kind === "resource"
                readonly property bool isResourcesRoot: kind === "resourceGroup"
                readonly property bool isActor: kind === "actor"
                readonly property bool isActorsRoot: kind === "actorGroup"
                // The active collection, tracked reactively via a property (no
                // tree rebuild needed to move the highlight).
                readonly property bool isActiveProject: del.isProject && del.projectRoot === AppController.projectRoot
                // Live run-status token for this operation (running/success/error/…),
                // for the trailing status dot. Empty when the op hasn't run.
                readonly property string opRunToken: del.isOperation ? (AppController.chainStatus[del.operationId] || "") : ""

                onCurrentChanged: {
                    if (del.current) {
                        panel.currentOperationId = del.isOperation ? del.operationId : "";
                        panel.currentProjectRoot = del.projectRoot;
                    }
                }

                background: Rectangle {
                    anchors.fill: parent
                    anchors.topMargin: 1
                    anchors.bottomMargin: 1
                    radius: DesignTokens.radiusSm
                    color: del.current ? DesignTokens.accentMuted : del.hovered ? Qt.rgba(1, 1, 1, 0.04) : "transparent"
                    border.width: del.current ? 1 : 0
                    border.color: del.current ? DesignTokens.accent : "transparent"
                    Behavior on color {
                        ColorMotion {}
                    }
                }

                // The default chevron slot is removed (width 0); the chevron is
                // drawn as the first item of contentItem so it always sits
                // directly left of the folder/method icon and indents with it.
                indicator: Item {
                    implicitWidth: 0
                    implicitHeight: del.height
                }

                contentItem: RowLayout {
                    spacing: DesignTokens.spaceXs

                    // Expand/collapse chevron, kept in a fixed-width slot so
                    // leaf rows (examples) still align under their parents.
                    Item {
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        Layout.alignment: Qt.AlignVCenter
                        AppIcon {
                            anchors.centerIn: parent
                            visible: del.hasChildren
                            name: del.expanded ? "chevron-down" : "chevron-right"
                            size: 13
                            color: del.current ? DesignTokens.accent : DesignTokens.textSecondary
                        }
                        TapHandler {
                            enabled: del.hasChildren
                            onTapped: panel.toggleAndRecord(del)
                        }
                    }

                    AppIcon {
                        visible: !del.isOperation
                        Layout.alignment: Qt.AlignVCenter
                        size: 15
                        opacity: 0.85
                        name: del.kind === "example" ? "zap" : (del.expanded ? "folder-open" : "folder")
                        color: del.isExample ? DesignTokens.accent : DesignTokens.textSecondary
                    }
                    MethodBadge {
                        visible: del.isOperation
                        Layout.alignment: Qt.AlignVCenter
                        method: del.isOperation ? del.method : "GET"
                        // Uniform column width so endpoint names align whether
                        // the verb is GET (3) or DELETE/OPTIONS (6–7).
                        minWidth: 58
                    }
                    Label {
                        Layout.alignment: Qt.AlignVCenter
                        text: del.name
                        color: del.isActiveProject ? DesignTokens.accent : (del.isExample ? DesignTokens.textSecondary : DesignTokens.textPrimary)
                        font.pixelSize: DesignTokens.fontBody
                        font.weight: (del.current || del.isProject) ? DesignTokens.weightSemiBold : DesignTokens.weightRegular
                        elide: Text.ElideRight
                        Layout.maximumWidth: implicitWidth
                        HoverHandler {
                            id: hoverHandler
                        }
                        GlassToolTip {
                            active: hoverHandler.hovered && del.tooltip.length > 0
                            text: del.tooltip
                        }
                    }
                    // Child-count pill on folder rows (Actors 5, Resources 12…).
                    Rectangle {
                        visible: del.count > 0 && !del.isOperation && !del.isExample
                        Layout.alignment: Qt.AlignVCenter
                        implicitHeight: 18
                        implicitWidth: countLabel.implicitWidth + DesignTokens.spaceSm * 2
                        radius: DesignTokens.radiusSm
                        color: DesignTokens.surfaceSunken
                        Label {
                            id: countLabel
                            anchors.centerIn: parent
                            text: del.count
                            color: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontCaption
                            font.weight: DesignTokens.weightMedium
                        }
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    // Saved-example HTTP status pill (200 / 4xx …).
                    Rectangle {
                        visible: del.isExample && del.status > 0
                        Layout.alignment: Qt.AlignVCenter
                        implicitHeight: 18
                        implicitWidth: exStatusLabel.implicitWidth + DesignTokens.spaceSm * 2
                        radius: DesignTokens.radiusSm
                        readonly property color hue: del.statusToken.length > 0 ? DesignTokens.statusColor(del.statusToken) : DesignTokens.textSecondary
                        color: Qt.rgba(hue.r, hue.g, hue.b, 0.16)
                        Label {
                            id: exStatusLabel
                            anchors.centerIn: parent
                            text: del.status
                            color: parent.hue
                            font.pixelSize: DesignTokens.fontCaption
                            font.weight: DesignTokens.weightSemiBold
                            font.family: DesignTokens.fontMono
                        }
                    }
                    // Operation status dot: live run status when available, else
                    // a subtle marker that the endpoint has captured responses.
                    Rectangle {
                        id: statusDot
                        visible: del.isOperation && (del.opRunToken.length > 0 || del.hasChildren)
                        Layout.alignment: Qt.AlignVCenter
                        Layout.rightMargin: DesignTokens.spaceSm
                        implicitWidth: 8
                        implicitHeight: 8
                        radius: 4
                        color: del.opRunToken.length > 0 ? DesignTokens.statusColor(del.opRunToken) : DesignTokens.statusSuccess
                        HoverHandler {
                            id: dotHover
                        }
                        GlassToolTip {
                            active: dotHover.hovered
                            text: panel.statusDotTip(del.opRunToken, del.hasChildren)
                        }
                    }
                }

                onClicked: {
                    if (del.isOperation) {
                        AppController.selectOperationInProject(del.projectRoot, del.operationId);
                    } else if (del.isExample) {
                        AppController.selectExample(del.operationId, del.exampleName);
                    } else if (del.isActor) {
                        AppController.selectActor(del.projectRoot, del.name);
                    } else if (del.isProject) {
                        AppController.activateProjectByRoot(del.projectRoot);
                        panel.toggleAndRecord(del);
                    } else {
                        panel.toggleAndRecord(del);
                    }
                }

                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onDoubleTapped: {
                        if (del.isOperation) {
                            AppController.activateOperationInProject(del.projectRoot, del.operationId);
                        }
                    }
                }

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: {
                        // Activate the row's collection first so New/Rename/Delete
                        // and the create dialogs target the right project. If the
                        // switch is refused (e.g. a run is in flight), don't open
                        // the menu — otherwise actions would hit the active project.
                        if (!AppController.activateProjectByRoot(del.projectRoot)) {
                            return;
                        }
                        panel.ctxOperationId = del.operationId;
                        panel.ctxResourceId = del.resourceId;
                        panel.ctxExampleName = del.exampleName;
                        panel.ctxActorId = del.isActor ? del.name : "";
                        panel.ctxProjectRoot = del.projectRoot;
                        if (del.isExample) {
                            exampleMenu.popup();
                        } else if (del.isOperation) {
                            operationMenu.popup();
                        } else if (del.isResource) {
                            resourceMenu.popup();
                        } else if (del.isResourcesRoot) {
                            rootMenu.popup();
                        } else if (del.isActor) {
                            actorMenu.popup();
                        } else if (del.isActorsRoot) {
                            actorsRootMenu.popup();
                        } else if (del.isProject) {
                            projectMenu.popup();
                        }
                    }
                }
            }
        }
    }

    // Current operation row id (for Enter-to-run), tracked from selection.
    property string currentOperationId: ""

    // ── Add menu (header "+") ────────────────────────────────────────────────
    GlassMenu {
        id: addMenu
        GlassMenuItem {
            text: qsTr("New Endpoint…")
            onTriggered: newEndpointDialog.openFor("")
        }
        GlassMenuItem {
            text: qsTr("New Module…")
            onTriggered: newModuleDialog.openDialog()
        }
        GlassMenuItem {
            text: qsTr("New Actor…")
            onTriggered: AppController.newActor()
        }
    }

    // ── Per-row-type context menus ───────────────────────────────────────────
    GlassMenu {
        id: operationMenu
        GlassMenuItem {
            text: qsTr("Edit")
            onTriggered: AppController.selectOperationById(panel.ctxOperationId)
        }
        GlassMenuItem {
            text: qsTr("Rename")
            onTriggered: renameDialog.openFor("operation", panel.ctxOperationId)
        }
        MenuSeparator {}
        GlassMenuItem {
            text: qsTr("Delete")
            onTriggered: deleteDialog.openFor("operation", panel.ctxOperationId)
        }
    }
    GlassMenu {
        id: resourceMenu
        GlassMenuItem {
            text: qsTr("New Endpoint…")
            onTriggered: newEndpointDialog.openFor(panel.ctxResourceId)
        }
        MenuSeparator {}
        GlassMenuItem {
            text: qsTr("Rename")
            onTriggered: renameDialog.openFor("resource", panel.ctxResourceId)
        }
        MenuSeparator {}
        GlassMenuItem {
            text: qsTr("Delete")
            onTriggered: deleteDialog.openFor("resource", panel.ctxResourceId)
        }
    }
    GlassMenu {
        id: rootMenu
        GlassMenuItem {
            text: qsTr("New Module…")
            onTriggered: newModuleDialog.openDialog()
        }
        GlassMenuItem {
            text: qsTr("New Endpoint…")
            onTriggered: newEndpointDialog.openFor("")
        }
    }
    // Per-collection menu (right-click a Project node). The row's project was
    // activated on right-click, so the create dialogs target it.
    GlassMenu {
        id: projectMenu
        GlassMenuItem {
            text: qsTr("New Endpoint…")
            onTriggered: newEndpointDialog.openFor("")
        }
        GlassMenuItem {
            text: qsTr("New Module…")
            onTriggered: newModuleDialog.openDialog()
        }
        GlassMenuItem {
            text: qsTr("New Actor…")
            onTriggered: AppController.newActor()
        }
        MenuSeparator {}
        GlassMenuItem {
            text: qsTr("Close collection")
            onTriggered: AppController.closeProjectByRoot(panel.ctxProjectRoot)
        }
    }
    GlassMenu {
        id: exampleMenu
        GlassMenuItem {
            text: qsTr("Rename")
            onTriggered: exampleRenameDialog.openFor(panel.ctxOperationId, panel.ctxExampleName)
        }
        GlassMenuItem {
            text: qsTr("Duplicate")
            onTriggered: AppController.duplicateExample(panel.ctxOperationId, panel.ctxExampleName)
        }
        MenuSeparator {}
        GlassMenuItem {
            text: qsTr("Delete")
            onTriggered: exampleDeleteDialog.openFor(panel.ctxOperationId, panel.ctxExampleName)
        }
    }
    GlassMenu {
        id: actorsRootMenu
        GlassMenuItem {
            text: qsTr("New Actor…")
            onTriggered: AppController.newActor()
        }
    }
    GlassMenu {
        id: actorMenu
        GlassMenuItem {
            text: qsTr("Edit…")
            onTriggered: AppController.requestActorEdit(panel.ctxProjectRoot, panel.ctxActorId)
        }
        MenuSeparator {}
        GlassMenuItem {
            text: qsTr("Delete")
            onTriggered: deleteDialog.openFor("actor", panel.ctxActorId)
        }
    }

    // ── Dialogs ──────────────────────────────────────────────────────────────
    NewModuleDialog {
        id: newModuleDialog
    }
    NewEndpointDialog {
        id: newEndpointDialog
    }

    Dialog {
        id: renameDialog
        modal: true
        enter: PopupEnter {}
        exit: PopupExit {}
        anchors.centerIn: Overlay.overlay
        width: 380
        padding: DesignTokens.spaceLg
        title: qsTr("Rename")
        header: DialogHeader {
            title: qsTr("Rename")
        }

        property string targetKind: ""
        property string targetId: ""

        function openFor(kind, id) {
            targetKind = kind;
            targetId = id;
            // Seed with the short name (part after the last dot for ops).
            const dot = id.lastIndexOf(".");
            renameField.text = (kind === "operation" && dot >= 0) ? id.substring(dot + 1) : id;
            open();
            renameField.forceActiveFocus();
            renameField.selectAll();
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
                text: qsTr("New name")
            }
            GlassTextField {
                id: renameField
                Layout.fillWidth: true
                error: renameField.text.length > 0 && !AppController.isValidName(renameField.text)
            }
            FieldError {
                text: renameField.error ? qsTr("Name can't contain '.', '/', or '\\'.") : ""
            }
        }

        footer: DialogButtons {
            okText: qsTr("Rename")
            okEnabled: AppController.isValidName(renameField.text)
            onAccepted: renameDialog.accept()
            onRejected: renameDialog.reject()
        }

        onAccepted: {
            if (targetKind === "operation") {
                AppController.renameOperation(targetId, renameField.text.trim());
            } else if (targetKind === "resource") {
                AppController.renameResource(targetId, renameField.text.trim());
            }
        }
    }

    Dialog {
        id: deleteDialog
        modal: true
        enter: PopupEnter {}
        exit: PopupExit {}
        anchors.centerIn: Overlay.overlay
        width: 380
        padding: DesignTokens.spaceLg
        title: qsTr("Delete")
        header: DialogHeader {
            title: qsTr("Delete")
        }

        property string targetKind: ""
        property string targetId: ""

        function openFor(kind, id) {
            targetKind = kind;
            targetId = id;
            open();
        }

        background: Rectangle {
            radius: DesignTokens.radiusLg
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.glassBorder
        }

        contentItem: Label {
            text: deleteDialog.targetKind === "resource" ? qsTr("Delete module “%1” and all its endpoints?").arg(deleteDialog.targetId) : (deleteDialog.targetKind === "actor" ? qsTr("Delete actor “%1”? Operations using it will become unauthenticated.").arg(deleteDialog.targetId) : qsTr("Delete endpoint “%1”?").arg(deleteDialog.targetId))
            color: DesignTokens.textPrimary
            font.pixelSize: DesignTokens.fontBody
            wrapMode: Text.WordWrap
        }

        footer: DialogButtons {
            okText: qsTr("Delete")
            okDestructive: true
            onAccepted: deleteDialog.accept()
            onRejected: deleteDialog.reject()
        }

        onAccepted: {
            if (targetKind === "operation") {
                AppController.deleteOperation(targetId);
            } else if (targetKind === "resource") {
                AppController.deleteResource(targetId);
            } else if (targetKind === "actor") {
                AppController.deleteActor(targetId);
            }
        }
    }

    // Rename a saved example — needs a fresh name, so it prompts (unlike
    // duplicate/delete which act immediately).
    Dialog {
        id: exampleRenameDialog
        modal: true
        enter: PopupEnter {}
        exit: PopupExit {}
        anchors.centerIn: Overlay.overlay
        width: 380
        padding: DesignTokens.spaceLg
        title: qsTr("Rename example")
        header: DialogHeader {
            title: qsTr("Rename example")
        }

        property string targetOperationId: ""
        property string targetName: ""

        function openFor(operationId, name) {
            targetOperationId = operationId;
            targetName = name;
            exampleRenameField.text = name;
            open();
            exampleRenameField.forceActiveFocus();
            exampleRenameField.selectAll();
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
                text: qsTr("New name")
            }
            GlassTextField {
                id: exampleRenameField
                Layout.fillWidth: true
            }
        }

        footer: DialogButtons {
            okText: qsTr("Rename")
            okEnabled: exampleRenameField.text.trim().length > 0
            onAccepted: exampleRenameDialog.accept()
            onRejected: exampleRenameDialog.reject()
        }

        onAccepted: AppController.renameExample(targetOperationId, targetName, exampleRenameField.text.trim())
    }

    // Confirm deleting a saved example (destructive, no undo).
    Dialog {
        id: exampleDeleteDialog
        modal: true
        enter: PopupEnter {}
        exit: PopupExit {}
        anchors.centerIn: Overlay.overlay
        width: 380
        padding: DesignTokens.spaceLg
        title: qsTr("Delete example")
        header: DialogHeader {
            title: qsTr("Delete example")
        }

        property string targetOperationId: ""
        property string targetName: ""

        function openFor(operationId, name) {
            targetOperationId = operationId;
            targetName = name;
            open();
        }

        background: Rectangle {
            radius: DesignTokens.radiusLg
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.glassBorder
        }

        contentItem: Label {
            text: qsTr("Delete saved example “%1”? This can't be undone.").arg(exampleDeleteDialog.targetName)
            color: DesignTokens.textPrimary
            font.pixelSize: DesignTokens.fontBody
            wrapMode: Text.WordWrap
        }

        footer: DialogButtons {
            okText: qsTr("Delete")
            okDestructive: true
            onAccepted: exampleDeleteDialog.accept()
            onRejected: exampleDeleteDialog.reject()
        }

        onAccepted: AppController.deleteExample(targetOperationId, targetName)
    }
}
