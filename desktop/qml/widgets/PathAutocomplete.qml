// PathAutocomplete — a completion popup for an extraction path TextField. On
// focus/typing it lists leaf JSONPaths from the step's available response
// (AppController.suggestExtractionPaths) filtered by the current text; picking
// one replaces the field via the `picked` signal. Whole-field replace (paths
// aren't embedded like `{{vars}}`), so it's simpler than VariableAutocomplete.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

Item {
    id: root

    required property var field  // the path TextField being completed
    required property string operationId
    signal picked(string path)

    property var _items: []

    function refresh() {
        if (!field || !field.activeFocus) {
            popup.close();
            return;
        }
        root._items = AppController.suggestExtractionPaths(root.operationId, field.text);
        if (root._items.length === 0) {
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

    Connections {
        target: root.field
        function onTextChanged() {
            Qt.callLater(root.refresh);
        }
        function onActiveFocusChanged() {
            if (root.field.activeFocus) {
                Qt.callLater(root.refresh);
            } else {
                popup.close();
            }
        }
    }

    Popup {
        id: popup
        parent: root.field
        width: Math.max(240, root.field.width)
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
