pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App

IconButton {
    id: bell
    implicitWidth: 30
    implicitHeight: 30
    boxRadius: 8
    iconName: "bell"
    iconSize: 17
    active: centerPopup.visible
    hoverColor: Qt.alpha(Theme.accent, 0.14)
    iconColor: Theme.textSecondary
    iconHoverColor: Theme.textSecondary
    tip: qsTr("Notifications")
    onClicked: centerPopup.toggle()

    CountBadge {
        anchors { top: parent.top; right: parent.right; topMargin: -3; rightMargin: -5 }
        count: App.library.unseenNotifications
    }

    function follow(entry) {
        centerPopup.close()
        if (entry.kind === "episode" || entry.kind === "tracker") {
            App.openShowInfo(entry.link, entry.provider, entry.title)
        } else if (entry.kind === "provider") {
            Ui.root.openSettings("providers")
        } else {
            App.playlist.openUrl(Ui.fileUrl(entry.link), true)
            Ui.gotoPage(MainWindow.Player)
        }
    }

    AppPopup {
        id: centerPopup
        toggledByParent: true
        property var entries: []
        property real clearedUpTo: 0
        x: bell.width - width
        y: bell.height + 8
        width: 380
        padding: 10
        onAboutToShow: entries = App.library.notifications().filter(n => n.id > clearedUpTo)
        onOpened: App.library.markNotificationsSeen()

        contentItem: ColumnLayout {
            spacing: 6

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 6
                Layout.rightMargin: 6
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Notifications")
                    color: Theme.textPrimary
                    font.pixelSize: Ui.sp(Theme.bodySize)
                    font.weight: Font.DemiBold
                }
                LinkText {
                    visible: centerPopup.entries.length > 0
                    text: qsTr("Clear")
                    onClicked: {
                        const count = centerPopup.entries.length
                        const newest = centerPopup.entries[0].id
                        Ui.offerUndo(count === 1 ? qsTr("Cleared 1 notification.") : qsTr("Cleared %1 notifications.").arg(count),
                                     () => {
                                         centerPopup.clearedUpTo = 0
                                         if (centerPopup.opened) centerPopup.entries = App.library.notifications()
                                     },
                                     () => { centerPopup.clearedUpTo = 0; App.library.clearNotifications(newest) })
                        centerPopup.clearedUpTo = newest
                        centerPopup.entries = []
                        centerPopup.close()
                    }
                }
            }

            ColumnLayout {
                visible: centerPopup.entries.length === 0
                Layout.fillWidth: true
                Layout.topMargin: 14
                Layout.bottomMargin: 14
                spacing: 6
                AppIcon {
                    Layout.alignment: Qt.AlignHCenter
                    name: "bell"
                    size: Ui.sp(26)
                    color: Theme.textMuted
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("No notifications")
                    color: Theme.textSecondary
                    font.pixelSize: Ui.sp(Theme.compactSize)
                }
                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: qsTr("New episodes, finished downloads and tracker problems show up here")
                    color: Theme.textMuted
                    font.pixelSize: Ui.sp(Theme.captionSize)
                }
            }

            ListView {
                id: entryList
                visible: count > 0
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, 420)
                clip: true
                spacing: 2
                boundsBehavior: Flickable.StopAtBounds
                model: centerPopup.entries
                ScrollBar.vertical: AppScrollBar {}

                delegate: Rectangle {
                    id: entryRow
                    required property var modelData
                    width: entryList.width
                    height: entryCol.implicitHeight + 16
                    radius: 8
                    color: entryHover.hovered ? Theme.hoverFill : "transparent"

                    HoverHandler { id: entryHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: bell.follow(entryRow.modelData) }

                    AppIcon {
                        id: entryIcon
                        anchors { left: parent.left; leftMargin: 8; top: parent.top; topMargin: 10 }
                        name: entryRow.modelData.kind === "episode" ? "tv" : entryRow.modelData.kind === "provider" ? "server"
                              : entryRow.modelData.kind === "tracker" ? "refresh-cw" : "download"
                        size: 17
                        color: entryRow.modelData.seen ? Theme.textMuted : Theme.accent
                    }
                    Column {
                        id: entryCol
                        anchors { left: entryIcon.right; leftMargin: 10; right: parent.right; rightMargin: 10
                                  verticalCenter: parent.verticalCenter }
                        spacing: 2
                        Text {
                            width: parent.width
                            text: entryRow.modelData.title
                            elide: Text.ElideRight
                            color: Theme.textPrimary
                            font.pixelSize: Ui.sp(Theme.compactSize)
                            font.weight: entryRow.modelData.seen ? Font.Normal : Font.DemiBold
                        }
                        Text {
                            width: parent.width
                            text: String(entryRow.modelData.message).replace(/\.$/, "") + "  ·  " + Ui.ago(entryRow.modelData.at)
                            wrapMode: Text.Wrap
                            maximumLineCount: 3
                            elide: Text.ElideRight
                            color: Theme.textMuted
                            font.pixelSize: Ui.sp(Theme.captionSize)
                        }
                    }
                }
            }
        }
    }
}
