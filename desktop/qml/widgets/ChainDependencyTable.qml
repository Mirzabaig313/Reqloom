// ChainDependencyTable — the whole chain as ONE table. A single header, then
// one row per step (method badge + endpoint id in the Endpoint column, that
// step's own extracted variables stacked in the Variable/Path columns), rows
// separated by dividers, and a single "+ Add dependency" + path hint at the
// bottom. Mirrors the New Endpoint dialog's single-table layout.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

ColumnLayout {
    id: root
    spacing: 0

    component CellField: TextField {
        color: DesignTokens.textPrimary
        placeholderTextColor: DesignTokens.textSecondary
        font.pixelSize: DesignTokens.fontLabel
        implicitHeight: 32
        background: Rectangle {
            radius: DesignTokens.radiusSm
            color: DesignTokens.surfaceSunken
            border.width: 1
            border.color: parent.activeFocus ? DesignTokens.accent : DesignTokens.borderSubtle
        }
    }

    // Column headers (shown once).
    RowLayout {
        Layout.fillWidth: true
        Layout.bottomMargin: DesignTokens.spaceSm
        spacing: DesignTokens.spaceSm
        Label {
            // Pin to exactly the data row's Endpoint column width so the two
            // independent RowLayouts line up.
            Layout.preferredWidth: 150
            Layout.minimumWidth: 150
            Layout.maximumWidth: 150
            text: qsTr("Endpoint")
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontCaption
            font.weight: DesignTokens.weightSemiBold
        }
        // Mirror the data row's nested field column (a fill column holding the
        // two equal fields) so "Save as" / "Extract from response" sit exactly
        // above their inputs.
        RowLayout {
            Layout.fillWidth: true
            spacing: DesignTokens.spaceSm
            Label {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                text: qsTr("Save as")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontCaption
                font.weight: DesignTokens.weightSemiBold
            }
            Label {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                text: qsTr("Extract from response")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontCaption
                font.weight: DesignTokens.weightSemiBold
            }
        }
        Item {
            Layout.preferredWidth: 28
        }
    }
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: DesignTokens.borderSubtle
    }

    // One row per step in the chain.
    Repeater {
        model: AppController.chainEditor
        delegate: ColumnLayout {
            id: step
            required property int index
            required property string operationId
            required property string method
            required property bool isTarget
            required property var extractModel
            required property string forEachOver
            required property bool forEachContinueOnError
            Layout.fillWidth: true
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: DesignTokens.spaceMd
                Layout.bottomMargin: DesignTokens.spaceMd
                spacing: DesignTokens.spaceSm

                // Endpoint column: method badge + id (+ target marker).
                ColumnLayout {
                    Layout.preferredWidth: 150
                    Layout.minimumWidth: 150
                    Layout.maximumWidth: 150
                    Layout.alignment: Qt.AlignTop
                    spacing: DesignTokens.spaceXs
                    MethodBadge {
                        method: step.method
                        Layout.preferredWidth: 54
                    }
                    Label {
                        Layout.fillWidth: true
                        text: step.operationId
                        color: DesignTokens.textPrimary
                        font.pixelSize: DesignTokens.fontLabel
                        font.family: DesignTokens.fontMono
                        wrapMode: Text.WrapAnywhere
                    }
                    Label {
                        visible: step.isTarget
                        text: qsTr("target")
                        color: DesignTokens.accent
                        font.pixelSize: DesignTokens.fontCaption
                    }
                }

                // This step's own extractions (editable, with a trailing ghost
                // row that grows as you type).
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: DesignTokens.spaceXs
                    Repeater {
                        model: step.extractModel
                        delegate: ColumnLayout {
                            id: exRow
                            required property int index
                            required property string key
                            required property string value
                            Layout.fillWidth: true
                            spacing: 2
                            // Live validity of the path against the step's
                            // available response (debounced to avoid re-parsing
                            // on every keystroke).
                            property var pathEval: ({
                                    state: "neutral"
                                })
                            Timer {
                                id: evalTimer
                                interval: 250
                                onTriggered: exRow.pathEval = AppController.evaluateExtractionPath(step.operationId, exRow.value)
                            }
                            onValueChanged: evalTimer.restart()
                            Component.onCompleted: evalTimer.restart()

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: DesignTokens.spaceSm
                                CellField {
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 1
                                    text: exRow.key
                                    placeholderText: qsTr("variable_name")
                                    onTextEdited: step.extractModel.setKey(exRow.index, text)
                                }
                                CellField {
                                    id: pathField
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 1
                                    text: exRow.value
                                    placeholderText: qsTr("data.id")
                                    rightPadding: 22
                                    onTextEdited: {
                                        step.extractModel.setValue(exRow.index, text);
                                        evalTimer.restart();
                                    }
                                    // Path autocomplete from the step's response.
                                    PathAutocomplete {
                                        field: pathField
                                        operationId: step.operationId
                                        onPicked: function (p) {
                                            step.extractModel.setValue(exRow.index, p);
                                            pathField.text = p;
                                            exRow.pathEval = AppController.evaluateExtractionPath(step.operationId, p);
                                        }
                                    }
                                    // Resolves (green) / no match (amber); hidden when neutral.
                                    Rectangle {
                                        width: 8
                                        height: 8
                                        radius: 4
                                        anchors.right: parent.right
                                        anchors.rightMargin: DesignTokens.spaceSm
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: exRow.pathEval.state === "match" || exRow.pathEval.state === "nomatch"
                                        color: exRow.pathEval.state === "match" ? DesignTokens.statusSuccess : DesignTokens.statusWarning
                                    }
                                }
                            }
                            // Resolved value preview (the "= 9f7e…" confirmation).
                            Label {
                                Layout.fillWidth: true
                                visible: exRow.pathEval.state === "match"
                                text: qsTr("= %1").arg(exRow.pathEval.value !== undefined ? exRow.pathEval.value : "")
                                color: DesignTokens.statusSuccess
                                font.pixelSize: DesignTokens.fontCaption
                                font.family: DesignTokens.fontMono
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                // Action column — always 28px wide so target rows (no remove
                // button) keep the same column alignment as removable rows.
                Item {
                    Layout.preferredWidth: 28
                    Layout.alignment: Qt.AlignTop
                    implicitHeight: 28
                    ToolButton {
                        id: stepRemove
                        visible: !step.isTarget
                        anchors.fill: parent
                        text: "✕"
                        onClicked: {
                            AppController.chainRemoveStep(step.operationId);
                            Qt.callLater(AppController.syncChainEditorMembership);
                        }
                        contentItem: Text {
                            text: stepRemove.text
                            color: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontLabel
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: DesignTokens.radiusSm
                            color: stepRemove.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                        }
                    }
                }
            }

            // Per-step fan-out control: run once, or once per item of an
            // upstream list resource (for-each / data-driven execution).
            RowLayout {
                Layout.fillWidth: true
                Layout.bottomMargin: DesignTokens.spaceMd
                spacing: DesignTokens.spaceSm
                visible: AppController.chainForEachOptions(step.operationId).length > 0
                Label {
                    text: qsTr("Run")
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontCaption
                    font.weight: DesignTokens.weightSemiBold
                }
                GlassComboBox {
                    id: forEachCombo
                    // Dynamic width: fit the current selection's text (+ chevron)
                    // so it never truncates and never sprawls.
                    Layout.preferredWidth: Math.ceil(forEachMetrics.width) + 56
                    TextMetrics {
                        id: forEachMetrics
                        font: forEachCombo.font
                        text: forEachCombo.displayText
                    }
                    GlassToolTip {
                        active: forEachCombo.hovered
                        text: qsTr("Run this step once, or once per item of an upstream list response (data-driven fan-out).")
                    }
                    readonly property var options: [qsTr("once")].concat(AppController.chainForEachOptions(step.operationId))
                    model: forEachCombo.options.map(function (o, i) {
                        return i === 0 ? o : qsTr("for each ") + o;
                    })
                    currentIndex: {
                        const i = forEachCombo.options.indexOf(step.forEachOver);
                        return i > 0 ? i : 0;
                    }
                    onActivated: function (i) {
                        AppController.chainSetForEach(step.operationId, i > 0 ? forEachCombo.options[i] : "");
                    }
                }
                Label {
                    visible: step.forEachOver.length > 0
                    Layout.fillWidth: true
                    text: qsTr("runs once per {{%1.…}} item").arg(step.forEachOver)
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontCaption
                    wrapMode: Text.WordWrap
                }
            }

            // For-each continue-on-error toggle (only when fanning out).
            CheckBox {
                id: continueOnErrorCheck
                visible: step.forEachOver.length > 0
                Layout.leftMargin: DesignTokens.spaceLg
                Layout.bottomMargin: DesignTokens.spaceMd
                leftPadding: 0
                spacing: DesignTokens.spaceXs
                checked: step.forEachContinueOnError
                onToggled: AppController.chainSetForEachContinueOnError(step.operationId, checked)
                // Themed indicator pinned left + vertically centred (the default
                // Basic indicator rendered black and collided with the label).
                indicator: Rectangle {
                    implicitWidth: 16
                    implicitHeight: 16
                    x: continueOnErrorCheck.leftPadding
                    y: (continueOnErrorCheck.height - height) / 2
                    radius: DesignTokens.radiusSm
                    color: continueOnErrorCheck.checked ? DesignTokens.accent : DesignTokens.surfaceSunken
                    border.width: 1
                    border.color: continueOnErrorCheck.checked ? DesignTokens.accent : DesignTokens.borderStrong
                    Text {
                        anchors.centerIn: parent
                        visible: continueOnErrorCheck.checked
                        text: "✓"
                        color: DesignTokens.textInverse
                        font.pixelSize: 11
                        font.weight: DesignTokens.weightBold
                    }
                }
                contentItem: Text {
                    text: qsTr("keep going if an item fails")
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontCaption
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: continueOnErrorCheck.indicator.width + continueOnErrorCheck.spacing
                }
            }

            // Row separator (under every step).
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: DesignTokens.borderSubtle
            }
        }
    }

    // Single add-dependency picker.
    GlassComboBox {
        id: addCombo
        Layout.fillWidth: true
        Layout.topMargin: DesignTokens.spaceMd
        readonly property var options: [qsTr("+ Add dependency")].concat(AppController.operationIds)
        model: addCombo.options
        currentIndex: 0
        onActivated: function (i) {
            if (i > 0) {
                AppController.chainAddDependency(addCombo.options[i]);
                addCombo.currentIndex = 0;
            }
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: DesignTokens.spaceSm
        text: qsTr("Body paths need no $ (data.id · items[0].id). Special sources need it: $.status_code · $.headers.X · $.cookies.X")
        color: DesignTokens.textSecondary
        font.pixelSize: DesignTokens.fontCaption
        wrapMode: Text.WordWrap
    }
}
