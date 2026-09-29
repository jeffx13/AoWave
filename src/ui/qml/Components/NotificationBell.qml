pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
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

    Rectangle {
        visible: App.library.unseenNotifications > 0
        anchors { top: parent.top; right: parent.right; topMargin: -3; rightMargin: -5 }
        width: Math.max(height, unseenText.implicitWidth + 8)
        height: 16
        radius: 8
        color: Theme.danger
        Text {
            id: unseenText
            anchors.centerIn: parent
            text: Math.min(App.library.unseenNotifications, 99)
            color: Theme.inkOn(Theme.danger)
            font.pixelSize: 11
            font.bold: true
        }
    }

    function follow(entry) {
        centerPopup.close()
        if (entry.kind === "episode") {
            App.openShowInfo(entry.link, entry.provider, entry.title)
        } else {
            App.playlist.openUrl("file:///" + entry.link, true)
            Globals.gotoPage(AppShell.Player)
        }
    }

    AppPopup {
        id: centerPopup
        toggledByParent: true
        property var entries: []
        x: bell.width - width
        y: bell.height + 8
        width: 380
        padding: 10
        backgroundRadius: 12
        onAboutToShow: entries = App.library.notifications()
        // Read on opening: the unseen dots stay until it is shown again.
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
                    font.pixelSize: Globals.sp(Theme.bodySize)
                    font.weight: Font.DemiBold
                }
                Text {
                    visible: centerPopup.entries.length > 0
                    text: qsTr("Clear")
                    color: clearHover.hovered ? Theme.textPrimary : Theme.textMuted
                    font.pixelSize: Globals.sp(Theme.compactSize)
                    font.underline: clearHover.hovered
                    HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: {
                            App.library.clearNotifications()
                            centerPopup.entries = []
                        }
                    }
                }
            }

            Text {
                visible: centerPopup.entries.length === 0
                Layout.fillWidth: true
                Layout.margins: 6
                wrapMode: Text.Wrap
                text: qsTr("Nothing yet. New episodes of shows you are watching, and finished downloads, show up here.")
                color: Theme.textMuted
                font.pixelSize: Globals.sp(Theme.compactSize)
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
                        name: entryRow.modelData.kind === "episode" ? "tv" : "download"
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
                            font.pixelSize: Globals.sp(Theme.compactSize)
                            font.weight: entryRow.modelData.seen ? Font.Normal : Font.DemiBold
                        }
                        Text {
                            width: parent.width
                            text: entryRow.modelData.message + "  ·  " + Globals.ago(entryRow.modelData.at)
                            elide: Text.ElideRight
                            color: Theme.textMuted
                            font.pixelSize: Globals.sp(13)
                        }
                    }
                }
            }
        }
    }
}
