// PathTokenBar — the read-mode request path, with each `{{resource.var}}`
// rendered as a clickable chip that opens the ValuePicker (so you can pin which
// concrete id this run uses, Apidog-style). Plain segments render as mono text;
// the whole thing scrolls horizontally when the path is long.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Item {
    id: root

    required property string path

    // Split `path` into ordered segments: { text, token }. A non-empty `token`
    // (and only `resource.var`, not `$.` builtins) becomes an interactive chip.
    readonly property var _segments: {
        const out = [];
        const re = /\{\{([^}]+)\}\}/g;
        let last = 0;
        let m = re.exec(root.path);
        while (m !== null) {
            if (m.index > last) {
                out.push({
                    "text": root.path.substring(last, m.index),
                    "token": ""
                });
            }
            const inner = m[1].trim();
            const interactive = inner.indexOf(".") >= 0 && inner.indexOf("$") !== 0;
            out.push({
                "text": m[0],
                "token": interactive ? inner : ""
            });
            last = m.index + m[0].length;
            m = re.exec(root.path);
        }
        if (last < root.path.length) {
            out.push({
                "text": root.path.substring(last),
                "token": ""
            });
        }
        return out;
    }

    Flickable {
        anchors.fill: parent
        anchors.leftMargin: DesignTokens.spaceMd
        anchors.rightMargin: DesignTokens.spaceMd
        contentWidth: segmentRow.implicitWidth
        contentHeight: height
        flickableDirection: Flickable.HorizontalFlick
        clip: true

        Row {
            id: segmentRow
            height: parent.height
            spacing: 0

            Repeater {
                model: root._segments
                delegate: Loader {
                    id: segLoader
                    required property var modelData
                    height: segmentRow.height
                    sourceComponent: segLoader.modelData.token.length > 0 ? chipComponent : textComponent

                    Component {
                        id: textComponent
                        Text {
                            height: segmentRow.height
                            verticalAlignment: Text.AlignVCenter
                            text: segLoader.modelData.text
                            color: DesignTokens.textPrimary
                            font.pixelSize: DesignTokens.fontBody
                            font.family: DesignTokens.fontMono
                        }
                    }

                    Component {
                        id: chipComponent
                        Item {
                            id: chip
                            width: chipRect.width
                            height: segmentRow.height

                            // Re-read the pin on every change so the chip
                            // reflects pin/clear immediately (function calls in
                            // bindings don't re-evaluate on a signal).
                            property string pinnedValue: AppController.variableOverride(segLoader.modelData.token)
                            readonly property bool pinned: pinnedValue.length > 0

                            Connections {
                                target: AppController
                                function onVariableOverridesChanged() {
                                    chip.pinnedValue = AppController.variableOverride(segLoader.modelData.token);
                                }
                            }

                            Rectangle {
                                id: chipRect
                                anchors.verticalCenter: parent.verticalCenter
                                height: 26
                                width: chipRow.implicitWidth + DesignTokens.spaceSm * 2
                                radius: DesignTokens.radiusSm
                                color: chip.pinned ? Qt.rgba(DesignTokens.accent.r, DesignTokens.accent.g, DesignTokens.accent.b, 0.14) : DesignTokens.surfaceRaised
                                border.width: 1
                                border.color: chipMouse.containsMouse ? DesignTokens.accent : DesignTokens.borderSubtle

                                RowLayout {
                                    id: chipRow
                                    anchors.centerIn: parent
                                    spacing: DesignTokens.spaceXs
                                    Label {
                                        // Show the pinned value when set, else the token name.
                                        text: chip.pinned ? chip.pinnedValue : segLoader.modelData.token
                                        color: DesignTokens.accent
                                        font.pixelSize: DesignTokens.fontLabel
                                        font.family: DesignTokens.fontMono
                                        elide: Text.ElideRight
                                        Layout.maximumWidth: 220
                                    }
                                    AppIcon {
                                        name: "chevron-down"
                                        size: 12
                                    }
                                }

                                MouseArea {
                                    id: chipMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: chipPicker.openAt(segLoader.modelData.token, chipRect)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    ValuePicker {
        id: chipPicker
    }
}
