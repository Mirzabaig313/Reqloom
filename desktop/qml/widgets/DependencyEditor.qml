// DependencyEditor — edits an operation's depends_on list as a column of
// combo pickers over existing operation ids (mirrors the old
// DependencyListEditor). A dependency can never name something undefined, and
// an always-present trailing blank row grows the list. Backed by a
// DependencyEditModel (C++ owns state).
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

ColumnLayout {
    id: root

    // A DependencyEditModel and the pickable operation ids.
    required property var depModel
    required property var candidates

    spacing: DesignTokens.spaceXs

    Repeater {
        model: root.depModel

        delegate: ColumnLayout {
            id: row
            required property int index
            required property string value
            required property bool isGhost
            Layout.fillWidth: true
            spacing: 2

            // The {{...}} variables this dependency makes available (so you
            // know which prerequisite provides which variable).
            readonly property var providedVars: (!row.isGhost && row.value.length > 0) ? AppController.extractedVariablesFor(row.value) : []

            RowLayout {
                Layout.fillWidth: true
                spacing: DesignTokens.spaceXs

                GlassComboBox {
                    id: combo
                    Layout.fillWidth: true
                    // The picker options: a leading "add" placeholder + every
                    // candidate operation id. Shared by the model, the current
                    // selection, and the activation handler so they never diverge.
                    readonly property var options: [qsTr("+ add dependency…")].concat(root.candidates)
                    model: combo.options
                    currentIndex: {
                        if (row.value.length === 0) {
                            return 0;
                        }
                        const idx = combo.options.indexOf(row.value);
                        return idx < 0 ? 0 : idx;
                    }
                    onActivated: function (i) {
                        root.depModel.setSelection(row.index, i === 0 ? "" : combo.options[i]);
                    }
                }

                ToolButton {
                    id: depRemove
                    visible: !row.isGhost
                    implicitWidth: 28
                    implicitHeight: 28
                    text: "✕"
                    onClicked: root.depModel.removeRow(row.index)
                    contentItem: Text {
                        text: depRemove.text
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontLabel
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: DesignTokens.radiusSm
                        color: depRemove.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                    }
                }
            }

            // What this dependency provides — its own extracted variables.
            // Always shown for a chosen dependency so the relationship is clear
            // (a prerequisite with no extract block says so explicitly).
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: DesignTokens.spaceMd
                Layout.bottomMargin: DesignTokens.spaceXs
                visible: !row.isGhost && row.value.length > 0
                text: row.providedVars.length > 0 ? qsTr("provides  ") + row.providedVars.join("   ") : qsTr("provides no variables (add an Extract on %1 to expose one)").arg(row.value)
                color: row.providedVars.length > 0 ? DesignTokens.statusSuccess : DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontCaption
                font.family: row.providedVars.length > 0 ? DesignTokens.fontMono : DesignTokens.fontSans
                wrapMode: Text.WrapAnywhere
            }
        }
    }
}
