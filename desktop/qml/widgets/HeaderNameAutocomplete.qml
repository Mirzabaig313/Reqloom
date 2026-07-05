// HeaderNameAutocomplete — a Postman-style dropdown of common HTTP header names
// for a header Key TextField. On focus it lists the standard headers; typing
// filters them (case-insensitive substring); picking one emits `picked(name)`.
// The host wires keys via `Keys.forwardTo: [<id>.keyTarget]` — keys are only
// consumed while the popup is open, so normal typing/editing is unaffected.
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

    // Emitted when the user accepts a header name (click / Enter / Tab).
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
    property int _index: 0
    // Set on pick so the model-driven text change that follows (which still
    // substring-matches sibling headers, e.g. "Accept" → "Accept-Charset")
    // doesn't immediately reopen the popup. Cleared on the next refresh.
    property bool _suppress: false

    function refresh() {
        if (root._suppress) {
            root._suppress = false;
            popup.close();
            return;
        }
        if (!root.active || !root.field || !root.field.activeFocus) {
            popup.close();
            return;
        }
        const q = (root.field.text || "").toLowerCase();
        const out = [];
        for (let i = 0; i < root.commonHeaders.length; ++i) {
            const h = root.commonHeaders[i];
            const hl = h.toLowerCase();
            // Skip an exact full match (nothing left to complete).
            if (hl === q) {
                continue;
            }
            if (q.length === 0 || hl.indexOf(q) >= 0) {
                out.push(h);
            }
        }
        root._filtered = out;
        if (out.length === 0) {
            popup.close();
            return;
        }
        root._index = 0;
        if (!popup.visible) {
            popup.open();
        }
    }

    function apply(name) {
        root._suppress = true;
        root.picked(name);
        popup.close();
    }

    Connections {
        target: root.field
        enabled: root.active
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
                    root.apply(root._filtered[root._index]);
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
        y: root.field ? root.field.height + 2 : 0
        width: root.field ? Math.max(root.field.width, 240) : 240
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
