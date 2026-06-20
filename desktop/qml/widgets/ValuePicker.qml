// ValuePicker — a concrete-value dropdown for a `{{resource.var}}` reference.
// Two ways in: (a) attached to a text field, it opens when the caret sits
// inside a complete `{{resource.var}}`; (b) `openAt(token, item)` opens it
// anchored under an item (the view-mode path token chip). It lists the values
// captured from the producing (list) endpoint (AppController.candidateValues),
// lets the user search / pick / add a custom one, and pins the choice
// (AppController.setVariableOverride, Option A). Re-fetch re-runs the producer
// for fresh ids; the reference text is never modified.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Item {
    id: root

    // Optional: the text input for caret-driven mode (edit fields). When null,
    // only the explicit openAt() entry point is used (view-mode chips).
    property var field: null

    property string _token: ""
    property var _all: []
    property var _filtered: []
    property int _index: 0

    implicitWidth: 0
    implicitHeight: 0

    // The complete, closed `{{resource.var}}` the caret is inside, or null.
    function _region() {
        if (!field) {
            return null;
        }
        const pos = field.cursorPosition;
        if (pos <= 0) {
            return null;
        }
        const text = field.text;
        const open = text.lastIndexOf("{{", pos - 1);
        if (open < 0) {
            return null;
        }
        const mid = text.substring(open + 2, pos);
        if (mid.indexOf("}}") >= 0) {
            return null;
        }
        const close = text.indexOf("}}", pos);
        if (close < 0) {
            return null;
        }
        const tail = text.substring(pos, close);
        if (tail.indexOf("{{") >= 0) {
            return null;
        }
        const token = text.substring(open + 2, close).trim();
        if (token.length === 0 || token.indexOf(".") < 0 || token.indexOf("$") === 0) {
            return null;
        }
        return token;
    }

    function _applyFilter() {
        const q = search.text.toLowerCase();
        const out = [];
        for (let i = 0; i < root._all.length; ++i) {
            if (q.length === 0 || root._all[i].toLowerCase().indexOf(q) >= 0) {
                out.push(root._all[i]);
            }
        }
        root._filtered = out;
        root._index = 0;
    }

    // Caret-driven open (edit-mode text fields).
    function refresh() {
        const token = _region();
        if (!token) {
            popup.close();
            return;
        }
        root._token = token;
        root._all = AppController.candidateValues(token);
        _applyFilter();
        popup.parent = field;
        const r = field.cursorRectangle;
        popup.x = r.x;
        popup.y = r.y + r.height + 2;
        if (!popup.visible) {
            popup.open();
        }
    }

    // Explicit open anchored under `anchorItem` (view-mode token chip).
    function openAt(token, anchorItem) {
        root._token = token;
        root._all = AppController.candidateValues(token);
        _applyFilter();
        popup.parent = anchorItem;
        popup.x = 0;
        popup.y = anchorItem.height + 2;
        popup.open();
    }

    function pin(value) {
        AppController.setVariableOverride(root._token, value);
        popup.close();
    }

    Connections {
        target: root.field
        function onCursorPositionChanged() {
            Qt.callLater(root.refresh);
        }
        function onActiveFocusChanged() {
            if (!root.field.activeFocus && !popup.activeFocus) {
                popup.close();
            }
        }
    }

    // Re-query candidates after a re-fetch run saves a fresh example.
    Connections {
        target: AppController
        function onVariableOverridesChanged() {
            if (popup.visible) {
                root._all = AppController.candidateValues(root._token);
                root._applyFilter();
            }
        }
    }

    Popup {
        id: popup
        width: 360
        focus: true
        padding: DesignTokens.spaceXs
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        enter: PopupEnter {}
        exit: PopupExit {}
        onOpened: {
            search.text = "";
            root._applyFilter();
            search.forceActiveFocus();
        }
        background: Rectangle {
            radius: DesignTokens.radiusSm
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.glassBorder
        }

        contentItem: ColumnLayout {
            spacing: DesignTokens.spaceXs

            Label {
                Layout.fillWidth: true
                text: qsTr("Value for {{%1}}").arg(root._token)
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontCaption
                elide: Text.ElideRight
            }

            TextField {
                id: search
                Layout.fillWidth: true
                placeholderText: qsTr("Search or type a value")
                color: DesignTokens.textPrimary
                placeholderTextColor: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontLabel
                onTextChanged: root._applyFilter()
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: DesignTokens.surfaceSunken
                    border.width: 1
                    border.color: search.activeFocus ? DesignTokens.accent : DesignTokens.borderSubtle
                }
                Keys.onDownPressed: root._index = Math.min(root._index + 1, root._filtered.length - 1)
                Keys.onUpPressed: root._index = Math.max(root._index - 1, 0)
                Keys.onReturnPressed: {
                    if (root._filtered.length > 0) {
                        root.pin(root._filtered[root._index]);
                    } else if (search.text.trim().length > 0) {
                        root.pin(search.text.trim());
                    }
                }
            }

            ListView {
                Layout.fillWidth: true
                implicitHeight: Math.min(contentHeight, 200)
                model: root._filtered
                clip: true
                visible: root._filtered.length > 0
                currentIndex: root._index
                boundsBehavior: Flickable.StopAtBounds
                delegate: ItemDelegate {
                    id: vrow
                    required property int index
                    required property var modelData
                    width: ListView.view.width
                    implicitHeight: 32
                    highlighted: index === root._index
                    onClicked: root.pin(vrow.modelData)
                    background: Rectangle {
                        radius: DesignTokens.radiusSm
                        color: vrow.highlighted ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                    }
                    contentItem: RowLayout {
                        spacing: DesignTokens.spaceSm
                        Label {
                            Layout.fillWidth: true
                            text: vrow.modelData
                            color: DesignTokens.textPrimary
                            font.pixelSize: DesignTokens.fontLabel
                            font.family: DesignTokens.fontMono
                            elide: Text.ElideRight
                        }
                        Label {
                            visible: vrow.modelData === AppController.variableOverride(root._token)
                            text: "✓"
                            color: DesignTokens.accent
                            font.pixelSize: DesignTokens.fontLabel
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: root._filtered.length === 0 && search.text.trim().length === 0
                text: AppController.producerOpFor(root._token).length === 0 ? qsTr("Nothing produces {{%1}} — add an Extract that returns it on the list step, and point the path at that variable.").arg(root._token) : qsTr("No saved values yet — Re-fetch to pull them from %1.").arg(AppController.producerOpFor(root._token))
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontCaption
                wrapMode: Text.WordWrap
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: DesignTokens.borderSubtle
            }

            // Explicit "Add custom" — pins exactly what's typed in the search
            // box. Mirrors the Apidog/Postman "+ Add custom" affordance.
            ItemDelegate {
                id: addCustom
                Layout.fillWidth: true
                implicitHeight: 32
                enabled: search.text.trim().length > 0
                onClicked: root.pin(search.text.trim())
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: addCustom.hovered && addCustom.enabled ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
                contentItem: Label {
                    text: search.text.trim().length > 0 ? qsTr("＋ Add custom “%1”").arg(search.text.trim()) : qsTr("＋ Add custom value")
                    color: addCustom.enabled ? DesignTokens.accent : DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontLabel
                    elide: Text.ElideRight
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: DesignTokens.spaceSm
                Button {
                    id: refetchBtn
                    text: qsTr("Re-fetch")
                    enabled: !AppController.running
                    onClicked: AppController.refreshCandidates(root._token)
                    background: Rectangle {
                        radius: DesignTokens.radiusSm
                        color: refetchBtn.down ? DesignTokens.surfaceSunken : "transparent"
                        border.width: 1
                        border.color: DesignTokens.borderSubtle
                    }
                    contentItem: Text {
                        text: refetchBtn.text
                        color: DesignTokens.textPrimary
                        font.pixelSize: DesignTokens.fontLabel
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                Item {
                    Layout.fillWidth: true
                }
                Button {
                    id: clearBtn
                    text: qsTr("Clear pin")
                    enabled: AppController.variableOverride(root._token).length > 0
                    onClicked: root.pin("")
                    background: Rectangle {
                        radius: DesignTokens.radiusSm
                        color: clearBtn.down ? DesignTokens.surfaceSunken : "transparent"
                        border.width: 1
                        border.color: DesignTokens.borderSubtle
                    }
                    contentItem: Text {
                        text: clearBtn.text
                        color: clearBtn.enabled ? DesignTokens.textPrimary : DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontLabel
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }
    }
}
