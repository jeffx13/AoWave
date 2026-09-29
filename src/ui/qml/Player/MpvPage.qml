pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import "../Components"
import App
import ".."

Item {
    id: mpvPage
    focus: true
    property int   volumeStep: 5
    property bool  isDoubleSpeed: false

    // 0 hidden, 1 fully open
    property real sbAnim: playlistBar.shown ? 1 : 0
    Behavior on sbAnim { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }

    // Snapped to four pixels: the player is an FBO, so every width rebuilds one.
    readonly property real sidebarW: compact ? 0 : Math.round(width * 0.27 * sbAnim / 4) * 4

    // The corner player while browsing: video only, no chrome.
    property bool compact: false
    onCompactChanged: if (compact) { playerPanel.close(); isDoubleSpeed = false }

    MpvPlayer {
        id: mpv
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        width: mpvPage.width - mpvPage.sidebarW
        // Straight on, unless the Up next card was cancelled.
        onPlayNext: if (!nextCountdown.cancelled) { mpv.holdPage(); App.playlist.stepItem(1) }
        onPlaybackError: { mpv.holdPage(); App.playlist.tryNextServer() }

        // A load the app starts by itself leaves you on the page you are browsing.
        function holdPage() {
            if (Globals.page !== AppShell.Player) Globals.miniHold = true
        }
        Component.onCompleted: {
            Globals.mpv = mpv
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
                if (!mpv.isLoading && !Globals.miniHold) Globals.gotoPage(AppShell.Player)
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
            cursorShape: Globals.pipMode || controlBar.visible ? Qt.ArrowCursor : Qt.BlankCursor

            onPressed: (mouse) => {
                mpvPage.forceActiveFocus()   // clicking the video restores the shortcuts
                if (mouse.button === Qt.LeftButton && Globals.pipMode) {
                    pressPos = mapToGlobal(mouse.x, mouse.y)
                    pressWindow = Qt.point(Globals.root.x, Globals.root.y)
                    pipDragging = false
                }
            }
            onDoubleClicked: Globals.pipMode ? Globals.togglePip() : Globals.toggleFullscreen()
            onClicked: (mouse) => { if (mouse.button === Qt.RightButton) contextMenu.popup() }
            onPositionChanged: (mouse) => {
                mpv.peek()
                // Moved by hand, not startSystemMove: that ends in Windows' own loop with no
                // release here, so the window could neither snap to an edge nor remember its place.
                if (Globals.pipMode && mouseArea.pressed && (mouse.buttons & Qt.LeftButton)) {
                    const at = mapToGlobal(mouse.x, mouse.y)
                    if (!pipDragging && Math.hypot(at.x - pressPos.x, at.y - pressPos.y) < 4) return
                    pipDragging = true
                    Globals.root.x = pressWindow.x + at.x - pressPos.x
                    Globals.root.y = pressWindow.y + at.y - pressPos.y
                }
            }
            onCanceled: {
                if (Globals.pipMode && pipDragging) Globals.root.settlePip()
                pipDragging = false
            }
            // The wheel over the picture is the volume, a step per notch; a touchpad's small deltas add up.
            property real wheelRest: 0
            onWheel: (wheel) => {
                wheelRest += wheel.angleDelta.y
                const steps = Math.trunc(wheelRest / 120)
                if (steps === 0) return
                wheelRest -= steps * 120
                mpv.volume += steps * mpvPage.volumeStep
            }
            onReleased: (mouse) => {
                // A click on the video dismisses the panel rather than pausing under it.
                if (playerPanel.opened && mouse.button === Qt.LeftButton) {
                    playerPanel.close()
                    return
                }
                if (Globals.pipMode && pipDragging) {
                    Globals.root.settlePip()
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

        // Over the video's last seconds, counting down with it: a pause holds it, a seek back hides
        // it. At the end the next episode starts, unless this was cancelled.
        Rectangle {
            id: nextCountdown
            readonly property int remaining: mpv.nextIn
            readonly property bool due: remaining > 0 && remaining <= 10
            readonly property string nextName: due ? App.playlist.nextItemName() : ""
            // For this video only.
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
                    text: qsTr("Up next in %1").arg(nextCountdown.remaining)
                    color: Theme.onOverlayDim
                    font.pixelSize: Globals.sp(Theme.compactSize)
                }
                Text {
                    Layout.maximumWidth: Globals.sp(360)
                    text: nextCountdown.nextName
                    color: Theme.onOverlay
                    font.pixelSize: Globals.sp(Theme.bodySize)
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
            // Room for five groups along the top.
            width:  Math.min(860, Math.max(460, parent.width * 0.62))
            height: Math.min(560, Math.max(300, parent.height * 0.65))
            visible: false
            onClosed: mpvPage.forceActiveFocus()

            // No tab given reopens the last active one.
            function toggle(tabId) {
                if (Globals.pipMode) Globals.togglePip()
                // Servers with one server shows Quality, so the same button closes it again.
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
            // Picture-in-picture has its own, smaller controls.
            visible: shown && !mpvPage.compact && !Globals.pipMode
            height: 64

            onPlaylistRequested: mpvPage.showPlaylist()
            onPanelRequested: (tab) => playerPanel.toggle(tab)
            onOpenFileRequested: folderDialog.open()
        }

        // Only one is active at a time, so they share the corner.
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
                font.pixelSize: Globals.sp(Theme.bodySize)
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
                label: qsTr("Skip Intro")
                visible: mpv.hasOP && !App.settings.aniskipAuto
                         && mpv.time >= mpv.aniOPStart
                         && mpv.time < mpv.aniOPStart + mpv.aniOPLength
                onActivated: { mpv.seek(mpv.aniOPStart + mpv.aniOPLength); mpv.peek() }
            }

            SkipPill {
                label: qsTr("Next Episode  ▶")
                visible: mpv.hasED && !App.settings.aniskipAuto && mpv.duration > 0
                         && mpv.time >= mpv.duration - mpv.aniEDLength
                onActivated: App.playlist.stepItem(1)
            }
        }

        AppMenu {
            id: contextMenu
            modal: true

            // Each key is the action's as bound now, shown at the right of its item.
            AppMenu {
                title: qsTr("Open"); modal: false
                Action { text: qsTr("Open file"); property string hint: Globals.keyFor("openFile"); onTriggered: fileDialog.open() }
                Action { text: qsTr("Open folder"); property string hint: Globals.keyFor("openFolder"); onTriggered: folderDialog.open() }
            }
            Action { text: qsTr("Paste link"); property string hint: Globals.keyFor("openClipboard"); onTriggered: App.playlist.openUrl("", true) }
            Action { text: qsTr("Copy link"); property string hint: Globals.keyFor("copyLink"); onTriggered: mpv.copyVideoLink() }
            Action { text: qsTr("Screenshot"); property string hint: Globals.keyFor("screenshot"); onTriggered: mpv.screenshot() }
            Action { text: qsTr("Copy frame"); property string hint: Globals.keyFor("copyFrame"); onTriggered: mpv.copyFrame() }
            Action { text: qsTr("A-B loop"); property string hint: Globals.keyFor("abLoop"); onTriggered: mpv.cycleABLoop() }
            Action { text: qsTr("Reload"); property string hint: Globals.keyFor("reload"); onTriggered: App.playlist.reload() }
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
            if (Globals.pipMode) Globals.togglePip()
            shown = !shown
            mpvPage.forceActiveFocus()
        }
    }

    // For the command palette, which knows the action but not the tab.
    function openDanmakuTab() {
        if (!playerPanel.opened) playerPanel.toggle("danmaku")
        else { playerPanel.activeTabId = "danmaku"; playerPanel.syncActiveTab() }
    }

    // Panel and sidebar both want the right-hand side.

    function showPlaylist() {
        if (playerPanel.opened) playerPanel.close()
        playlistBar.toggle()
    }

    Connections {
        target: Globals
        function onPipModeChanged() {
            if (Globals.pipMode) { playlistBar.shown = false; playerPanel.close() }
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
            "Video and Audio files (*.mp4 *.mkv *.avi *.mp3 *.flac *.wav *.ogg *.webm *.m3u8 *.ts *.mov)",
            "Subtitle files (*.srt *.ass *.ssa *.vtt *.sub *.idx)"
        ]
        onAccepted: { App.playlist.openUrl(selectedFile, true); mpvPage.forceActiveFocus() }
    }

    onVisibleChanged: {
        if (visible) playlistBar.scrollToIndex(playlistBar.treeView.currentIndex)
        else isDoubleSpeed = false
    }

    // Held, not toggled, and never saved: the player doubles its speed without changing it.
    // Whatever takes the keys away (another page, another window) also ends it, since the
    // Shift release then lands somewhere else.
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
        // Held for double speed rather than bound.
        if (event.key === Qt.Key_Shift) { isDoubleSpeed = true; return }
        if (event.key === Qt.Key_Control) return
        const ctrl = event.modifiers & Qt.ControlModifier
        if (!ctrl && (event.modifiers & Qt.AltModifier)) return
        // Shift also held for double speed: Shift+Right still seeks.
        const shortcuts = App.shortcuts
        const action = shortcuts.actionFor(shortcuts.keyText(event.key, event.modifiers))
                       || shortcuts.actionFor(shortcuts.keyText(event.key, event.modifiers & ~Qt.ShiftModifier))
        if (!runAction(action, event))
            mpv.sendKeyPress(ctrl ? "CTRL+" + event.text : event.text)
    }

    // False for anything not the player's, which then goes on to mpv's own bindings.
    function runAction(action, event) {
        switch (action) {
        case "panel":           playerPanel.toggle(); break
        case "mute":            mpv.muted = !mpv.muted; break
        case "seekBack":        mpv.seek(mpv.time - 5); break
        case "seekForward":     mpv.seek(mpv.time + 5); break
        case "skipBack":        mpv.seek(mpv.time - 90); break
        case "skipForward":     mpv.seek(mpv.time + 90); break
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
        case "pip":             playlistBar.shown = false; Globals.togglePip(); break
        case "leave":
            // The page holds focus while the panel is open, so CloseOnEscape never fires.
            if (playerPanel.opened) playerPanel.close()
            else if (Globals.pipMode) Globals.togglePip()
            else if (Globals.fullscreen) Globals.toggleFullscreen()
            break
        case "subtitles":
            mpv.subVisible = !mpv.subVisible
            mpv.showText(mpv.subVisible ? qsTr("Subtitles on") : qsTr("Subtitles off"))
            break
        case "fullscreen":
            if (event.isAutoRepeat) break
            if (Globals.pipMode) Globals.togglePip()
            else Globals.toggleFullscreen()
            break
        default: return false
        }
        return true
    }
}
