pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import App

Item {
    id: mpvPage
    focus: true
    readonly property int volumeStep: App.settings.volumeStep
    property bool  isDoubleSpeed: false

    property real sbAnim: playlistBar.shown ? 1 : 0
    Behavior on sbAnim { NumberAnimation { duration: Theme.dur(320); easing.type: Easing.OutCubic } }

    readonly property real sidebarW: compact ? 0 : Math.round(width * 0.27 * sbAnim / 4) * 4

    property bool compact: false
    readonly property bool canPrevious: controlBar.canPrevious
    readonly property bool canNext: controlBar.canNext
    onCompactChanged: if (compact) { playerPanel.close(); endBoost() }
    property real videoRadius: 0

    MpvPlayer {
        id: mpv
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        width: mpvPage.width - mpvPage.sidebarW
        radius: mpvPage.videoRadius
        onPlayNext: {
            if (nextCountdown.cancelled) return
            if (App.settings.afterEpisode === 0) {
                mpvPage.step(1)
                return
            }
            const next = App.playlist.nextItemName()
            if (next === "") return
            mpv.pause()
            nextCountdown.ended = App.settings.afterEpisode === 1
            if (nextCountdown.ended && mpvPage.compact) mpv.showText(qsTr("Up next: %1").arg(next), "skip-forward")
        }
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
            App.copyToClipboard(url, false)
            mpv.showText(qsTr("Copied %1").arg(url), "link")
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
            property bool  doubleClicking: false

            anchors {
                top: mpv.top
                bottom: controlBar.visible ? controlBar.top : mpv.bottom
                left: mpv.left; right: mpv.right
            }
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Ui.pipMode || controlBar.visible ? Qt.ArrowCursor : Qt.BlankCursor

            onPressed: mpvPage.forceActiveFocus()
            onDoubleClicked: (mouse) => {
                if (mouse.button !== Qt.LeftButton) return
                clickToggle.stop()
                doubleClicking = true
                if (Ui.pipMode) Ui.togglePip()
                else Ui.toggleFullscreen()
            }
            onClicked: (mouse) => { if (mouse.button === Qt.RightButton) contextMenu.popup() }
            onPositionChanged: mpv.peek()
            property real wheelRest: 0
            onWheel: (wheel) => {
                wheelRest += wheel.angleDelta.y
                const steps = Math.trunc(wheelRest / 120)
                if (steps === 0) return
                wheelRest -= steps * 120
                mpv.stepVolume(steps * mpvPage.volumeStep)
            }
            onReleased: (mouse) => {
                const afterDoubleClick = doubleClicking
                doubleClicking = false
                if (playerPanel.opened && mouse.button === Qt.LeftButton) {
                    playerPanel.close()
                    return
                }
                if (mouse.button === Qt.LeftButton && !afterDoubleClick) clickToggle.restart()
            }

            // a double-click toggles fullscreen only, so the single click waits to see if one follows
            Timer {
                id: clickToggle
                interval: Math.min(Application.styleHints.mouseDoubleClickInterval, 250)
                onTriggered: mpv.togglePlayPause()
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
            visible: (App.playlist.isLoading || mpv.isLoading || mpv.buffering) && !mpvPage.compact && !Ui.pipMode
            running: visible
            width: Ui.sp(76)
            height: width
            color: Theme.onOverlayAccent
        }

        Rectangle {
            id: nextCountdown
            readonly property int remaining: mpv.nextIn
            readonly property bool autoplay: App.settings.afterEpisode === 0
            property bool ended: false
            readonly property bool due: autoplay ? remaining > 0 && remaining <= 10 : ended
            readonly property string nextName: due ? App.playlist.nextItemName() : ""
            property bool cancelled: false
            property string upNextLink: ""
            onDueChanged: upNextLink = due ? App.playlist.nextItemLink() : ""
            property bool overlaySurface: true
            readonly property bool slim: Ui.pipMode

            function playNow() {
                mpvPage.step(1)
            }

            visible: due && nextName !== "" && !cancelled && !mpvPage.compact
            anchors { right: parent.right; bottom: parent.bottom
                      rightMargin: slim ? Ui.sp(10) : 24; bottomMargin: slim ? Ui.sp(48) : 88 }
            width: countdownColumn.implicitWidth + (slim ? Ui.sp(20) : 32)
            height: countdownColumn.implicitHeight + (slim ? Ui.sp(14) : 24)
            radius: slim ? 10 : 12
            color: Theme.overlayScrim
            border.color: Theme.overlayLine
            border.width: 1

            Connections {
                target: App.playlist
                function onRowsRemoved() {
                    if (nextCountdown.due && App.playlist.nextItemLink() !== nextCountdown.upNextLink)
                        nextCountdown.cancelled = true
                }
            }

            Connections {
                target: mpv
                function onIsLoadingChanged() {
                    if (!mpv.isLoading) return
                    nextCountdown.cancelled = false
                    nextCountdown.ended = false
                }
                function onMpvStateChanged() { if (mpv.state === MpvPlayer.Playing) nextCountdown.ended = false }
            }

            ColumnLayout {
                id: countdownColumn
                anchors.centerIn: parent
                spacing: nextCountdown.slim ? 4 : 8
                Text {
                    text: nextCountdown.autoplay ? qsTr("Up next in %1 s").arg(nextCountdown.remaining) : qsTr("Up next")
                    color: Theme.onOverlayDim
                    font.pixelSize: Ui.sp(nextCountdown.slim ? Theme.captionSize : Theme.compactSize)
                }
                Text {
                    Layout.maximumWidth: nextCountdown.slim ? Math.max(0, mpv.width - Ui.sp(40)) : Ui.sp(360)
                    text: nextCountdown.nextName
                    color: Theme.onOverlay
                    font.pixelSize: Ui.sp(nextCountdown.slim ? Theme.captionSize : Theme.bodySize)
                    font.bold: true
                    elide: Text.ElideRight
                }
                RowLayout {
                    spacing: nextCountdown.slim ? 6 : 8
                    AppButton { text: qsTr("Play now"); compact: nextCountdown.slim; onClicked: nextCountdown.playNow() }
                    AppButton {
                        text: qsTr("Cancel"); secondary: true; compact: nextCountdown.slim
                        onClicked: nextCountdown.cancelled = true
                    }
                }
            }
        }

        PlayerPanel {
            id: playerPanel
            anchors.centerIn: parent
            player: mpv
            width:  Math.min(Ui.sp(748), Math.max(Ui.sp(400), parent.width * 0.62))
            height: Math.min(Ui.sp(487), Math.max(Ui.sp(261), parent.height * 0.65))
            visible: false
            onClosed: mpvPage.forceActiveFocus()
            onEditorReleased: mpvPage.forceActiveFocus()

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
            height: implicitHeight

            onPlaylistRequested: mpvPage.showPlaylist()
            onPanelRequested: (tab) => playerPanel.toggle(tab)
            onOpenFileRequested: fileDialog.open()
        }

        component SkipPill: Rectangle {
            id: pill
            property alias label: pillLabel.text
            property string iconName: ""
            readonly property bool slim: mpvPage.compact || Ui.pipMode
            signal activated()

            anchors {
                right: parent.right
                bottom: parent.bottom
                rightMargin: slim ? Ui.sp(10) : Ui.sp(24)
                bottomMargin: controlBar.visible ? controlBar.height + Ui.sp(16) : Ui.pipMode ? Ui.sp(48) : Ui.sp(24)
            }
            width: pillRow.implicitWidth + (slim ? Ui.sp(20) : Ui.sp(31))
            height: slim ? Ui.sp(28) : Ui.sp(38)
            radius: height / 2
            color: pillArea.containsMouse ? Theme.accent : Theme.overlayScrim
            border.color: Theme.accent
            border.width: 1.5

            Row {
                id: pillRow
                anchors.centerIn: parent
                spacing: Ui.sp(5)
                Text {
                    id: pillLabel
                    anchors.verticalCenter: parent.verticalCenter
                    color: pillArea.containsMouse ? Theme.inkOn(Theme.accent) : Theme.onOverlay
                    font.pixelSize: Ui.sp(pill.slim ? Theme.captionSize : Theme.bodySize)
                    font.weight: Font.Medium
                }
                AppIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: pill.iconName !== ""
                    name: pill.iconName
                    size: pill.slim ? Ui.sp(12) : Ui.sp(14)
                    color: pillLabel.color
                }
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
                label: qsTr("Next episode")
                iconName: "skip-forward"
                visible: mpv.hasED && !App.settings.aniskipAuto && mpv.duration > 0 && mpvPage.canNext
                         && mpv.time >= mpv.duration - mpv.aniEDLength
                         && !nextCountdown.visible
                onActivated: mpvPage.step(1)
            }
        }

        // the show, the episode, its times and the source: on Tab, on Mark watched and as an episode starts
        Rectangle {
            id: card
            readonly property bool small: mpvPage.compact || Ui.pipMode
            readonly property int pad: small ? Ui.sp(10) : Ui.sp(16)
            property bool shown: false
            property bool watched: false
            property var info: ({})
            property date now: new Date()
            property bool introPending: false
            property string introKey: ""
            readonly property string episode: {
                const parts = card.watched ? [qsTr("Watched")] : []
                if (info.season > 0) parts.push(qsTr("Season %1").arg(info.season))
                const n = Math.round((info.number ?? -1) * 10) / 10
                if (n > -1)
                    parts.push(info.count > 0 && info.count >= n ? qsTr("Episode %1 of %2").arg(n).arg(info.count)
                                                                 : qsTr("Episode %1").arg(n))
                if (info.name) parts.push(info.name)
                return parts.join(" · ")
            }
            readonly property string source: {
                const parts = [info.provider, info.server].filter(part => !!part)
                if (mpv.videoHeight > 0) parts.push(mpv.videoHeight + "p")
                if (Math.abs(mpv.speed - 1) > 0.001) parts.push(Math.round(mpv.speed * 100) / 100 + "×")
                const id = App.settings.picture.upscaler || ""
                const upscaler = id !== "" ? mpv.upscalers.find(option => option.id === id) : undefined
                if (upscaler) parts.push(upscaler.name)
                return parts.join(" · ")
            }
            readonly property date endsAt: new Date(now.getTime() + Math.max(0, mpv.duration - mpv.time) * 1000
                                                    / (mpv.speed * (mpvPage.isDoubleSpeed ? 2 : 1)))

            function show(watched) {
                info = App.playlist.nowPlaying()
                if (!info.title) return
                card.watched = watched
                now = new Date()
                shown = true
                cardHold.restart()
            }
            function hide() {
                shown = false
                cardHold.stop()
            }
            // once per episode, so a reload or a server switch does not bring it back
            function intro() {
                if (!introPending || mpv.state !== MpvPlayer.Playing) return
                introPending = false
                const next = App.playlist.nowPlaying()
                const key = [next.title, next.number, next.name].join("\n")
                if (key === introKey) return
                introKey = key
                if (!small) show(false)
            }

            // clears the title the mini player and picture-in-picture draw on hover
            x: small ? Ui.sp(10) : Ui.sp(18)
            y: small ? Ui.sp(40) : Ui.sp(18)
            z: controlBar.z + 1
            width: Math.min(small ? Ui.sp(300) : Ui.sp(560), Math.max(0, mpv.width - x * 2))
            height: cardColumn.implicitHeight + pad * 2
            radius: 12
            color: Theme.overlayScrim
            border.color: Theme.overlayFillActive
            border.width: 1
            opacity: shown ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: Theme.dur(180); easing.type: Easing.OutCubic } }

            Timer { id: cardHold; interval: 4000; onTriggered: card.shown = false }
            Timer { interval: 1000; repeat: true; running: card.visible; onTriggered: card.now = new Date() }
            Connections {
                target: App.playlist
                function onNowPlayingRequested() { card.show(true) }
            }
            Connections {
                target: mpv
                function onFileLoaded() {
                    card.introPending = true
                    // the playlist moves to the new episode in its own fileLoaded handler
                    Qt.callLater(card.intro)
                }
                function onMpvStateChanged() {
                    if (mpv.state === MpvPlayer.Stopped) card.hide()
                    else card.intro()
                }
            }

            ColumnLayout {
                id: cardColumn
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: card.pad }
                spacing: Ui.sp(3)
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Ui.sp(14)
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Ui.sp(2)
                        Text {
                            Layout.fillWidth: true
                            visible: text !== "" && !card.small
                            text: card.info.path ?? ""
                            color: Theme.onOverlayDim
                            font.pixelSize: Ui.sp(Theme.captionSize)
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.fillWidth: true
                            text: card.info.title ?? ""
                            color: Theme.onOverlay
                            font.pixelSize: Ui.sp(card.small ? Theme.compactSize : Theme.headingSize)
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                    }
                    Text {
                        Layout.alignment: Qt.AlignTop
                        text: Qt.formatTime(card.now, "HH:mm")
                        color: Theme.onOverlay
                        font.pixelSize: Ui.sp(card.small ? Theme.compactSize : Theme.headingSize)
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: card.episode !== ""
                    spacing: Ui.sp(6)
                    AppIcon {
                        visible: card.watched
                        name: "check"
                        size: Ui.sp(card.small ? 12 : 16)
                        color: Theme.onOverlayAccent
                    }
                    Text {
                        Layout.fillWidth: true
                        text: card.episode
                        color: Theme.onOverlayMuted
                        font.pixelSize: Ui.sp(card.small ? Theme.captionSize : Theme.compactSize)
                        elide: Text.ElideRight
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: Ui.sp(card.small ? 6 : 10)
                    visible: mpv.duration > 0
                    implicitHeight: 4
                    radius: 2
                    color: Theme.overlayFillActive
                    Rectangle {
                        width: parent.width * (mpv.duration > 0 ? Math.min(1, mpv.time / mpv.duration) : 0)
                        height: parent.height
                        radius: parent.radius
                        color: Theme.accent
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: mpv.duration > 0
                    TimeCode {
                        text: Ui.timeText(mpv.time) + " / " + Ui.timeText(mpv.duration)
                        color: Theme.onOverlay
                        pixelSize: Ui.sp(card.small ? Theme.captionSize : Theme.metadataSize)
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: qsTr("Ends at %1").arg(Qt.formatTime(card.endsAt, "HH:mm"))
                        color: Theme.onOverlayMuted
                        font.pixelSize: Ui.sp(card.small ? Theme.captionSize : Theme.metadataSize)
                    }
                }
                Text {
                    Layout.fillWidth: true
                    Layout.topMargin: Ui.sp(2)
                    visible: text !== "" && !card.small
                    text: card.source
                    color: Theme.onOverlayDim
                    font.pixelSize: Ui.sp(Theme.captionSize)
                    elide: Text.ElideRight
                }
            }
        }

        Rectangle {
            id: osd
            readonly property bool small: mpvPage.compact || Ui.pipMode
            readonly property int pad: small ? Ui.sp(10) : Ui.sp(14)
            readonly property int vpad: small ? Ui.sp(6) : Ui.sp(9)
            property string icon: ""
            property real level: -1
            readonly property real iconSpace: icon !== "" ? osdIcon.width + Ui.sp(8) : 0
            readonly property real barSpace: level >= 0 ? Ui.sp(10) : 0
            x: card.x
            y: card.visible ? card.y + card.height + Ui.sp(8) : card.y
            z: controlBar.z + 1
            width: Math.min(Math.max(osdText.implicitWidth + iconSpace, level >= 0 ? Ui.sp(small ? 120 : 180) : 0) + pad * 2,
                            Math.max(0, mpv.width - x * 2))
            height: osdText.implicitHeight + barSpace + vpad * 2
            radius: small ? 10 : 12
            color: Theme.overlayScrim
            border.color: Theme.overlayFillActive
            border.width: 1
            opacity: 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: Theme.dur(150); easing.type: Easing.OutCubic } }

            AppIcon {
                id: osdIcon
                anchors { left: parent.left; leftMargin: osd.pad; verticalCenter: osdText.verticalCenter }
                visible: osd.icon !== ""
                name: osd.icon
                size: Ui.sp(osd.small ? 14 : 19)
                color: Theme.onOverlay
            }
            Text {
                id: osdText
                anchors { left: parent.left; right: parent.right; top: parent.top
                          leftMargin: osd.pad + osd.iconSpace; rightMargin: osd.pad; topMargin: osd.vpad }
                color: Theme.onOverlay
                font.pixelSize: Ui.sp(osd.small ? Theme.captionSize : Theme.bodySize)
                font.weight: Font.Medium
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                maximumLineCount: 4
                elide: Text.ElideRight
            }
            // volume and speed: a tick marks 100% and 1x, and volume past 100% turns amber
            Rectangle {
                visible: osd.level >= 0
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom
                          leftMargin: osd.pad; rightMargin: osd.pad; bottomMargin: osd.vpad }
                height: Ui.sp(4)
                radius: height / 2
                color: Theme.overlayFillActive
                Rectangle {
                    width: parent.width * Math.min(1, osd.level)
                    height: parent.height
                    radius: parent.radius
                    color: osd.level > 0.5 && osd.icon.startsWith("volume") ? Theme.onOverlayWarning : Theme.onOverlayAccent
                }
                Rectangle {
                    x: Math.round(parent.width / 2) - 1
                    anchors.verticalCenter: parent.verticalCenter
                    width: 2
                    height: parent.height + 4
                    radius: 1
                    color: Theme.onOverlayMuted
                }
            }
            Timer {
                id: osdHold
                onTriggered: osd.opacity = 0
            }
            Connections {
                target: mpv
                function onOsdMessage(text, icon, level) {
                    osdText.text = text
                    osd.icon = icon
                    osd.level = level
                    osdHold.interval = Math.min(4000, 1200 + Math.max(0, text.length - 24) * 40)
                    osdHold.restart()
                    osd.opacity = 1
                }
            }
        }

        Rectangle {
            id: boost
            readonly property bool small: mpvPage.compact || Ui.pipMode
            visible: mpvPage.isDoubleSpeed && mpv.state !== MpvPlayer.Stopped
            anchors.horizontalCenter: parent.horizontalCenter
            y: card.visible && card.x + card.width + Ui.sp(8) > x ? card.y + card.height + Ui.sp(8) : card.y
            z: controlBar.z + 1
            width: boostRow.implicitWidth + height
            height: small ? Ui.sp(28) : Ui.sp(40)
            radius: height / 2
            color: Theme.overlayScrim
            border.color: Theme.overlayFillActive
            border.width: 1
            Row {
                id: boostRow
                anchors.centerIn: parent
                spacing: Ui.sp(6)
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: Math.round(mpv.speed * 200) / 100 + "×"
                    color: Theme.onOverlay
                    font.pixelSize: Ui.sp(boost.small ? Theme.captionSize : Theme.bodySize)
                    font.weight: Font.DemiBold
                }
                AppIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "fast-forward"
                    size: Ui.sp(boost.small ? 13 : 18)
                    color: Theme.onOverlayAccent
                }
            }
        }

        AppMenu {
            id: contextMenu
            modal: true
            // a window of its own in picture-in-picture, so it can reach past the small window's edges
            popupType: Ui.pipMode ? Popup.Window : Popup.Item

            AppMenu {
                title: qsTr("Open"); modal: false; iconName: "folder-open"
                popupType: contextMenu.popupType
                Action { text: qsTr("Open file"); icon.name: "file"; property string hint: Ui.keyFor("openFile"); onTriggered: fileDialog.open() }
                Action { text: qsTr("Open folder"); icon.name: "folder"; property string hint: Ui.keyFor("openFolder"); onTriggered: folderDialog.open() }
            }
            Action { text: qsTr("Paste link"); icon.name: "clipboard"; property string hint: Ui.keyFor("openClipboard"); onTriggered: App.playlist.openUrl("", true) }
            Action { text: qsTr("Copy link"); icon.name: "link"; property string hint: Ui.keyFor("copyLink"); onTriggered: mpv.copyVideoLink() }
            TextMenu.Separator { shown: controlBar.canPrevious || controlBar.canNext }
            Action {
                text: qsTr("Previous episode"); icon.name: "skip-back"
                property bool shown: controlBar.canPrevious
                property string hint: Ui.keyFor("previousEpisode")
                onTriggered: mpvPage.runAction("previousEpisode", null)
            }
            Action {
                text: qsTr("Next episode"); icon.name: "skip-forward"
                property bool shown: controlBar.canNext
                property string hint: Ui.keyFor("nextEpisode")
                onTriggered: mpvPage.runAction("nextEpisode", null)
            }
            TextMenu.Separator {}
            Action { text: qsTr("Screenshot"); icon.name: "camera"; property string hint: Ui.keyFor("screenshot"); onTriggered: mpv.screenshot() }
            Action { text: qsTr("Copy frame"); icon.name: "image"; property string hint: Ui.keyFor("copyFrame"); onTriggered: mpv.copyFrame() }
            Action { text: qsTr("A-B loop"); icon.name: "repeat"; property string hint: Ui.keyFor("abLoop"); onTriggered: mpv.cycleABLoop() }
            TextMenu.Separator {}
            Action {
                text: mpv.subVisible ? qsTr("Hide subtitles") : qsTr("Show subtitles")
                icon.name: mpv.subVisible ? "captions" : "captions-off"
                property string hint: Ui.keyFor("subtitles")
                onTriggered: mpvPage.runAction("subtitles", null)
            }
            Action {
                text: Ui.fullscreen ? qsTr("Exit fullscreen") : qsTr("Fullscreen")
                icon.name: Ui.fullscreen ? "minimize" : "maximize"
                property string hint: Ui.keyFor("fullscreen")
                onTriggered: mpvPage.runAction("fullscreen", null)
            }
            Action {
                text: Ui.pipMode ? qsTr("Exit picture-in-picture") : qsTr("Picture-in-picture")
                icon.name: "picture-in-picture-2"
                property string hint: Ui.keyFor("pip")
                onTriggered: mpvPage.runAction("pip", null)
            }
            Action {
                text: qsTr("Playlist"); icon.name: "list-video"
                property bool shown: !mpvPage.compact
                property string hint: Ui.keyFor("playlist")
                onTriggered: mpvPage.runAction("playlist", null)
            }
            Action {
                text: qsTr("Settings"); icon.name: "settings"
                property bool shown: !mpvPage.compact
                property string hint: Ui.keyFor("panel")
                onTriggered: mpvPage.runAction("panel", null)
            }
            TextMenu.Separator {}
            Action { text: qsTr("Reload"); icon.name: "refresh-cw"; property string hint: Ui.keyFor("reload"); onTriggered: mpvPage.runAction("reload", null) }
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
            qsTr("All files (*)"),
            qsTr("Video and audio files (*.mp4 *.mkv *.avi *.mp3 *.flac *.wav *.ogg *.webm *.m3u8 *.mov *.ts *.m2ts *.m4v *.flv *.wmv *.m4a *.opus)"),
            qsTr("Subtitle files (*.srt *.ass *.ssa *.vtt *.sub *.idx)")
        ]
        onAccepted: { App.playlist.openUrl(selectedFile, true); mpvPage.forceActiveFocus() }
    }

    onVisibleChanged: {
        if (visible) playlistBar.scrollToIndex(playlistBar.treeView.selectionModel.currentIndex)
        else endBoost()
    }

    onIsDoubleSpeedChanged: mpv.setSpeedBoost(isDoubleSpeed)
    onActiveFocusChanged: if (!activeFocus) endBoost()
    readonly property bool windowActive: Window.active
    onWindowActiveChanged: if (!windowActive) endBoost()
    function endBoost() { isDoubleSpeed = false }
    function step(offset) {
        if (!App.playlist.canStep(offset)) return
        mpv.holdPage()
        App.playlist.stepItem(offset)
    }
    function holdWhile(action) {
        mpv.holdPage()
        action()
        if (!mpv.isLoading && !App.playlist.isLoading) Ui.miniHold = false
    }
    function increaseSpeed(increment) {
        mpv.setSpeed(Math.round((mpv.speed + increment) * 100) / 100)
    }

    Keys.enabled: true
    Keys.onReleased: (event) => {
        if (event.isAutoRepeat) return
        if (event.key === Qt.Key_Shift) endBoost()
    }
    Keys.onPressed: (event) => {
        if (!visible) return
        if (event.key === Qt.Key_Shift) {
            if (!event.isAutoRepeat) isDoubleSpeed = (event.modifiers & ~(Qt.ShiftModifier | Qt.KeypadModifier)) === 0
            return
        }
        const shortcuts = App.shortcuts
        const action = shortcuts.actionFor(shortcuts.keyText(event.key, event.modifiers))
                       || shortcuts.actionFor(shortcuts.keyText(event.key, event.modifiers & ~Qt.ShiftModifier))
        // any other key with it ("+", "*", Ctrl+Shift+S) ends the boost; Shift held for a seek keeps it
        if (!/^(seek|skip)(Back|Forward)$/.test(action)) endBoost()
        if (event.key === Qt.Key_Control) return
        const ctrl = event.modifiers & Qt.ControlModifier
        if (!ctrl && (event.modifiers & Qt.AltModifier)) return
        if (!runAction(action, event))
            mpv.sendKeyPress(ctrl ? ctrlKeyName(event) : event.text)
        // an unaccepted Tab walks focus off the page
        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) event.accepted = true
    }

    // mpv's name for a Ctrl chord: a Ctrl+letter's text is a control character
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
        case "seekBack":        mpv.seek(-App.settings.seekShort, false, !!event && event.isAutoRepeat); break
        case "seekForward":     mpv.seek(App.settings.seekShort, false, !!event && event.isAutoRepeat); break
        case "skipBack":        mpv.seek(-App.settings.seekLong, false, !!event && event.isAutoRepeat); break
        case "skipForward":     mpv.seek(App.settings.seekLong, false, !!event && event.isAutoRepeat); break
        case "title":           if (card.shown) card.hide(); else card.show(false); break
        case "peek":            mpv.peek(); break
        case "screenshot":      mpv.screenshot(); break
        case "copyFrame":       mpv.copyFrame(); break
        case "abLoop":          mpv.cycleABLoop(); break
        case "openFile":        fileDialog.open(); break
        case "openFolder":      folderDialog.open(); break
        case "openDownloads":   App.downloads.openFolder(App.settings.downloadDir); break
        case "openClipboard":   App.playlist.openUrl("", true); break
        case "playlist":        mpvPage.showPlaylist(); break
        case "volumeUp":        mpv.stepVolume(volumeStep); break
        case "volumeDown":      mpv.stepVolume(-volumeStep); break
        case "playPause":       if (!(controlBar.idle && App.playlist.resumeStopped())) mpv.togglePlayPause(); break
        case "nextEpisode":     step(1); break
        case "previousEpisode": step(-1); break
        case "nextPlaylist":    holdWhile(() => App.playlist.stepPlaylist(1)); break
        case "previousPlaylist": holdWhile(() => App.playlist.stepPlaylist(-1)); break
        case "nextChapter":     mpv.stepChapter(1); break
        case "previousChapter": mpv.stepChapter(-1); break
        case "subtitlesEarlier": mpv.subDelay -= 0.1; break
        case "subtitlesLater":  mpv.subDelay += 0.1; break
        case "audioEarlier":    mpv.audioDelay -= 0.05; break
        case "audioLater":      mpv.audioDelay += 0.05; break
        case "faster":          increaseSpeed(App.settings.speedStep / 100); break
        case "slower":          increaseSpeed(-App.settings.speedStep / 100); break
        case "doubleSpeed":     mpv.setSpeed(mpv.speed > 1.0 ? 1.0 : 2.0); break
        case "reload":          holdWhile(() => App.playlist.reload()); break
        case "copyLink":        mpv.copyVideoLink(); break
        case "pip":             playlistBar.shown = false; Ui.togglePip(); break
        case "leave":
            // the page holds focus while the panel is open, so CloseOnEscape never fires
            if (playerPanel.opened) playerPanel.close()
            else if (Ui.pipMode) Ui.togglePip()
            else if (Ui.fullscreen) Ui.toggleFullscreen()
            break
        case "subtitles":
            mpv.subVisible = !mpv.subVisible
            mpv.showText(mpv.subVisible ? qsTr("Subtitles on") : qsTr("Subtitles off"), mpv.subVisible ? "captions" : "captions-off")
            break
        case "fullscreen":
            if (event && event.isAutoRepeat) break
            if (Ui.pipMode) Ui.togglePip()
            else Ui.toggleFullscreen()
            break
        default: return false
        }
        return true
    }
}
