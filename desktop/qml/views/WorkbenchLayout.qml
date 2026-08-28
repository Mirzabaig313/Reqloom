// Concrete three-pane workbench with durable fluid layout state.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Item {
    id: root

    property bool explorerCollapsed: false
    property bool responseCollapsed: false
    property string orientationMode: "auto"
    property bool historyReplayActive: false
    readonly property bool creatingEndpoint: requestEditor.creating
    readonly property bool endpointSaveConfirmationActive: requestEditor.saveConfirmationActive
    readonly property bool editConfirmationActive: requestEditor.persistedEditConfirmationActive || actorDetail.editConfirmationActive

    signal newProjectRequested
    signal openProjectRequested
    signal importRequested
    signal endpointSaveConfirmationClosed
    signal environmentSourceRequested(string environmentName, string key)
    signal secretSourceRequested(string name)
    signal diagnosticNavigationFailed(string message)

    readonly property int _railSize: 32
    readonly property int _handleSize: 6
    readonly property int _initialThreshold: 1040
    readonly property int _verticalThreshold: 1008
    readonly property int _horizontalThreshold: 1072
    readonly property int _explorerMinimum: 180
    readonly property int _explorerMaximum: 400
    readonly property int _editorMinimumWidth: 320
    readonly property int _editorMinimumHeight: 200
    readonly property int _responseMinimumWidth: 200
    readonly property int _responseMaximumWidth: 700
    readonly property int _responseMinimumHeight: 160
    readonly property int _defaultResponseWidth: Math.round((1280 - 280) / (DesignTokens.phi + 1))
    readonly property int _defaultResponseHeight: 320
    readonly property bool _responseStacked: _effectiveOrientation === "vertical"

    property string _effectiveOrientation: "horizontal"
    property bool _restoring: true
    property bool _restored: false
    property int _expandedExplorerWidth: 280
    property int _expandedWorkbenchWidth: 1000
    property int _horizontalEditorWidth: 618
    property int _horizontalResponseWidth: _defaultResponseWidth
    property int _verticalEditorHeight: 480
    property int _verticalResponseHeight: _defaultResponseHeight

    function openNewModule() {
        explorerPanel.openNewModule();
    }

    function openNewEndpoint(moduleName) {
        if (requestEditor.creating) {
            return;
        }
        requestEditor.beginCreation(moduleName);
    }

    function submitEndpointCreation() {
        requestEditor.submitCreation();
    }

    function closeEndpointSaveConfirmation() {
        requestEditor.closeSaveConfirmation();
    }

    function editDiagnosticSource(action) {
        if (!action || action.canEditSource !== true) {
            return;
        }
        if (action.editKind === "environment") {
            environmentSourceRequested(action.editId, action.editField);
        } else if (action.editKind === "secret") {
            secretSourceRequested(action.editId);
        } else if (action.editKind === "actor") {
            if (AppController.actorNames.indexOf(action.editId) < 0) {
                diagnosticNavigationFailed(qsTr("That actor no longer exists."));
            } else {
                AppController.requestActorEdit(AppController.projectRoot, action.editId);
            }
        } else if (action.editKind === "extraction") {
            if (!requestEditor.openExtractionSource(action.editOperationId, action.editField)) {
                diagnosticNavigationFailed(qsTr("The operation for that extraction no longer exists."));
            }
        }
    }

    function restoreFocusAfterDraftDiscard() {
        Qt.callLater(function () {
            if (requestEditor.visible) {
                requestEditor.restoreFocus();
            } else if (AppController.hasActor) {
                actorDetail.restoreFocus();
            } else if (!root.explorerCollapsed) {
                explorerPanel.restoreFocus();
            } else {
                root.forceActiveFocus();
            }
        });
    }

    function _normalizedOrientation(value) {
        return value === "horizontal" || value === "vertical" ? value : "auto";
    }

    function _clamp(value, minimum, maximum) {
        return Math.max(minimum, Math.min(maximum, value));
    }

    function _mainAvailable() {
        return Math.max(0, Math.round(root.width) - root._handleSize);
    }

    function _horizontalAvailable() {
        const current = Math.round(centerSplit.width) - root._handleSize;
        return Math.max(0, current > 0 ? current : root._expandedWorkbenchWidth - root._handleSize);
    }

    function _verticalAvailable() {
        return Math.max(0, Math.round(root.height) - root._handleSize);
    }

    function _minimumWorkbenchWidth() {
        return root._effectiveOrientation === "horizontal" ? root._editorMinimumWidth + root._handleSize + root._responseMinimumWidth : root._editorMinimumWidth;
    }

    // Scale one saved pair to the current extent. The fill pane receives the
    // rounding remainder so the result always consumes the full SplitView.
    function _scaledPair(saved, available, firstMinimum, firstMaximum, secondMinimum, secondMaximum, fillIndex) {
        if (!saved || saved.length !== 2 || available <= 0) {
            return [];
        }
        const total = saved[0] + saved[1];
        if (total <= 0 || available < firstMinimum + secondMinimum) {
            return [];
        }
        const fixedIndex = fillIndex === 0 ? 1 : 0;
        const fixedMinimum = fixedIndex === 0 ? firstMinimum : secondMinimum;
        const fixedMaximum = fixedIndex === 0 ? firstMaximum : secondMaximum;
        const fixed = root._clamp(Math.round(available * saved[fixedIndex] / total), fixedMinimum, fixedMaximum);
        const fill = available - fixed;
        const fillMinimum = fillIndex === 0 ? firstMinimum : secondMinimum;
        const fillMaximum = fillIndex === 0 ? firstMaximum : secondMaximum;
        if (fill < fillMinimum || fill > fillMaximum) {
            return [];
        }
        return fillIndex === 0 ? [fill, fixed] : [fixed, fill];
    }

    function _defaultMainPair(available) {
        const maximumExplorer = Math.min(root._explorerMaximum, available - root._minimumWorkbenchWidth());
        if (maximumExplorer < root._explorerMinimum) {
            return [];
        }
        const explorer = root._clamp(280, root._explorerMinimum, maximumExplorer);
        return [explorer, available - explorer];
    }

    function _defaultHorizontalPair(available) {
        const maximumResponse = Math.min(root._responseMaximumWidth, available - root._editorMinimumWidth);
        if (maximumResponse < root._responseMinimumWidth) {
            return [];
        }
        const response = root._clamp(root._defaultResponseWidth, root._responseMinimumWidth, maximumResponse);
        return [available - response, response];
    }

    function _defaultVerticalPair(available) {
        const maximumResponse = available - root._editorMinimumHeight;
        if (maximumResponse < root._responseMinimumHeight) {
            return [];
        }
        const response = root._clamp(root._defaultResponseHeight, root._responseMinimumHeight, maximumResponse);
        return [available - response, response];
    }

    function _fitMainPair(saved) {
        const available = root._mainAvailable();
        let pair = root._scaledPair(saved, available, root._explorerMinimum, root._explorerMaximum, root._minimumWorkbenchWidth(), available, 1);
        if (pair.length !== 2) {
            pair = root._defaultMainPair(available);
        }
        if (pair.length === 2) {
            root._expandedExplorerWidth = pair[0];
            root._expandedWorkbenchWidth = pair[1];
        }
    }

    function _fitHorizontalPair(saved) {
        const available = root._horizontalAvailable();
        let pair = root._scaledPair(saved, available, root._editorMinimumWidth, available, root._responseMinimumWidth, root._responseMaximumWidth, 0);
        if (pair.length !== 2) {
            pair = root._defaultHorizontalPair(available);
        }
        if (pair.length === 2) {
            root._horizontalEditorWidth = pair[0];
            root._horizontalResponseWidth = pair[1];
        }
    }

    function _fitVerticalPair(saved) {
        const available = root._verticalAvailable();
        let pair = root._scaledPair(saved, available, root._editorMinimumHeight, available, root._responseMinimumHeight, available, 0);
        if (pair.length !== 2) {
            pair = root._defaultVerticalPair(available);
        }
        if (pair.length === 2) {
            root._verticalEditorHeight = pair[0];
            root._verticalResponseHeight = pair[1];
        }
    }

    function _applyMainPreferredSizes() {
        explorerPane.SplitView.preferredWidth = root.explorerCollapsed ? root._railSize : root._expandedExplorerWidth;
        centerSplit.SplitView.preferredWidth = root._expandedWorkbenchWidth;
    }

    function _applyCenterPreferredSizes() {
        if (root._responseStacked) {
            root._fitVerticalPair([root._verticalEditorHeight, root._verticalResponseHeight]);
            centerPane.SplitView.preferredHeight = root._verticalEditorHeight;
            responsePane.SplitView.preferredHeight = root.responseCollapsed ? root._railSize : root._verticalResponseHeight;
        } else {
            root._fitHorizontalPair([root._horizontalEditorWidth, root._horizontalResponseWidth]);
            centerPane.SplitView.preferredWidth = root._horizontalEditorWidth;
            responsePane.SplitView.preferredWidth = root.responseCollapsed ? root._railSize : root._horizontalResponseWidth;
        }
    }

    function _applyPreferredSizes() {
        root._applyMainPreferredSizes();
        root._applyCenterPreferredSizes();
    }

    function _captureMainIfExpanded() {
        // Rail-width checks (not the collapsed flag) gate this: the collapse
        // change-handler runs after the flag flips, so a flag guard here would
        // dead-block the pre-collapse snapshot. Rail-size still filters a
        // clamped pane if the layout pass already ran.
        if (explorerPane.width <= root._railSize || centerSplit.width <= 0) {
            return;
        }
        root._expandedExplorerWidth = Math.round(explorerPane.width);
        root._expandedWorkbenchWidth = Math.round(centerSplit.width);
    }

    function _captureCenterIfExpanded() {
        // See _captureMainIfExpanded: the per-dimension rail-size checks below
        // gate capture, not the collapsed flag.
        if (!responsePane.visible) {
            return;
        }
        if (root._responseStacked) {
            if (centerPane.height > 0 && responsePane.height > root._railSize) {
                root._verticalEditorHeight = Math.round(centerPane.height);
                root._verticalResponseHeight = Math.round(responsePane.height);
            }
        } else if (centerPane.width > 0 && responsePane.width > root._railSize) {
            root._horizontalEditorWidth = Math.round(centerPane.width);
            root._horizontalResponseWidth = Math.round(responsePane.width);
        }
    }

    function _saveMainPair() {
        LayoutSettingsController.saveSplitter("main", root._expandedExplorerWidth, root._expandedWorkbenchWidth);
    }

    function _saveCenterPair(orientation) {
        if (orientation === "vertical") {
            LayoutSettingsController.saveSplitter("centerVertical", root._verticalEditorHeight, root._verticalResponseHeight);
        } else {
            LayoutSettingsController.saveSplitter("centerHorizontal", root._horizontalEditorWidth, root._horizontalResponseWidth);
        }
    }

    function saveLayout() {
        if (root._restoring) {
            return;
        }
        root._captureMainIfExpanded();
        root._captureCenterIfExpanded();
        root._saveMainPair();
        root._saveCenterPair("horizontal");
        root._saveCenterPair("vertical");
    }

    function _changeEffectiveOrientation(next) {
        if (next === root._effectiveOrientation) {
            return;
        }
        if (root._restored) {
            root._captureCenterIfExpanded();
        }
        root._effectiveOrientation = next;
        Qt.callLater(function () {
            root._applyCenterPreferredSizes();
        });
    }

    function _updateAutoOrientation(initial) {
        if (root.orientationMode !== "auto" || root.width <= 0 || root._restoring && root._restored) {
            return;
        }
        if (initial) {
            root._changeEffectiveOrientation(root.width < root._initialThreshold ? "vertical" : "horizontal");
        } else if (root._effectiveOrientation === "horizontal" && root.width < root._verticalThreshold) {
            root._changeEffectiveOrientation("vertical");
        } else if (root._effectiveOrientation === "vertical" && root.width >= root._horizontalThreshold) {
            root._changeEffectiveOrientation("horizontal");
        }
    }

    function _applyRequestedOrientation(initial) {
        if (root.orientationMode === "auto") {
            root._updateAutoOrientation(initial);
        } else {
            root._changeEffectiveOrientation(root.orientationMode);
        }
    }

    function _tryRestoreLayout() {
        if (root._restored || root.width <= 0 || root.height <= 0) {
            return;
        }

        root.orientationMode = root._normalizedOrientation(LayoutSettingsController.responseOrientation);
        root._applyRequestedOrientation(true);
        root._fitMainPair(LayoutSettingsController.loadSplitter("main"));
        root._fitHorizontalPair(LayoutSettingsController.loadSplitter("centerHorizontal"));
        root._fitVerticalPair(LayoutSettingsController.loadSplitter("centerVertical"));
        root.explorerCollapsed = LayoutSettingsController.explorerCollapsed;
        root.responseCollapsed = LayoutSettingsController.responseCollapsed;
        root._restored = true;
        Qt.callLater(function () {
            root._applyPreferredSizes();
            root._restoring = false;
        });
    }

    onExplorerCollapsedChanged: {
        if (!root._restoring && root.explorerCollapsed) {
            root._captureMainIfExpanded();
        }
        root._applyMainPreferredSizes();
        if (!root._restoring) {
            LayoutSettingsController.explorerCollapsed = root.explorerCollapsed;
            root._saveMainPair();
        }
    }

    onResponseCollapsedChanged: {
        if (!root._restoring && root.responseCollapsed) {
            root._captureCenterIfExpanded();
        }
        root._applyCenterPreferredSizes();
        if (!root._restoring) {
            LayoutSettingsController.responseCollapsed = root.responseCollapsed;
            root._saveCenterPair(root._effectiveOrientation);
        }
    }

    onOrientationModeChanged: {
        const normalized = root._normalizedOrientation(root.orientationMode);
        if (normalized !== root.orientationMode) {
            root.orientationMode = normalized;
            return;
        }
        if (!root._restoring) {
            root._captureCenterIfExpanded();
            root._saveCenterPair(root._effectiveOrientation);
            LayoutSettingsController.responseOrientation = root.orientationMode;
        }
        root._applyRequestedOrientation(true);
    }

    onWidthChanged: {
        if (!root._restored) {
            root._tryRestoreLayout();
        } else if (!root._restoring) {
            root._updateAutoOrientation(false);
        }
    }
    onHeightChanged: {
        if (!root._restored) {
            root._tryRestoreLayout();
        }
    }
    Component.onCompleted: Qt.callLater(root._tryRestoreLayout)

    Connections {
        target: AppController
        function onRunReplayed() {
            root.historyReplayActive = true;
            root.responseCollapsed = false;
            responsePanel.showTimeline();
        }
        function onOperationChanged() {
            root.historyReplayActive = false;
        }
        function onProjectChanged() {
            root.historyReplayActive = false;
        }
        function onActiveTabChanged() {
            root.historyReplayActive = false;
        }
        function onRunningChanged() {
            if (AppController.running) {
                root.historyReplayActive = false;
            }
        }
    }

    SplitView {
        id: mainSplit
        anchors.fill: parent
        spacing: 0
        orientation: Qt.Horizontal
        handle: Rectangle {
            id: mainHandle
            implicitWidth: root._handleSize
            implicitHeight: root._handleSize
            color: DesignTokens.surfaceBase
            readonly property bool active: SplitHandle.pressed
            readonly property bool hovered: SplitHandle.hovered

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 1
                height: parent.height
                color: mainHandle.active ? DesignTokens.accent : (mainHandle.hovered ? DesignTokens.borderStrong : DesignTokens.borderSubtle)
                Behavior on color {
                    ColorMotion {}
                }
            }

            SplitHandle.onPressedChanged: {
                if (!SplitHandle.pressed && root._restored && !root._restoring) {
                    root._captureMainIfExpanded();
                    root._saveMainPair();
                }
            }
        }

        Rectangle {
            id: explorerPane
            SplitView.preferredWidth: 280
            SplitView.minimumWidth: root.explorerCollapsed ? root._railSize : root._explorerMinimum
            SplitView.maximumWidth: root.explorerCollapsed ? root._railSize : root._explorerMaximum
            color: DesignTokens.surfaceRaised
            clip: true

            ExplorerPanel {
                id: explorerPanel
                anchors.fill: parent
                visible: !root.explorerCollapsed
                color: DesignTokens.surfaceRaised
                border.width: 0
                onCollapseRequested: root.explorerCollapsed = true
                onNewEndpointRequested: resourceId => root.openNewEndpoint(resourceId)
            }

            Rectangle {
                id: explorerRail
                anchors.fill: parent
                visible: root.explorerCollapsed
                radius: 0
                color: explorerRailArea.containsMouse ? DesignTokens.accentMuted : DesignTokens.surfaceRaised
                border.width: 1
                border.color: DesignTokens.borderSubtle
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
                    onClicked: root.explorerCollapsed = false
                }
                GlassToolTip {
                    active: explorerRailArea.containsMouse
                    text: qsTr("Show Explorer")
                    x: parent.width + root._handleSize
                    y: DesignTokens.spaceLg
                }
            }
        }

        SplitView {
            id: centerSplit
            SplitView.fillWidth: true
            orientation: root._responseStacked ? Qt.Vertical : Qt.Horizontal
            spacing: 0
            handle: Rectangle {
                id: centerHandle
                implicitWidth: root._handleSize
                implicitHeight: root._handleSize
                color: DesignTokens.surfaceBase
                readonly property bool active: SplitHandle.pressed
                readonly property bool hovered: SplitHandle.hovered

                Rectangle {
                    anchors.centerIn: parent
                    width: centerSplit.orientation === Qt.Horizontal ? 1 : parent.width
                    height: centerSplit.orientation === Qt.Horizontal ? parent.height : 1
                    color: centerHandle.active ? DesignTokens.accent : (centerHandle.hovered ? DesignTokens.borderStrong : DesignTokens.borderSubtle)
                    Behavior on color {
                        ColorMotion {}
                    }
                }

                SplitHandle.onPressedChanged: {
                    if (!SplitHandle.pressed && root._restored && !root._restoring) {
                        root._captureCenterIfExpanded();
                        root._saveCenterPair(root._effectiveOrientation);
                    }
                }
            }

            Rectangle {
                id: centerPane
                SplitView.fillWidth: true
                SplitView.fillHeight: true
                SplitView.minimumWidth: root._editorMinimumWidth
                SplitView.minimumHeight: root._editorMinimumHeight
                radius: 0
                clip: true
                color: DesignTokens.surfaceBase
                border.width: 0

                EditorTabBar {
                    id: editorTabs
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    visible: AppController.tabCount > 0
                    enabled: visible
                }

                EmptyState {
                    visible: !requestEditor.creating && !AppController.hasOperation && !AppController.hasActor && AppController.resourceCount === 0
                    enabled: visible
                    anchors.centerIn: parent
                    useBrandLogo: true
                    heading: qsTr("Welcome to Reqloom")
                    body: qsTr("Create a new project to start building requests, open an existing one, or import an OpenAPI spec or Postman collection.")
                    actionText: qsTr("New Project")
                    onActionTriggered: root.newProjectRequested()
                    secondaryActionText: qsTr("Open Project")
                    onSecondaryActionTriggered: root.openProjectRequested()
                    tertiaryActionText: qsTr("Import (OpenAPI, Postman, Insomnia, …)…")
                    onTertiaryActionTriggered: root.importRequested()
                }

                ColumnLayout {
                    // Sit below the tab strip when it's shown, like RequestEditor
                    // and ActorDetail do — otherwise the module header paints over
                    // the open tabs.
                    anchors.top: editorTabs.visible ? editorTabs.bottom : parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: DesignTokens.spaceXl
                    spacing: DesignTokens.spaceLg
                    visible: !requestEditor.creating && !AppController.hasOperation && !AppController.hasActor && AppController.resourceCount > 0
                    enabled: visible

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
                        onCountChanged: epCountLabel.text = count === 1 ? qsTr("1 endpoint") : qsTr("%1 endpoints").arg(count)

                        delegate: ItemDelegate {
                            id: opRow
                            required property string method
                            required property string name
                            required property string path
                            width: ListView.view.width
                            height: 56
                            background: Rectangle {
                                radius: DesignTokens.radiusSm
                                color: opRow.visualFocus ? DesignTokens.accentMuted : (opRow.hovered ? DesignTokens.surfaceSunken : "transparent")
                                border.width: opRow.hovered || opRow.visualFocus ? 1 : 0
                                border.color: opRow.visualFocus ? DesignTokens.accent : DesignTokens.borderStrong
                                Behavior on color {
                                    ColorMotion {}
                                }
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
                                    opacity: opRow.hovered || opRow.visualFocus ? 1.0 : 0.4
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
                            onActionTriggered: root.openNewEndpoint(AppController.selectedModule)
                        }
                    }
                }

                RequestEditor {
                    id: requestEditor
                    anchors.top: editorTabs.visible ? editorTabs.bottom : parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: DesignTokens.spaceLg
                    visible: requestEditor.creating || AppController.hasOperation
                    enabled: visible
                    onSaveConfirmationClosed: root.endpointSaveConfirmationClosed()
                    onDiagnosticNavigationFailed: message => root.diagnosticNavigationFailed(message)
                }

                ActorDetail {
                    id: actorDetail
                    anchors.top: editorTabs.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: DesignTokens.spaceLg
                    visible: !requestEditor.creating && AppController.hasActor
                    enabled: visible
                }
            }

            Rectangle {
                id: responsePane
                SplitView.preferredWidth: root._defaultResponseWidth
                SplitView.minimumWidth: root.responseCollapsed ? root._railSize : root._responseMinimumWidth
                SplitView.maximumWidth: root.responseCollapsed ? root._railSize : root._responseMaximumWidth
                SplitView.preferredHeight: root._defaultResponseHeight
                SplitView.minimumHeight: root.responseCollapsed ? root._railSize : root._responseMinimumHeight
                color: DesignTokens.surfaceRaised
                clip: true
                visible: !requestEditor.creating && (AppController.hasOperation || AppController.hasResponse || root.historyReplayActive)
                enabled: visible

                ResponsePanel {
                    id: responsePanel
                    anchors.fill: parent
                    visible: !root.responseCollapsed
                    color: DesignTokens.surfaceRaised
                    border.width: 0
                    stacked: root._responseStacked
                    onCloseRequested: root.responseCollapsed = true
                    onToggleStackRequested: root.orientationMode = root._responseStacked ? "horizontal" : "vertical"
                    onSetStackedRequested: value => root.orientationMode = value ? "vertical" : "horizontal"
                    onOpenRequestFieldRequested: (operationId, field, key) => {
                        if (!requestEditor.openRequestField(operationId, field, key)) {
                            root.diagnosticNavigationFailed(qsTr("The operation for that diagnostic no longer exists."));
                        }
                    }
                    onEditSourceRequested: action => root.editDiagnosticSource(action)
                }

                Rectangle {
                    anchors.fill: parent
                    visible: root.responseCollapsed
                    radius: 0
                    color: responseRailArea.containsMouse ? DesignTokens.accentMuted : DesignTokens.surfaceRaised
                    border.width: 1
                    border.color: DesignTokens.borderSubtle
                    Behavior on color {
                        ColorMotion {}
                    }

                    Item {
                        anchors.fill: parent
                        visible: !root._responseStacked
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
                    RowLayout {
                        anchors.centerIn: parent
                        visible: root._responseStacked
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
                        onClicked: root.responseCollapsed = false
                    }
                    GlassToolTip {
                        active: responseRailArea.containsMouse
                        text: qsTr("Show Response")
                        x: -width - root._handleSize
                        y: DesignTokens.spaceLg
                    }
                }
            }
        }
    }
}
