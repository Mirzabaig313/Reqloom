// HistoryDialog — surfaces persisted run history (AppController.history).
// Lists past runs newest-first with their outcome, target, environment, time
// and duration, plus a cross-run duration trend. Clicking a run replays its
// timeline into the live timeline panel.
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Reqloom

Dialog {
    id: dialog
    title: qsTr("Run History")
    header: DialogHeader {
        title: qsTr("Run History")
    }
    modal: true
    enter: PopupEnter {}
    exit: PopupExit {}
    anchors.centerIn: Overlay.overlay
    width: 640
    padding: DesignTokens.spaceLg

    function openDialog() {
        AppController.refreshHistory();
        open();
    }

    background: Rectangle {
        radius: DesignTokens.radiusLg
        color: DesignTokens.surfaceRaised
        border.width: 1
        border.color: DesignTokens.glassBorder
    }

    contentItem: ColumnLayout {
        spacing: DesignTokens.spaceMd

        RowLayout {
            Layout.fillWidth: true
            spacing: DesignTokens.spaceMd

            Label {
                Layout.fillWidth: true
                text: qsTr("Past runs are stored locally. Select one to replay its timeline.")
                color: DesignTokens.textSecondary
                font.pixelSize: DesignTokens.fontLabel
                wrapMode: Text.WordWrap
            }

            Button {
                id: clearBtn
                visible: AppController.history.count > 0
                text: qsTr("Clear")
                implicitHeight: 28
                leftPadding: DesignTokens.spaceMd
                rightPadding: DesignTokens.spaceMd
                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: clearBtn.hovered ? Qt.rgba(DesignTokens.statusError.r, DesignTokens.statusError.g, DesignTokens.statusError.b, 0.12) : "transparent"
                    border.width: 1
                    border.color: DesignTokens.statusError
                }
                contentItem: Text {
                    text: clearBtn.text
                    color: DesignTokens.statusError
                    font.pixelSize: DesignTokens.fontLabel
                    font.weight: DesignTokens.weightMedium
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: confirmClear.open()
            }
        }

        // Cross-run duration trend: one bar per run (newest on the left),
        // height scaled to the slowest run. Runs without a recorded duration
        // (in-flight / crashed) show no bar.
        Item {
            id: trend
            Layout.fillWidth: true
            implicitHeight: 48
            visible: AppController.history.count > 1

            readonly property var durations: AppController.history.durations
            readonly property real maxMs: {
                let m = 1;
                for (let i = 0; i < durations.length; ++i) {
                    if (durations[i] > m) {
                        m = durations[i];
                    }
                }
                return m;
            }

            Row {
                id: trendRow
                anchors.fill: parent
                layoutDirection: Qt.RightToLeft
                spacing: 2
                Repeater {
                    model: AppController.history
                    delegate: Rectangle {
                        required property real durationMs
                        required property string statusToken
                        width: Math.max(2, (trendRow.width - trendRow.spacing * (AppController.history.count - 1)) / Math.max(1, AppController.history.count))
                        height: durationMs >= 0 ? Math.max(2, trend.height * (durationMs / trend.maxMs)) : 0
                        anchors.bottom: parent.bottom
                        radius: 1
                        color: DesignTokens.statusColor(statusToken)
                        opacity: 0.8
                    }
                }
            }
        }

        EmptyState {
            Layout.fillWidth: true
            visible: AppController.history.count === 0
            heading: qsTr("No runs yet")
            body: qsTr("Send a request to record your first run.")
        }

        ListView {
            id: runList
            Layout.fillWidth: true
            implicitHeight: Math.min(contentHeight, 380)
            visible: AppController.history.count > 0
            clip: true
            model: AppController.history
            spacing: 2
            ScrollBar.vertical: ScrollBar {}

            delegate: ItemDelegate {
                id: runRow
                width: ListView.view.width
                required property var model
                implicitHeight: 52

                background: Rectangle {
                    radius: DesignTokens.radiusSm
                    color: runRow.hovered ? DesignTokens.accentMuted : "transparent"
                    Behavior on color {
                        ColorMotion {}
                    }
                }

                contentItem: RowLayout {
                    spacing: DesignTokens.spaceMd

                    StatusBadge {
                        token: runRow.model.statusToken
                        label: runRow.model.outcome.length > 0 ? runRow.model.outcome : qsTr("running")
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            text: runRow.model.target
                            color: DesignTokens.textPrimary
                            font.pixelSize: DesignTokens.fontBody
                            font.weight: DesignTokens.weightMedium
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Label {
                            text: {
                                const env = runRow.model.env.length > 0 ? runRow.model.env : qsTr("default");
                                const when = runRow.model.startedAt.length > 0 ? new Date(runRow.model.startedAt).toLocaleString(Qt.locale(), Locale.ShortFormat) : qsTr("unknown");
                                return env + " · " + when + " · " + runRow.model.chainSize + qsTr(" steps");
                            }
                            color: DesignTokens.textSecondary
                            font.pixelSize: DesignTokens.fontCaption
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    Label {
                        text: {
                            const ms = runRow.model.durationMs;
                            if (ms < 0) {
                                return "—";
                            }
                            return ms < 1000 ? (ms + " ms") : ((ms / 1000).toFixed(2) + " s");
                        }
                        color: DesignTokens.textSecondary
                        font.pixelSize: DesignTokens.fontCaption
                        font.family: DesignTokens.fontMono
                    }
                }

                onClicked: {
                    AppController.replayRun(runRow.model.runId);
                    dialog.close();
                }
            }
        }
    }

    footer: DialogButtons {
        okText: qsTr("Close")
        showCancel: false
        onAccepted: dialog.close()
    }

    // Confirm before wiping history — a destructive, irreversible action.
    Dialog {
        id: confirmClear
        title: qsTr("Clear run history?")
        header: DialogHeader {
            title: qsTr("Clear run history?")
        }
        modal: true
        enter: PopupEnter {}
        exit: PopupExit {}
        anchors.centerIn: Overlay.overlay
        width: 420
        padding: DesignTokens.spaceLg

        background: Rectangle {
            radius: DesignTokens.radiusLg
            color: DesignTokens.surfaceRaised
            border.width: 1
            border.color: DesignTokens.glassBorder
        }

        contentItem: Label {
            text: qsTr("This permanently deletes all recorded runs for this project. This cannot be undone.")
            color: DesignTokens.textSecondary
            font.pixelSize: DesignTokens.fontLabel
            wrapMode: Text.WordWrap
        }

        footer: DialogButtons {
            okText: qsTr("Clear history")
            cancelText: qsTr("Cancel")
            onAccepted: {
                AppController.clearHistory();
                confirmClear.close();
            }
            onRejected: confirmClear.close()
        }
    }
}
