pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import App
import "."
import "./Player"
import "./Components"
import "./Pages"

ApplicationWindow {
    id: root

    // A preset saved on a desktop has to shrink for a laptop panel.
    function fitWidth(w)  { return Math.max(Globals.minWidth,  Math.min(w, Screen.desktopAvailableWidth)) }
    function fitHeight(h) { return Math.max(Globals.minHeight, Math.min(h, Screen.desktopAvailableHeight)) }

    // Picture-in-picture is far smaller than the app.
    minimumWidth: Globals.pipMode ? root.pipMinWidth : Math.min(Globals.minWidth, Screen.desktopAvailableWidth)
    minimumHeight: Globals.pipMode ? Math.round(root.pipMinWidth / 2.4) : Math.min(Globals.minHeight, Screen.desktopAvailableHeight)
    width: fitWidth(Globals.defaultWidth)
    height: fitHeight(Globals.defaultHeight)
    x: (Screen.desktopAvailableWidth - width) / 2
    y: (Screen.desktopAvailableHeight - height) / 2

    visible: true
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowMinimizeButtonHint
    onClosing: {
        App.playlist.saveProgress()
        // Only the plain window is worth remembering; savedW/savedH hold it while a mode is on.
        const w = chromeVisible ? width : savedW
        const h = chromeVisible ? height : savedH
        App.settings.windowWidth = Math.round(w)
        App.settings.windowHeight = Math.round(h)
    }

    // Applies straight away unless a mode owns the geometry.
    Connections {
        target: App.settings
        function onWindowSizeChanged() {
            const w = root.fitWidth(App.settings.windowWidth)
            const h = root.fitHeight(App.settings.windowHeight)
            root.savedW = w
            root.savedH = h
            if (!root.chromeVisible || Globals.maximised) return
            root.applyGeometry(Math.round((Screen.desktopAvailableWidth - w) / 2),
                               Math.round((Screen.desktopAvailableHeight - h) / 2), w, h)
        }
    }

    readonly property bool onPlayer: Globals.page === AppShell.Player

    color: onPlayer ? "#000000" : Theme.background

    Binding { target: Theme; property: "name";         value: App.settings.themeName }
    Binding { target: Theme; property: "reduceMotion"; value: App.settings.reduceMotion }
    Binding { target: Theme; property: "customAccent"; value: App.settings.accentColor }
    Binding { target: Globals; property: "uiScale";    value: App.settings.uiScale }

    // Sidebar order, which is what Ctrl+Tab follows.
    readonly property var navOrder: [
        AppShell.Search, AppShell.Info, AppShell.Library, AppShell.Player,
        AppShell.Download, AppShell.History, AppShell.Log, AppShell.Settings
    ]

    function stepPage(delta) {
        let at = navOrder.indexOf(Globals.page)
        if (at < 0) at = 0
        for (let i = 0; i < navOrder.length; ++i) {
            at = (at + delta + navOrder.length) % navOrder.length
            const page = navOrder[at]
            if (page === AppShell.Info && !App.show.exists) continue
            gotoPage(page)
            return
        }
    }

    property real savedX: x
    property real savedY: y
    property real savedW: width
    property real savedH: height

    function saveGeometry() {
        savedX = x
        savedY = y
        savedW = width
        savedH = height
    }

    function applyGeometry(nx, ny, nw, nh) {
        x = nx
        y = ny
        width = nw
        height = nh
    }

    function restoreGeometry() {
        applyGeometry(savedX, savedY, savedW, savedH)
    }

    function fillDesktop() {
        applyGeometry(Screen.virtualX, Screen.virtualY,
                      Screen.desktopAvailableWidth, Screen.desktopAvailableHeight)
    }

    function fillScreen() {
        applyGeometry(Screen.virtualX, Screen.virtualY, Screen.width, Screen.height)
        raise()
    }

    function toggleMaximised() {
        if (Globals.pipMode || Globals.fullscreen) return

        if (Globals.maximised) {
            Globals.maximised = false
            restoreGeometry()
        } else {
            saveGeometry()
            Globals.maximised = true
            fillDesktop()
        }
    }

    function toggleFullscreen() {
        if (Globals.pipMode) togglePip()

        if (Globals.fullscreen) {
            Globals.fullscreen = false
            if (Globals.maximised) fillDesktop()
            else                   restoreGeometry()
        } else {
            if (!Globals.maximised) saveGeometry()
            Globals.fullscreen = true
            fillScreen()
        }
    }

    // The page picture-in-picture was entered from, to go back to, and whether the app entered it
    // by itself (minimised, or hidden behind another window) rather than on request.
    property int pipReturnPage: AppShell.Player
    property bool pipAuto: false
    property real pipSince: 0
    readonly property real pipMinWidth: Globals.sp(240)
    readonly property real pipMargin: 16
    readonly property real pipAspect: Globals.mpv && Globals.mpv.videoAspect > 0
                                      ? Math.max(0.75, Math.min(2.4, Globals.mpv.videoAspect)) : 16 / 9

    function togglePip(auto) {
        if (Globals.pipMode) {
            pipSettle.stop()
            savePipRect()
            App.setPipWindow(root, false)
            Globals.pipMode = false
            pipAuto = false

            if (Globals.fullscreen)      fillScreen()
            else if (Globals.maximised)  fillDesktop()
            else                         restoreGeometry()
            if (pipReturnPage !== AppShell.Player) gotoPage(pipReturnPage)
        } else {
            if (!Globals.maximised && !Globals.fullscreen) saveGeometry()
            Globals.fullscreen = false
            Globals.maximised = false
            pipReturnPage = Globals.page
            pipAuto = auto === true
            pipSince = Date.now()
            // The video, not whatever page it was playing under.
            gotoPage(AppShell.Player)
            Globals.pipMode = true
            const r = pipRect()
            applyGeometry(r.x, r.y, r.width, r.height)
            // Natively, not through flags: changing flags re-shows the window, taking focus from
            // the app just switched to.
            App.setPipWindow(root, true)
            // Started from here (its button, key or Minimise), the focus goes back to what was used
            // before, as Alt+Tab would; started by being covered, it is there already.
            if (root.active) App.focusPreviousWindow(root)
        }
    }

    // Where picture-in-picture was last left and how wide, in the video's shape; the first time, a
    // third of the screen wide in the bottom-right corner. It keeps to the corner it was nearest.
    function pipRect() {
        const areaW = Screen.desktopAvailableWidth, areaH = Screen.desktopAvailableHeight
        const saved = String(App.settings.value("pip/rect", "")).split(",").map(Number)
        const known = saved.length === 4 && saved.every(n => isFinite(n)) && saved[2] > 0
        const w = Math.round(Math.max(pipMinWidth, Math.min(known ? saved[2] : areaW * 0.33,
                                                             areaW * 0.8, areaH * 0.8 * pipAspect)) / 4) * 4
        const h = Math.round(w / pipAspect)
        let nx = areaW - w - pipMargin, ny = areaH - h - pipMargin
        if (known) {
            nx = saved[0] + saved[2] / 2 > areaW / 2 ? saved[0] + saved[2] - w : saved[0]
            ny = saved[1] + saved[3] / 2 > areaH / 2 ? saved[1] + saved[3] - h : saved[1]
        }
        return Qt.rect(Math.max(0, Math.min(nx, areaW - w)), Math.max(0, Math.min(ny, areaH - h)), w, h)
    }
    function savePipRect() {
        if (Globals.pipMode)
            App.settings.setValue("pip/rect", [x, y, width, height].map(Math.round).join(","))
    }
    // Resized about the corner nearest the screen's, which stays put.
    function resizePip(newWidth) {
        const areaW = Screen.desktopAvailableWidth, areaH = Screen.desktopAvailableHeight
        const w = Math.round(Math.max(pipMinWidth, Math.min(newWidth, areaW * 0.8, areaH * 0.8 * pipAspect)) / 4) * 4
        const h = Math.round(w / pipAspect)
        const right = x + width / 2 > areaW / 2, bottom = y + height / 2 > areaH / 2
        applyGeometry(right ? x + width - w : x, bottom ? y + height - h : y, w, h)
    }
    // After a drag: back on screen, and against an edge it was dropped near.
    function settlePip() {
        const areaW = Screen.desktopAvailableWidth, areaH = Screen.desktopAvailableHeight
        const snap = Globals.sp(40)
        let nx = Math.max(0, Math.min(x, areaW - width))
        let ny = Math.max(0, Math.min(y, areaH - height))
        if (nx < snap) nx = pipMargin
        else if (areaW - width - nx < snap) nx = areaW - width - pipMargin
        if (ny < snap) ny = pipMargin
        else if (areaH - height - ny < snap) ny = areaH - height - pipMargin
        pipSettleX.to = nx
        pipSettleY.to = ny
        pipSettle.restart()
    }
    ParallelAnimation {
        id: pipSettle
        NumberAnimation { id: pipSettleX; target: root; property: "x"; duration: 180; easing.type: Easing.OutCubic }
        NumberAnimation { id: pipSettleY; target: root; property: "y"; duration: 180; easing.type: Easing.OutCubic }
        onFinished: root.savePipRect()
    }
    // A new episode in another shape keeps the window free of bars.
    onPipAspectChanged: if (Globals.pipMode) resizePip(width)

    readonly property bool chromeVisible: !(Globals.pipMode || Globals.fullscreen)

    // A video playing out of sight carries on in picture-in-picture, unless Settings says
    // otherwise: minimised, or hidden behind another window. The boss key minimises on purpose,
    // so its cover screen rules this out.
    readonly property bool autoPipReady: App.settings.autoPip && !Globals.pipMode && !bossScreen.visible
                                         && !!Globals.mpv && Globals.mpv.state === MpvPlayer.Playing
    // Minimised some other way (taskbar, Win+Down): come back as picture-in-picture, leaving the
    // focus where it went.
    onVisibilityChanged: (visibility) => {
        if (visibility !== Window.Minimized || !autoPipReady) return
        App.restoreWithoutFocus(root)
        togglePip(true)
    }
    // Only while another app has the focus, so a window of this one is never what hides it; and
    // only for a window on screen, since a hidden one reads as wholly covered.
    Timer {
        interval: 700
        repeat: true
        running: root.autoPipReady && root.visible && !root.active && root.visibility !== Window.Minimized
        onTriggered: if (App.coveredFraction(root) > 0.8) root.togglePip(true)
    }
    // Back to the app by Alt+Tab or the taskbar ends a picture-in-picture it entered by itself; a
    // click on the small window (the pointer is over it) is using it instead.
    onActiveChanged: {
        if (active && Globals.pipMode && pipAuto && Date.now() - pipSince > 1000 && !pipOverlay.hovered)
            togglePip()
    }

    Component.onCompleted: {
        Globals.root = root

        // Application's constructor already started one. The old guard raced: QFutureWatcher
        // delivers finished through the event loop, so page one got fetched twice.
        if (App.playlist.playAt(0)) {
            root.requestActivate()
            Globals.page = AppShell.Player
            history = [AppShell.Player]
        }

        deferredStartupTimer.start()
    }

    property var history: [AppShell.Search]
    property int historyIndex: 0

    property string nowPlayingTitle: ""
    property string nowPlayingEpisode: ""
    // Something is loaded, playing, paused or on its way: the title bar and the mini player
    // offer it only then.
    readonly property bool nowPlayingActive: !!Globals.mpv
        && (Globals.mpv.state !== MpvPlayer.Stopped || Globals.mpv.isLoading || App.playlist.isLoading)
    Connections {
        target: App.playlist
        function onCurrentItemChanged() {
            root.nowPlayingTitle = App.playlist.currentShowName()
            root.nowPlayingEpisode = App.playlist.currentItemName().replace(/\s*\n\s*/g, "  ")
        }
    }

    function gotoPage(page, isHistory = false) {
        if (Globals.fullscreen || Globals.page === page) return
        if (page === AppShell.Info && !App.show.exists) return

        if (page === AppShell.Player)
            Globals.mpv.peek(2000)

        Globals.page = page
        if (!isHistory) {
            history.splice(historyIndex + 1)
            history.push(page)
            historyIndex = history.length - 1
        }
    }

    // gotoPage() bails while fullscreen.
    function goHistory(delta) {
        if (Globals.fullscreen) return
        const at = historyIndex + delta
        if (at < 0 || at >= history.length) return
        historyIndex = at
        gotoPage(history[at], true)
    }

    Connections {
        target: Globals
        // Deferred: the Loader activates this same turn, and a disabled item cannot take focus.
        function onPageChanged() { Qt.callLater(root.focusCurrentPage) }
    }

    function focusCurrentPage() {
        if (root.onPlayer) { mpvPage.forceActiveFocus(); return }
        for (let i = 0; i < pageStack.children.length; i++) {
            const loader = pageStack.children[i] as Loader
            const page = loader ? loader.item as Item : null
            if (page && page.visible) { page.forceActiveFocus(); return }
        }
    }

    TitleBar {
        id: titleBar
        height: root.chromeVisible ? 44 : 0
        z: 6
        anchors { top: parent.top; left: parent.left; right: parent.right }

        canGoBack: root.historyIndex > 0
        canGoForward: root.historyIndex + 1 < root.history.length
        nowPlayingTitle: root.nowPlayingActive ? root.nowPlayingTitle : ""
        nowPlayingEpisode: root.nowPlayingActive ? root.nowPlayingEpisode : ""

        onMoveRequested: root.startSystemMove()
        onMaximiseToggled: root.toggleMaximised()
        onMinimiseRequested: root.autoPipReady ? root.togglePip(true) : root.showMinimized()
        onCloseRequested: root.close()
        onHistoryStep: (delta) => root.goHistory(delta)
        onPlayerRequested: root.gotoPage(AppShell.Player)
    }

    Item {
        id: contentArea
        anchors {
            top: titleBar.bottom
            left: parent.left
            leftMargin: root.chromeVisible ? sideBar.width : 0
            right: parent.right
            bottom: parent.bottom
        }
    }

    SideBar {
        id: sideBar
        z: 5
        chromeVisible: root.chromeVisible
        anchors {
            left: parent.left
            top: titleBar.bottom
            bottom: parent.bottom
        }
        onPageRequested: (page) => root.gotoPage(page)
    }

    Timer {
        id: deferredStartupTimer
        interval: 3000
        repeat: false
        onTriggered: App.library.fetchUnwatchedEpisodes(App.library.libraryType)
    }

    // New episodes surface while the app sits open. 0 is the Watching list.
    Timer {
        interval: 3 * 60 * 60 * 1000
        repeat: true
        running: true
        onTriggered: App.library.fetchUnwatchedEpisodes(0, true)
    }

    Connections {
        target: App.library
        function onNewEpisodesFound(shows) {
            if (Globals.page !== AppShell.Library) Globals.newEpisodeShows += shows.length
        }
    }

    Item {
        z: 1
        anchors.fill: contentArea
        opacity: root.onPlayer ? 0 : 1
        visible: opacity > 0.001
        Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

        // One texture during the cross-fade.
        layer.enabled: opacity > 0.001 && opacity < 0.999

        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: Theme.background }
                GradientStop { position: 1.0; color: Theme.bgBottom }
            }
        }

        Rectangle {
            id: updateBanner
            visible: App.updateVersion !== ""
            anchors { top: parent.top; left: parent.left; right: parent.right }
            height: visible ? Globals.sp(44) : 0
            color: Theme.accentSoft

            RowLayout {
                anchors { fill: parent; leftMargin: 16; rightMargin: 8 }
                spacing: 10
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Version %1 is available.").arg(App.updateVersion)
                    color: Theme.textPrimary
                    font.pixelSize: Globals.sp(Theme.bodySize)
                    elide: Text.ElideRight
                }
                AppButton {
                    text: qsTr("See what's new")
                    fontSize: 16
                    radius: 8
                    Layout.preferredHeight: Globals.sp(32)
                    onClicked: Qt.openUrlExternally(App.updateUrl)
                }
                IconButton {
                    iconName: "x"
                    iconSize: 16
                    implicitWidth: Globals.sp(32)
                    implicitHeight: Globals.sp(32)
                    tip: qsTr("Not this version")
                    onClicked: App.dismissUpdate()
                }
            }
        }

        StackLayout {
            id: pageStack
            anchors { fill: parent; topMargin: updateBanner.height }

            readonly property var pageOrder: [
                AppShell.Search, AppShell.Info, AppShell.Library,
                AppShell.Download, AppShell.Log, AppShell.Settings, AppShell.History
            ]
            currentIndex: Math.max(0, pageOrder.indexOf(Globals.page))

            // The settings page, loaded if it has not been, for the command palette.
            function settingsPage() {
                const loader = pageLoaders.itemAt(pageOrder.indexOf(AppShell.Settings)) as Loader
                if (!loader) return null
                loader.active = true
                return loader.item as SettingsPage
            }

            Repeater {
                id: pageLoaders
                model: [
                    "Pages/ExplorerPage.qml",
                    "Pages/InfoPage.qml",
                    "Pages/LibraryPage.qml",
                    "Pages/DownloadPage.qml",
                    "Pages/LogPage.qml",
                    "Pages/SettingsPage.qml",
                    "Pages/HistoryPage.qml"
                ]
                delegate: Loader {
                    id: pageLoader
                    required property int index
                    required property string modelData
                    readonly property bool current: pageStack.currentIndex === index && !root.onPlayer
                    active: false
                    source: active ? modelData : ""
                    Component.onCompleted: if (current) active = true
                    function focusPage() {
                        const page = item as Item
                        if (page) page.forceActiveFocus()
                    }
                    onCurrentChanged: {
                        if (!current) return
                        arrival.restart()
                        if (!active) active = true
                        else focusPage()
                    }
                    onLoaded: if (current) focusPage()

                    transform: Translate { id: rise }
                    ParallelAnimation {
                        id: arrival
                        NumberAnimation { target: pageLoader; property: "opacity"; from: 0; to: 1
                                          duration: Theme.reduceMotion ? 0 : 180; easing.type: Easing.OutCubic }
                        NumberAnimation { target: rise; property: "y"; from: Theme.reduceMotion ? 0 : 10; to: 0
                                          duration: Theme.reduceMotion ? 0 : 220; easing.type: Easing.OutCubic }
                    }
                }
            }
        }
    }

    LoadingScreen {
        z: 100
        anchors.fill: contentArea
        // Only pages that can start a load.
        readonly property bool cancellablePage: Globals.page === AppShell.Search
                                             || Globals.page === AppShell.Info
                                             || Globals.page === AppShell.Library
                                             || Globals.page === AppShell.History
        loading: {
            switch (Globals.page) {
            case AppShell.Search:  return App.explorer.isLoading || App.show.isLoading
            case AppShell.Info:    return App.playlist.isLoading || App.show.isLoading
            case AppShell.Library:
            case AppShell.History: return App.show.isLoading
            default: return false
            }
        }
        cancellable: cancellablePage
        onCancelled: {
            if (App.explorer.isLoading) App.explorer.cancel()
            if (App.show.isLoading)     App.show.cancel()
            if (App.playlist.isLoading) App.playlist.cancel()
        }
    }

    // Playback carries on while browsing, in a corner of the page. It can be dragged to another
    // corner or resized from its inner corner, and both are remembered.
    property bool miniDismissed: false
    onOnPlayerChanged: if (onPlayer) miniDismissed = false
    // Loading counts: an episode change passes through Stopped, and the video should not blink out.
    readonly property bool miniPlayer: App.settings.miniPlayer && !onPlayer && !Globals.pipMode && !miniDismissed
                                       && nowPlayingActive
    readonly property real miniMargin: Globals.sp(16)
    readonly property real miniRadius: Globals.sp(12)
    // The video's own shape, within reason; 16:9 until it is known.
    readonly property real miniAspect: Globals.mpv && Globals.mpv.videoAspect > 0
                                       ? Math.max(0.75, Math.min(2.4, Globals.mpv.videoAspect)) : 16 / 9
    // Four-pixel steps: the player is an FBO, rebuilt on every width.
    readonly property real miniWidth: Math.round(Math.max(Globals.sp(220),
                                          Math.min(App.settings.miniWidth, contentArea.width * 0.6,
                                                   contentArea.height * 0.6 * miniAspect)) / 4) * 4
    readonly property real miniHeight: Math.round(miniWidth / miniAspect)
    readonly property bool miniRight: App.settings.miniCorner % 2 === 1
    readonly property bool miniBottom: App.settings.miniCorner >= 2
    property bool miniDragging: false
    property bool miniSnapping: false
    property point miniDragPos
    readonly property real miniX: miniDragging ? miniDragPos.x
        : miniRight ? contentArea.x + contentArea.width - miniWidth - miniMargin : contentArea.x + miniMargin
    readonly property real miniY: miniDragging ? miniDragPos.y
        : miniBottom ? contentArea.y + contentArea.height - miniHeight - miniMargin : contentArea.y + miniMargin

    Timer { id: miniSnapTimer; interval: 260; onTriggered: root.miniSnapping = false }

    // Once the load a mini-player button began is over, loaded or failed.
    readonly property bool anyLoading: !!Globals.mpv && (Globals.mpv.isLoading || App.playlist.isLoading)
    onAnyLoadingChanged: if (!anyLoading) Qt.callLater(() => { Globals.miniHold = false })

    MpvPage {
        id: mpvPage
        // Or right-clicks elsewhere reach the player's context menu.
        enabled: root.onPlayer || Globals.pipMode
        compact: root.miniPlayer
        z: compact ? 2 : 0
        x: compact ? root.miniX : contentArea.x
        y: compact ? root.miniY : contentArea.y
        width: compact ? root.miniWidth : contentArea.width
        height: compact ? root.miniHeight : contentArea.height
        Behavior on x { enabled: root.miniSnapping; NumberAnimation { duration: 240; easing.type: Easing.OutCubic } }
        Behavior on y { enabled: root.miniSnapping; NumberAnimation { duration: 240; easing.type: Easing.OutCubic } }
        layer.enabled: compact
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: miniMask
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
        }
    }

    Item {
        id: miniMask
        visible: false
        layer.enabled: true
        width: mpvPage.width
        height: mpvPage.height
        Rectangle { anchors.fill: parent; radius: root.miniRadius; antialiasing: true }
    }

    RectangularShadow {
        visible: root.miniPlayer
        z: 1.5
        x: mpvPage.x
        y: mpvPage.y
        width: mpvPage.width
        height: mpvPage.height
        radius: root.miniRadius
        offset.y: Globals.sp(6)
        blur: Globals.sp(28)
        color: Qt.alpha("black", Theme.isLight ? 0.35 : 0.6)
    }

    Item {
        id: miniOverlay
        objectName: "miniOverlay"
        visible: root.miniPlayer
        z: 3
        x: mpvPage.x
        y: mpvPage.y
        width: mpvPage.width
        height: mpvPage.height
        readonly property bool active: miniHover.hovered || root.miniDragging || miniGrip.pressed
        readonly property bool busy: !!Globals.mpv && (Globals.mpv.isLoading || App.playlist.isLoading)

        HoverHandler { id: miniHover }

        // A click opens the player; a drag carries it to the nearest corner.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            cursorShape: root.miniDragging ? Qt.ClosedHandCursor : Qt.PointingHandCursor
            property point pressAt
            property point startAt
            onPressed: (mouse) => {
                pressAt = mapToItem(root.contentItem, mouse.x, mouse.y)
                startAt = Qt.point(mpvPage.x, mpvPage.y)
            }
            onPositionChanged: (mouse) => {
                const at = mapToItem(root.contentItem, mouse.x, mouse.y)
                if (!root.miniDragging && Math.hypot(at.x - pressAt.x, at.y - pressAt.y) < 6) return
                root.miniDragging = true
                root.miniDragPos = Qt.point(
                    Math.max(contentArea.x, Math.min(contentArea.x + contentArea.width - mpvPage.width,
                                                     startAt.x + at.x - pressAt.x)),
                    Math.max(contentArea.y, Math.min(contentArea.y + contentArea.height - mpvPage.height,
                                                     startAt.y + at.y - pressAt.y)))
            }
            onReleased: {
                if (!root.miniDragging) { root.gotoPage(AppShell.Player); return }
                const right = mpvPage.x + mpvPage.width / 2 > contentArea.x + contentArea.width / 2
                const bottom = mpvPage.y + mpvPage.height / 2 > contentArea.y + contentArea.height / 2
                root.miniSnapping = true
                App.settings.miniCorner = (bottom ? 2 : 0) + (right ? 1 : 0)
                root.miniDragging = false
                miniSnapTimer.restart()
            }
        }

        Rectangle {
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: Math.min(parent.height * 0.45, Globals.sp(52))
            radius: root.miniRadius
            opacity: miniOverlay.active ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 150 } }
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.alpha("black", 0.72) }
                GradientStop { position: 1.0; color: "transparent" }
            }
            // The resize grip shares the top row when the player sits at the bottom.
            Text {
                anchors { left: parent.left; right: miniWindowButtons.left; top: parent.top
                          leftMargin: root.miniBottom && root.miniRight ? Globals.sp(30) : Globals.sp(12)
                          rightMargin: 6; topMargin: Globals.sp(9) }
                text: root.nowPlayingEpisode !== "" ? root.nowPlayingTitle + "  ·  " + root.nowPlayingEpisode
                                                    : root.nowPlayingTitle
                color: Theme.onOverlay
                font.pixelSize: Globals.sp(Theme.compactSize)
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
            Row {
                id: miniWindowButtons
                anchors { right: parent.right; top: parent.top; margins: 4
                          rightMargin: root.miniBottom && !root.miniRight ? Globals.sp(28) : 4 }
                IconButton {
                    iconName: "maximize"
                    iconSize: 15
                    tip: qsTr("Back to the player")
                    iconColor: Theme.onOverlay
                    iconHoverColor: Theme.onOverlay
                    hoverColor: Theme.overlayFillHover
                    onClicked: root.gotoPage(AppShell.Player)
                }
                IconButton {
                    iconName: "x"
                    iconSize: 15
                    tip: qsTr("Close and pause")
                    iconColor: Theme.onOverlay
                    iconHoverColor: Theme.onOverlay
                    hoverColor: Theme.overlayFillHover
                    onClicked: {
                        if (Globals.mpv && Globals.mpv.state === MpvPlayer.Playing) Globals.mpv.pause()
                        root.miniDismissed = true
                    }
                }
            }
        }

        Row {
            anchors.centerIn: parent
            spacing: Globals.sp(10)
            opacity: miniOverlay.active && !miniOverlay.busy ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 150 } }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                iconName: "skip-back"
                iconSize: 18
                tip: qsTr("Previous episode")
                iconColor: Theme.onOverlay
                iconHoverColor: Theme.onOverlay
                hoverColor: Theme.overlayFillHover
                onClicked: { Globals.miniHold = true; App.playlist.stepItem(-1) }
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: Globals.sp(48)
                implicitHeight: Globals.sp(48)
                boxRadius: implicitWidth / 2
                iconName: Globals.mpv && Globals.mpv.state === MpvPlayer.Playing ? "pause" : "play"
                tip: Globals.mpv && Globals.mpv.state === MpvPlayer.Playing ? qsTr("Pause") : qsTr("Play")
                iconSize: 24
                active: true
                hoverColor: hovered ? Theme.accent : Qt.alpha(Theme.accent, 0.85)
                iconColor: Theme.onAccent
                iconHoverColor: Theme.onAccent
                onClicked: Globals.mpv.togglePlayPause()
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                iconName: "skip-forward"
                iconSize: 18
                tip: qsTr("Next episode")
                iconColor: Theme.onOverlay
                iconHoverColor: Theme.onOverlay
                hoverColor: Theme.overlayFillHover
                onClicked: { Globals.miniHold = true; App.playlist.stepItem(1) }
            }
        }

        AppSpinner {
            anchors.centerIn: parent
            visible: miniOverlay.busy
        }

        // Where the episode is; a click seeks there.
        Item {
            id: miniProgress
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom
                      leftMargin: root.miniRadius * 0.6; rightMargin: root.miniRadius * 0.6 }
            height: Globals.sp(14)
            readonly property real fraction: Globals.mpv && Globals.mpv.duration > 0
                                             ? Math.min(1, Globals.mpv.time / Globals.mpv.duration) : 0
            Rectangle {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom; bottomMargin: 3 }
                height: miniOverlay.active ? 4 : 3
                radius: height / 2
                color: Qt.alpha("white", 0.25)
                Rectangle {
                    width: parent.width * miniProgress.fraction
                    height: parent.height
                    radius: parent.radius
                    color: Theme.accent
                }
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: (mouse) => {
                    if (Globals.mpv && Globals.mpv.duration > 0)
                        Globals.mpv.seek(Globals.mpv.duration * Math.max(0, Math.min(1, mouse.x / width)))
                }
            }
        }

        // Resizes from the corner that faces the page.
        Item {
            width: Globals.sp(26)
            height: width
            x: root.miniRight ? 0 : parent.width - width
            y: root.miniBottom ? 0 : parent.height - height
            opacity: miniOverlay.active ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 150 } }
            AppIcon {
                anchors.centerIn: parent
                name: "arrow-up-left"
                size: 14
                color: Theme.onOverlay
                // Points out of the corner it sits in.
                rotation: root.miniRight ? (root.miniBottom ? 0 : 270) : (root.miniBottom ? 90 : 180)
            }
            MouseArea {
                id: miniGrip
                anchors.fill: parent
                cursorShape: root.miniRight === root.miniBottom ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
                property real startX
                property real startWidth
                onPressed: (mouse) => {
                    startX = mapToItem(root.contentItem, mouse.x, 0).x
                    startWidth = root.miniWidth
                }
                onPositionChanged: (mouse) => {
                    const dx = mapToItem(root.contentItem, mouse.x, 0).x - startX
                    App.settings.miniWidth = Math.round(startWidth + (root.miniRight ? -dx : dx))
                }
            }
        }
    }

    PipOverlay {
        id: pipOverlay
        visible: Globals.pipMode
        z: 3
        x: mpvPage.x
        y: mpvPage.y
        width: mpvPage.width
        height: mpvPage.height
    }

    QuickSearch {
        id: quickSearch
        anchors.fill: parent
        z: 200
        onSearched: (query) => Globals.searchProviders(query)
        // Loaded when the palette opens, so typing finds settings straight away.
        onOpenChanged: if (open) settingsSource = pageStack.settingsPage()
        property SettingsPage settingsSource: null
        findSettings: q => settingsSource ? settingsSource.find(q) : []
        onShowRequested: (link, provider, title) => App.openShowInfo(link, provider, title)
        onSettingRequested: label => {
            root.gotoPage(AppShell.Settings)
            const page = pageStack.settingsPage()
            if (page) page.reveal(label)
        }
        // The palette names the actions; main.qml performs them.
        onActionRequested: (id) => {
            switch (id) {
            case "markWatched":      App.playlist.markCurrentWatched(); break
            case "toggleDanmaku":
                App.settings.danmakuEnabled = !App.settings.danmakuEnabled
                if (Globals.mpv) Globals.mpv.showText(App.settings.danmakuEnabled
                                                      ? qsTr("Danmaku on") : qsTr("Danmaku off"))
                break
            case "danmakuSettings":
                root.gotoPage(AppShell.Player)
                mpvPage.openDanmakuTab()
                break
            case "migrate":          root.gotoPage(AppShell.Info); break
            case "openMpvConf":      Qt.openUrlExternally("file:///" + App.settings.dataDir + "/mpv/mpv.conf"); break
            case "openSettings":     Qt.openUrlExternally(App.settings.path); break
            case "gotoLogs":         root.gotoPage(AppShell.Log); break
            case "gotoDownloads":    root.gotoPage(AppShell.Download); break
            case "connectTracker":   root.gotoPage(AppShell.Settings); break
            case "reloadEpisode":    App.playlist.reload(); break
            }
        }
    }

    Toast {
        id: toast
    }
    function offerUndo(message, undo, commit) { toast.offerUndo(message, undo, commit) }

    Notifier {
        id: notifier
        onLogsRequested: root.gotoPage(AppShell.Log)
        onClosed: {
            if (mpvPage.visible) mpvPage.forceActiveFocus()
            else root.focusCurrentPage()
        }
    }

    // Above everything, so nothing of the app shows through. Loaded only while shown, at the
    // window's size: a hidden Image still decodes its 4K source at startup and keeps it.
    Image {
        id: bossScreen
        anchors.fill: parent
        z: 1000
        visible: false
        fillMode: Image.PreserveAspectCrop
        sourceSize: Qt.size(width, height)
        source: visible ? "qrc:/App/resources/images/periodic-table.jpg" : ""
    }

    Connections {
        target: AppShell
        function onErrorReported(message, header) { notifier.show(message, header) }
        function onInfoReported(message, header)  { toast.show(message, header) }
        function onNavigateRequested(page) {
            if (page === AppShell.Player && Globals.miniHold) return
            root.gotoPage(page)
        }
        function onHistoryStepRequested(delta)    { root.goHistory(delta) }
    }

    // A mouse's side buttons step through pages, as in a browser. Nothing else takes them, so
    // they reach this from anywhere in the window.
    Item {
        anchors.fill: parent
        z: -1
        TapHandler {
            acceptedButtons: Qt.BackButton | Qt.ForwardButton
            onTapped: (eventPoint, button) => root.goHistory(button === Qt.ForwardButton ? 1 : -1)
        }
    }

    // Keys come from App.shortcuts, where Settings rebinds them.
    readonly property var keys: Globals.recordingKey
        ? Object.keys(App.shortcuts.bindings).reduce((none, action) => { none[action] = []; return none }, {})
        : App.shortcuts.bindings
    Shortcut { sequences: root.keys.forward; onActivated: root.goHistory(1) }
    Shortcut { sequences: root.keys.back;    onActivated: root.goHistory(-1) }
    Shortcut { sequences: root.keys.nextPage;     enabled: !Globals.pipMode; onActivated: root.stepPage(1) }
    Shortcut { sequences: root.keys.previousPage; enabled: !Globals.pipMode; onActivated: root.stepPage(-1) }

    Shortcut { sequences: root.keys.close; onActivated: root.close() }
    Shortcut {
        sequences: root.keys.reload
        enabled: Globals.page === AppShell.Info && App.show.exists
        onActivated: App.reloadShow()
    }
    Shortcut { sequences: root.keys.quickSearch; onActivated: quickSearch.open = !quickSearch.open }
    Shortcut { sequences: root.keys.pageSearch;    onActivated: root.gotoPage(AppShell.Search) }
    Shortcut { sequences: root.keys.pageInfo;      onActivated: root.gotoPage(AppShell.Info) }
    Shortcut { sequences: root.keys.pageLibrary;   onActivated: root.gotoPage(AppShell.Library) }
    Shortcut { sequences: root.keys.pagePlayer;    onActivated: root.gotoPage(AppShell.Player) }
    Shortcut { sequences: root.keys.pageDownloads; onActivated: root.gotoPage(AppShell.Download) }
    Shortcut { sequences: root.keys.pageLogs;      onActivated: root.gotoPage(AppShell.Log) }

    Shortcut {
        sequences: root.keys.bossKey
        onActivated: {
            if (Globals.pipMode) root.togglePip()
            if (Globals.maximised) root.toggleMaximised()
            if (Globals.fullscreen) root.toggleFullscreen()
            bossScreen.visible = true
            root.lower()
            root.showMinimized()
            if (Globals.mpv) Globals.mpv.pause()
        }
    }
    Shortcut {
        sequences: root.keys.bossScreen
        onActivated: bossScreen.visible = !bossScreen.visible
    }
}
