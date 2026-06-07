// ExplorerPanel — the project explorer (QML Migration Roadmap WS-A). A single
// tree mirroring the old ProjectExplorerWidget: an "Actors" group and a
// "Resources" group → resource folders → operation leaves (method badge + name)
// → saved-example child rows. Live fuzzy filter (op id + method verb, empty
// parents hidden), select-to-preview, activate-to-run (double-click/Enter),
// per-row-type context menus, truncation tooltips, and a project-name header
// with "N operations · M actors". Presentation only; logic is in AppController.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

pragma ComponentBehavior: Bound

Rectangle {
    id: panel

    signal collapseRequested()

    radius: DesignTokens.radius
    color: DesignTokens.surfaceRaised
    border.width: 1
    border.color: DesignTokens.borderSubtle

    // Context target captured when a row's menu opens.
    property string ctxOperationId: ""
    property string ctxResourceId: ""
    property string ctxExampleName: ""

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: DesignTokens.spaceMd
        spacing: DesignTokens.spaceSm

        PanelHeader {
            Layout.fillWidth: true
            title: AppController.projectName.length > 0 ? AppController.projectName
                                                        : qsTr("Explorer")
            subtitle: AppController.projectName.length > 0
                      ? qsTr("%1 operations · %2 actors").arg(AppController.operationCount)
                                                         .arg(AppController.actorCount)
                      : qsTr("No project open")

            ToolButton {
                id: addBtn
                implicitWidth: 28
                implicitHeight: 28
                text: "＋"
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Add module or endpoint")
                onClicked: addMenu.popup()
                contentItem: Text {
                    text: addBtn.text
                    color: DesignTokens.textSecondary
                    font.pixelSize: 15
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: addBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
            }
            ToolButton {
                id: collapseBtn
                implicitWidth: 28
                implicitHeight: 28
                text: "‹"
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Collapse sidebar")
                onClicked: panel.collapseRequested()
                contentItem: Text {
                    text: collapseBtn.text
                    color: DesignTokens.textSecondary
                    font.pixelSize: 16
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: collapseBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
            }
        }

        // Live filter.
        Rectangle {
            Layout.fillWidth: true
            height: 34
            radius: DesignTokens.radiusSm
            color: DesignTokens.surfaceSunken
            border.width: 1
            border.color: filterField.activeFocus ? DesignTokens.accent : DesignTokens.borderSubtle
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: DesignTokens.spaceSm
                anchors.rightMargin: DesignTokens.spaceSm
                spacing: DesignTokens.spaceXs
                Label { text: "🔍"; font.pixelSize: 12; opacity: 0.6 }
                TextField {
                    id: filterField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Filter operations…")
                    color: DesignTokens.textPrimary
                    placeholderTextColor: DesignTokens.textSecondary
                    font.pixelSize: 13
                    background: null
                    verticalAlignment: TextInput.AlignVCenter
                    onTextChanged: AppController.setExplorerFilter(text)
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
            selectionModel: ItemSelectionModel { model: tree.model }

            // Re-expand after the filter or project rebuilds the model.
            Connections {
                target: AppController
                function onProjectChanged() { Qt.callLater(tree.expandRecursively) }
            }
            Component.onCompleted: Qt.callLater(expandRecursively)

            // Enter / Return activates (runs) the current operation row.
            Keys.onReturnPressed: {
                if (panel.currentOperationId.length > 0) {
                    AppController.activateOperationById(panel.currentOperationId)
                }
            }
            Keys.onEnterPressed: {
                if (panel.currentOperationId.length > 0) {
                    AppController.activateOperationById(panel.currentOperationId)
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

                implicitHeight: 30
                indentation: 14

                readonly property bool isOperation: kind === "operation"
                readonly property bool isExample: kind === "example"
                readonly property bool isResource: kind === "resource"
                readonly property bool isResourcesRoot: kind === "resourceGroup"

                onCurrentChanged: {
                    if (del.current) {
                        panel.currentOperationId = del.isOperation ? del.operationId : ""
                    }
                }

                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: del.current ? DesignTokens.accentMuted
                           : del.hovered ? Qt.rgba(1, 1, 1, 0.04) : "transparent"
                }

                contentItem: RowLayout {
                    spacing: DesignTokens.spaceXs

                    Label {
                        visible: !del.isOperation
                        text: del.kind === "actorGroup" || del.kind === "resourceGroup" ? "📁"
                              : del.kind === "resource" ? "📂"
                              : del.kind === "example" ? "⚡" : ""
                        font.pixelSize: 12
                        opacity: 0.8
                    }
                    MethodBadge {
                        visible: del.isOperation
                        method: del.isOperation ? del.method : "GET"
                    }
                    Label {
                        Layout.fillWidth: true
                        text: del.name
                        color: del.isExample ? DesignTokens.textSecondary
                                             : DesignTokens.textPrimary
                        font.pixelSize: 13
                        font.weight: del.current ? Font.DemiBold : Font.Normal
                        elide: Text.ElideRight
                        ToolTip.visible: hoverHandler.hovered && del.tooltip.length > 0
                        ToolTip.text: del.tooltip
                        HoverHandler { id: hoverHandler }
                    }
                }

                onClicked: {
                    if (del.isOperation) {
                        AppController.selectOperationById(del.operationId)
                    } else if (del.isExample) {
                        AppController.selectExample(del.operationId, del.exampleName)
                    } else {
                        tree.toggleExpanded(del.row)
                    }
                }

                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onDoubleTapped: {
                        if (del.isOperation) {
                            AppController.activateOperationById(del.operationId)
                        }
                    }
                }

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: {
                        panel.ctxOperationId = del.operationId
                        panel.ctxResourceId = del.resourceId
                        panel.ctxExampleName = del.exampleName
                        if (del.isExample) {
                            exampleMenu.popup()
                        } else if (del.isOperation) {
                            operationMenu.popup()
                        } else if (del.isResource) {
                            resourceMenu.popup()
                        } else if (del.isResourcesRoot) {
                            rootMenu.popup()
                        }
                    }
                }
            }
        }
    }

    // Current operation row id (for Enter-to-run), tracked from selection.
    property string currentOperationId: ""

    // ── Add menu (header "+") ────────────────────────────────────────────────
    Menu {
        id: addMenu
        MenuItem {
            text: qsTr("New Endpoint…")
            onTriggered: newEndpointDialog.openFor("")
        }
        MenuItem {
            text: qsTr("New Module…")
            onTriggered: newModuleDialog.openDialog()
        }
    }

    // ── Per-row-type context menus ───────────────────────────────────────────
    Menu {
        id: operationMenu
        MenuItem {
            text: qsTr("Edit")
            onTriggered: AppController.selectOperationById(panel.ctxOperationId)
        }
        MenuItem {
            text: qsTr("Rename")
            onTriggered: renameDialog.openFor("operation", panel.ctxOperationId)
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Delete")
            onTriggered: deleteDialog.openFor("operation", panel.ctxOperationId)
        }
    }
    Menu {
        id: resourceMenu
        MenuItem {
            text: qsTr("New Endpoint…")
            onTriggered: newEndpointDialog.openFor(panel.ctxResourceId)
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Rename")
            onTriggered: renameDialog.openFor("resource", panel.ctxResourceId)
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Delete")
            onTriggered: deleteDialog.openFor("resource", panel.ctxResourceId)
        }
    }
    Menu {
        id: rootMenu
        MenuItem {
            text: qsTr("New Module…")
            onTriggered: newModuleDialog.openDialog()
        }
        MenuItem {
            text: qsTr("New Endpoint…")
            onTriggered: newEndpointDialog.openFor("")
        }
    }
    Menu {
        id: exampleMenu
        MenuItem {
            text: qsTr("Rename")
            onTriggered: AppController.renameExample(panel.ctxOperationId, panel.ctxExampleName)
        }
        MenuItem {
            text: qsTr("Duplicate")
            onTriggered: AppController.duplicateExample(panel.ctxOperationId, panel.ctxExampleName)
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Delete")
            onTriggered: AppController.deleteExample(panel.ctxOperationId, panel.ctxExampleName)
        }
    }

    // ── Dialogs ──────────────────────────────────────────────────────────────
    NewModuleDialog { id: newModuleDialog }
    NewEndpointDialog { id: newEndpointDialog }

    Dialog {
        id: renameDialog
        modal: true
        anchors.centerIn: Overlay.overlay
        width: 380
        padding: DesignTokens.spaceLg
        title: qsTr("Rename")

        property string targetKind: ""
        property string targetId: ""

        function openFor(kind, id) {
            targetKind = kind
            targetId = id
            // Seed with the short name (part after the last dot for ops).
            const dot = id.lastIndexOf(".")
            renameField.text = (kind === "operation" && dot >= 0) ? id.substring(dot + 1) : id
            open()
            renameField.forceActiveFocus()
            renameField.selectAll()
        }

        background: Rectangle {
            radius: DesignTokens.radius
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.borderSubtle
        }

        contentItem: ColumnLayout {
            spacing: DesignTokens.spaceSm
            Label {
                text: qsTr("New name")
                color: DesignTokens.textSecondary
                font.pixelSize: 12
            }
            TextField {
                id: renameField
                Layout.fillWidth: true
                color: DesignTokens.textPrimary
                onTextChanged: {
                    const okBtn = renameButtons.standardButton(Dialog.Ok)
                    if (okBtn) {
                        okBtn.enabled = AppController.isValidName(text)
                    }
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: DesignTokens.surfaceSunken
                    border.width: 1
                    border.color: renameField.activeFocus ? DesignTokens.accent
                                                           : DesignTokens.borderSubtle
                }
            }
        }

        footer: DialogButtonBox {
            id: renameButtons
            standardButtons: Dialog.Ok | Dialog.Cancel
        }

        onAccepted: {
            if (targetKind === "operation") {
                AppController.renameOperation(targetId, renameField.text.trim())
            } else if (targetKind === "resource") {
                AppController.renameResource(targetId, renameField.text.trim())
            }
        }
    }

    Dialog {
        id: deleteDialog
        modal: true
        anchors.centerIn: Overlay.overlay
        width: 380
        padding: DesignTokens.spaceLg
        title: qsTr("Delete")

        property string targetKind: ""
        property string targetId: ""

        function openFor(kind, id) {
            targetKind = kind
            targetId = id
            open()
        }

        background: Rectangle {
            radius: DesignTokens.radius
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.borderSubtle
        }

        contentItem: Label {
            text: deleteDialog.targetKind === "resource"
                  ? qsTr("Delete module “%1” and all its endpoints?").arg(deleteDialog.targetId)
                  : qsTr("Delete endpoint “%1”?").arg(deleteDialog.targetId)
            color: DesignTokens.textPrimary
            font.pixelSize: 13
            wrapMode: Text.WordWrap
        }

        footer: DialogButtonBox {
            standardButtons: Dialog.Ok | Dialog.Cancel
        }

        onAccepted: {
            if (targetKind === "operation") {
                AppController.deleteOperation(targetId)
            } else if (targetKind === "resource") {
                AppController.deleteResource(targetId)
            }
        }
    }
}
