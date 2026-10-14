pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App

Item {
    id: historyPage
    focus: true
    onVisibleChanged: if (visible) {
        historyList.forceActiveFocus()
        historyPage.refreshStats()
    }
    onActiveFocusChanged: if (activeFocus) historyList.forceActiveFocus()

    function refreshStats() { stats.values = App.library.watchStats() }

    Connections {
        target: App.library.historyModel
        function onRowsInserted() { Qt.callLater(historyPage.refreshStats) }
        function onRowsRemoved() { Qt.callLater(historyPage.refreshStats) }
        function onModelReset() { Qt.callLater(historyPage.refreshStats) }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: qsTr("History")
                color: Theme.textPrimary
                font.pixelSize: Ui.sp(Theme.headingSize)
                font.weight: Font.DemiBold
                Layout.fillWidth: true
            }
            AppTextField {
                id: searchField
                visible: App.library.historyModel.count > 0
                Layout.preferredWidth: Ui.sp(280)
                Layout.preferredHeight: Ui.sp(34)
                placeholderText: qsTr("Search history")
                showClearButton: true
                text: App.library.historyView.filterText
                onTextChanged: {
                    App.library.historyView.filterText = text
                    historyList.currentIndex = -1
                    historyList.keyboardActive = false
                }
                Keys.onDownPressed: {
                    historyList.forceActiveFocus()
                    historyList.step(1)
                }
            }
            Text {
                text: historyList.count === 1 ? qsTr("1 show") : qsTr("%1 shows").arg(historyList.count)
                color: Theme.textMuted
                font.pixelSize: Ui.sp(Theme.bodySize)
            }

            AppButton {
                text: qsTr("Clear")
                fontSize: Theme.compactSize
                radius: 8
                secondary: true
                contentItemTextColor: Theme.danger
                visible: historyList.count > 0 && searchField.text === ""
                Layout.preferredHeight: Ui.sp(34)
                onClicked: {
                    const rows = App.library.clearHistory()
                    if (rows.length > 0)
                        Ui.offerUndo(qsTr("History cleared."), () => App.library.restoreRows("history", rows))
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
            font.pixelSize: Ui.sp(Theme.bodySize)
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
            title: searchField.text !== "" ? qsTr("No results for “%1”.").arg(searchField.text.trim())
                                           : qsTr("No history yet.")
            hint: searchField.text !== "" ? "" : qsTr("Episodes you play appear here.")
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

            property int hoveredIndex: -1
            property bool keyboardActive: false
            currentIndex: -1
            keyNavigationEnabled: false

            function step(delta) {
                if (count <= 0) return
                keyboardActive = true
                currentIndex = Math.max(0, Math.min(count - 1, currentIndex + delta))
                positionViewAtIndex(currentIndex, ListView.Contain)
            }

            ScrollBar.vertical: AppScrollBar {}

            function removeLink(link) {
                const rows = App.library.removeFromHistory(link)
                if (rows.length > 0)
                    Ui.offerUndo(qsTr("Removed “%1” from the history.").arg(rows[0].title),
                                      () => App.library.restoreRows("history", rows))
            }

            Keys.onPressed: function (event) {
                if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                    step(event.key === Qt.Key_Down ? 1 : -1)
                    event.accepted = true
                } else if ([Qt.Key_Return, Qt.Key_Enter, Qt.Key_Delete, Qt.Key_Backspace].includes(event.key)) {
                    const target = keyboardActive ? currentIndex : hoveredIndex
                    const link = target >= 0 ? App.library.historyView.linkAt(target) : ""
                    if (link !== "" && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter))
                        App.resumeFromHistory(link)
                    else if (link !== "")
                        removeLink(link)
                    event.accepted = true
                } else if (event.key === Qt.Key_Slash || (event.key === Qt.Key_F && (event.modifiers & Qt.ControlModifier))) {
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
                width: ListView.view.width - Ui.sp(Theme.scrollbarGutter)
                height: Math.max(84, Ui.sp(84))
                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                    onHoveredChanged: {
                        historyList.hoveredIndex = hovered ? row.index : -1
                        if (hovered) historyList.keyboardActive = false
                    }
                }
                focusPolicy: Qt.NoFocus
                readonly property bool lit: row.hovered || (historyList.keyboardActive && historyList.currentIndex === row.index)

                background: Rectangle {
                    radius: 12
                    color: row.lit ? Theme.accentMuted : Theme.surface
                    border.color: row.lit ? Theme.accent : Theme.border
                    border.width: row.lit ? 2 : 1
                    Behavior on color { ColorAnimation { duration: 120 } }
                }

                onClicked: App.resumeFromHistory(row.link)

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: {
                        historyMenu.link = row.link
                        historyMenu.popup()
                    }
                }

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
                            font.pixelSize: Ui.sp(Theme.bodySize)
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Text {
                            text: [row.episode <= 0 ? qsTr("Not started")
                                   : row.total > 0 ? qsTr("Episode %1 of %2").arg(row.episode).arg(row.total)
                                                   : qsTr("Episode %1").arg(row.episode),
                                   row.episodeTitle.trim(),
                                   row.playedAt > 0 ? Ui.ago(row.playedAt) : ""]
                                  .filter(part => part.length > 0).join("  ·  ")
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                            color: row.hovered ? Theme.textSecondary : Theme.textMuted
                            font.pixelSize: Ui.sp(Theme.compactSize)
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
                        implicitWidth: Ui.sp(40)
                        implicitHeight: Ui.sp(40)
                        boxRadius: height / 2
                        active: row.hovered
                        hoverColor: hovered ? Theme.accent : Theme.accentMuted
                        iconColor: row.hovered ? Theme.textSecondary : Theme.textMuted
                        iconHoverColor: Theme.onAccent
                        tip: qsTr("Resume playback")
                        onClicked: App.resumeFromHistory(row.link)

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
                        id: detailsBtn
                        visible: App.providers.has(row.provider)
                        implicitWidth: Ui.sp(40)
                        implicitHeight: Ui.sp(40)
                        boxRadius: height / 2
                        iconName: "details"
                        iconSize: 20
                        iconColor: row.hovered ? Theme.textSecondary : Theme.textMuted
                        tip: qsTr("Details")
                        onClicked: App.openShowInfo(row.link, row.provider, row.title)
                    }

                    IconButton {
                        id: removeBtn
                        readonly property bool reached: row.lit || playBtn.visualFocus || detailsBtn.visualFocus || visualFocus
                        Layout.preferredWidth: 28
                        Layout.preferredHeight: 28
                        enabled: reached
                        opacity: reached ? 1 : 0
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

    AppMenu {
        id: historyMenu
        modal: true
        property string link: ""

        Action {
            icon.name: "play"
            text: qsTr("Play")
            onTriggered: App.resumeFromHistory(historyMenu.link)
        }

        Action {
            property bool destructive: true
            icon.name: "trash-2"
            text: qsTr("Remove")
            onTriggered: historyList.removeLink(historyMenu.link)
        }
    }
}
