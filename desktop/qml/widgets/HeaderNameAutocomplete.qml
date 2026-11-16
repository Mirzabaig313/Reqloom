// HeaderNameAutocomplete — a Postman-style dropdown of common HTTP header names
// for a header Key TextField. Typing lists the standard headers that contain the
// text (prefix matches first); a click, or Enter on a row chosen with the arrow
// keys, emits `picked(name)`. The host wires keys via
// `Keys.forwardTo: [<id>.keyTarget]` — keys are only consumed while the popup is
// open, so normal typing/editing is unaffected.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import Reqloom

Item {
    id: root

    // The header Key TextField being completed. `var` (not Item) so the
    // TextField text API resolves without qmllint noise.
    required property var field
    // Off for non-header key fields (query/form) — then it never opens.
    property bool active: true

    // Emitted when the user accepts a header name (click / Enter on a chosen row).
    signal picked(string name)

    // Forward target for the host's Keys.forwardTo.
    readonly property alias keyTarget: keyItem

    implicitWidth: 0
    implicitHeight: 0

    // Standard request header names (IANA + common). Kept sorted so the list
    // reads predictably. Purely a convenience list — any header can still be
    // typed freely.
    readonly property var commonHeaders: ["Accept", "Accept-Charset", "Accept-Encoding", "Accept-Language", "Access-Control-Request-Headers", "Access-Control-Request-Method", "Authorization", "Cache-Control", "Connection", "Content-Disposition", "Content-Encoding", "Content-Language", "Content-Length", "Content-Type", "Cookie", "Date", "ETag", "Expect", "Forwarded", "From", "Host", "If-Match", "If-Modified-Since", "If-None-Match", "If-Range", "If-Unmodified-Since", "Idempotency-Key", "Origin", "Pragma", "Prefer", "Proxy-Authorization", "Range", "Referer", "TE", "Trailer", "Transfer-Encoding", "Upgrade", "User-Agent", "Via", "Warning", "X-Api-Key", "X-Correlation-ID", "X-CSRF-Token", "X-Forwarded-For", "X-Forwarded-Host", "X-Forwarded-Proto", "X-Requested-With"]

    property var _filtered: []
    // -1 until the user moves the highlight with the arrow keys; Enter only
    // accepts a highlighted row, so an untouched popup never rewrites a key.
    property int _index: -1

    function refresh() {
        if (!root.active || !root.field || !root.field.activeFocus) {
            popup.close();
            return;
        }
        const q = (root.field.text || "").toLowerCase();
        const prefix = [];
        const inner = [];
        for (let i = 0; i < root.commonHeaders.length; ++i) {
            const h = root.commonHeaders[i];
            const at = h.toLowerCase().indexOf(q);
            if (at === 0) {
                prefix.push(h);
            } else if (at > 0) {
                inner.push(h);
            }
        }
        const out = prefix.concat(inner);
        root._filtered = out;
        root._index = -1;
        const exact = out.length > 0 && out[0].toLowerCase() === q;
        if (q.length === 0 || out.length === 0 || exact) {
            popup.close();
            return;
        }
        if (!popup.visible) {
            popup.open();
        }
    }

    function apply(name) {
        popup.close();
        root.picked(name);
    }

    // Opens on typing only: focus alone, or a key set by the model, must not
    // offer to replace what is already there.
    Connections {
        target: root.field
        enabled: root.active
        function onTextEdited() {
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
            } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && root._index >= 0) {
                root.apply(root._filtered[root._index]);
                event.accepted = true;
            } else if (event.key === Qt.Key_Escape) {
                popup.close();
                event.accepted = true;
            } else {
                // Tab and an un-navigated Enter keep their normal meaning.
                if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    popup.close();
                }
                event.accepted = false;
            }
        }
    }

    Popup {
        id: popup
        parent: root.field
        y: root.field ? root.field.height + 2 : 0
        // Keep the popup inside the window (Qt repositions within `margins`,
        // sliding it up when it would overflow the bottom) and cap the width so
        // it never runs off the edge in a narrow editor.
        margins: DesignTokens.spaceSm
        width: Math.min(root.field ? Math.max(root.field.width, 240) : 240, (Overlay.overlay ? Overlay.overlay.width : 640) - 2 * DesignTokens.spaceSm)
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
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                id: hdrRow
                required property int index
                required property var modelData
                width: ListView.view.width
                implicitHeight: 32
                highlighted: index === root._index
                onClicked: root.apply(hdrRow.modelData)

                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: hdrRow.highlighted ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
                contentItem: Label {
                    text: hdrRow.modelData
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
