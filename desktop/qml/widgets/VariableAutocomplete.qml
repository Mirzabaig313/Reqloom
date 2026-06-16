// VariableAutocomplete — a `{{` completion popup for a TextField/TextArea.
// When the caret sits inside an unclosed `{{ … }}`, it lists the variables the
// engine says are referenceable here (AppController.variableSuggestions) and,
// on pick, inserts `{{token}}` at the caret. The host wires its key events in
// via `Keys.forwardTo: [<id>.keyTarget]`; keys are only consumed while the
// popup is open, so normal typing is unaffected.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Item {
    id: root

    // The text input being completed and the operation whose chain defines the
    // available variables ("<module>.<op>"). `var` (not `Item`) so the
    // TextField/TextArea text API resolves without qmllint noise.
    required property var field
    required property string operationId

    // Forward target for the host's Keys.forwardTo.
    readonly property alias keyTarget: keyItem

    implicitWidth: 0
    implicitHeight: 0

    property var _all: []
    property var _filtered: []
    property int _start: -1
    property int _index: 0

    // The active `{{…` region under the caret, or null. `start` is the index of
    // the `{{`, `query` is the (trimmed) text typed after it.
    function _active() {
        if (!field) {
            return null;
        }
        const pos = field.cursorPosition;
        const head = field.text.substring(0, pos);
        const open = head.lastIndexOf("{{");
        if (open < 0) {
            return null;
        }
        const between = head.substring(open + 2);
        if (between.indexOf("}}") >= 0) {
            return null;
        }
        // If the caret sits inside a CLOSED, complete `{{resource.var}}`, defer
        // to the value picker (ValuePicker) rather than offering name suggestions.
        const close = field.text.indexOf("}}", pos);
        if (close >= 0) {
            const tail = field.text.substring(pos, close);
            if (tail.indexOf("{{") < 0) {
                const full = field.text.substring(open + 2, close).trim();
                if (full.indexOf(".") >= 0) {
                    return null;
                }
            }
        }
        return {
            "start": open,
            "query": between.trim()
        };
    }

    function refresh() {
        const active = _active();
        if (!active) {
            popup.close();
            root._all = [];
            return;
        }
        if (root._all.length === 0) {
            root._all = AppController.variableSuggestions(root.operationId);
        }
        const q = active.query.toLowerCase();
        const out = [];
        for (let i = 0; i < root._all.length; ++i) {
            const s = root._all[i];
            if (q.length === 0 || s.token.toLowerCase().indexOf(q) >= 0) {
                out.push(s);
            }
        }
        root._filtered = out;
        root._start = active.start;
        if (out.length === 0) {
            popup.close();
            return;
        }
        root._index = 0;
        const r = field.cursorRectangle;
        popup.x = r.x;
        popup.y = r.y + r.height + 2;
        if (!popup.visible) {
            popup.open();
        }
    }

    function apply(token) {
        const pos = field.cursorPosition;
        const insertText = "{{" + token + "}}";
        field.remove(root._start, pos);
        field.insert(root._start, insertText);
        field.cursorPosition = root._start + insertText.length;
        popup.close();
        root._all = [];
    }

    onOperationIdChanged: root._all = []

    Connections {
        target: root.field
        function onTextChanged() {
            Qt.callLater(root.refresh);
        }
        function onCursorPositionChanged() {
            Qt.callLater(root.refresh);
        }
        function onActiveFocusChanged() {
            if (!root.field.activeFocus) {
                popup.close();
            }
        }
    }

    Item {
        id: keyItem
        Keys.onPressed: function (event) {
            if (!popup.visible) {
                event.accepted = false;
                return;
            }
            if (event.key === Qt.Key_Down) {
                root._index = Math.min(root._index + 1, root._filtered.length - 1);
                event.accepted = true;
            } else if (event.key === Qt.Key_Up) {
                root._index = Math.max(root._index - 1, 0);
                event.accepted = true;
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Tab) {
                if (root._filtered.length > 0) {
                    root.apply(root._filtered[root._index].token);
                }
                event.accepted = true;
            } else if (event.key === Qt.Key_Escape) {
                popup.close();
                event.accepted = true;
            } else {
                event.accepted = false;
            }
        }
    }

    Popup {
        id: popup
        parent: root.field
        width: 340
        padding: DesignTokens.spaceXs
        closePolicy: Popup.NoAutoClose
        enter: PopupEnter {}

        background: Rectangle {
            radius: DesignTokens.radiusSm
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.glassBorder
        }

        contentItem: ListView {
            implicitHeight: Math.min(contentHeight, 240)
            model: root._filtered
            clip: true
            currentIndex: root._index
            boundsBehavior: Flickable.StopAtBounds
            delegate: ItemDelegate {
                id: row
                required property int index
                required property var modelData
                width: ListView.view.width
                implicitHeight: 34
                highlighted: index === root._index
                onClicked: root.apply(row.modelData.token)

                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: row.highlighted ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
                contentItem: RowLayout {
                    spacing: DesignTokens.spaceSm
                    Label {
                        text: row.modelData.token
                        color: DesignTokens.textPrimary
                        font.pixelSize: DesignTokens.fontLabel
                        font.family: DesignTokens.fontMono
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        text: row.modelData.kind
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontCaption
                    }
                }
            }
        }
    }
}
