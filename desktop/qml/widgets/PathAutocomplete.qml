// PathAutocomplete — a completion popup for an extraction path TextField. When
// the user types, it lists leaf JSONPaths from the step's available response
// (AppController.suggestExtractionPaths) filtered by the current text, prefix
// matches first; clicking one replaces the field via the `picked` signal.
// Whole-field replace (paths aren't embedded like `{{vars}}`), so it's simpler
// than VariableAutocomplete. It never accepts from the keyboard.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

Item {
    id: root

    required property var field  // the path TextField being completed
    required property string operationId
    signal picked(string path)

    // A read-only or disabled field belongs to another step; picking would edit it.
    readonly property bool _live: root.enabled && !!root.field && !root.field.readOnly

    property var _items: []

    function refresh() {
        if (!root._live || !field.activeFocus) {
            popup.close();
            return;
        }
        const q = (field.text || "").toLowerCase();
        const all = AppController.suggestExtractionPaths(root.operationId, field.text);
        const prefix = [];
        const inner = [];
        for (let i = 0; i < all.length; ++i) {
            const p = String(all[i]).toLowerCase();
            if (p === q) {
                popup.close();
                return;
            }
            if (p.indexOf(q) === 0) {
                prefix.push(all[i]);
            } else {
                inner.push(all[i]);
            }
        }
        root._items = prefix.concat(inner);
        if (q.length === 0 || root._items.length === 0) {
            popup.close();
            return;
        }
        const r = field.cursorRectangle;
        popup.x = r.x;
        popup.y = r.y + r.height + 2;
        if (!popup.visible) {
            popup.open();
        }
    }

    on_LiveChanged: {
        if (!root._live) {
            popup.close();
        }
    }

    // Opens on typing only, so focusing a filled-in path offers nothing.
    Connections {
        target: root.field
        enabled: root._live
        function onTextEdited() {
            Qt.callLater(root.refresh);
        }
        function onActiveFocusChanged() {
            if (!root.field.activeFocus) {
                popup.close();
            }
        }
    }

    Popup {
        id: popup
        parent: root.field
        // Keep the popup inside the window and cap its width for narrow editors.
        margins: DesignTokens.spaceSm
        width: Math.min(Math.max(240, root.field.width), (Overlay.overlay ? Overlay.overlay.width : 640) - 2 * DesignTokens.spaceSm)
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
            implicitHeight: Math.min(contentHeight, 200)
            model: root._items
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            delegate: ItemDelegate {
                id: pathRow
                required property int index
                required property var modelData
                width: ListView.view.width
                implicitHeight: 30
                onClicked: {
                    root.picked(pathRow.modelData);
                    popup.close();
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: pathRow.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
                contentItem: Text {
                    text: pathRow.modelData
                    color: DesignTokens.textPrimary
                    font.pixelSize: DesignTokens.fontLabel
                    font.family: DesignTokens.fontMono
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }
}
