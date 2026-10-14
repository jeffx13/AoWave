pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import App

Item {
    id: downloadPage
    readonly property var finished: App.downloads.finished
    onVisibleChanged: if (visible) App.downloads.pruneMissing()
    Component.onCompleted: App.downloads.pruneMissing()

    FolderDialog {
        id: folderDialog
        currentFolder: "file:///" + workDirField.text
        onAccepted: {
            let path = selectedFolder.toString().replace(/^file:\/\/\//, "")
            App.settings.downloadDir = path
            workDirField.text = App.settings.downloadDir
        }
    }
    HoverHandler {
        cursorShape: Qt.ArrowCursor
    }

    AppPopup {
        id: cleanupDialog
        property var files: []
        readonly property string size: Qt.locale().formattedDataSize(files.reduce((sum, file) => sum + file.size, 0))
        modal: true
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        width: Math.min(480, parent ? parent.width - 60 : 480)
        padding: 18
        backgroundRadius: 14

        contentItem: ColumnLayout {
            spacing: 14

            Text {
                Layout.fillWidth: true
                text: qsTr("Clean up watched downloads")
                color: Theme.textPrimary
                font.pixelSize: Ui.sp(20)
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.textSecondary
                font.pixelSize: Ui.sp(Theme.bodySize)
                text: cleanupDialog.files.length === 0
                      ? qsTr("No watched episodes in the download folder. An episode counts once you play it past the Mark watched at setting.")
                      : cleanupDialog.files.length === 1
                        ? qsTr("Move 1 watched episode (%1) to the Recycle Bin?").arg(cleanupDialog.size)
                        : qsTr("Move %1 watched episodes (%2) to the Recycle Bin?").arg(cleanupDialog.files.length).arg(cleanupDialog.size)
            }
            RowLayout {
                spacing: 8
                Item { Layout.fillWidth: true }
                AppButton {
                    secondary: true
                    text: cleanupDialog.files.length > 0 ? qsTr("Cancel") : qsTr("Close")
                    onClicked: cleanupDialog.close()
                }
                AppButton {
                    visible: cleanupDialog.files.length > 0
                    text: qsTr("Move to Recycle Bin")
                    onClicked: {
                        App.library.trashFiles(cleanupDialog.files.map(file => file.path))
                        cleanupDialog.close()
                    }
                }
            }
        }
    }

    function download() {
        if (nameField.text === "" || urlField.text === "") return
        App.downloads.downloadLink(nameField.text, urlField.text)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: Ui.controlHeight
            Layout.maximumHeight: Ui.controlHeight
            spacing: 6

            AppTextField {
                id: workDirField
                text: App.settings.downloadDir
                fontSize: Theme.bodySize
                placeholderText: qsTr("Download folder")
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: Ui.sp(120)
                onAccepted: {
                    App.settings.downloadDir = text
                    text = App.settings.downloadDir
                }
            }

            AppButton {
                text: qsTr("Browse")
                fontSize: Theme.bodySize
                Layout.fillHeight: true
                onClicked: folderDialog.open()
            }

            IconButton {
                iconName: "folder"
                tip: qsTr("Open folder")
                implicitWidth: Ui.controlHeight
                implicitHeight: Ui.controlHeight
                onClicked: Qt.openUrlExternally("file:///" + App.settings.downloadDir)
            }

            IconButton {
                id: optionsButton
                iconName: "settings"
                tip: qsTr("Download options")
                active: optionsPopup.visible
                implicitWidth: Ui.controlHeight
                implicitHeight: Ui.controlHeight
                onClicked: optionsPopup.toggle()

                AppPopup {
                    id: optionsPopup
                    toggledByParent: true
                    x: optionsButton.width - width
                    y: optionsButton.height + 6
                    width: Ui.sp(340)
                    padding: 14
                    backgroundRadius: 10

                    contentItem: ColumnLayout {
                        spacing: 10

                        RowLayout {
                            spacing: 8
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Downloads at once")
                                color: Theme.textSecondary
                                font.pixelSize: Ui.sp(Theme.bodySize)
                                elide: Text.ElideRight
                            }
                            AppSpinBox {
                                from: 1
                                to: 8
                                value: App.downloads.maxDownloads
                                onValueModified: App.downloads.maxDownloads = value
                                Layout.preferredWidth: Ui.sp(104)
                                Layout.preferredHeight: Ui.sp(34)
                            }
                        }

                        RowLayout {
                            spacing: 8
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Speed limit")
                                color: Theme.textSecondary
                                font.pixelSize: Ui.sp(Theme.bodySize)
                                elide: Text.ElideRight
                            }
                            AppTextField {
                                placeholderText: qsTr("e.g. 5M")
                                text: App.settings.maxSpeed
                                fontSize: Theme.bodySize
                                Layout.preferredWidth: Ui.sp(104)
                                Layout.preferredHeight: Ui.sp(34)
                                onEditingFinished: {
                                    App.settings.commitMaxSpeed(text)
                                    text = App.settings.maxSpeed
                                }
                            }
                        }

                        RowLayout {
                            spacing: 8
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Text {
                                    Layout.fillWidth: true
                                    text: qsTr("Include danmaku")
                                    color: Theme.textSecondary
                                    font.pixelSize: Ui.sp(Theme.bodySize)
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: qsTr("Saved as a subtitle track")
                                    color: Theme.textMuted
                                    font.pixelSize: Ui.sp(Theme.compactSize)
                                    wrapMode: Text.Wrap
                                }
                            }
                            AppSwitch {
                                checked: App.settings.downloadDanmaku
                                onToggled: App.settings.downloadDanmaku = checked
                            }
                        }

                        AppButton {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Ui.sp(34)
                            text: qsTr("Clean up watched")
                            fontSize: 18
                            radius: 8
                            secondary: true
                            onClicked: {
                                optionsPopup.close()
                                cleanupDialog.files = App.library.watchedFilesUnder(App.settings.downloadDir)
                                cleanupDialog.open()
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: Ui.controlHeight
            Layout.maximumHeight: Ui.controlHeight
            spacing: 6

            AppTextField {
                id: nameField
                fontSize: Theme.bodySize
                placeholderText: qsTr("Filename")
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 3
                Layout.minimumWidth: Ui.sp(80)
                onAccepted: downloadPage.download()
            }

            AppTextField {
                id: urlField
                fontSize: Theme.bodySize
                placeholderText: qsTr("m3u8 / video URL")
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 6
                Layout.minimumWidth: Ui.sp(120)
                onAccepted: downloadPage.download()
            }

            AppComboBox {
                readonly property var heights: [0, 1080, 720, 480]
                model: [qsTr("Best quality"), "1080p", "720p", "480p"]
                currentIndex: Math.max(0, heights.indexOf(App.settings.downloadMaxHeight))
                onActivated: index => App.settings.downloadMaxHeight = heights[index]
                Layout.fillHeight: true
            }

            AppButton {
                text: qsTr("Download")
                fontSize: Theme.bodySize
                enabled: nameField.text !== "" && urlField.text !== ""
                Layout.fillHeight: true
                onClicked: downloadPage.download()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            visible: taskList.count > 0
            spacing: 2

            Text {
                Layout.fillWidth: true
                text: (taskList.count === 1 ? qsTr("1 download") : qsTr("%1 downloads").arg(taskList.count))
                      + (App.downloads.overview ? "  ·  " + App.downloads.overview : "")
                color: Theme.textSecondary
                font.pixelSize: Ui.sp(Theme.bodySize)
                elide: Text.ElideRight
            }
            IconButton {
                iconName: "pause"
                tip: qsTr("Pause all")
                implicitWidth: Ui.sp(34)
                implicitHeight: Ui.sp(34)
                onClicked: App.downloads.pauseAll()
            }
            IconButton {
                iconName: "play"
                tip: qsTr("Resume all")
                implicitWidth: Ui.sp(34)
                implicitHeight: Ui.sp(34)
                onClicked: App.downloads.resumeAll()
            }
            IconButton {
                iconName: "x"
                destructive: true
                tip: qsTr("Cancel all")
                implicitWidth: Ui.sp(34)
                implicitHeight: Ui.sp(34)
                onClicked: {
                    const held = App.downloads.holdCancel(-1)
                    if (held.length > 0)
                        Ui.offerUndo(held.length === 1 ? qsTr("Cancelled 1 download.") : qsTr("Cancelled %1 downloads.").arg(held.length),
                                          () => App.downloads.undoCancel(held), () => App.downloads.settleCancel(held))
                }
            }
        }

        ListView {
            id: taskList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            boundsBehavior: Flickable.StopAtBounds
            model: App.downloads
            readonly property real rowWidth: width - Ui.sp(Theme.scrollbarGutter)

            ScrollBar.vertical: AppScrollBar { width: 6 }

            EmptyState {
                parent: taskList
                anchors.centerIn: parent
                visible: taskList.count === 0 && downloadPage.finished.length === 0
                icon: "download"
                title: qsTr("No downloads")
                hint: qsTr("Queue episodes from a show's page, or paste a video URL above.")
            }

            footer: Column {
                width: taskList.rowWidth
                visible: downloadPage.finished.length > 0
                height: visible ? implicitHeight : 0
                topPadding: taskList.count > 0 ? 16 : 0
                spacing: 6

                RowLayout {
                    width: parent.width
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Done")
                        color: Theme.textSecondary
                        font.pixelSize: Ui.sp(Theme.bodySize)
                        font.weight: Font.DemiBold
                    }
                    LinkText {
                        text: qsTr("Clear list")
                        onClicked: App.downloads.clearFinished()
                    }
                }

                Repeater {
                    model: downloadPage.finished
                    delegate: Card {
                        id: done
                        required property var modelData
                        width: parent.width
                        height: doneRow.implicitHeight + 16
                        radius: 10

                        RowLayout {
                            id: doneRow
                            anchors { fill: parent; leftMargin: 12; rightMargin: 8; topMargin: 8; bottomMargin: 8 }
                            spacing: 10

                            AppIcon { name: "download-done"; size: 18; color: Theme.success }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    Layout.fillWidth: true
                                    text: done.modelData.title
                                    elide: Text.ElideRight
                                    color: Theme.textPrimary
                                    font.pixelSize: Ui.sp(Theme.bodySize)
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: Ui.ago(done.modelData.at) + "  ·  " + done.modelData.path
                                    elide: Text.ElideMiddle
                                    color: Theme.textMuted
                                    font.pixelSize: Ui.sp(Theme.compactSize)
                                }
                            }
                            AppButton {
                                text: qsTr("Play")
                                fontSize: 18
                                radius: 8
                                Layout.preferredHeight: Ui.sp(32)
                                onClicked: {
                                    App.playlist.openUrl("file:///" + done.modelData.path, true)
                                    Ui.gotoPage(MainWindow.Player)
                                }
                            }
                            AppButton {
                                visible: App.playlist.playingList !== done.modelData.folder
                                         && (done.modelData.showLink === "" || App.playlist.playingList !== done.modelData.showLink)
                                text: qsTr("Queue")
                                secondary: true
                                fontSize: 18
                                radius: 8
                                Layout.preferredHeight: Ui.sp(32)
                                onClicked: App.playlist.queueDownloaded(done.modelData.path)
                            }
                            IconButton {
                                iconName: "folder"
                                iconSize: 17
                                implicitWidth: Ui.sp(32)
                                implicitHeight: Ui.sp(32)
                                boxRadius: 8
                                tip: qsTr("Show in folder")
                                onClicked: App.downloads.showInFolder(done.modelData.path)
                            }
                            IconButton {
                                iconName: "x"
                                iconSize: 17
                                implicitWidth: Ui.sp(32)
                                implicitHeight: Ui.sp(32)
                                boxRadius: 8
                                tip: qsTr("Remove from list")
                                onClicked: App.downloads.forgetFinished(done.modelData.path)
                            }
                        }
                    }
                }
            }

            delegate: Card {
                id: task
                required property int    progressValue
                required property string progressText
                required property string downloadName
                required property string downloadPath
                required property int    status
                required property string stats
                required property int    index
                readonly property bool failed: status === DownloadTask.Failed
                readonly property bool stopped: failed || status === DownloadTask.Paused

                width: taskList.rowWidth
                height: taskCol.implicitHeight + 24
                radius: 10

                ColumnLayout {
                    id: taskCol
                    anchors { fill: parent; leftMargin: 14; rightMargin: 8; topMargin: 12; bottomMargin: 12 }
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Text {
                            Layout.fillWidth: true
                            text: task.downloadName
                            color: Theme.textPrimary
                            font.pixelSize: Ui.sp(Theme.bodySize)
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                            HoverHandler { id: nameHover }
                            AppToolTip { text: task.downloadPath; visible: nameHover.hovered }
                        }
                        IconButton {
                            iconName: task.failed ? "refresh-cw" : task.stopped ? "play" : "pause"
                            iconColor: task.failed ? Theme.warning : Theme.textSecondary
                            tip: task.failed ? qsTr("Retry") : task.stopped ? qsTr("Resume") : qsTr("Pause")
                            implicitWidth: Ui.sp(32)
                            implicitHeight: Ui.sp(32)
                            onClicked: {
                                if (task.stopped) App.downloads.resumeTask(task.index)
                                else App.downloads.pauseTask(task.index)
                            }
                        }
                        IconButton {
                            iconName: "x"
                            destructive: true
                            tip: qsTr("Cancel")
                            implicitWidth: Ui.sp(32)
                            implicitHeight: Ui.sp(32)
                            onClicked: {
                                const held = App.downloads.holdCancel(task.index)
                                Ui.offerUndo(qsTr("Cancelled “%1”.").arg(task.downloadName),
                                                  () => App.downloads.undoCancel(held), () => App.downloads.settleCancel(held))
                            }
                        }
                    }

                    ProgressBar {
                        id: taskProgress
                        Layout.fillWidth: true
                        Layout.rightMargin: 6
                        from: 0
                        to: 100
                        value: task.progressValue
                        indeterminate: task.progressValue === 0 && !task.stopped

                        background: Rectangle {
                            implicitHeight: 6
                            radius: 3
                            color: Theme.border
                        }

                        contentItem: Item {
                            implicitHeight: 6
                            Rectangle {
                                width: taskProgress.visualPosition * parent.width
                                height: parent.height
                                radius: 3
                                color: task.failed ? Theme.warning : task.stopped ? Theme.textMuted : Theme.accent
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.rightMargin: 6
                        spacing: 12

                        Text {
                            Layout.fillWidth: true
                            text: task.progressText
                            color: task.failed ? Theme.warning : Theme.textMuted
                            font.pixelSize: Ui.sp(Theme.compactSize)
                            elide: Text.ElideRight
                        }
                        Text {
                            visible: task.stats !== "" && !task.stopped
                            text: task.stats
                            color: Theme.textSecondary
                            font.pixelSize: Ui.sp(Theme.compactSize)
                        }
                    }
                }
            }
        }
    }
}
