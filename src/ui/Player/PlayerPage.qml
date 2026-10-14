pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import App

Item {
    id: mpvPage
    focus: true
    property int   volumeStep: 5
    property bool  isDoubleSpeed: false

    property real sbAnim: playlistBar.shown ? 1 : 0
    Behavior on sbAnim { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }

    readonly property real sidebarW: compact ? 0 : Math.round(width * 0.27 * sbAnim / 4) * 4

    property bool compact: false
    onCompactChanged: if (compact) { playerPanel.close(); isDoubleSpeed = false }
    property real videoRadius: 0

    MpvPlayer {
        id: mpv
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        width: mpvPage.width - mpvPage.sidebarW
        radius: mpvPage.videoRadius
        onPlayNext: if (!nextCountdown.cancelled) { mpv.holdPage(); App.playlist.stepItem(1) }
        onStreamFailed: mpv.holdPage()

        function holdPage() {
            if (Ui.page !== MainWindow.Player) Ui.miniHold = true
        }
        Component.onCompleted: {
            Ui.mpv = mpv
            mpv.setMpvProperty("sub-scale", App.settings.subFontSize / 40.0)
            mpv.setSubPos(App.settings.subPos)
            mpv.applySubtitleStyle()
            mpv.applyPicture()
        }

        function copyVideoLink() {
            let url = mpv.currentVideoUrl().toString()
            App.copyToClipboard(url)
            mpv.showText(qsTr("Copied %1").arg(url))
        }

        function peek(time) {
            controlBar.shown = true
            inactivityTimer.interval = time || 2000
            inactivityTimer.restart()
        }

        Connections {
            target: mpv
            function onIsLoadingChanged() {
                if (!mpv.isLoading && !Ui.miniHold) Ui.gotoPage(MainWindow.Player)
            }
        }

        DropArea {
            anchors.fill: parent
            onEntered: (drag) => drag.accept(Qt.LinkAction)
            onDropped: (drop) => {
                for (let i = 0; i < drop.urls.length; i++)
                    App.playlist.openUrl(drop.urls[i], false)
            }
        }

        MouseArea {
            id: mouseArea
            property point pressPos: Qt.point(0, 0)
            property bool  pipDragging: false
            property point pressWindow: Qt.point(0, 0)

            anchors {
                top: mpv.top
                bottom: controlBar.visible ? controlBar.top : mpv.bottom
                left: mpv.left; right: mpv.right
            }
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Ui.pipMode || controlBar.visible ? Qt.ArrowCursor : Qt.BlankCursor

            onPressed: (mouse) => {
                mpvPage.forceActiveFocus()
                if (mouse.button === Qt.LeftButton && Ui.pipMode) {
                    pressPos = mapToGlobal(mouse.x, mouse.y)
                    pressWindow = Qt.point(Ui.root.x, Ui.root.y)
                    pipDragging = false
                }
            }
            onDoubleClicked: Ui.pipMode ? Ui.togglePip() : Ui.toggleFullscreen()
            onClicked: (mouse) => { if (mouse.button === Qt.RightButton) contextMenu.popup() }
            onPositionChanged: (mouse) => {
                mpv.peek()
                // Moved by hand: startSystemMove ends in Windows' own loop with no release, so no edge snap or saved place.
                if (Ui.pipMode && mouseArea.pressed && (mouse.buttons & Qt.LeftButton)) {
                    const at = mapToGlobal(mouse.x, mouse.y)
                    if (!pipDragging && Math.hypot(at.x - pressPos.x, at.y - pressPos.y) < 4) return
                    pipDragging = true
                    Ui.root.x = pressWindow.x + at.x - pressPos.x
                    Ui.root.y = pressWindow.y + at.y - pressPos.y
                }
            }
            onCanceled: {
                if (Ui.pipMode && pipDragging) Ui.root.settlePip()
                pipDragging = false
            }
            property real wheelRest: 0
            onWheel: (wheel) => {
                wheelRest += wheel.angleDelta.y
                const steps = Math.trunc(wheelRest / 120)
                if (steps === 0) return
                wheelRest -= steps * 120
                mpv.volume += steps * mpvPage.volumeStep
            }
            onReleased: (mouse) => {
                if (playerPanel.opened && mouse.button === Qt.LeftButton) {
                    playerPanel.close()
                    return
                }
                if (Ui.pipMode && pipDragging) {
                    Ui.root.settlePip()
                    pipDragging = false
                } else if (!pipDragging && mouse.button === Qt.LeftButton) {
                    mpv.togglePlayPause()
                }
            }

            Timer {
                id: inactivityTimer
                interval: 2000
                onTriggered: {
                    if (mpv.visible && !mouseArea.pressed && !controlBar.hovered)
                        controlBar.shown = false
                }
            }
        }

        EmptyState {
            anchors.centerIn: parent
            visible: mpv.state === MpvPlayer.Stopped && !mpv.isLoading && !App.playlist.isLoading
            icon: "tv"
            title: qsTr("Nothing playing")
            readonly property var fileKeys: App.shortcuts.bindings.openFile ?? []
            readonly property var folderKeys: App.shortcuts.bindings.openFolder ?? []
            hint: qsTr("Pick an episode from a show, or open a file%1 or a folder%2.")
                  .arg(fileKeys.length > 0 ? " (" + fileKeys[0] + ")" : "")
                  .arg(folderKeys.length > 0 ? " (" + folderKeys[0] + ")" : "")
            actionText: qsTr("Open file")
            titleColor: Theme.onOverlay
            hintColor: Theme.onOverlayDim
            onActionTriggered: fileDialog.open()
        }

        AppSpinner {
            anchors.centerIn: parent
            visible: (App.playlist.isLoading || mpv.isLoading || mpv.buffering) && !mpvPage.compact
            running: visible
            width: Ui.sp(76)
            height: width
            color: Qt.lighter(Theme.accent, 1.3)
        }

        Rectangle {
            id: nextCountdown
            readonly property int remaining: mpv.nextIn
            readonly property bool due: remaining > 0 && remaining <= 10
            readonly property string nextName: due ? App.playlist.nextItemName() : ""
            property bool cancelled: false

            function playNow() {
                mpv.holdPage()
                App.playlist.stepItem(1)
            }

            visible: due && nextName !== "" && !cancelled && !mpvPage.compact
            anchors { right: parent.right; bottom: parent.bottom; rightMargin: 24; bottomMargin: 88 }
            width: countdownColumn.implicitWidth + 32
            height: countdownColumn.implicitHeight + 24
            radius: 12
            color: Theme.overlayScrim
            border.color: Theme.overlayLine
            border.width: 1

            Connections {
                target: mpv
                function onIsLoadingChanged() { if (mpv.isLoading) nextCountdown.cancelled = false }
            }

            ColumnLayout {
                id: countdownColumn
                anchors.centerIn: parent
                spacing: 8
                Text {
                    text: qsTr("Up next in %1 s").arg(nextCountdown.remaining)
                    color: Theme.onOverlayDim
                    font.pixelSize: Ui.sp(Theme.compactSize)
                }
                Text {
                    Layout.maximumWidth: Ui.sp(360)
                    text: nextCountdown.nextName
                    color: Theme.onOverlay
                    font.pixelSize: Ui.sp(Theme.bodySize)
                    font.bold: true
                    elide: Text.ElideRight
                }
                RowLayout {
                    spacing: 8
                    AppButton { text: qsTr("Play now"); onClicked: nextCountdown.playNow() }
                    AppButton { text: qsTr("Cancel"); secondary: true; onClicked: nextCountdown.cancelled = true }
                }
            }
        }

        PlayerPanel {
            id: playerPanel
            anchors.centerIn: parent
            player: mpv
            width:  Math.min(860, Math.max(460, parent.width * 0.62))
            height: Math.min(560, Math.max(300, parent.height * 0.65))
            visible: false
            onClosed: mpvPage.forceActiveFocus()

            function toggle(tabId) {
                if (Ui.pipMode) Ui.togglePip()
                const target = playerPanel.shownPage(tabId ? tabId : playerPanel.activeTabId)
                if (playerPanel.opened && playerPanel.activeTabId === target) {
                    playerPanel.close()
                } else {
                    playerPanel.activeTabId = target
                    if (!playerPanel.opened) { playerPanel.open(); playlistBar.shown = false }
                }
                mpvPage.forceActiveFocus()
            }
        }

        ControlBar {
            id: controlBar
            anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
            z: mpv.z + 1
            player: mpv
            property bool shown: false
            visible: shown && !mpvPage.compact && !Ui.pipMode
            height: 64

            onPlaylistRequested: mpvPage.showPlaylist()
            onPanelRequested: (tab) => playerPanel.toggle(tab)
            onOpenFileRequested: folderDialog.open()
        }

        component SkipPill: Rectangle {
            id: pill
            property alias label: pillLabel.text
            signal activated()

            anchors {
                right: parent.right
                bottom: parent.bottom
                rightMargin: 28
                bottomMargin: controlBar.visible ? controlBar.height + 18 : 28
            }
            width: pillLabel.implicitWidth + 36
            height: 44
            radius: 22
            color: pillArea.containsMouse ? Theme.accent : Theme.overlayScrim
            border.color: Theme.accent
            border.width: 1.5

            Text {
                id: pillLabel
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: Ui.sp(Theme.bodySize)
                font.weight: Font.Medium
            }
            MouseArea {
                id: pillArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: pill.activated()
            }
        }

        Item {
            anchors.fill: parent
            z: controlBar.z + 1

            SkipPill {
                label: qsTr("Skip opening")
                visible: mpv.hasOP && !App.settings.aniskipAuto
                         && mpv.time >= mpv.aniOPStart
                         && mpv.time < mpv.aniOPStart + mpv.aniOPLength
                onActivated: { mpv.seek(mpv.aniOPStart + mpv.aniOPLength); mpv.peek() }
            }

            SkipPill {
                label: qsTr("Next episode ▶")
                visible: mpv.hasED && !App.settings.aniskipAuto && mpv.duration > 0
                         && mpv.time >= mpv.duration - mpv.aniEDLength
                onActivated: App.playlist.stepItem(1)
            }
        }

        AppMenu {
            id: contextMenu
            modal: true

            AppMenu {
                title: qsTr("Open"); modal: false
                Action { text: qsTr("Open file"); property string hint: Ui.keyFor("openFile"); onTriggered: fileDialog.open() }
                Action { text: qsTr("Open folder"); property string hint: Ui.keyFor("openFolder"); onTriggered: folderDialog.open() }
            }
            Action { text: qsTr("Paste link"); property string hint: Ui.keyFor("openClipboard"); onTriggered: App.playlist.openUrl("", true) }
            Action { text: qsTr("Copy link"); property string hint: Ui.keyFor("copyLink"); onTriggered: mpv.copyVideoLink() }
            Action { text: qsTr("Screenshot"); property string hint: Ui.keyFor("screenshot"); onTriggered: mpv.screenshot() }
            Action { text: qsTr("Copy frame"); property string hint: Ui.keyFor("copyFrame"); onTriggered: mpv.copyFrame() }
            Action { text: qsTr("A-B loop"); property string hint: Ui.keyFor("abLoop"); onTriggered: mpv.cycleABLoop() }
            Action { text: qsTr("Reload"); property string hint: Ui.keyFor("reload"); onTriggered: App.playlist.reload() }
        }
    }

    PlaylistSidebar {
        id: playlistBar
        property bool shown: false
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
        width: mpvPage.sidebarW
        clip: true
        visible: mpvPage.sbAnim > 0.001 && !mpvPage.compact
        onHideRequested: playlistBar.toggle()
        onEditorReleased: mpvPage.forceActiveFocus()
        function toggle() {
            if (Ui.pipMode) Ui.togglePip()
            shown = !shown
            mpvPage.forceActiveFocus()
        }
    }

    function openDanmakuTab() {
        if (!playerPanel.opened) playerPanel.toggle("danmaku")
        else { playerPanel.activeTabId = "danmaku"; playerPanel.syncActiveTab() }
    }

    function showPlaylist() {
        if (playerPanel.opened) playerPanel.close()
        playlistBar.toggle()
    }

    Connections {
        target: Ui
        function onPipModeChanged() {
            if (Ui.pipMode) { playlistBar.shown = false; playerPanel.close() }
        }
    }

    FolderDialog {
        id: folderDialog
        currentFolder: "file:///" + App.settings.downloadDir
        onAccepted: { App.playlist.openUrl(selectedFolder, true); mpvPage.forceActiveFocus() }
    }
    FileDialog {
        id: fileDialog
        currentFolder: "file:///" + App.settings.downloadDir
        fileMode: FileDialog.OpenFile
        nameFilters: [
            "All files (*)",
            "Video and Audio files (*.mp4 *.mkv *.avi *.mp3 *.flac *.wav *.ogg *.webm *.m3u8 *.mov *.ts *.m2ts *.m4v *.flv *.wmv *.m4a *.opus)",
            "Subtitle files (*.srt *.ass *.ssa *.vtt *.sub *.idx)"
        ]
        onAccepted: { App.playlist.openUrl(selectedFile, true); mpvPage.forceActiveFocus() }
    }

    onVisibleChanged: {
        if (visible) playlistBar.scrollToIndex(playlistBar.treeView.selectionModel.currentIndex)
        else isDoubleSpeed = false
    }

    onIsDoubleSpeedChanged: mpv.setSpeedBoost(isDoubleSpeed)
    onActiveFocusChanged: if (!activeFocus) isDoubleSpeed = false
    readonly property bool windowActive: Window.active
    onWindowActiveChanged: if (!windowActive) isDoubleSpeed = false
    function increaseSpeed(increment) {
        mpv.setSpeed(mpv.speed + increment)
    }

    Keys.enabled: true
    Keys.onReleased: (event) => {
        if (event.isAutoRepeat) return
        if (event.key === Qt.Key_Shift) isDoubleSpeed = false
    }
    Keys.onPressed: (event) => {
        if (!visible) return
        if (event.key === Qt.Key_Shift) { isDoubleSpeed = true; return }
        if (event.key === Qt.Key_Control) return
        const ctrl = event.modifiers & Qt.ControlModifier
        if (!ctrl && (event.modifiers & Qt.AltModifier)) return
        const shortcuts = App.shortcuts
        const action = shortcuts.actionFor(shortcuts.keyText(event.key, event.modifiers))
                       || shortcuts.actionFor(shortcuts.keyText(event.key, event.modifiers & ~Qt.ShiftModifier))
        if (!runAction(action, event))
            mpv.sendKeyPress(ctrl ? ctrlKeyName(event) : event.text)
        // An unaccepted Tab would walk focus off the page onto a button, and the keys would stop reaching the player.
        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) event.accepted = true
    }

    // mpv's name for a Ctrl chord: on Windows a Ctrl+letter's text is a control character and a Ctrl+digit's is empty.
    function ctrlKeyName(event) {
        const key = event.key
        if (key >= Qt.Key_0 && key <= Qt.Key_9) return "CTRL+" + String.fromCharCode(key)
        if (key >= Qt.Key_A && key <= Qt.Key_Z) {
            const letter = String.fromCharCode(key)
            return "CTRL+" + ((event.modifiers & Qt.ShiftModifier) ? letter : letter.toLowerCase())
        }
        return "CTRL+" + event.text
    }

    function runAction(action, event) {
        switch (action) {
        case "panel":           playerPanel.toggle(); break
        case "mute":            mpv.muted = !mpv.muted; break
        // Relative: mpv adds up the steps a held key queues; absolute ones from the whole-second time would fight each other.
        case "seekBack":        mpv.seek(-5, false, !!event && event.isAutoRepeat); break
        case "seekForward":     mpv.seek(5, false, !!event && event.isAutoRepeat); break
        case "skipBack":        mpv.seek(-90, false, !!event && event.isAutoRepeat); break
        case "skipForward":     mpv.seek(90, false, !!event && event.isAutoRepeat); break
        case "title":           App.playlist.showCurrentItemName(); break
        case "peek":            mpv.peek(); break
        case "screenshot":      mpv.screenshot(); break
        case "copyFrame":       mpv.copyFrame(); break
        case "abLoop":          mpv.cycleABLoop(); break
        case "openFile":        fileDialog.open(); break
        case "openFolder":      folderDialog.open(); break
        case "openDownloads":   Qt.openUrlExternally("file:///" + App.settings.downloadDir); break
        case "openClipboard":   App.playlist.openUrl("", true); break
        case "playlist":        mpvPage.showPlaylist(); break
        case "volumeUp":        mpv.volume += volumeStep; break
        case "volumeDown":      mpv.volume -= volumeStep; break
        case "playPause":       mpv.togglePlayPause(); break
        case "nextEpisode":     App.playlist.stepItem(1); break
        case "previousEpisode": App.playlist.stepItem(-1); break
        case "nextPlaylist":    App.playlist.stepPlaylist(1); break
        case "previousPlaylist": App.playlist.stepPlaylist(-1); break
        case "faster":          increaseSpeed(0.1); break
        case "slower":          increaseSpeed(-0.1); break
        case "doubleSpeed":     mpv.setSpeed(mpv.speed > 1.0 ? 1.0 : 2.0); break
        case "reload":          App.playlist.reload(); break
        case "copyLink":        mpv.copyVideoLink(); break
        case "pip":             playlistBar.shown = false; Ui.togglePip(); break
        case "leave":
            // The page holds focus while the panel is open, so CloseOnEscape never fires.
            if (playerPanel.opened) playerPanel.close()
            else if (Ui.pipMode) Ui.togglePip()
            else if (Ui.fullscreen) Ui.toggleFullscreen()
            break
        case "subtitles":
            mpv.subVisible = !mpv.subVisible
            mpv.showText(mpv.subVisible ? qsTr("Subtitles on") : qsTr("Subtitles off"))
            break
        case "fullscreen":
            if (event.isAutoRepeat) break
            if (Ui.pipMode) Ui.togglePip()
            else Ui.toggleFullscreen()
            break
        default: return false
        }
        return true
    }
}
