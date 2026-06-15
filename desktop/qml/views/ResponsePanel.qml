// ResponsePanel — the run result surface (QML Migration Roadmap WS-C). A top
// Response | Timeline tab bar: Response shows the status line (code coloured by
// class), a Save-as-example action, an examples dropdown to re-show a saved
// response, and Body (Tree) / Body (Raw) / Headers / Diff views; Timeline
// embeds the live per-step stream. Presentation only; state comes from
// AppController.
//
// Body (Tree) is a collapsible JSONPath-addressable tree (click a container to
// expand/collapse, click a leaf to copy its JSONPath). Diff reuses the tested
// LCS widgets::diff::lineDiff via AppController. The full collapsible
// ResponseBodyModel (roadmap T-C5) can still replace the JS tree for very large
// bodies.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Rectangle {
    id: panel
    radius: 0
    color: DesignTokens.glassFill
    border.width: 1
    border.color: DesignTokens.glassBorder

    function statusColor(code) {
        if (code >= 200 && code < 300)
            return DesignTokens.statusSuccess;
        if (code >= 300 && code < 400)
            return DesignTokens.statusWarning;
        if (code >= 400)
            return DesignTokens.statusError;
        return DesignTokens.textSecondary;
    }

    /// Emitted when the user closes the response panel; Main collapses it.
    signal closeRequested

    /// Whether the editor/response are stacked (true) or side-by-side (false),
    /// and a request to flip it — driven from Main's window.responseStacked.
    property bool stacked: false
    signal toggleStackRequested

    /// Pretty-print the Body (Raw) view (indented JSON) vs show it verbatim.
    property bool prettyRaw: true

    // Response body display format, auto-detected from the Content-Type header
    // and overridable via the Body (Raw) format dropdown (JSON/XML/HTML/…).
    readonly property var formatLabels: ["JSON", "XML", "HTML", "YAML", "JavaScript", "Markdown", "Text"]
    readonly property var formatValues: ["json", "xml", "html", "yaml", "javascript", "markdown", "text"]
    readonly property string detectedFormat: detectFormat(AppController.respHeaders)
    property string respFormat: detectedFormat
    // Re-snap to the detected format whenever a new response arrives.
    onDetectedFormatChanged: respFormat = detectedFormat
    // Preview rendered HTML / Markdown instead of the coloured source.
    property bool previewMode: false

    // Map a response's Content-Type to a display-format token. Defaults to JSON
    // (the overwhelmingly common API shape) when there's no usable hint.
    function detectFormat(headers) {
        const m = headers.toLowerCase().match(/content-type:\s*([^\n;]+)/);
        const ct = m ? m[1].trim() : "";
        if (ct.indexOf("json") >= 0)
            return "json";
        if (ct.indexOf("html") >= 0)
            return "html";
        if (ct.indexOf("xml") >= 0)
            return "xml";
        if (ct.indexOf("yaml") >= 0)
            return "yaml";
        if (ct.indexOf("javascript") >= 0)
            return "javascript";
        if (ct.indexOf("markdown") >= 0)
            return "markdown";
        if (ct.length === 0)
            return "json";
        return "text";
    }

    // The body text to display, pretty-printed when it's JSON and Pretty is on.
    function displayBody() {
        const body = AppController.respBody;
        if (body.length === 0)
            return "";
        if (respFormat === "json" && prettyRaw)
            return prettyJson(body);
        return body;
    }

    // Full flattened JSON tree for the open response body. Each row carries an
    // id + parentId (for collapse), a JSONPath (for copy), and hasChildren so
    // the delegate can draw an expander. Recomputed when the body changes.
    property var treeRows: buildTree(AppController.respBody)
    // Set of collapsed container node ids.
    property var collapsedNodes: ({})
    // Rows whose ancestors are all expanded; the tree ListView's model. Set by
    // recomputeVisible() (not a live binding, to avoid a model binding loop).
    property var visibleRows: []

    onTreeRowsChanged: {
        collapsedNodes = ({});
        recomputeVisible();
    }
    Component.onCompleted: recomputeVisible()

    function toggleCollapse(id) {
        const m = collapsedNodes;
        if (m[id]) {
            delete m[id];
        } else {
            m[id] = true;
        }
        collapsedNodes = m;
        recomputeVisible();
    }

    // Recompute the visible-row list: a row is hidden if any ancestor (via
    // parentId) is currently collapsed.
    function recomputeVisible() {
        const all = treeRows;
        const byId = {};
        for (const r of all)
            byId[r.id] = r;
        const result = [];
        for (const r of all) {
            let visible = true;
            let pid = r.parentId;
            while (pid >= 0) {
                if (collapsedNodes[pid]) {
                    visible = false;
                    break;
                }
                pid = byId[pid] ? byId[pid].parentId : -1;
            }
            if (visible)
                result.push(r);
        }
        visibleRows = result;
    }

    // Pretty-print a JSON body for display; returns the raw text unchanged when
    // it isn't valid JSON. Pure presentation formatting.
    function prettyJson(body) {
        if (body.length === 0)
            return "";
        try {
            return JSON.stringify(JSON.parse(body), null, 2);
        } catch (e) {
            return body;
        }
    }

    // Flatten a JSON body into collapsible rows for the Tree view. Each row:
    // {depth, field, value, isLeaf, hasChildren, id, parentId, path}. Non-JSON
    // bodies yield a single "(not JSON)" row. `path` is a JSONPath for copy.
    function buildTree(body) {
        const rows = [];
        if (body.length === 0)
            return rows;
        let root;
        try {
            root = JSON.parse(body);
        } catch (e) {
            rows.push({
                depth: 0,
                field: "(not JSON)",
                value: body,
                isLeaf: true,
                hasChildren: false,
                id: 0,
                parentId: -1,
                path: "$"
            });
            return rows;
        }
        let nextId = 0;
        function walk(node, field, depth, parentId, path) {
            const id = nextId++;
            if (node === null) {
                rows.push({
                    depth,
                    field,
                    value: "null",
                    isLeaf: true,
                    hasChildren: false,
                    id,
                    parentId,
                    path
                });
            } else if (Array.isArray(node)) {
                rows.push({
                    depth,
                    field,
                    value: "[" + node.length + "]",
                    isLeaf: false,
                    hasChildren: node.length > 0,
                    id,
                    parentId,
                    path
                });
                for (let i = 0; i < node.length; ++i)
                    walk(node[i], String(i), depth + 1, id, path + "[" + i + "]");
            } else if (typeof node === "object") {
                const keys = Object.keys(node);
                rows.push({
                    depth,
                    field,
                    value: "{" + keys.length + "}",
                    isLeaf: false,
                    hasChildren: keys.length > 0,
                    id,
                    parentId,
                    path
                });
                for (const k of keys)
                    walk(node[k], k, depth + 1, id, path + "." + k);
            } else {
                rows.push({
                    depth,
                    field,
                    value: String(node),
                    isLeaf: true,
                    hasChildren: false,
                    id,
                    parentId,
                    path
                });
            }
        }
        // Don't render a redundant root row for the top-level container.
        if (root !== null && typeof root === "object") {
            const keys = Array.isArray(root) ? root.map((_, i) => String(i)) : Object.keys(root);
            for (const k of keys) {
                const childPath = Array.isArray(root) ? "$[" + k + "]" : "$." + k;
                walk(root[k], k, 0, -1, childPath);
            }
        } else {
            walk(root, "value", 0, -1, "$");
        }
        return rows;
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: DesignTokens.spaceLg
        spacing: DesignTokens.spaceMd

        // ── Response | Timeline switch + close button ───────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: DesignTokens.spaceSm
            TabBar {
                id: topTabs
                Layout.fillWidth: true
                background: Rectangle {
                    color: "transparent"
                }
                Repeater {
                    model: [qsTr("Response"), qsTr("Timeline")]
                    delegate: TabButton {
                        id: topTabButton
                        required property string modelData
                        required property int index
                        contentItem: Text {
                            text: topTabButton.modelData
                            color: topTabs.currentIndex === topTabButton.index ? DesignTokens.textPrimary : DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontBody
                            font.weight: topTabs.currentIndex === topTabButton.index ? DesignTokens.weightSemiBold : DesignTokens.weightRegular
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            color: "transparent"
                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: 2
                                color: topTabs.currentIndex === topTabButton.index ? DesignTokens.accent : "transparent"
                            }
                        }
                    }
                }
            }
            Button {
                id: stackBtn
                implicitWidth: 28
                implicitHeight: 28
                GlassToolTip {
                    active: stackBtn.hovered
                    text: panel.stacked ? qsTr("Stacked — switch to side-by-side") : qsTr("Side-by-side — switch to stacked")
                }
                onClicked: panel.toggleStackRequested()
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: stackBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
                contentItem: AppIcon {
                    name: panel.stacked ? "rows" : "columns"
                    size: 16
                    anchors.centerIn: parent
                }
            }
            Button {
                id: closeBtn
                implicitWidth: 28
                implicitHeight: 28
                GlassToolTip {
                    active: closeBtn.hovered
                    text: qsTr("Close response panel")
                }
                onClicked: panel.closeRequested()
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: closeBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
                contentItem: AppIcon {
                    name: "x"
                    size: 16
                    anchors.centerIn: parent
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: topTabs.currentIndex

            // ── Response content ────────────────────────────────────────────
            ColumnLayout {
                spacing: DesignTokens.spaceMd

                // Status line + Save-as-example + examples dropdown.
                RowLayout {
                    Layout.fillWidth: true
                    visible: AppController.hasResponse
                    spacing: DesignTokens.spaceMd
                    Label {
                        text: AppController.respStatus > 0 ? ("HTTP " + AppController.respStatus) : AppController.runOutcome
                        color: panel.statusColor(AppController.respStatus)
                        font.pixelSize: DesignTokens.fontSubtitle
                        font.weight: DesignTokens.weightSemiBold
                    }
                    Label {
                        visible: AppController.shownExample.length > 0
                        text: "· " + AppController.shownExample
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontLabel
                        elide: Text.ElideRight
                    }
                    Label {
                        text: AppController.respElapsedMs + " ms"
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontLabel
                    }
                    Label {
                        text: AppController.respBodySize + " B"
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontLabel
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    Button {
                        id: saveExampleBtn
                        text: qsTr("Save as example")
                        onClicked: saveExampleDialog.open()
                        contentItem: Text {
                            text: saveExampleBtn.text
                            color: DesignTokens.accent
                            font.pixelSize: DesignTokens.fontLabel
                            font.weight: DesignTokens.weightSemiBold
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: DesignTokens.radiusSm
                            color: saveExampleBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                            border.width: 1
                            border.color: DesignTokens.borderSubtle
                        }
                    }
                }

                // Examples dropdown — re-show a saved example for this op.
                RowLayout {
                    Layout.fillWidth: true
                    visible: examplesCombo.count > 0
                    spacing: DesignTokens.spaceSm
                    Label {
                        text: qsTr("Examples")
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontLabel
                    }
                    GlassComboBox {
                        id: examplesCombo
                        Layout.fillWidth: true
                        model: AppController.examples
                        textRole: "name"
                        onActivated: AppController.showExample(currentText)
                    }
                }

                // Inner Body(Tree) | Body(Raw) | Headers | Diff tabs + Pretty.
                RowLayout {
                    Layout.fillWidth: true
                    visible: AppController.hasResponse
                    spacing: DesignTokens.spaceSm
                    TabBar {
                        id: respTabs
                        Layout.fillWidth: true
                        visible: AppController.hasResponse
                        background: Rectangle {
                            color: "transparent"
                        }
                        Repeater {
                            model: [qsTr("Body (Tree)"), qsTr("Body (Raw)"), qsTr("Headers"), qsTr("Diff")]
                            delegate: TabButton {
                                id: respTabButton
                                required property string modelData
                                required property int index
                                contentItem: Text {
                                    text: respTabButton.modelData
                                    color: respTabs.currentIndex === respTabButton.index ? DesignTokens.textPrimary : DesignTokens.textSecondary
                                    font.pixelSize: DesignTokens.fontBody
                                    font.weight: respTabs.currentIndex === respTabButton.index ? DesignTokens.weightSemiBold : DesignTokens.weightRegular
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    color: "transparent"
                                    Rectangle {
                                        anchors.bottom: parent.bottom
                                        width: parent.width
                                        height: 2
                                        color: respTabs.currentIndex === respTabButton.index ? DesignTokens.accent : "transparent"
                                    }
                                }
                            }
                        }
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    Button {
                        id: prettyBtn
                        visible: respTabs.currentIndex === 3
                        implicitHeight: 26
                        leftPadding: DesignTokens.spaceSm
                        rightPadding: DesignTokens.spaceSm
                        checkable: true
                        checked: panel.prettyRaw
                        onToggled: panel.prettyRaw = checked
                        background: Rectangle {
                            radius: DesignTokens.radiusSm
                            color: prettyBtn.checked ? DesignTokens.accentMuted : (prettyBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent")
                            border.width: 1
                            border.color: prettyBtn.checked ? DesignTokens.accent : DesignTokens.borderSubtle
                        }
                        contentItem: Text {
                            text: qsTr("Pretty")
                            color: prettyBtn.checked ? DesignTokens.accent : DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontLabel
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: AppController.hasResponse
                    currentIndex: respTabs.currentIndex

                    // Body (Tree) — flattened JSON rows (field → value), indented
                    // by depth. Non-JSON bodies render as a single "(not JSON)" row.
                    Rectangle {
                        radius: DesignTokens.radiusSm
                        color: DesignTokens.surfaceSunken
                        border.width: 1
                        border.color: DesignTokens.borderSubtle
                        ListView {
                            id: treeView
                            anchors.fill: parent
                            anchors.margins: DesignTokens.spaceSm
                            clip: true
                            model: panel.visibleRows
                            ScrollBar.vertical: ScrollBar {}
                            delegate: Rectangle {
                                id: treeRow
                                required property var modelData
                                width: ListView.view.width
                                height: 22
                                radius: DesignTokens.radiusSm
                                color: rowMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.04) : "transparent"

                                MouseArea {
                                    id: rowMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (treeRow.modelData.hasChildren) {
                                            panel.toggleCollapse(treeRow.modelData.id);
                                        } else {
                                            AppController.copyToClipboard(treeRow.modelData.value, qsTr("value"));
                                        }
                                    }
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: DesignTokens.spaceXs
                                    anchors.rightMargin: DesignTokens.spaceXs
                                    spacing: DesignTokens.spaceSm
                                    Item {
                                        Layout.preferredWidth: treeRow.modelData.depth * 16
                                    }
                                    Text {
                                        Layout.preferredWidth: 12
                                        text: treeRow.modelData.hasChildren ? (panel.collapsedNodes[treeRow.modelData.id] ? "\u25B8" : "\u25BE") : ""
                                        color: DesignTokens.textSecondary
                                        font.pixelSize: DesignTokens.fontCaption
                                        horizontalAlignment: Text.AlignHCenter
                                    }
                                    Text {
                                        id: fieldText
                                        text: treeRow.modelData.field
                                        color: fieldMouse.containsMouse ? DesignTokens.accent : DesignTokens.textSecondary
                                        font.pixelSize: DesignTokens.fontLabel
                                        font.family: DesignTokens.fontMono
                                        // Clicking the key copies its JSONPath
                                        // (the value is copied by clicking the row).
                                        MouseArea {
                                            id: fieldMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: AppController.copyToClipboard(treeRow.modelData.path, treeRow.modelData.path)
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: treeRow.modelData.value
                                        color: treeRow.modelData.isLeaf ? DesignTokens.textPrimary : DesignTokens.textSecondary
                                        font.pixelSize: DesignTokens.fontLabel
                                        font.family: DesignTokens.fontMono
                                        font.weight: treeRow.modelData.isLeaf ? DesignTokens.weightRegular : DesignTokens.weightSemiBold
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        visible: fieldMouse.containsMouse
                                        text: qsTr("copy path")
                                        color: DesignTokens.accent
                                        font.pixelSize: DesignTokens.fontCaption
                                    }
                                    Text {
                                        visible: rowMouse.containsMouse && !fieldMouse.containsMouse && treeRow.modelData.isLeaf
                                        text: qsTr("copy value")
                                        color: DesignTokens.accent
                                        font.pixelSize: DesignTokens.fontCaption
                                    }
                                    // Save this response value as a chain variable
                                    // (extraction) on the current endpoint. The
                                    // tree already carries the JSONPath (with any
                                    // array index), so picking is one click.
                                    Text {
                                        id: extractHint
                                        visible: extractMouse.containsMouse || (rowMouse.containsMouse && treeRow.modelData.isLeaf)
                                        text: qsTr("＋ save as variable")
                                        color: extractMouse.containsMouse ? DesignTokens.accentHover : DesignTokens.accent
                                        font.pixelSize: DesignTokens.fontCaption
                                        font.weight: DesignTokens.weightSemiBold
                                        MouseArea {
                                            id: extractMouse
                                            anchors.fill: parent
                                            anchors.margins: -4
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: extractDialog.openFor(treeRow.modelData.path, treeRow.modelData.field)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Body (Raw) — syntax-highlighted source with a format
                    // dropdown + a Preview toggle (renders HTML / Markdown).
                    ColumnLayout {
                        spacing: DesignTokens.spaceSm

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: DesignTokens.spaceSm
                            GlassComboBox {
                                id: formatCombo
                                Layout.preferredWidth: 150
                                model: panel.formatLabels
                                Component.onCompleted: currentIndex = panel.formatValues.indexOf(panel.respFormat)
                                onActivated: panel.respFormat = panel.formatValues[currentIndex]
                                Connections {
                                    target: panel
                                    function onRespFormatChanged() {
                                        formatCombo.currentIndex = panel.formatValues.indexOf(panel.respFormat);
                                    }
                                }
                            }
                            Button {
                                id: previewBtn
                                visible: panel.respFormat === "html" || panel.respFormat === "markdown"
                                implicitHeight: 30
                                leftPadding: DesignTokens.spaceMd
                                rightPadding: DesignTokens.spaceMd
                                checkable: true
                                checked: panel.previewMode
                                onToggled: panel.previewMode = checked
                                background: Rectangle {
                                    radius: DesignTokens.radiusSm
                                    color: previewBtn.checked ? DesignTokens.accentMuted : (previewBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent")
                                    border.width: 1
                                    border.color: previewBtn.checked ? DesignTokens.accent : DesignTokens.borderSubtle
                                }
                                contentItem: Text {
                                    text: qsTr("Preview")
                                    color: previewBtn.checked ? DesignTokens.accent : DesignTokens.textSecondary
                                    font.pixelSize: DesignTokens.fontLabel
                                    font.weight: DesignTokens.weightSemiBold
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }
                            // JSON-only indent toggle — lives with the format
                            // dropdown so body formatting is one control cluster.
                            Button {
                                id: rawPrettyBtn
                                visible: panel.respFormat === "json"
                                implicitHeight: 30
                                leftPadding: DesignTokens.spaceMd
                                rightPadding: DesignTokens.spaceMd
                                checkable: true
                                checked: panel.prettyRaw
                                onToggled: panel.prettyRaw = checked
                                background: Rectangle {
                                    radius: DesignTokens.radiusSm
                                    color: rawPrettyBtn.checked ? DesignTokens.accentMuted : (rawPrettyBtn.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent")
                                    border.width: 1
                                    border.color: rawPrettyBtn.checked ? DesignTokens.accent : DesignTokens.borderSubtle
                                }
                                contentItem: Text {
                                    text: qsTr("Pretty")
                                    color: rawPrettyBtn.checked ? DesignTokens.accent : DesignTokens.textSecondary
                                    font.pixelSize: DesignTokens.fontLabel
                                    font.weight: DesignTokens.weightSemiBold
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                        }

                        // Coloured source view.
                        CodeView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            visible: !panel.previewMode
                            language: panel.respFormat
                            text: panel.displayBody()
                            placeholder: qsTr("(body not captured — enable “Capture bodies”)")
                        }

                        // Rendered preview (HTML / Markdown).
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            visible: panel.previewMode
                            radius: DesignTokens.radiusSm
                            color: DesignTokens.surfaceSunken
                            border.width: 1
                            border.color: DesignTokens.borderSubtle
                            ScrollView {
                                anchors.fill: parent
                                anchors.margins: DesignTokens.spaceSm
                                clip: true
                                TextArea {
                                    readOnly: true
                                    selectByMouse: true
                                    wrapMode: TextArea.Wrap
                                    textFormat: panel.respFormat === "markdown" ? TextEdit.MarkdownText : TextEdit.RichText
                                    text: AppController.respBody
                                    color: DesignTokens.textPrimary
                                    selectionColor: DesignTokens.accent
                                    selectedTextColor: DesignTokens.textInverse
                                    font.pixelSize: DesignTokens.fontBody
                                    background: null
                                }
                            }
                        }
                    }

                    // Headers
                    SelectableTextBox {
                        text: AppController.respHeaders
                    }

                    // Diff — compare the live body against a saved example.
                    ColumnLayout {
                        spacing: DesignTokens.spaceSm
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: DesignTokens.spaceSm
                            Label {
                                text: qsTr("Compare with")
                                color: DesignTokens.textSecondary
                                font.pixelSize: DesignTokens.fontLabel
                            }
                            GlassComboBox {
                                id: diffCombo
                                Layout.fillWidth: true
                                model: AppController.examples
                                textRole: "name"
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            radius: DesignTokens.radiusSm
                            color: DesignTokens.surfaceSunken
                            border.width: 1
                            border.color: DesignTokens.borderSubtle
                            ListView {
                                id: diffView
                                anchors.fill: parent
                                anchors.margins: DesignTokens.spaceSm
                                clip: true
                                ScrollBar.vertical: ScrollBar {}
                                model: diffCombo.count > 0 ? AppController.lineDiff(panel.prettyRaw ? panel.prettyJson(AppController.exampleBody(diffCombo.currentText)) : AppController.exampleBody(diffCombo.currentText), panel.prettyRaw ? panel.prettyJson(AppController.respBody) : AppController.respBody) : []
                                delegate: Text {
                                    id: diffRow
                                    required property var modelData
                                    width: ListView.view.width
                                    text: diffRow.modelData.sign + " " + diffRow.modelData.text
                                    color: diffRow.modelData.sign === "+" ? DesignTokens.statusSuccess : (diffRow.modelData.sign === "-" ? DesignTokens.statusError : DesignTokens.textSecondary)
                                    font.pixelSize: DesignTokens.fontLabel
                                    font.family: DesignTokens.fontMono
                                    wrapMode: Text.WrapAnywhere
                                }
                            }
                        }
                    }
                }

                // Empty state (no response yet).
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: !AppController.hasResponse
                    Item {
                        Layout.fillHeight: true
                    }
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("No response yet")
                        color: DesignTokens.textPrimary
                        font.pixelSize: DesignTokens.fontBody
                        font.weight: DesignTokens.weightMedium
                    }
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("Press Send to run this endpoint's chain.")
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontLabel
                    }
                    Item {
                        Layout.fillHeight: true
                    }
                }
            }

            // ── Timeline content ────────────────────────────────────────────
            TimelinePanel {
                // Embedded as a borderless surface inside the panel.
                color: "transparent"
                border.width: 0
            }
        }
    }

    // Save-as-example prompt.
    Dialog {
        id: saveExampleDialog
        modal: true
        anchors.centerIn: Overlay.overlay
        width: 380
        padding: DesignTokens.spaceLg
        title: qsTr("Save response as example")
        header: DialogHeader {
            title: qsTr("Save response as example")
        }

        onOpened: {
            exampleNameField.text = "HTTP " + AppController.respStatus;
            exampleNameField.forceActiveFocus();
            exampleNameField.selectAll();
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
                text: qsTr("Example name")
            }
            GlassTextField {
                id: exampleNameField
                Layout.fillWidth: true
            }
        }

        footer: DialogButtons {
            okText: qsTr("Save Response")
            okEnabled: exampleNameField.text.trim().length > 0
            onAccepted: saveExampleDialog.accept()
            onRejected: saveExampleDialog.reject()
        }

        onAccepted: AppController.saveResponse(exampleNameField.text.trim())
    }

    // Save-as-variable prompt (response-driven extract picker). The JSONPath
    // comes from the clicked tree node; the user just names the variable.
    Dialog {
        id: extractDialog
        modal: true
        anchors.centerIn: Overlay.overlay
        width: 440
        padding: DesignTokens.spaceLg
        title: qsTr("Save as variable")
        header: DialogHeader {
            title: qsTr("Save as variable")
        }

        property string sourcePath: ""

        function openFor(path, suggestedName) {
            sourcePath = path;
            extractNameField.text = suggestedName;
            open();
            extractNameField.forceActiveFocus();
            extractNameField.selectAll();
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
                text: qsTr("Variable name")
            }
            GlassTextField {
                id: extractNameField
                Layout.fillWidth: true
            }
            FieldLabel {
                text: qsTr("From this response path")
            }
            Label {
                Layout.fillWidth: true
                text: extractDialog.sourcePath
                color: DesignTokens.textPrimary
                font.pixelSize: DesignTokens.fontLabel
                font.family: DesignTokens.fontMono
                wrapMode: Text.WrapAnywhere
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Saved on this endpoint. Later steps reference it as {{%1.%2}}.").arg(AppController.selectedModule).arg(extractNameField.text.trim().length > 0 ? extractNameField.text.trim() : "<variable>")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontCaption
                wrapMode: Text.WordWrap
            }
        }

        footer: DialogButtons {
            okText: qsTr("Save variable")
            okEnabled: extractNameField.text.trim().length > 0
            onAccepted: extractDialog.accept()
            onRejected: extractDialog.reject()
        }

        onAccepted: AppController.addExtraction(extractNameField.text.trim(), extractDialog.sourcePath)
    }
}
