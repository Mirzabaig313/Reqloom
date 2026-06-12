// Main — the Reqloom application shell (ADR-007 WS-D). A 3-pane SplitView
// (explorer | editor | response/timeline), collapsible left+right rails,
// a native MenuBar, top toolbar, keyboard shortcuts, toasts, and an empty
// state. Logic lives in AppController/SecretsController/ThemeController.
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
            contentItem: Text {
                text: parent.text
                color: DesignTokens.textPrimary
                font.pixelSize: DesignTokens.fontBody
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: parent.highlighted ? DesignTokens.accentMuted : "transparent"
                radius: 4
            }
        }

        Menu {
            title: qsTr("File")
            MenuItem {
                text: qsTr("Open Project…")
                onTriggered: folderDialog.open()
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
                text: explorerCollapsed ? qsTr("Show Explorer") : qsTr("Hide Explorer")
                onTriggered: explorerCollapsed = !explorerCollapsed
            }
            MenuItem {
                text: responseCollapsed ? qsTr("Show Response") : qsTr("Hide Response")
                onTriggered: responseCollapsed = !responseCollapsed
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

    // ── Global shortcuts ───────────────────────────────────────────────────
    Shortcut {
        sequence: "Ctrl+B"
        onActivated: explorerCollapsed = !explorerCollapsed
    }
    Shortcut {
        sequence: "Ctrl+J"
        onActivated: responseCollapsed = !responseCollapsed
    }
    Shortcut {
        sequence: "Ctrl+P"
        onActivated: commandPalette.open()
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

            // Open Project
            Button {
                text: qsTr("Open Project")
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
                    text: parent.text
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontLabel
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: folderDialog.open()
            }

            // Manage Secrets
            Button {
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
                    text: parent.text
                    color: parent.enabled ? DesignTokens.textSecondary : DesignTokens.borderStrong
                    font.pixelSize: DesignTokens.fontLabel
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: secretsDialog.openDialog()
            }

            Item {
                Layout.fillWidth: true
                Layout.maximumWidth: 200
            }

            // Global filter — a pill search that narrows the explorer tree,
            // the prominent control the layout is built around.
            Rectangle {
                Layout.fillWidth: true
                Layout.maximumWidth: 460
                implicitHeight: 36
                radius: DesignTokens.radiusPill
                color: DesignTokens.surfaceSunken
                border.width: 1
                border.color: headerSearch.activeFocus ? DesignTokens.accent : DesignTokens.borderSubtle
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: DesignTokens.spaceMd
                    anchors.rightMargin: DesignTokens.spaceSm
                    spacing: DesignTokens.spaceSm
                    AppIcon {
                        name: "search"
                        size: 16
                    }
                    TextField {
                        id: headerSearch
                        Layout.fillWidth: true
                        placeholderText: qsTr("Search operations")
                        color: DesignTokens.textPrimary
                        placeholderTextColor: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontBody
                        background: null
                        leftPadding: 0
                        onTextChanged: AppController.setExplorerFilter(text)
                    }
                    AppIcon {
                        visible: headerSearch.text.length > 0
                        name: "x"
                        size: 14
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -6
                            cursorShape: Qt.PointingHandCursor
                            onClicked: headerSearch.clear()
                        }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.maximumWidth: 200
            }
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
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Appearance: %1").arg(apBtn.modelData.m)
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
                NumberAnimation {
                    duration: 120
                }
            }
        }

        // Left: Explorer panel or collapsed rail.
        Rectangle {
            id: explorerPane
            SplitView.preferredWidth: explorerCollapsed ? 32 : 280
            SplitView.minimumWidth: explorerCollapsed ? 32 : 180
            SplitView.maximumWidth: explorerCollapsed ? 32 : 400
            color: "transparent"

            ExplorerPanel {
                id: explorerPanel
                anchors.fill: parent
                visible: !explorerCollapsed
                onCollapseRequested: explorerCollapsed = true
            }

            // Collapsed rail: single expand chevron.
            Rectangle {
                id: explorerRail
                anchors.fill: parent
                visible: explorerCollapsed
                radius: 0
                color: explorerRailArea.containsMouse ? DesignTokens.accentMuted : DesignTokens.glassFill
                border.width: 1
                border.color: DesignTokens.glassBorder
                Behavior on color {
                    ColorAnimation {
                        duration: 120
                    }
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
                    onClicked: explorerCollapsed = false
                }
                ToolTip.visible: explorerRailArea.containsMouse
                ToolTip.text: qsTr("Show Explorer")
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
                    NumberAnimation {
                        duration: 120
                    }
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
                color: DesignTokens.glassFill
                border.width: 1
                border.color: DesignTokens.glassBorder

                // Empty state when no project loaded.
                EmptyState {
                    visible: !AppController.hasOperation && AppController.resourceCount === 0
                    anchors.centerIn: parent
                }

                // Endpoint list for the selected module.
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: DesignTokens.spaceXl
                    spacing: DesignTokens.spaceLg
                    visible: !AppController.hasOperation && AppController.resourceCount > 0

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
                        Label {
                            anchors.centerIn: parent
                            visible: endpointList.count === 0 && AppController.resourceCount > 0
                            text: qsTr("No endpoints in this module yet.")
                            color: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontBody
                        }
                    }
                }

                // Request editor.
                RequestEditor {
                    anchors.fill: parent
                    anchors.margins: DesignTokens.spaceMd
                    visible: AppController.hasOperation
                }
            }

            // Right: Response + Timeline (or collapsed rail).
            Rectangle {
                id: responsePane
                SplitView.preferredWidth: responseCollapsed ? 32 : 400
                SplitView.minimumWidth: responseCollapsed ? 32 : 200
                SplitView.maximumWidth: responseCollapsed ? 32 : 700
                SplitView.preferredHeight: responseCollapsed ? 32 : 320
                SplitView.minimumHeight: responseCollapsed ? 32 : 160
                color: "transparent"
                visible: AppController.hasOperation || AppController.hasResponse

                ResponsePanel {
                    anchors.fill: parent
                    visible: !responseCollapsed
                    stacked: window.responseStacked
                    onCloseRequested: responseCollapsed = true
                    onToggleStackRequested: window.responseStacked = !window.responseStacked
                }

                Rectangle {
                    anchors.fill: parent
                    visible: responseCollapsed
                    radius: 0
                    color: responseRailArea.containsMouse ? DesignTokens.accentMuted : DesignTokens.glassFill
                    border.width: 1
                    border.color: DesignTokens.glassBorder
                    Behavior on color {
                        ColorAnimation {
                            duration: 120
                        }
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
                        onClicked: responseCollapsed = false
                    }
                    ToolTip.visible: responseRailArea.containsMouse
                    ToolTip.text: qsTr("Show Response")
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

    GlassMenu {
        id: envMenu
        GlassMenuItem {
            text: qsTr("New Environment…")
            onTriggered: environmentDialog.openFor("")
        }
        GlassMenuItem {
            text: AppController.environment.length > 0 ? qsTr("Edit “%1”…").arg(AppController.environment) : qsTr("Edit…")
            enabled: AppController.environment.length > 0
            onTriggered: environmentDialog.openFor(AppController.environment)
        }
        MenuSeparator {}
        GlassMenuItem {
            text: AppController.environment.length > 0 ? qsTr("Delete “%1”").arg(AppController.environment) : qsTr("Delete")
            enabled: AppController.environments.length > 1 && AppController.environment.length > 0
            onTriggered: AppController.deleteEnvironment(AppController.environment)
        }
    }

    ManageEnvironmentDialog {
        id: manageEnvironmentDialog
    }
    SecretsDialog {
        id: secretsDialog
    }

    // ── Command palette (Ctrl+P) ────────────────────────────────────────────
    Popup {
        id: commandPalette
        width: 480
        implicitHeight: Math.min(paletteList.contentHeight + palInput.implicitHeight + DesignTokens.spaceLg * 3, 440)
        anchors.centerIn: Overlay.overlay
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        modal: false
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
                                itemLabel: qsTr("Open Project…"),
                                itemAction: "openProject"
                            },
                            {
                                itemLabel: qsTr("Manage Secrets"),
                                itemAction: "secrets"
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
                    required property string itemLabel
                    required property string itemAction
                    width: ListView.view.width
                    height: 36
                    background: Rectangle {
                        radius: DesignTokens.radiusSm
                        color: parent.hovered ? DesignTokens.accentMuted : "transparent"
                    }
                    contentItem: Text {
                        text: itemLabel
                        color: DesignTokens.textPrimary
                        font.pixelSize: DesignTokens.fontBody
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: {
                        commandPalette.close();
                        switch (itemAction) {
                        case "openProject":
                            folderDialog.open();
                            break;
                        case "secrets":
                            secretsDialog.openDialog();
                            break;
                        case "newModule":
                            explorerPanel.newModuleDialog.openDialog();
                            break;
                        case "newEndpoint":
                            explorerPanel.newEndpointDialog.openFor("");
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
