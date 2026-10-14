pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQml.Models
import App

Item {
    id: downloadPage
    onVisibleChanged: if (visible) App.downloads.pruneMissing()
    Component.onCompleted: App.downloads.pruneMissing()

    function sizeText(bytes) { return Qt.locale().formattedDataSize(bytes) }

    FolderDialog {
        id: folderDialog
        currentFolder: "file:///" + workDirField.text
        onAccepted: {
            let path = decodeURIComponent(selectedFolder.toString().replace(/^file:\/\/\//, ""))
            App.settings.downloadDir = path
            workDirField.text = App.settings.downloadDir
            workDirField.rejected = false
        }
    }
    HoverHandler {
        cursorShape: Qt.ArrowCursor
    }

    AppPopup {
        id: cleanupDialog
        property var files: []
        readonly property string size: downloadPage.sizeText(files.reduce((sum, file) => sum + file.size, 0))
        modal: true
        focus: true
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        width: Math.min(480, parent ? parent.width - 60 : 480)
        padding: 18
        backgroundRadius: 14
        // Enter lands on Cancel, so deleting takes a click or Tab
        onOpened: cleanupCancel.forceActiveFocus(Qt.TabFocusReason)

        contentItem: ColumnLayout {
            spacing: 14

            Text {
                Layout.fillWidth: true
                text: qsTr("Clean up watched downloads")
                color: Theme.textPrimary
                font.pixelSize: Ui.sp(Theme.bodySize)
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
                    id: cleanupCancel
                    secondary: true
                    text: cleanupDialog.files.length > 0 ? qsTr("Cancel") : qsTr("Close")
                    onClicked: cleanupDialog.close()
                }
                AppButton {
                    visible: cleanupDialog.files.length > 0
                    text: qsTr("Move to Recycle Bin")
                    onClicked: {
                        App.library.trashFiles(cleanupDialog.files.map(file => file.path))
                        App.downloads.pruneMissing()
                        cleanupDialog.close()
                    }
                }
            }
        }
    }

    AppPopup {
        id: deleteDialog
        property string folder: ""
        property string path: ""
        property string name: ""
        property int files: 0
        property real size: 0
        property int unfinished: 0
        function ask(folder, path, name, files, size, unfinished) {
            deleteDialog.folder = folder
            deleteDialog.path = path
            deleteDialog.name = name
            deleteDialog.files = files
            deleteDialog.size = size
            deleteDialog.unfinished = unfinished
            open()
        }
        modal: true
        focus: true
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        width: Math.min(480, parent ? parent.width - 60 : 480)
        padding: 18
        backgroundRadius: 14
        onOpened: deleteCancel.forceActiveFocus(Qt.TabFocusReason)

        contentItem: ColumnLayout {
            spacing: 14

            Text {
                Layout.fillWidth: true
                text: qsTr("Delete “%1”").arg(deleteDialog.name)
                color: Theme.textPrimary
                font.pixelSize: Ui.sp(Theme.bodySize)
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.textSecondary
                font.pixelSize: Ui.sp(Theme.bodySize)
                text: {
                    const files = deleteDialog.files, unfinished = deleteDialog.unfinished
                    const size = downloadPage.sizeText(deleteDialog.size)
                    if (files === 0)
                        return unfinished === 1 ? qsTr("Delete 1 unfinished download?")
                                                : qsTr("Delete %1 unfinished downloads?").arg(unfinished)
                    return (files === 1 ? qsTr("Move 1 episode (%1) to the Recycle Bin?").arg(size)
                                        : qsTr("Move %1 episodes (%2) to the Recycle Bin?").arg(files).arg(size))
                           + (unfinished === 0 ? "" : " " + (unfinished === 1 ? qsTr("1 unfinished download is deleted too.")
                                                                              : qsTr("%1 unfinished downloads are deleted too.").arg(unfinished)))
                }
            }
            RowLayout {
                spacing: 8
                Item { Layout.fillWidth: true }
                AppButton {
                    id: deleteCancel
                    secondary: true
                    text: qsTr("Cancel")
                    onClicked: deleteDialog.close()
                }
                AppButton {
                    text: deleteDialog.files > 0 ? qsTr("Move to Recycle Bin") : qsTr("Delete")
                    onClicked: {
                        App.downloads.deleteDownloads(deleteDialog.folder, deleteDialog.path)
                        deleteDialog.close()
                    }
                }
            }
        }
    }

    AppPopup {
        id: detailsDialog
        property string name: ""
        property string reason: ""
        property string detail: ""
        modal: true
        focus: true
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        width: Math.min(720, parent ? parent.width - 60 : 720)
        padding: 18
        backgroundRadius: 14

        contentItem: ColumnLayout {
            spacing: 12

            Text {
                Layout.fillWidth: true
                text: detailsDialog.name
                color: Theme.textPrimary
                font.pixelSize: Ui.sp(Theme.bodySize)
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                text: detailsDialog.reason
                color: Theme.warning
                font.pixelSize: Ui.sp(Theme.bodySize)
                wrapMode: Text.Wrap
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(detailText.implicitHeight + 16, Ui.sp(320))
                clip: true
                ScrollBar.vertical: AppScrollBar {}
                TextArea {
                    id: detailText
                    text: detailsDialog.detail
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.Wrap
                    font.family: "Consolas"
                    font.pixelSize: Ui.sp(Theme.compactSize)
                    color: Theme.textSecondary
                    selectionColor: Theme.accent
                    selectedTextColor: Theme.onAccent
                    background: Rectangle { radius: 8; color: Theme.surfaceDeep }

                    ContextMenu.menu: null
                    ContextMenu.onRequested: (position) => { detailMenu.active = true; (detailMenu.item as TextMenu).popup(position) }
                    Loader { id: detailMenu; active: false; sourceComponent: TextMenu { target: detailText } }
                }
            }
            RowLayout {
                spacing: 8
                Item { Layout.fillWidth: true }
                AppButton {
                    secondary: true
                    text: qsTr("Copy")
                    onClicked: App.copyToClipboard(detailText.text)
                }
                AppButton {
                    text: qsTr("Close")
                    onClicked: detailsDialog.close()
                }
            }
        }
    }

    property string submitted: ""
    function download() {
        if (nameField.text === "" || urlField.text === "") return
        submitted = urlField.text
        if (!App.downloads.downloadLink(nameField.text, urlField.text)) submitted = ""
    }
    Connections {
        target: App.downloads
        function onQueued(added) {
            if (downloadPage.submitted === "" || urlField.text !== downloadPage.submitted) return
            if (added > 0) {
                nameField.clear()
                urlField.clear()
            }
            downloadPage.submitted = ""
        }
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
                property bool rejected: false
                onTextEdited: rejected = false
                onEditingFinished: {
                    rejected = text.trim() !== "" && !App.settings.commitDownloadDir(text)
                    if (!rejected) text = App.settings.downloadDir
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
                onClicked: App.downloads.openFolder(App.settings.downloadDir)
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
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Text {
                                    Layout.fillWidth: true
                                    text: qsTr("Speed limit")
                                    color: Theme.textSecondary
                                    font.pixelSize: Ui.sp(Theme.bodySize)
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: qsTr("Applies to downloads that start next")
                                    color: Theme.textMuted
                                    font.pixelSize: Ui.sp(Theme.compactSize)
                                    wrapMode: Text.Wrap
                                }
                            }
                            AppTextField {
                                id: speedField
                                property bool rejected: false
                                placeholderText: qsTr("e.g. 5M")
                                text: App.settings.maxSpeed
                                fontSize: Theme.bodySize
                                Layout.preferredWidth: Ui.sp(104)
                                Layout.preferredHeight: Ui.sp(34)
                                onTextEdited: rejected = false
                                onEditingFinished: {
                                    rejected = !App.settings.commitMaxSpeed(text)
                                    if (!rejected) text = App.settings.maxSpeed
                                }
                            }
                        }
                        Text {
                            visible: speedField.rejected
                            Layout.fillWidth: true
                            text: qsTr("Use a number with K, M or G, like 5M.")
                            color: Theme.warning
                            font.pixelSize: Ui.sp(Theme.compactSize)
                            wrapMode: Text.Wrap
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
                                    text: qsTr("Saved as a subtitle track in downloads that start next")
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
                            fontSize: Theme.compactSize
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

        Text {
            visible: workDirField.rejected
            Layout.fillWidth: true
            text: qsTr("Could not use that folder. Pick one that exists and can be written to.")
            color: Theme.warning
            font.pixelSize: Ui.sp(Theme.compactSize)
            wrapMode: Text.Wrap
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
                onAccepted: urlField.text === "" ? urlField.forceActiveFocus() : downloadPage.download()
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
            visible: App.downloads.taskCount > 0 || App.downloads.list.hiddenCount > 0
            spacing: 2

            Text {
                Layout.fillWidth: true
                text: App.downloads.taskCount === 0 ? ""
                      : (App.downloads.taskCount === 1 ? qsTr("1 download") : qsTr("%1 downloads").arg(App.downloads.taskCount))
                        + (App.downloads.overview ? "  ·  " + App.downloads.overview : "")
                color: Theme.textSecondary
                font.pixelSize: Ui.sp(Theme.bodySize)
                elide: Text.ElideRight
            }
            Text {
                visible: App.downloads.list.hiddenCount > 0
                text: qsTr("Show hidden (%1)").arg(App.downloads.list.hiddenCount)
                color: Theme.textMuted
                font.pixelSize: Ui.sp(Theme.compactSize)
            }
            AppSwitch {
                visible: App.downloads.list.hiddenCount > 0
                Layout.rightMargin: 6
                checked: App.downloads.list.showHidden
                onToggled: App.downloads.list.showHidden = checked
            }
            IconButton {
                visible: App.downloads.taskCount > 0
                iconName: "pause"
                tip: qsTr("Pause all")
                implicitWidth: Ui.sp(34)
                implicitHeight: Ui.sp(34)
                onClicked: App.downloads.pauseAll()
            }
            IconButton {
                visible: App.downloads.taskCount > 0
                iconName: "play"
                tip: qsTr("Resume all")
                implicitWidth: Ui.sp(34)
                implicitHeight: Ui.sp(34)
                onClicked: App.downloads.resumeAll()
            }
            IconButton {
                visible: App.downloads.failed > 0
                iconName: "refresh-cw"
                iconColor: Theme.warning
                tip: qsTr("Retry all failed")
                implicitWidth: Ui.sp(34)
                implicitHeight: Ui.sp(34)
                onClicked: App.downloads.retryFailed()
            }
            IconButton {
                visible: App.downloads.taskCount > 0
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
            spacing: 4
            boundsBehavior: Flickable.StopAtBounds
            model: App.downloads.list
            readonly property real rowWidth: width - Ui.sp(Theme.scrollbarGutter)

            ScrollBar.vertical: AppScrollBar { width: 6 }

            EmptyState {
                parent: taskList
                anchors.centerIn: parent
                visible: taskList.count === 0
                icon: "download"
                title: qsTr("No downloads")
                hint: qsTr("Queue episodes from a show's page, or paste a video URL above.")
            }

            delegate: DelegateChooser {
                role: "isGroup"

                DelegateChoice {
                    roleValue: true
                    Card {
                        id: group
                        required property string folder
                        required property string title
                        required property string cover
                        required property int count
                        required property int tasks
                        required property int active
                        required property int paused
                        required property int failed
                        required property int progress
                        required property string stats
                        required property real size
                        required property real at
                        required property bool open
                        required property bool hidden
                        required property int total
                        required property real totalSize
                        readonly property bool showsHidden: App.downloads.list.showHidden && hidden

                        width: taskList.rowWidth
                        height: groupRow.implicitHeight + 20
                        radius: 10
                        color: groupHover.hovered ? Theme.surfaceAlt : Theme.surface
                        Accessible.role: Accessible.Button
                        Accessible.name: title

                        HoverHandler { id: groupHover }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: App.downloads.list.setOpen(group.folder, !group.open)
                        }

                        RowLayout {
                            id: groupRow
                            anchors { fill: parent; leftMargin: 6; rightMargin: 8; topMargin: 10; bottomMargin: 10 }
                            spacing: 10

                            IconButton {
                                iconName: group.open ? "chevron-down" : "chevron-right"
                                iconSize: 18
                                tip: group.open ? qsTr("Collapse") : qsTr("Expand")
                                implicitWidth: Ui.sp(30)
                                implicitHeight: Ui.sp(30)
                                boxRadius: 8
                                onClicked: App.downloads.list.setOpen(group.folder, !group.open)
                            }
                            CoverTile {
                                visible: group.cover !== ""
                                Layout.preferredWidth: Ui.sp(38)
                                Layout.preferredHeight: Ui.sp(54)
                                source: group.cover
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                Text {
                                    id: groupTitle
                                    Layout.fillWidth: true
                                    text: group.title
                                    color: Theme.textPrimary
                                    font.pixelSize: Ui.sp(Theme.bodySize)
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                    HoverHandler { id: titleHover }
                                    AppToolTip { text: group.folder; visible: titleHover.hovered && groupTitle.truncated }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: [group.count === 1 ? qsTr("1 episode") : qsTr("%1 episodes").arg(group.count),
                                           group.size > 0 ? downloadPage.sizeText(group.size) : "",
                                           group.at > 0 ? qsTr("Last %1").arg(Ui.ago(group.at)) : ""]
                                          .filter(part => part !== "").join("  ·  ")
                                    color: Theme.textMuted
                                    font.pixelSize: Ui.sp(Theme.compactSize)
                                    elide: Text.ElideRight
                                }
                                RowLayout {
                                    visible: group.tasks > 0
                                    spacing: 8
                                    Text {
                                        visible: group.active > 0
                                        text: [qsTr("%1 downloading").arg(group.active), group.progress + "%", group.stats]
                                              .filter(part => part !== "").join("  ·  ")
                                        color: Theme.textAccent
                                        font.pixelSize: Ui.sp(Theme.compactSize)
                                    }
                                    Text {
                                        visible: group.paused > 0
                                        text: qsTr("%1 paused").arg(group.paused)
                                        color: Theme.textMuted
                                        font.pixelSize: Ui.sp(Theme.compactSize)
                                    }
                                    Text {
                                        visible: group.failed > 0
                                        text: qsTr("%1 failed").arg(group.failed)
                                        color: Theme.warning
                                        font.pixelSize: Ui.sp(Theme.compactSize)
                                    }
                                }
                            }
                            IconButton {
                                iconName: "folder-open"
                                iconSize: 17
                                tip: qsTr("Open folder")
                                implicitWidth: Ui.sp(32)
                                implicitHeight: Ui.sp(32)
                                boxRadius: 8
                                onClicked: App.downloads.openFolder(group.folder)
                            }
                            IconButton {
                                visible: group.total > group.tasks || group.showsHidden
                                iconName: group.showsHidden ? "eye" : "eye-off"
                                iconSize: 17
                                tip: group.showsHidden ? qsTr("Unhide") : qsTr("Hide from list")
                                implicitWidth: Ui.sp(32)
                                implicitHeight: Ui.sp(32)
                                boxRadius: 8
                                onClicked: {
                                    if (group.showsHidden) {
                                        App.downloads.list.unhide(group.folder, 0)
                                        return
                                    }
                                    const folder = group.folder, mark = App.downloads.list.hide(folder)
                                    Ui.offerUndo(qsTr("Hid “%1”.").arg(group.title), () => App.downloads.list.unhide(folder, mark))
                                }
                            }
                            IconButton {
                                iconName: "trash-2"
                                iconSize: 17
                                destructive: true
                                tip: qsTr("Delete folder")
                                implicitWidth: Ui.sp(32)
                                implicitHeight: Ui.sp(32)
                                boxRadius: 8
                                onClicked: deleteDialog.ask(group.folder, "", group.title, group.total - group.tasks, group.totalSize, group.tasks)
                            }
                        }
                    }
                }

                DelegateChoice {
                    roleValue: false
                    Item {
                        id: episode
                        required property string folder
                        required property string title
                        required property string path
                        required property string showLink
                        required property int status
                        required property int progress
                        required property string progressText
                        required property string stats
                        required property string detail
                        required property int taskRow
                        required property real size
                        required property real at
                        required property bool hidden
                        readonly property bool isTask: taskRow >= 0
                        readonly property bool failed: status === DownloadTask.Failed
                        readonly property bool stopped: failed || status === DownloadTask.Paused

                        width: taskList.rowWidth
                        height: episodeCol.implicitHeight + 14
                        opacity: hidden ? 0.6 : 1

                        HoverHandler { id: episodeHover }
                        Rectangle {
                            anchors { fill: parent; leftMargin: Ui.sp(36) }
                            radius: 8
                            color: episodeHover.hovered ? Theme.hoverFill : "transparent"
                        }

                        ColumnLayout {
                            id: episodeCol
                            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter
                                      leftMargin: Ui.sp(48); rightMargin: 8 }
                            spacing: 6

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                AppIcon {
                                    name: !episode.isTask ? "download-done" : episode.failed ? "refresh-cw"
                                        : episode.stopped ? "pause" : "download"
                                    size: 17
                                    color: !episode.isTask ? Theme.success : episode.failed ? Theme.warning : Theme.textSecondary
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Text {
                                        id: episodeTitle
                                        Layout.fillWidth: true
                                        text: episode.title
                                        color: Theme.textPrimary
                                        font.pixelSize: Ui.sp(Theme.bodySize)
                                        elide: Text.ElideRight
                                        HoverHandler { id: episodeTitleHover }
                                        AppToolTip { text: episode.path; visible: episodeTitleHover.hovered }
                                    }
                                    Text {
                                        id: episodeLine
                                        Layout.fillWidth: true
                                        text: episode.isTask ? episode.progressText
                                              : [Ui.ago(episode.at), episode.size > 0 ? downloadPage.sizeText(episode.size) : ""]
                                                .filter(part => part !== "").join("  ·  ")
                                        color: episode.failed ? Theme.warning : Theme.textMuted
                                        font.pixelSize: Ui.sp(Theme.compactSize)
                                        elide: Text.ElideRight
                                        HoverHandler { id: lineHover }
                                        AppToolTip { text: episodeLine.text; visible: lineHover.hovered && episodeLine.truncated }
                                    }
                                }
                                Text {
                                    visible: episode.isTask && episode.stats !== "" && !episode.stopped
                                    text: episode.stats
                                    color: Theme.textSecondary
                                    font.pixelSize: Ui.sp(Theme.compactSize)
                                }

                                IconButton {
                                    visible: episode.isTask && episode.failed && episode.detail !== ""
                                    iconName: "details"
                                    iconSize: 17
                                    tip: qsTr("Details")
                                    implicitWidth: Ui.sp(32)
                                    implicitHeight: Ui.sp(32)
                                    boxRadius: 8
                                    onClicked: {
                                        detailsDialog.name = episode.title
                                        detailsDialog.reason = episode.progressText
                                        detailsDialog.detail = episode.detail
                                        detailsDialog.open()
                                    }
                                }
                                IconButton {
                                    visible: episode.isTask
                                    iconName: episode.failed ? "refresh-cw" : episode.stopped ? "play" : "pause"
                                    iconColor: episode.failed ? Theme.warning : Theme.textSecondary
                                    tip: episode.failed ? qsTr("Retry") : episode.stopped ? qsTr("Resume") : qsTr("Pause")
                                    implicitWidth: Ui.sp(32)
                                    implicitHeight: Ui.sp(32)
                                    boxRadius: 8
                                    onClicked: {
                                        if (episode.stopped) App.downloads.resumeTask(episode.taskRow)
                                        else App.downloads.pauseTask(episode.taskRow)
                                    }
                                }
                                IconButton {
                                    visible: episode.isTask
                                    iconName: "x"
                                    destructive: true
                                    tip: qsTr("Cancel")
                                    implicitWidth: Ui.sp(32)
                                    implicitHeight: Ui.sp(32)
                                    boxRadius: 8
                                    onClicked: {
                                        const held = App.downloads.holdCancel(episode.taskRow)
                                        Ui.offerUndo(qsTr("Cancelled “%1”.").arg(episode.title),
                                                     () => App.downloads.undoCancel(held), () => App.downloads.settleCancel(held))
                                    }
                                }

                                AppButton {
                                    visible: !episode.isTask
                                    text: qsTr("Play")
                                    fontSize: Theme.compactSize
                                    radius: 8
                                    Layout.preferredHeight: Ui.sp(30)
                                    onClicked: {
                                        // each part encoded: a "#" in a show's name would end the path
                                        App.playlist.openUrl("file:///" + episode.path.split("/").map(encodeURIComponent).join("/"), true)
                                        Ui.gotoPage(MainWindow.Player)
                                    }
                                }
                                AppButton {
                                    visible: !episode.isTask && App.playlist.playingList !== episode.folder
                                             && (episode.showLink === "" || App.playlist.playingList !== episode.showLink)
                                    text: qsTr("Queue")
                                    secondary: true
                                    fontSize: Theme.compactSize
                                    radius: 8
                                    Layout.preferredHeight: Ui.sp(30)
                                    onClicked: App.playlist.queueDownloaded(episode.path)
                                }
                                IconButton {
                                    visible: !episode.isTask
                                    iconName: "folder"
                                    iconSize: 17
                                    tip: qsTr("Show in folder")
                                    implicitWidth: Ui.sp(32)
                                    implicitHeight: Ui.sp(32)
                                    boxRadius: 8
                                    onClicked: App.downloads.showInFolder(episode.path)
                                }
                                IconButton {
                                    visible: !episode.isTask
                                    iconName: "trash-2"
                                    iconSize: 17
                                    destructive: true
                                    tip: qsTr("Delete")
                                    implicitWidth: Ui.sp(32)
                                    implicitHeight: Ui.sp(32)
                                    boxRadius: 8
                                    onClicked: deleteDialog.ask(episode.folder, episode.path, episode.title, 1, episode.size, 0)
                                }
                            }

                            ProgressBar {
                                id: taskProgress
                                visible: episode.isTask
                                Layout.fillWidth: true
                                Layout.leftMargin: Ui.sp(27)
                                Layout.rightMargin: 6
                                from: 0
                                to: 100
                                value: episode.progress
                                indeterminate: episode.progress === 0 && !episode.stopped

                                background: Rectangle {
                                    implicitHeight: 5
                                    radius: 3
                                    color: Theme.border
                                }

                                contentItem: Item {
                                    implicitHeight: 5
                                    Rectangle {
                                        width: taskProgress.visualPosition * parent.width
                                        height: parent.height
                                        radius: 3
                                        color: episode.failed ? Theme.warning : episode.stopped ? Theme.textMuted : Theme.accent
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
