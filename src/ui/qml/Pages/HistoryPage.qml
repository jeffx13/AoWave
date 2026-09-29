pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App
import "../Components"
import ".."

Item {
    id: historyPage

    focus: true
    onVisibleChanged: if (visible) {
        historyList.forceActiveFocus()
        stats.values = App.library.watchStats()
    }
    onActiveFocusChanged: if (activeFocus) historyList.forceActiveFocus()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: qsTr("History")
                color: Theme.textPrimary
                font.pixelSize: Globals.sp(26)
                font.weight: Font.DemiBold
                Layout.fillWidth: true
            }
            // By show or episode title.
            AppTextField {
                id: searchField
                visible: App.library.historyModel.count > 0
                Layout.preferredWidth: Globals.sp(280)
                Layout.preferredHeight: 34
                placeholderText: qsTr("Search history")
                showClearButton: true
                text: App.library.historyView.filterText
                onTextChanged: App.library.historyView.filterText = text
                Keys.onDownPressed: historyList.forceActiveFocus()
            }
            Text {
                text: historyList.count === 1 ? qsTr("1 show") : qsTr("%1 shows").arg(historyList.count)
                color: Theme.textMuted
                font.pixelSize: Globals.sp(Theme.bodySize)
            }

            AppButton {
                text: qsTr("Clear")
                fontSize: 18
                radius: 8
                secondary: true
                contentItemTextColor: Theme.danger
                visible: historyList.count > 0 && searchField.text === ""
                Layout.preferredHeight: 34
                onClicked: {
                    const rows = App.library.clearHistory()
                    if (rows.length > 0)
                        Globals.offerUndo(qsTr("Cleared the history"), () => App.library.restoreRows("history", rows))
                }
            }
        }

        Text {
            id: stats
            property var values: App.library.watchStats()
            visible: historyList.count > 0
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textMuted
            font.pixelSize: Globals.sp(Theme.bodySize)
            text: [
                values.episodesThisWeek === 1 ? qsTr("1 episode this week") : qsTr("%1 episodes this week").arg(values.episodesThisWeek),
                qsTr("active %1 of the last 30 days").arg(values.activeDays),
                values.streak > 1 ? qsTr("%1-day streak").arg(values.streak) : "",
                qsTr("%1 watching, %2 completed").arg(values.watching).arg(values.completed)
            ].filter(part => part.length > 0).join("  ·  ")
        }

        EmptyState {
            visible: historyList.count === 0
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 48
            icon: searchField.text !== "" ? "search" : "history"
            title: searchField.text !== "" ? qsTr("Nothing in the history matches \u201c%1\u201d.").arg(searchField.text.trim())
                                           : qsTr("Nothing watched yet.")
            hint: searchField.text !== "" ? "" : qsTr("Episodes you play appear here, so you can pick up where you left off.")
        }

        ListView {
            id: historyList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            boundsBehavior: Flickable.StopAtBounds
            focus: true
            model: App.library.historyView

            // Del acts on the row under the pointer.
            property int hoveredIndex: -1

            ScrollBar.vertical: AppScrollBar {}

            function removeLink(link) {
                const rows = App.library.removeFromHistory(link)
                if (rows.length > 0)
                    Globals.offerUndo(qsTr("Removed \u201c%1\u201d from the history").arg(rows[0].title),
                                      () => App.library.restoreRows("history", rows))
            }

            Keys.onPressed: function (event) {
                if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) && hoveredIndex >= 0) {
                    const link = App.library.historyView.linkAt(hoveredIndex)
                    if (link !== "") removeLink(link)
                    event.accepted = true
                } else if (event.key === Qt.Key_Slash || (event.key === Qt.Key_F && (event.modifiers & Qt.ControlModifier))) {
                    // As in the Explorer.
                    searchField.forceActiveFocus()
                    event.accepted = true
                }
            }

            delegate: ItemDelegate {
                id: row
                required property int index
                required property string link
                required property string title
                required property string cover
                required property int episode
                required property int total
                required property double progress
                required property string provider
                required property string episodeTitle
                required property real playedAt
                width: ListView.view.width - Globals.sp(Theme.scrollbarGutter)
                height: Math.max(84, Globals.sp(84))
                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                    onHoveredChanged: historyList.hoveredIndex = hovered ? row.index : -1
                }
                focusPolicy: Qt.NoFocus

                background: Rectangle {
                    radius: 12
                    color: row.hovered ? Theme.accentMuted : Theme.surface
                    border.color: row.hovered ? Theme.accent : Theme.border
                    border.width: row.hovered ? 2 : 1
                    Behavior on color { ColorAnimation { duration: 120 } }
                }

                onClicked: App.resumeFromHistory(row.link)

                contentItem: RowLayout {
                    spacing: 12

                    CoverTile {
                        Layout.preferredWidth: 50
                        Layout.preferredHeight: 68
                        source: row.cover
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Text {
                            text: row.title
                            color: Theme.textPrimary
                            font.pixelSize: Globals.sp(21)
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Text {
                            text: [row.episode > 0 ? qsTr("Episode ") + row.episode + (row.total > 0 ? " / " + row.total : "")
                                                   : qsTr("Not started"),
                                   row.episodeTitle.trim(),
                                   row.playedAt > 0 ? Globals.ago(row.playedAt) : ""]
                                  .filter(part => part.length > 0).join("  ·  ")
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                            color: Theme.textMuted
                            font.pixelSize: Globals.sp(19)
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 4
                            radius: 2
                            color: Theme.surfaceAlt
                            Rectangle {
                                width: parent.width * Math.max(0, Math.min(1, row.progress))
                                height: parent.height
                                radius: 2
                                color: Theme.accent
                            }
                        }
                    }

                    IconButton {
                        id: playBtn
                        iconName: "play"
                        iconSize: 22
                        implicitWidth: Globals.sp(40)
                        implicitHeight: Globals.sp(40)
                        boxRadius: height / 2
                        active: row.hovered
                        hoverColor: hovered ? Theme.accent : Theme.accentMuted
                        iconColor: row.hovered ? Theme.textSecondary : Theme.textMuted
                        iconHoverColor: Theme.onAccent
                        tip: qsTr("Resume playback")
                        onClicked: App.resumeFromHistory(row.link)

                        // Accent halo while the pointer is on it.
                        Rectangle {
                            z: -1
                            anchors { fill: parent; margins: -6 }
                            radius: height / 2
                            color: Qt.alpha(Theme.accent, playBtn.hovered ? 0.22 : 0)
                            border.color: Qt.alpha(Theme.accent, playBtn.hovered ? 0.45 : 0)
                            border.width: 2
                            scale: playBtn.hovered ? 1 : 0.8
                            Behavior on color { ColorAnimation { duration: 150 } }
                            Behavior on border.color { ColorAnimation { duration: 150 } }
                            Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                        }
                    }

                    IconButton {
                        // Only for a provider this build still has.
                        visible: App.providers.has(row.provider)
                        implicitWidth: Globals.sp(40)
                        implicitHeight: Globals.sp(40)
                        boxRadius: height / 2
                        iconName: "details"
                        iconSize: 20
                        iconColor: row.hovered ? Theme.textSecondary : Theme.textMuted
                        tip: qsTr("Details")
                        onClicked: App.openShowInfo(row.link, row.provider, row.title)
                    }

                    IconButton {
                        Layout.preferredWidth: 28
                        Layout.preferredHeight: 28
                        opacity: row.hovered ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: 120 } }

                        iconName: "x"
                        iconSize: 16
                        boxRadius: 7
                        destructive: true
                        tip: qsTr("Remove from history (Del)")
                        onClicked: historyList.removeLink(row.link)
                    }
                }
            }
        }
    }
}
