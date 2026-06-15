// ExtractionEditor — edits an operation's extract block as Variable + Path
// rows (mirrors the old ExtractionTableEditor). The source kind is derived from
// the path prefix in C++ on read-back, so it is never a separate field. Backed
// by an EditableKeyValueModel with a trailing ghost row.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

ColumnLayout {
    id: root

    // An EditableKeyValueModel.
    required property var extractModel
    // The owning endpoint's resource (e.g. "cart"). Extracted variables are
    // namespaced under it, so a row reads as {{resource.variable}}.
    property string resourcePrefix: ""

    spacing: DesignTokens.spaceXs

    // Extractions always read from THIS endpoint's own response; the value
    // becomes a {{...}} variable later steps can reference.
    Label {
        Layout.fillWidth: true
        text: qsTr("Pulled from this endpoint's response. Reference it later as %1.").arg(root.resourcePrefix.length > 0 ? "{{" + root.resourcePrefix + ".<variable>}}" : "{{<resource>.<variable>}}")
        color: DesignTokens.textSecondary
        font.pixelSize: DesignTokens.fontCaption
        wrapMode: Text.WordWrap
    }

    component Field: TextField {
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

    // Column headers — mirror the New Endpoint dialog's dependency table so the
    // two extraction surfaces read identically.
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: DesignTokens.spaceXs
        spacing: DesignTokens.spaceXs
        Label {
            Layout.preferredWidth: 150
            text: qsTr("Variable name")
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontCaption
            font.weight: DesignTokens.weightSemiBold
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Body path / Header")
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontCaption
            font.weight: DesignTokens.weightSemiBold
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

    Repeater {
        model: root.extractModel

        delegate: RowLayout {
            id: row
            required property int index
            required property string key
            required property string value
            required property bool isGhost
            Layout.fillWidth: true
            spacing: DesignTokens.spaceXs

            // Variable column groups the resource-prefix adornment with the
            // editable name so the "cart." namespace stays glued to its field
            // and the column lines up under the "Variable name" header.
            RowLayout {
                Layout.preferredWidth: 150
                spacing: 2
                Label {
                    visible: root.resourcePrefix.length > 0
                    text: root.resourcePrefix + "."
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontLabel
                    font.family: DesignTokens.fontMono
                }
                Field {
                    Layout.fillWidth: true
                    text: row.key
                    placeholderText: qsTr("variable")
                    onTextEdited: root.extractModel.setKey(row.index, text)
                }
            }
            Field {
                Layout.fillWidth: true
                text: row.value
                placeholderText: qsTr("$.body.path / $.headers.X")
                onTextEdited: root.extractModel.setValue(row.index, text)
            }
            ToolButton {
                id: extRemove
                visible: !row.isGhost
                implicitWidth: 28
                implicitHeight: 28
                text: "✕"
                onClicked: root.extractModel.removeRow(row.index)
                contentItem: Text {
                    text: extRemove.text
                    color: DesignTokens.textSecondary
                    font.pixelSize: DesignTokens.fontLabel
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: extRemove.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                }
            }
        }
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("$.headers.X · $.cookies.X · $.status_code · anything else is a JSON path")
        color: DesignTokens.textSecondary
        font.pixelSize: DesignTokens.fontCaption
        wrapMode: Text.WordWrap
    }
}
