pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import "./../Components"
import QtQuick.Layouts
import QtQuick.Dialogs
import App
import ".."

Item {
    id: downloadPage
    // Read once per change: each read is a query and a look at every file.
    readonly property var finished: App.downloads.finished

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
                font.pixelSize: Globals.sp(20)
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.textSecondary
                font.pixelSize: Globals.sp(Theme.bodySize)
                text: cleanupDialog.files.length === 0
                      ? qsTr("No watched episodes in the download folder. An episode counts once it is played here past the watched mark in Settings.")
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
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Layout.maximumHeight: Globals.controlHeight
            spacing: 6

            AppTextField {
                id: workDirField
                text: App.settings.downloadDir
                fontSize: Theme.bodySize
                placeholderText: qsTr("Working directory")
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 8
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

            AppButton {
                text: qsTr("Open")
                fontSize: Theme.bodySize
                Layout.fillHeight: true
                onClicked: Qt.openUrlExternally("file:///" + App.settings.downloadDir)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.maximumHeight: Globals.controlHeight
            spacing: 6

            AppTextField {
                id: nameField
                fontSize: Theme.bodySize
                placeholderText: qsTr("Filename")
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 3
                onAccepted: downloadPage.download()
            }

            AppTextField {
                id: urlField
                fontSize: Theme.bodySize
                placeholderText: qsTr("m3u8 / video URL")
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 6
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
                Layout.fillHeight: true
                onClicked: downloadPage.download()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.maximumHeight: Globals.sp(36)
            spacing: 8

            Text {
                text: (taskList.count === 1 ? qsTr("1 task") : qsTr("%1 tasks").arg(taskList.count))
                      + (App.downloads.overview ? "  \u00b7  " + App.downloads.overview : "")
                color: Theme.textMuted
                font.pixelSize: Globals.sp(Theme.bodySize)
            }

            Item { Layout.fillWidth: true }

            AppButton {
                text: qsTr("Clean up watched"); fontSize: 18; radius: 8
                secondary: true
                Layout.preferredHeight: Globals.sp(34)
                onClicked: {
                    cleanupDialog.files = App.library.watchedFilesUnder(App.settings.downloadDir)
                    cleanupDialog.open()
                }
            }

            Text {
                text: qsTr("Max concurrent")
                color: Theme.textSecondary
                font.pixelSize: Globals.sp(Theme.bodySize)
            }
            AppSpinBox {
                from: 1
                to: 8
                value: App.downloads.maxDownloads
                onValueModified: App.downloads.maxDownloads = value
                Layout.preferredWidth: Globals.sp(104)
                Layout.preferredHeight: Globals.sp(34)
            }

            Text {
                text: qsTr("Speed limit")
                color: Theme.textSecondary
                font.pixelSize: Globals.sp(Theme.bodySize)
            }
            AppTextField {
                id: speedLimitField
                placeholderText: qsTr("e.g. 5M")
                text: App.settings.maxSpeed
                fontSize: Theme.bodySize
                Layout.preferredWidth: Globals.sp(96)
                Layout.preferredHeight: Globals.sp(34)
                // commitMaxSpeed normalises, so read it back.
                onEditingFinished: {
                    App.settings.commitMaxSpeed(text)
                    text = App.settings.maxSpeed
                }
            }

            AppButton {
                text: qsTr("Pause all"); fontSize: 18; radius: 8
                secondary: true
                visible: taskList.count > 0
                Layout.preferredHeight: Globals.sp(34)
                onClicked: App.downloads.pauseAll()
            }
            AppButton {
                text: qsTr("Resume all"); fontSize: 18; radius: 8
                secondary: true
                visible: taskList.count > 0
                Layout.preferredHeight: Globals.sp(34)
                onClicked: App.downloads.resumeAll()
            }
            AppButton {
                text: qsTr("Cancel all"); fontSize: 18; radius: 8
                backgroundDefaultColor: Theme.danger
                visible: taskList.count > 0
                Layout.preferredHeight: Globals.sp(34)
                onClicked: {
                    const held = App.downloads.holdCancel(-1)
                    if (held.length > 0)
                        Globals.offerUndo(held.length === 1 ? qsTr("Cancelled 1 download") : qsTr("Cancelled %1 downloads").arg(held.length),
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

            ScrollBar.vertical: AppScrollBar { width: 6 }

            EmptyState {
                parent: taskList
                anchors.centerIn: parent
                visible: taskList.count === 0 && downloadPage.finished.length === 0
                icon: "download"
                title: qsTr("No downloads")
                hint: qsTr("Queue episodes from a show's page, or paste a video URL above.")
            }

            // Finished ones stay listed, to play or find, until their file goes.
            footer: Column {
                width: taskList.width
                visible: downloadPage.finished.length > 0
                height: visible ? implicitHeight : 0
                topPadding: taskList.count > 0 ? 16 : 0
                spacing: 6

                RowLayout {
                    width: parent.width
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Finished")
                        color: Theme.textSecondary
                        font.pixelSize: Globals.sp(Theme.bodySize)
                        font.weight: Font.DemiBold
                    }
                    Text {
                        text: qsTr("Clear list")
                        color: clearFinishedHover.hovered ? Theme.textPrimary : Theme.textMuted
                        font.pixelSize: Globals.sp(Theme.compactSize)
                        font.underline: clearFinishedHover.hovered
                        HoverHandler { id: clearFinishedHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: App.downloads.clearFinished() }
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

                            AppIcon { name: "check"; size: 18; color: Theme.success }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    Layout.fillWidth: true
                                    text: done.modelData.title
                                    elide: Text.ElideRight
                                    color: Theme.textPrimary
                                    font.pixelSize: Globals.sp(Theme.bodySize)
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: Globals.ago(done.modelData.at) + "  ·  " + done.modelData.path
                                    elide: Text.ElideMiddle
                                    color: Theme.textMuted
                                    font.pixelSize: Globals.sp(Theme.compactSize)
                                }
                            }
                            AppButton {
                                text: qsTr("Play")
                                fontSize: 18
                                radius: 8
                                Layout.preferredHeight: Globals.sp(32)
                                onClicked: {
                                    App.playlist.openUrl("file:///" + done.modelData.path, true)
                                    Globals.gotoPage(AppShell.Player)
                                }
                            }
                            IconButton {
                                iconName: "folder"
                                iconSize: 17
                                implicitWidth: Globals.sp(32)
                                implicitHeight: Globals.sp(32)
                                boxRadius: 8
                                tip: qsTr("Show in folder")
                                onClicked: App.downloads.showInFolder(done.modelData.path)
                            }
                            IconButton {
                                iconName: "x"
                                iconSize: 17
                                implicitWidth: Globals.sp(32)
                                implicitHeight: Globals.sp(32)
                                boxRadius: 8
                                tip: qsTr("Remove from the list")
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

                width: taskList.width
                height: taskCol.implicitHeight + 20
                radius: 10

                ColumnLayout {
                    id: taskCol
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    RowLayout {
                        spacing: 8

                        Text {
                            text: task.downloadName
                            color: Theme.textPrimary
                            font.pixelSize: Globals.sp(Theme.bodySize)
                            font.bold: true
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }

                        AppButton {
                            readonly property bool failed: task.status === DownloadTask.Failed
                            readonly property bool stalled: failed || task.status === DownloadTask.Paused

                            text: failed ? qsTr("Retry") : task.status === DownloadTask.Paused ? qsTr("Resume") : qsTr("Pause")
                            fontSize: 18
                            backgroundDefaultColor: failed ? Theme.warning : Theme.surfaceAlt
                            contentItemTextColor: failed ? Theme.inkOn(Theme.warning) : Theme.textPrimary
                            radius: 8
                            Layout.preferredWidth: Globals.sp(96)
                            Layout.preferredHeight: Globals.sp(32)
                            onClicked: {
                                if (stalled) App.downloads.resumeTask(task.index)
                                else App.downloads.pauseTask(task.index)
                            }
                        }

                        AppButton {
                            text: qsTr("Cancel")
                            fontSize: 18
                            backgroundDefaultColor: Theme.surfaceAlt
                            contentItemTextColor: Theme.danger
                            radius: 8
                            Layout.preferredWidth: Globals.sp(96)
                            Layout.preferredHeight: Globals.sp(32)
                            onClicked: {
                                const held = App.downloads.holdCancel(task.index)
                                Globals.offerUndo(qsTr("Cancelled \u201c%1\u201d").arg(task.downloadName),
                                                  () => App.downloads.undoCancel(held), () => App.downloads.settleCancel(held))
                            }
                        }
                    }

                    Text {
                        text: task.downloadPath
                        color: Theme.textMuted
                        font.pixelSize: Globals.sp(Theme.bodySize)
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }

                    ProgressBar {
                        id: taskProgress
                        Layout.fillWidth: true
                        from: 0
                        to: 100
                        value: task.progressValue
                        indeterminate: task.progressValue === 0

                        background: Rectangle {
                            implicitHeight: 8
                            radius: 4
                            color: Theme.border
                        }

                        contentItem: Item {
                            implicitHeight: 8
                            Rectangle {
                                width: taskProgress.visualPosition * parent.width
                                height: parent.height
                                radius: 4
                                color: Theme.accent
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            text: task.progressText
                            color: Theme.textMuted
                            font.pixelSize: Globals.sp(Theme.bodySize)
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }

                        Text {
                            visible: task.stats !== ""
                            text: task.stats
                            color: Theme.accent
                            font.pixelSize: Globals.sp(Theme.bodySize)
                            font.bold: true
                        }
                    }
                }
            }
        }
    }
}
