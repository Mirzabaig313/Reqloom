// Main — the Reqloom application shell . A 3-pane SplitView
// (explorer | editor | response/timeline), collapsible left+right rails,
// a native MenuBar, top toolbar, keyboard shortcuts, toasts, and an empty
// state. Logic lives in AppController/SecretsController/ThemeController.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQuick.Effects
import Reqloom

ApplicationWindow {
    id: window
    width: 1280
    height: 800
    minimumWidth: 800
    minimumHeight: 600
    visible: true
    title: {
        const p = AppController.projectName;
        return p.length > 0 ? (p + " — Reqloom") : qsTr("Reqloom");
    }
    color: DesignTokens.canvasBottom

    // ── Frosted backdrop ─────────────────────────────────────────────────────
    // An iridescent gradient with soft nacre glow-blobs, blurred so the
    // translucent glass panels above reveal a smooth abalone wash. The source
    // is drawn off-screen and shown only through the MultiEffect blur.
    Item {
        id: glassBackdrop
        anchors.fill: parent
        z: -10

        Rectangle {
            id: backdropSource
            anchors.fill: parent
            visible: false
            layer.enabled: true
            gradient: Gradient {
                GradientStop {
                    position: 0.0
                    color: DesignTokens.canvasTop
                }
                GradientStop {
                    position: 1.0
                    color: DesignTokens.canvasBottom
                }
            }
            Rectangle {
                width: 520
                height: 520
                radius: width / 2
                x: parent.width * 0.10 - width / 2
                y: parent.height * 0.08
                color: DesignTokens.glowTeal
            }
            Rectangle {
                width: 460
                height: 460
                radius: width / 2
                x: parent.width * 0.78
                y: parent.height * 0.62
                color: DesignTokens.glowSeafoam
            }
            Rectangle {
                width: 380
                height: 380
                radius: width / 2
                x: parent.width * 0.55
                y: parent.height * 0.05 - height / 2
                color: DesignTokens.glowBlush
            }
        }

        MultiEffect {
            anchors.fill: parent
            source: backdropSource
            blurEnabled: true
            blur: 1.0
            blurMax: 64
            autoPaddingEnabled: false
        }
    }

    // ── Native menu bar (macOS menu bar is automatic in Qt 6.8) ──────────────
    menuBar: MenuBar {
        background: Rectangle {
            color: DesignTokens.surfaceRaised
        }
        delegate: MenuBarItem {
            id: menuBarItem
            contentItem: Text {
                text: menuBarItem.text
                color: DesignTokens.textPrimary
                font.pixelSize: DesignTokens.fontBody
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: menuBarItem.highlighted ? DesignTokens.accentMuted : "transparent"
                radius: 4
            }
        }

        Menu {
            title: qsTr("File")
            MenuItem {
                text: qsTr("New Project…")
                onTriggered: newProjectDialog.openDialog()
            }
            MenuItem {
                text: qsTr("Open Project…")
                onTriggered: folderDialog.open()
            }
            MenuItem {
                text: qsTr("Import (OpenAPI, Postman, Insomnia, …)…")
                onTriggered: importSpecDialog.open()
            }
            MenuItem {
                text: qsTr("Manage Secrets")
                enabled: AppController.resourceCount > 0
                onTriggered: secretsDialog.openDialog()
            }
            MenuSeparator {}
            MenuItem {
                text: qsTr("Quit")
                onTriggered: Qt.quit()
            }
        }
        Menu {
            title: qsTr("View")
            MenuItem {
                text: window.explorerCollapsed ? qsTr("Show Explorer") : qsTr("Hide Explorer")
                onTriggered: window.explorerCollapsed = !window.explorerCollapsed
            }
            MenuItem {
                text: window.responseCollapsed ? qsTr("Show Response") : qsTr("Hide Response")
                onTriggered: window.responseCollapsed = !window.responseCollapsed
            }
        }
        Menu {
            title: qsTr("Appearance")
            MenuItem {
                text: qsTr("Light")
                checkable: true
                checked: ThemeController.mode === "light"
                onTriggered: ThemeController.mode = "light"
            }
            MenuItem {
                text: qsTr("Dark")
                checkable: true
                checked: ThemeController.mode === "dark"
                onTriggered: ThemeController.mode = "dark"
            }
            MenuItem {
                text: qsTr("System")
                checkable: true
                checked: ThemeController.mode === "system"
                onTriggered: ThemeController.mode = "system"
            }
            MenuSeparator {}
            MenuItem {
                text: qsTr("Comfortable")
                checkable: true
                checked: ThemeController.density === "comfortable"
                onTriggered: ThemeController.density = "comfortable"
            }
            MenuItem {
                text: qsTr("Compact")
                checkable: true
                checked: ThemeController.density === "compact"
                onTriggered: ThemeController.density = "compact"
            }
        }
    }

    // ── Rail / collapse state ──────────────────────────────────────────────
    property bool explorerCollapsed: false
    property bool responseCollapsed: false
    /// Editor + response stacked vertically (true) vs side-by-side (false).
    property bool responseStacked: false
    /// Once the user picks a split orientation we stop auto-managing it, so
    /// resizing never fights their choice. Until then the layout responds to
    /// width: side-by-side when wide, stacked when too narrow for two columns.
    property bool userChoseStack: false
    readonly property int autoStackBelow: 1040

    onWidthChanged: if (!userChoseStack) {
        responseStacked = width < autoStackBelow;
    }
    Component.onCompleted: if (!userChoseStack) {
        responseStacked = width < autoStackBelow;
    }
    /// Keeps the response/timeline pane visible while viewing a replayed run
    /// from history, even when no operation is open.
    property bool historyReplayActive: false

    // When a past run is replayed from history, reveal the pane and switch to
    // the Timeline tab so the replayed steps are actually visible.
    Connections {
        target: AppController
        function onRunReplayed() {
            window.historyReplayActive = true;
            window.responseCollapsed = false;
            responsePanel.showTimeline();
        }
    }

    // ── Global shortcuts ───────────────────────────────────────────────────
    Shortcut {
        sequence: "Ctrl+B"
        onActivated: window.explorerCollapsed = !window.explorerCollapsed
    }
    Shortcut {
        sequence: "Ctrl+J"
        onActivated: window.responseCollapsed = !window.responseCollapsed
    }
    Shortcut {
        sequence: "Ctrl+P"
        onActivated: commandPalette.open()
    }
    Shortcut {
        sequence: "Ctrl+W"
        onActivated: {
            if (AppController.activeTabIndex >= 0) {
                AppController.closeTab(AppController.activeTabIndex);
            }
        }
    }
    Shortcut {
        sequence: "Ctrl+Return"
        onActivated: {
            if (AppController.hasOperation) {
                AppController.runSelected(false, false);
            }
        }
    }

    // ── Notify → toast ─────────────────────────────────────────────────────
    Connections {
        target: AppController
        function onNotify(message, isError) {
            toast.show(message, isError);
        }
        function onImportNeedsOverwrite(specFile, targetDir) {
            importOverwriteDialog.confirmFor(specFile, targetDir);
        }
        function onImportReviewNotes(notes) {
            importNotesDialog.showNotes(notes);
        }
    }
    Connections {
        target: SecretsController
        function onNotify(message, isError) {
            toast.show(message, isError);
        }
    }

    // ── Top toolbar ────────────────────────────────────────────────────────
    header: Rectangle {
        implicitHeight: 60
        color: DesignTokens.glassFill
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: DesignTokens.borderSubtle
        }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: DesignTokens.spaceLg
            anchors.rightMargin: DesignTokens.spaceLg
            spacing: DesignTokens.spaceMd

            // Workspace switcher — every opened/imported collection lives here,
            // so many projects share one window (click to switch).
            Button {
                id: projectSwitcher
                implicitHeight: 32
                leftPadding: DesignTokens.spaceSm
                rightPadding: DesignTokens.spaceSm
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: projectSwitcher.hovered ? DesignTokens.accentMuted : "transparent"
                    border.width: 1
                    border.color: DesignTokens.borderSubtle
                }
                contentItem: RowLayout {
                    spacing: DesignTokens.spaceXs
                    AppIcon {
                        name: "folder"
                        size: 14
                    }
                    Text {
                        text: AppController.projectName.length > 0 ? AppController.projectName : qsTr("No project")
                        color: DesignTokens.textPrimary
                        font.pixelSize: DesignTokens.fontLabel
                        font.weight: DesignTokens.weightMedium
                        elide: Text.ElideRight
                        Layout.maximumWidth: 180
                        verticalAlignment: Text.AlignVCenter
                    }
                    Text {
                        text: "▾"
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontLabel
                    }
                }
                onClicked: {
                    switcherMenu.reload();
                    switcherMenu.popup(projectSwitcher, 0, projectSwitcher.height);
                }
            }

            GlassMenu {
                id: switcherMenu
                property var items: []
                // Open collections (active one marked ●), then a "Recent"
                // section of collections not currently open (click to reopen).
                // Paths are normalized upstream, so recent never duplicates an
                // open collection.
                function reload() {
                    const open = AppController.openProjects();
                    const openPaths = open.map(o => o.path);
                    const list = [];
                    for (const o of open) {
                        list.push({
                            kind: "open",
                            label: o.name,
                            projectIndex: o.index,
                            active: o.active
                        });
                    }
                    const recents = [];
                    for (const r of AppController.recentProjects()) {
                        if (!openPaths.includes(r.path)) {
                            recents.push({
                                kind: "recent",
                                label: r.name,
                                path: r.path
                            });
                        }
                    }
                    if (recents.length > 0) {
                        list.push({
                            kind: "header",
                            label: qsTr("Recent")
                        });
                        for (const r of recents) {
                            list.push(r);
                        }
                    }
                    items = list;
                }
                Instantiator {
                    model: switcherMenu.items
                    // Custom delegate so a "header" row renders as a section
                    // divider strip (small uppercase label + rule line) rather
                    // than looking like a clickable project.
                    delegate: MenuItem {
                        id: swItem
                        required property var modelData
                        readonly property bool isHeader: modelData.kind === "header"
                        enabled: !isHeader
                        implicitHeight: isHeader ? 26 : DesignTokens.controlHeight
                        horizontalPadding: DesignTokens.spaceMd
                        background: Rectangle {
                            radius: DesignTokens.radiusSm
                            color: (!swItem.isHeader && swItem.highlighted) ? DesignTokens.accentMuted : "transparent"
                        }
                        contentItem: Item {
                            // Drive the menu width from the visible child so long
                            // collection names widen the menu instead of clipping;
                            // the row Text also elides as a safety net.
                            implicitWidth: swItem.isHeader ? hdrRow.implicitWidth : rowText.implicitWidth
                            RowLayout {
                                id: hdrRow
                                anchors.fill: parent
                                visible: swItem.isHeader
                                spacing: DesignTokens.spaceSm
                                Label {
                                    text: swItem.modelData.label.toUpperCase()
                                    color: DesignTokens.textSecondary
                                    font.pixelSize: DesignTokens.fontCaption
                                    font.weight: DesignTokens.weightSemiBold
                                }
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignVCenter
                                    implicitHeight: 1
                                    color: DesignTokens.borderSubtle
                                }
                            }
                            Text {
                                id: rowText
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                visible: !swItem.isHeader
                                text: swItem.modelData.kind === "open" ? ((swItem.modelData.active ? "●  " : "") + swItem.modelData.label) : swItem.modelData.label
                                color: DesignTokens.textPrimary
                                font.pixelSize: DesignTokens.fontBody
                                font.family: DesignTokens.fontSans
                                elide: Text.ElideRight
                            }
                        }
                        onTriggered: {
                            if (swItem.modelData.kind === "open")
                                AppController.activateProject(swItem.modelData.projectIndex);
                            else if (swItem.modelData.kind === "recent")
                                AppController.openProjectPath(swItem.modelData.path);
                        }
                    }
                    onObjectAdded: (index, object) => switcherMenu.insertItem(index, object)
                    onObjectRemoved: (index, object) => switcherMenu.removeItem(object)
                }
                MenuSeparator {}
                GlassMenuItem {
                    text: qsTr("Close current project")
                    enabled: AppController.projectName.length > 0
                    onTriggered: {
                        for (const it of switcherMenu.items) {
                            if (it.kind === "open" && it.active) {
                                AppController.closeProject(it.projectIndex);
                                break;
                            }
                        }
                    }
                }
                MenuSeparator {}
                GlassMenuItem {
                    text: qsTr("New Project…")
                    onTriggered: newProjectDialog.openDialog()
                }
                GlassMenuItem {
                    text: qsTr("Open Project…")
                    onTriggered: folderDialog.open()
                }
                GlassMenuItem {
                    text: qsTr("Import (OpenAPI, Postman, Insomnia, …)…")
                    onTriggered: importSpecDialog.open()
                }
            }

            Button {
                id: manageSecretsBtn
                text: qsTr("Manage Secrets")
                enabled: AppController.resourceCount > 0
                implicitHeight: 32
                leftPadding: DesignTokens.spaceSm
                rightPadding: DesignTokens.spaceSm
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: "transparent"
                    border.width: 1
                    border.color: DesignTokens.borderSubtle
                }
                contentItem: Text {
                    text: manageSecretsBtn.text
                    color: manageSecretsBtn.enabled ? DesignTokens.textSecondary : DesignTokens.borderStrong
                    font.pixelSize: DesignTokens.fontLabel
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: secretsDialog.openDialog()
            }

            // Run History
            Button {
                id: historyBtn
                text: qsTr("History")
                implicitHeight: 32
                leftPadding: DesignTokens.spaceSm
                rightPadding: DesignTokens.spaceSm
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: "transparent"
                    border.width: 1
                    border.color: DesignTokens.borderSubtle
                }
                contentItem: Text {
                    text: historyBtn.text
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontLabel
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: historyDialog.openDialog()
            }

            Item {
                Layout.fillWidth: true
            }

            // (The operation search lives in the explorer sidebar now — see
            // ExplorerPanel's search field. Ctrl+P still opens the command
            // palette for a global keyboard jump.)
            Row {
                spacing: 0
                Repeater {
                    model: [
                        {
                            ic: "sun",
                            m: "light"
                        },
                        {
                            ic: "moon",
                            m: "dark"
                        },
                        {
                            ic: "monitor",
                            m: "system"
                        }
                    ]
                    delegate: Button {
                        id: apBtn
                        required property var modelData
                        implicitWidth: 32
                        implicitHeight: 30
                        background: Rectangle {
                            color: ThemeController.mode === apBtn.modelData.m ? DesignTokens.accentMuted : (apBtn.hovered ? Qt.rgba(1, 1, 1, 0.05) : "transparent")
                            border.width: 1
                            border.color: DesignTokens.borderSubtle
                        }
                        contentItem: AppIcon {
                            name: apBtn.modelData.ic
                            size: 16
                            anchors.centerIn: parent
                            color: ThemeController.mode === apBtn.modelData.m ? DesignTokens.accent : DesignTokens.textSecondary
                        }
                        onClicked: ThemeController.mode = apBtn.modelData.m
                    }
                }
            }

            // Split-orientation toggle for the editor/response arrangement now
            // lives in the Response panel header (more contextual).

            Label {
                text: qsTr("Environment")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontLabel
            }
            EnvironmentSelector {
                onNewRequested: manageEnvironmentDialog.openManager("")
                onManageRequested: manageEnvironmentDialog.openManager(AppController.environment)
            }

            CheckBox {
                id: captureCheck
                text: qsTr("Capture bodies")
                checked: AppController.captureBodies
                onToggled: AppController.captureBodies = checked
                contentItem: Text {
                    text: captureCheck.text
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontLabel
                    leftPadding: captureCheck.indicator.width + DesignTokens.spaceSm
                    verticalAlignment: Text.AlignVCenter
                }
                indicator: Rectangle {
                    implicitWidth: 16
                    implicitHeight: 16
                    radius: 4
                    y: (captureCheck.height - height) / 2
                    color: captureCheck.checked ? DesignTokens.accent : DesignTokens.surfaceRaised
                    border.width: 1
                    border.color: captureCheck.checked ? DesignTokens.accent : DesignTokens.borderStrong
                    Text {
                        anchors.centerIn: parent
                        visible: captureCheck.checked
                        text: "✓"
                        color: DesignTokens.textInverse
                        font.pixelSize: DesignTokens.fontCaption
                        font.weight: DesignTokens.weightBold
                    }
                }
            }
        }
    }

    // ── Main body: 3-pane SplitView ────────────────────────────────────────
    SplitView {
        id: mainSplit
        anchors.fill: parent
        anchors.margins: 0
        spacing: 0
        orientation: Qt.Horizontal
        handle: Rectangle {
            implicitWidth: 6
            implicitHeight: 6
            color: SplitHandle.pressed ? DesignTokens.accent : DesignTokens.accentMuted
            opacity: SplitHandle.pressed ? 0.7 : (SplitHandle.hovered ? 0.5 : 0)
            Behavior on opacity {
                FadeMotion {}
            }
        }

        // Left: Explorer panel or collapsed rail.
        Rectangle {
            id: explorerPane
            SplitView.preferredWidth: window.explorerCollapsed ? 32 : 280
            SplitView.minimumWidth: window.explorerCollapsed ? 32 : 180
            SplitView.maximumWidth: window.explorerCollapsed ? 32 : 400
            color: "transparent"
            clip: true

            ExplorerPanel {
                id: explorerPanel
                anchors.fill: parent
                visible: !window.explorerCollapsed
                onCollapseRequested: window.explorerCollapsed = true
            }

            // Collapsed rail: single expand chevron.
            Rectangle {
                id: explorerRail
                anchors.fill: parent
                visible: window.explorerCollapsed
                radius: 0
                color: explorerRailArea.containsMouse ? DesignTokens.accentMuted : DesignTokens.glassFill
                border.width: 1
                border.color: DesignTokens.glassBorder
                Behavior on color {
                    ColorMotion {}
                }

                AppIcon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: DesignTokens.spaceLg
                    name: "chevron-right"
                    size: 18
                    color: explorerRailArea.containsMouse ? DesignTokens.accent : DesignTokens.textSecondary
                }
                Label {
                    anchors.centerIn: parent
                    text: qsTr("Explorer")
                    rotation: -90
                    color: explorerRailArea.containsMouse ? DesignTokens.accent : DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontLabel
                    font.weight: DesignTokens.weightMedium
                }
                MouseArea {
                    id: explorerRailArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: window.explorerCollapsed = false
                }
                GlassToolTip {
                    active: explorerRailArea.containsMouse
                    text: qsTr("Show Explorer")
                    x: parent.width + 6
                    y: DesignTokens.spaceLg
                }
            }
        }

        // Centre + Response live in a nested SplitView so they can be arranged
        // side-by-side (horizontal) or stacked (vertical) via the toolbar.
        SplitView {
            id: centerSplit
            SplitView.fillWidth: true
            orientation: window.responseStacked ? Qt.Vertical : Qt.Horizontal
            spacing: 0
            handle: Rectangle {
                implicitWidth: 6
                implicitHeight: 6
                color: SplitHandle.pressed ? DesignTokens.accent : DesignTokens.accentMuted
                opacity: SplitHandle.pressed ? 0.7 : (SplitHandle.hovered ? 0.5 : 0)
                Behavior on opacity {
                    FadeMotion {}
                }
            }

            // Centre: endpoint list or request editor (or empty state).
            Rectangle {
                id: centerPane
                SplitView.fillWidth: true
                SplitView.fillHeight: true
                SplitView.minimumWidth: 320
                SplitView.minimumHeight: 200
                radius: 0
                // Clip so the editor's fixed-width content (action toolbar,
                // 200px graph cards) can't bleed over the response pane when
                // the pane is narrower than its content.
                clip: true
                color: DesignTokens.glassFill
                border.width: 1
                border.color: DesignTokens.glassBorder

                // Open-tabs strip (endpoints + actors). Hidden when nothing is
                // open; the empty state / endpoint list show in that case.
                EditorTabBar {
                    id: editorTabs
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    visible: AppController.tabCount > 0
                }

                // Empty state when no project loaded.
                EmptyState {
                    visible: !AppController.hasOperation && !AppController.hasActor && AppController.resourceCount === 0
                    anchors.centerIn: parent
                    useBrandLogo: true
                    heading: qsTr("Welcome to Reqloom")
                    body: qsTr("Create a new project to start building requests, open an existing one, or import an OpenAPI spec or Postman collection.")
                    actionText: qsTr("New Project")
                    onActionTriggered: newProjectDialog.openDialog()
                    secondaryActionText: qsTr("Open Project")
                    onSecondaryActionTriggered: folderDialog.open()
                    tertiaryActionText: qsTr("Import (OpenAPI, Postman, Insomnia, …)…")
                    onTertiaryActionTriggered: importSpecDialog.open()
                }

                // Endpoint list for the selected module.
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: DesignTokens.spaceXl
                    spacing: DesignTokens.spaceLg
                    visible: !AppController.hasOperation && !AppController.hasActor && AppController.resourceCount > 0

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            text: AppController.selectedModule.length > 0 ? AppController.selectedModule : qsTr("Select a module")
                            color: DesignTokens.textPrimary
                            font.pixelSize: DesignTokens.fontTitle
                            font.weight: DesignTokens.weightSemiBold
                        }
                        Label {
                            id: epCountLabel
                            color: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontBody
                        }
                    }
                    ListView {
                        id: endpointList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: DesignTokens.spaceXs
                        model: AppController.operations
                        onCountChanged: epCountLabel.text = (count === 1 ? qsTr("1 endpoint") : qsTr("%1 endpoints").arg(count))

                        delegate: ItemDelegate {
                            id: opRow
                            required property string method
                            required property string name
                            required property string path
                            width: ListView.view.width
                            height: 56
                            background: Rectangle {
                                radius: DesignTokens.radiusSm
                                color: opRow.hovered ? Qt.rgba(1, 1, 1, 0.04) : DesignTokens.surfaceSunken
                                border.width: 1
                                border.color: opRow.hovered ? DesignTokens.borderStrong : DesignTokens.borderSubtle
                            }
                            contentItem: RowLayout {
                                anchors.fill: parent
                                spacing: DesignTokens.spaceMd
                                MethodBadge {
                                    method: opRow.method
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Label {
                                        text: opRow.name
                                        color: DesignTokens.textPrimary
                                        font.pixelSize: DesignTokens.fontBody
                                        font.weight: DesignTokens.weightMedium
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: opRow.path
                                        color: DesignTokens.textSecondary
                                        font.pixelSize: DesignTokens.fontLabel
                                        font.family: DesignTokens.fontMono
                                        elide: Text.ElideRight
                                    }
                                }
                                Label {
                                    text: "›"
                                    color: DesignTokens.textSecondary
                                    font.pixelSize: DesignTokens.fontSubtitle
                                    opacity: opRow.hovered ? 1.0 : 0.4
                                }
                            }
                            onClicked: AppController.selectOperation(AppController.selectedModule, opRow.name)
                        }
                        EmptyState {
                            visible: endpointList.count === 0 && AppController.resourceCount > 0
                            iconName: "plus"
                            heading: qsTr("No endpoints yet")
                            body: qsTr("Add an endpoint to this module to start sending requests.")
                            actionText: qsTr("New Endpoint")
                            onActionTriggered: explorerPanel.openNewEndpoint(AppController.selectedModule)
                        }
                    }
                }

                // Request editor (active tab is an operation). Sits below the
                // tab strip.
                RequestEditor {
                    anchors.top: editorTabs.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: DesignTokens.spaceLg
                    visible: AppController.hasOperation
                }

                // Actor detail (active tab is an actor). Sits below the tab
                // strip; the request editor and this are driven by the active
                // tab's kind, so only one is visible at a time.
                ActorDetail {
                    anchors.top: editorTabs.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: DesignTokens.spaceLg
                    visible: AppController.hasActor
                }
            }

            // Right: Response + Timeline (or collapsed rail).
            Rectangle {
                id: responsePane
                // Golden split: at the default 1280 window the centre area
                // (window − explorer) divides editor:response = φ:1, so
                // response ≈ (1280−280)/(φ+1). A constant, not a live binding,
                // so manual drags and window resizes don't re-snap it.
                SplitView.preferredWidth: window.responseCollapsed ? 32 : Math.round((1280 - 280) / (DesignTokens.phi + 1))
                SplitView.minimumWidth: window.responseCollapsed ? 32 : 200
                SplitView.maximumWidth: window.responseCollapsed ? 32 : 700
                SplitView.preferredHeight: window.responseCollapsed ? 32 : 320
                SplitView.minimumHeight: window.responseCollapsed ? 32 : 160
                color: "transparent"
                clip: true
                visible: AppController.hasOperation || AppController.hasResponse || window.historyReplayActive

                ResponsePanel {
                    id: responsePanel
                    anchors.fill: parent
                    visible: !window.responseCollapsed
                    stacked: window.responseStacked
                    onCloseRequested: window.responseCollapsed = true
                    onToggleStackRequested: {
                        window.userChoseStack = true;
                        window.responseStacked = !window.responseStacked;
                    }
                    onSetStackedRequested: value => {
                        window.userChoseStack = true;
                        window.responseStacked = value;
                    }
                }

                Rectangle {
                    anchors.fill: parent
                    visible: window.responseCollapsed
                    radius: 0
                    color: responseRailArea.containsMouse ? DesignTokens.accentMuted : DesignTokens.glassFill
                    border.width: 1
                    border.color: DesignTokens.glassBorder
                    Behavior on color {
                        ColorMotion {}
                    }

                    // Side-by-side: vertical strip — chevron on top, rotated label.
                    Item {
                        anchors.fill: parent
                        visible: !window.responseStacked
                        AppIcon {
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: parent.top
                            anchors.topMargin: DesignTokens.spaceLg
                            name: "chevron-left"
                            size: 18
                            color: responseRailArea.containsMouse ? DesignTokens.accent : DesignTokens.textSecondary
                        }
                        Label {
                            anchors.centerIn: parent
                            text: qsTr("Response")
                            rotation: -90
                            color: responseRailArea.containsMouse ? DesignTokens.accent : DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontLabel
                            font.weight: DesignTokens.weightMedium
                        }
                    }
                    // Stacked: horizontal bar — chevron + label in a row.
                    RowLayout {
                        anchors.centerIn: parent
                        visible: window.responseStacked
                        spacing: DesignTokens.spaceSm
                        AppIcon {
                            name: "chevron-up"
                            size: 18
                            color: responseRailArea.containsMouse ? DesignTokens.accent : DesignTokens.textSecondary
                        }
                        Label {
                            text: qsTr("Response")
                            color: responseRailArea.containsMouse ? DesignTokens.accent : DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontLabel
                            font.weight: DesignTokens.weightMedium
                        }
                    }
                    MouseArea {
                        id: responseRailArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: window.responseCollapsed = false
                    }
                    GlassToolTip {
                        active: responseRailArea.containsMouse
                        text: qsTr("Show Response")
                        x: -width - 6
                        y: DesignTokens.spaceLg
                    }
                }
            }
        }
    }

    // ── Toast overlay (bottom centre) ──────────────────────────────────────
    Toast {
        id: toast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: DesignTokens.spaceLg
        z: 100
    }

    // ── Dialogs ────────────────────────────────────────────────────────────
    FolderDialog {
        id: folderDialog
        title: qsTr("Open Reqloom Project")
        onAccepted: AppController.openProject(selectedFolder)
    }

    // ── New Project: a single modal with a name + a location (Browse) — no
    //    surprise file manager opening before the user knows what's happening.
    FolderDialog {
        id: newProjectFolderDialog
        title: qsTr("Choose a location for the new project")
        // Only fills the dialog's location field; it never creates on its own.
        onAccepted: newProjectDialog.folderUrl = selectedFolder
    }

    Dialog {
        id: newProjectDialog
        modal: true
        enter: PopupEnter {}
        exit: PopupExit {}
        anchors.centerIn: Overlay.overlay
        width: 460
        padding: DesignTokens.spaceLg
        title: qsTr("New Project")

        property url folderUrl

        function openDialog() {
            npNameField.text = "";
            folderUrl = "";
            open();
        }
        onOpened: npNameField.forceActiveFocus()

        readonly property string folderDisplay: folderUrl.toString().length > 0 ? decodeURIComponent(folderUrl.toString().replace(/^file:\/\//, "")) : ""
        readonly property bool canCreate: AppController.isValidName(npNameField.text) && folderDisplay.length > 0

        header: DialogHeader {
            title: qsTr("New Project")
        }
        background: Rectangle {
            radius: DesignTokens.radiusLg
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.glassBorder
        }

        contentItem: ColumnLayout {
            spacing: DesignTokens.spaceSm

            Label {
                text: qsTr("Project name")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontLabel
            }
            TextField {
                id: npNameField
                Layout.fillWidth: true
                placeholderText: qsTr("My API project")
                color: DesignTokens.textPrimary
                placeholderTextColor: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontBody
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: DesignTokens.surfaceSunken
                    border.width: 1
                    border.color: npNameField.activeFocus ? DesignTokens.accent : DesignTokens.borderSubtle
                }
                onAccepted: if (newProjectDialog.canCreate) {
                    newProjectDialog.accept();
                }
            }

            Label {
                text: qsTr("Location")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontLabel
                Layout.topMargin: DesignTokens.spaceXs
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: DesignTokens.spaceSm
                TextField {
                    id: npLocationField
                    Layout.fillWidth: true
                    readOnly: true
                    text: newProjectDialog.folderDisplay
                    placeholderText: qsTr("Choose a folder…")
                    color: DesignTokens.textPrimary
                    placeholderTextColor: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontLabel
                    font.family: DesignTokens.fontMono
                    background: Rectangle {
                        radius: DesignTokens.radiusSm
                        color: DesignTokens.surfaceSunken
                        border.width: 1
                        border.color: DesignTokens.borderSubtle
                    }
                }
                Button {
                    id: npBrowseBtn
                    text: qsTr("Browse…")
                    implicitHeight: 34
                    leftPadding: DesignTokens.spaceMd
                    rightPadding: DesignTokens.spaceMd
                    onClicked: newProjectFolderDialog.open()
                    background: Rectangle {
                        radius: DesignTokens.radiusSm
                        color: "transparent"
                        border.width: 1
                        border.color: DesignTokens.borderSubtle
                    }
                    contentItem: Text {
                        text: npBrowseBtn.text
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontLabel
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("A reqloom.yaml is created in the chosen folder.")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontLabel
                wrapMode: Text.WordWrap
            }
        }

        footer: DialogButtons {
            okText: qsTr("Create")
            okEnabled: newProjectDialog.canCreate
            onAccepted: newProjectDialog.accept()
            onRejected: newProjectDialog.reject()
        }

        onAccepted: AppController.createProject(folderUrl, npNameField.text)
    }

    // ── Import flow: pick a spec/collection; the project is created in a
    //    named sub-folder next to the chosen file (no destination prompt). ────
    FileDialog {
        id: importSpecDialog
        title: qsTr("Import an API definition")
        nameFilters: [qsTr("API definitions (*.yaml *.yml *.json *.http *.rest *.bru)"), qsTr("All files (*)")]
        // Empty destination → the engine defaults it to the file's own folder
        // and creates a project sub-folder named after the collection.
        onAccepted: AppController.importOpenApi(selectedFile, "", false)
    }

    // Confirm overwriting an existing imported project folder.
    Dialog {
        id: importOverwriteDialog
        modal: true
        enter: PopupEnter {}
        exit: PopupExit {}
        anchors.centerIn: Overlay.overlay
        width: 420
        padding: DesignTokens.spaceLg
        title: qsTr("Overwrite project")

        property url specUrl
        property url targetUrl

        function confirmFor(spec, target) {
            specUrl = spec;
            targetUrl = target;
            open();
        }

        header: DialogHeader {
            title: qsTr("Overwrite project")
        }

        background: Rectangle {
            radius: DesignTokens.radiusLg
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.glassBorder
        }

        contentItem: Label {
            text: qsTr("A project already exists at this location. Replace it with the imported project?")
            color: DesignTokens.textPrimary
            font.pixelSize: DesignTokens.fontBody
            wrapMode: Text.WordWrap
        }

        footer: DialogButtons {
            okText: qsTr("Overwrite")
            okDestructive: true
            onAccepted: importOverwriteDialog.accept()
            onRejected: importOverwriteDialog.reject()
        }

        onAccepted: AppController.importOpenApi(specUrl, targetUrl, true)
    }

    // Per-operation review notes surfaced after a successful import.
    Dialog {
        id: importNotesDialog
        modal: true
        enter: PopupEnter {}
        exit: PopupExit {}
        anchors.centerIn: Overlay.overlay
        width: 560
        padding: DesignTokens.spaceLg
        title: qsTr("Import review notes")

        property string notes: ""

        function showNotes(text) {
            notes = text;
            open();
        }

        header: DialogHeader {
            title: qsTr("Import review notes")
        }

        background: Rectangle {
            radius: DesignTokens.radiusLg
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.glassBorder
        }

        contentItem: ScrollView {
            implicitHeight: 260
            clip: true

            TextArea {
                readOnly: true
                wrapMode: TextArea.Wrap
                text: importNotesDialog.notes
                color: DesignTokens.textPrimary
                font.pixelSize: DesignTokens.fontBody
                font.family: DesignTokens.fontMono
                background: null
            }
        }

        footer: DialogButtons {
            okText: qsTr("Done")
            onAccepted: importNotesDialog.accept()
            onRejected: importNotesDialog.reject()
        }
    }

    // Per-actor cookie jar inspector. Refreshed on open and after each run.
    Dialog {
        id: cookieDialog
        modal: true
        enter: PopupEnter {}
        exit: PopupExit {}
        anchors.centerIn: Overlay.overlay
        width: 560
        padding: DesignTokens.spaceLg
        title: qsTr("Cookies")

        property var jars: []
        function refresh() {
            jars = AppController.cookieJars();
        }
        function openDialog() {
            refresh();
            open();
        }

        header: DialogHeader {
            title: qsTr("Cookies")
        }
        background: Rectangle {
            radius: DesignTokens.radiusLg
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.glassBorder
        }

        Connections {
            target: AppController
            function onCookiesChanged() {
                if (cookieDialog.visible) {
                    cookieDialog.refresh();
                }
            }
        }

        contentItem: ColumnLayout {
            spacing: DesignTokens.spaceSm

            Label {
                Layout.fillWidth: true
                visible: cookieDialog.jars.length === 0
                text: qsTr("No cookies yet. Run a request whose actor receives a Set-Cookie.")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontBody
                wrapMode: Text.WordWrap
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 320
                visible: cookieDialog.jars.length > 0
                clip: true

                ColumnLayout {
                    width: parent.width
                    spacing: DesignTokens.spaceMd

                    Repeater {
                        model: cookieDialog.jars
                        delegate: ColumnLayout {
                            id: jarBlock
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: DesignTokens.spaceXs

                            Label {
                                text: jarBlock.modelData.actor
                                color: DesignTokens.textPrimary
                                font.pixelSize: DesignTokens.fontLabel
                                font.weight: DesignTokens.weightSemiBold
                            }
                            Repeater {
                                model: jarBlock.modelData.cookies
                                delegate: RowLayout {
                                    id: cookieRow
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: DesignTokens.spaceSm
                                    Label {
                                        text: cookieRow.modelData.name
                                        color: DesignTokens.textSecondary
                                        font.family: DesignTokens.fontMono
                                        font.pixelSize: DesignTokens.fontCaption
                                        Layout.preferredWidth: 160
                                        elide: Text.ElideRight
                                    }
                                    Label {
                                        text: cookieRow.modelData.value
                                        color: DesignTokens.textPrimary
                                        font.family: DesignTokens.fontMono
                                        font.pixelSize: DesignTokens.fontCaption
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        textFormat: Text.PlainText
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        footer: DialogButtons {
            okText: qsTr("Done")
            showCancel: false
            onAccepted: cookieDialog.accept()
            onRejected: cookieDialog.reject()
        }
    }

    GlassMenu {
        id: envMenu
        GlassMenuItem {
            text: qsTr("New Environment…")
            onTriggered: manageEnvironmentDialog.openManager("")
        }
        GlassMenuItem {
            text: AppController.environment.length > 0 ? qsTr("Edit “%1”…").arg(AppController.environment) : qsTr("Edit…")
            enabled: AppController.environment.length > 0
            onTriggered: manageEnvironmentDialog.openManager(AppController.environment)
        }
        MenuSeparator {}
        GlassMenuItem {
            text: AppController.environment.length > 0 ? qsTr("Delete “%1”").arg(AppController.environment) : qsTr("Delete")
            enabled: AppController.environments.length > 1 && AppController.environment.length > 0
            onTriggered: envDeleteDialog.openFor(AppController.environment)
        }
    }

    // Confirm deleting the active environment (destructive, no undo).
    Dialog {
        id: envDeleteDialog
        modal: true
        enter: PopupEnter {}
        exit: PopupExit {}
        anchors.centerIn: Overlay.overlay
        width: 400
        padding: DesignTokens.spaceLg
        title: qsTr("Delete environment")
        header: DialogHeader {
            title: qsTr("Delete environment")
        }

        property string targetEnv: ""

        function openFor(env) {
            targetEnv = env;
            open();
        }

        background: Rectangle {
            radius: DesignTokens.radiusLg
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.glassBorder
        }

        contentItem: Label {
            text: qsTr("Delete environment “%1” and its variables? This can't be undone.").arg(envDeleteDialog.targetEnv)
            color: DesignTokens.textPrimary
            font.pixelSize: DesignTokens.fontBody
            wrapMode: Text.WordWrap
        }

        footer: DialogButtons {
            okText: qsTr("Delete")
            okDestructive: true
            onAccepted: envDeleteDialog.accept()
            onRejected: envDeleteDialog.reject()
        }

        onAccepted: AppController.deleteEnvironment(targetEnv)
    }

    ManageEnvironmentDialog {
        id: manageEnvironmentDialog
    }
    SecretsDialog {
        id: secretsDialog
    }

    HistoryDialog {
        id: historyDialog
    }

    // ── Command palette (Ctrl+P) ────────────────────────────────────────────
    Popup {
        id: commandPalette
        width: 480
        implicitHeight: Math.min(paletteList.contentHeight + palInput.implicitHeight + DesignTokens.spaceLg * 3, 440)
        anchors.centerIn: Overlay.overlay
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        modal: false
        enter: PopupEnter {}
        exit: PopupExit {}
        padding: DesignTokens.spaceLg

        background: Rectangle {
            radius: DesignTokens.radius
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.borderStrong
        }

        contentItem: ColumnLayout {
            spacing: DesignTokens.spaceSm

            TextField {
                id: palInput
                Layout.fillWidth: true
                placeholderText: qsTr("Search commands…")
                color: DesignTokens.textPrimary
                placeholderTextColor: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontBody
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: DesignTokens.surfaceSunken
                    border.width: 1
                    border.color: palInput.activeFocus ? DesignTokens.accent : DesignTokens.borderSubtle
                }
                onTextChanged: paletteModel.applyFilter(text)
                Component.onCompleted: forceActiveFocus()
            }

            ListView {
                id: paletteList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                model: ListModel {
                    id: paletteModel
                    property string filter: ""

                    function applyFilter(text) {
                        filter = text.toLowerCase().trim();
                        buildItems();
                    }

                    function buildItems() {
                        clear();
                        const all = [
                            {
                                itemLabel: qsTr("New Project…"),
                                itemAction: "newProject"
                            },
                            {
                                itemLabel: qsTr("Open Project…"),
                                itemAction: "openProject"
                            },
                            {
                                itemLabel: qsTr("Import (OpenAPI or Postman)…"),
                                itemAction: "importOpenApi"
                            },
                            {
                                itemLabel: qsTr("Generate Hook Typings"),
                                itemAction: "hookTypings"
                            },
                            {
                                itemLabel: qsTr("Manage Secrets"),
                                itemAction: "secrets"
                            },
                            {
                                itemLabel: qsTr("View Cookies"),
                                itemAction: "cookies"
                            },
                            {
                                itemLabel: qsTr("New Module…"),
                                itemAction: "newModule"
                            },
                            {
                                itemLabel: qsTr("New Endpoint…"),
                                itemAction: "newEndpoint"
                            },
                            {
                                itemLabel: qsTr("Run"),
                                itemAction: "run"
                            },
                            {
                                itemLabel: qsTr("Dry Run"),
                                itemAction: "dryRun"
                            },
                            {
                                itemLabel: qsTr("Light mode"),
                                itemAction: "light"
                            },
                            {
                                itemLabel: qsTr("Dark mode"),
                                itemAction: "dark"
                            },
                        ];
                        for (const item of all) {
                            if (filter.length === 0 || item.itemLabel.toLowerCase().includes(filter)) {
                                append(item);
                            }
                        }
                    }

                    Component.onCompleted: buildItems()
                }

                delegate: ItemDelegate {
                    id: paletteItem
                    required property string itemLabel
                    required property string itemAction
                    width: ListView.view.width
                    height: 36
                    background: Rectangle {
                        radius: DesignTokens.radiusSm
                        color: paletteItem.hovered ? DesignTokens.accentMuted : "transparent"
                    }
                    contentItem: Text {
                        text: paletteItem.itemLabel
                        color: DesignTokens.textPrimary
                        font.pixelSize: DesignTokens.fontBody
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: {
                        commandPalette.close();
                        switch (paletteItem.itemAction) {
                        case "newProject":
                            newProjectDialog.openDialog();
                            break;
                        case "openProject":
                            folderDialog.open();
                            break;
                        case "importOpenApi":
                            importSpecDialog.open();
                            break;
                        case "hookTypings":
                            AppController.generateHookTypings();
                            break;
                        case "secrets":
                            secretsDialog.openDialog();
                            break;
                        case "cookies":
                            cookieDialog.openDialog();
                            break;
                        case "newModule":
                            explorerPanel.openNewModule();
                            break;
                        case "newEndpoint":
                            explorerPanel.openNewEndpoint("");
                            break;
                        case "run":
                            AppController.runSelected(false, false);
                            break;
                        case "dryRun":
                            AppController.runSelected(false, true);
                            break;
                        case "light":
                            ThemeController.mode = "light";
                            break;
                        case "dark":
                            ThemeController.mode = "dark";
                            break;
                        }
                    }
                }
            }
        }
    }
}
