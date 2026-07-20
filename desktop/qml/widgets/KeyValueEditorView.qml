// KeyValueEditorView — editable two-column (key, value) list backed by an
// EditableKeyValueModel with an always-present trailing ghost row (Apidog
// pattern; mirrors the old Widgets KeyValueEditor). Reused for an operation's
// headers, query params, and form-data body in Edit mode. C++ owns row state.
// When `allowFiles` is set (form-data body), each row gains a file-attach
// button that fills the value with the engine's `@<path>` upload convention.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import Reqloom

ColumnLayout {
    id: root

    // An EditableKeyValueModel.
    required property var kvModel
    property string keyPlaceholder: qsTr("key")
    property string valuePlaceholder: qsTr("value")
    // Show a per-row file-attach button (form-data body only).
    property bool allowFiles: false
    // Offer a Postman-style dropdown of common HTTP header names on the Key
    // field (headers tab only; off for query/form/config tables).
    property bool suggestHeaderNames: false

    // Row whose attach button opened the file dialog (-1 = none).
    property int fileTargetRow: -1

    spacing: DesignTokens.spaceXs

    function isFileValue(v) {
        return typeof v === "string" && v.startsWith("@");
    }
    function fileBaseName(v) {
        const path = v.substring(1);
        const slash = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
        return slash >= 0 ? path.substring(slash + 1) : path;
    }

    component Field: TextField {
        color: DesignTokens.textPrimary
        placeholderTextColor: DesignTokens.textSecondary
        font.pixelSize: DesignTokens.fontLabel
        font.family: DesignTokens.fontMono
        // Content-driven so the field grows with OS text instead of a fixed 32
        // that clips; floored at the density control height.
        topPadding: DesignTokens.spaceXs
        bottomPadding: DesignTokens.spaceXs
        implicitHeight: Math.max(DesignTokens.controlHeight, contentHeight + topPadding + bottomPadding)
        // Explicit, shared metrics so the placeholder (ghost row) and typed
        // text sit on the exact same horizontal + vertical axis.
        leftPadding: DesignTokens.spaceSm
        rightPadding: DesignTokens.spaceSm
        verticalAlignment: TextInput.AlignVCenter
        background: Rectangle {
            radius: DesignTokens.radiusSm
            color: DesignTokens.surfaceSunken
            border.width: 1
            border.color: parent.activeFocus ? DesignTokens.accent : DesignTokens.borderSubtle
        }
    }

    FileDialog {
        id: kvFileDialog
        title: qsTr("Choose a file to upload")
        onAccepted: {
            if (root.fileTargetRow >= 0) {
                root.kvModel.setValue(root.fileTargetRow, "@" + AppController.localFileFromUrl(selectedFile));
            }
            root.fileTargetRow = -1;
        }
        onRejected: root.fileTargetRow = -1
    }

    Repeater {
        model: root.kvModel

        delegate: RowLayout {
            id: row
            required property int index
            required property string key
            required property string value
            required property bool isGhost
            Layout.fillWidth: true
            spacing: DesignTokens.spaceXs

            Field {
                id: keyField
                // Shrinkable so the value keeps a usable editing width as the
                // editor narrows (one-line stays usable down to ~270 DIP, under
                // the 320 minimum); the value field carries its own floor.
                Layout.preferredWidth: 200
                Layout.minimumWidth: 96
                text: row.key
                placeholderText: root.keyPlaceholder
                onTextEdited: root.kvModel.setKey(row.index, text)
                // Postman-style header-name completion (headers tab only).
                Keys.forwardTo: root.suggestHeaderNames ? [hdrAutocomplete.keyTarget] : []
                HeaderNameAutocomplete {
                    id: hdrAutocomplete
                    field: keyField
                    active: root.suggestHeaderNames
                    // setKey updates the model; the `text: row.key` binding
                    // above reflects it — don't write keyField.text directly
                    // (that would break the binding for later model updates).
                    onPicked: function (name) {
                        root.kvModel.setKey(row.index, name);
                    }
                }
            }

            // Value: a file chip when the value is an `@<path>` upload, else a
            // plain editable field.
            Field {
                Layout.fillWidth: true
                // Useful editing floor; with the key's 96 shrink minimum the row
                // stays one-line and usable down to ~270 DIP.
                Layout.minimumWidth: 120
                visible: !root.allowFiles || !root.isFileValue(row.value)
                text: row.value
                placeholderText: root.valuePlaceholder
                onTextEdited: root.kvModel.setValue(row.index, text)
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.minimumWidth: 120
                visible: root.allowFiles && root.isFileValue(row.value)
                implicitHeight: Math.max(DesignTokens.controlHeight, chipRow.implicitHeight + DesignTokens.spaceXs * 2)
                radius: DesignTokens.radiusSm
                color: DesignTokens.surfaceSunken
                border.width: 1
                border.color: DesignTokens.accent
                RowLayout {
                    id: chipRow
                    anchors.fill: parent
                    anchors.leftMargin: DesignTokens.spaceSm
                    anchors.rightMargin: DesignTokens.spaceXs
                    spacing: DesignTokens.spaceXs
                    AppIcon {
                        name: "paperclip"
                        size: 14
                        color: DesignTokens.accent
                    }
                    Label {
                        Layout.fillWidth: true
                        // A bare "@" is an imported file slot with no path yet
                        // (Postman doesn't export file contents/paths). Show a
                        // prompt instead of a blank chip so it's clear the row
                        // needs a file attached.
                        readonly property bool hasPath: root.isFileValue(row.value) && row.value.length > 1
                        text: hasPath ? root.fileBaseName(row.value) : qsTr("Choose a file…")
                        color: hasPath ? DesignTokens.textPrimary : DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontLabel
                        font.family: DesignTokens.fontMono
                        elide: Text.ElideMiddle
                        HoverHandler {
                            id: chipHover
                        }
                        GlassToolTip {
                            active: chipHover.hovered && row.value.length > 1
                            text: row.value.substring(1)
                        }
                    }
                    ToolButton {
                        id: clearFile
                        implicitWidth: 24
                        implicitHeight: 24
                        text: "\u2715"
                        onClicked: root.kvModel.setValue(row.index, "")
                        contentItem: Text {
                            text: clearFile.text
                            color: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontCaption
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: DesignTokens.radiusSm
                            color: clearFile.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                        }
                    }
                }
            }

            // Attach a file (form-data only).
            ToolButton {
                id: kvAttach
                visible: root.allowFiles
                implicitWidth: 28
                implicitHeight: 28
                onClicked: {
                    root.fileTargetRow = row.index;
                    kvFileDialog.open();
                }
                contentItem: AppIcon {
                    name: "paperclip"
                    size: 15
                    color: root.isFileValue(row.value) ? DesignTokens.accent : DesignTokens.textSecondary
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: kvAttach.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
                GlassToolTip {
                    active: kvAttach.hovered
                    text: qsTr("Attach file")
                }
            }

            ToolButton {
                id: kvRemove
                visible: !row.isGhost
                implicitWidth: 28
                implicitHeight: 28
                text: "\u2715"
                onClicked: root.kvModel.removeRow(row.index)
                contentItem: Text {
                    text: kvRemove.text
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontLabel
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: kvRemove.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
            }
        }
    }
}
