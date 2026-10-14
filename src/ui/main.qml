pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import App

ApplicationWindow {
    id: root

    function fitWidth(w)  { return Math.max(Ui.minWidth,  Math.min(w, Screen.desktopAvailableWidth)) }
    function fitHeight(h) { return Math.max(Ui.minHeight, Math.min(h, Screen.desktopAvailableHeight)) }

    minimumWidth: Ui.pipMode ? root.pipMinWidth : Math.min(Ui.minWidth, Screen.desktopAvailableWidth)
    minimumHeight: Ui.pipMode ? Math.round(root.pipMinWidth / 2.4) : Math.min(Ui.minHeight, Screen.desktopAvailableHeight)
    width: fitWidth(Ui.defaultWidth)
    height: fitHeight(Ui.defaultHeight)
    x: (Screen.desktopAvailableWidth - width) / 2
    y: (Screen.desktopAvailableHeight - height) / 2

    visible: true
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowMinimizeButtonHint
    onClosing: {
        App.playlist.saveProgress()
        const w = chromeVisible ? width : savedW
        const h = chromeVisible ? height : savedH
        App.settings.windowWidth = Math.round(w)
        App.settings.windowHeight = Math.round(h)
    }

    Connections {
        target: App.settings
        function onWindowSizeChanged() {
            const w = root.fitWidth(App.settings.windowWidth)
            const h = root.fitHeight(App.settings.windowHeight)
            root.savedW = w
            root.savedH = h
            if (!root.chromeVisible || Ui.maximised) return
            root.applyGeometry(Math.round((Screen.desktopAvailableWidth - w) / 2),
                               Math.round((Screen.desktopAvailableHeight - h) / 2), w, h)
        }
    }

    readonly property bool onPlayer: Ui.page === MainWindow.Player

    // The window itself stays see-through, for the video under it (MpvPlayer); this paints it.
    color: "transparent"
    background: Rectangle { color: root.onPlayer ? "#000000" : Theme.background }

    Binding { target: Theme; property: "name";         value: App.settings.themeName }
    Binding { target: Theme; property: "reduceMotion"; value: App.settings.reduceMotion }
    Binding { target: Theme; property: "highContrast"; value: App.settings.highContrast }
    Binding { target: Theme; property: "customAccent"; value: App.settings.accentColor }
    Binding { target: Ui; property: "uiScale";    value: App.settings.uiScale }

    readonly property var navOrder: [
        MainWindow.Explore, MainWindow.Details, MainWindow.Library, MainWindow.Player,
        MainWindow.Downloads, MainWindow.History, MainWindow.Logs, MainWindow.Settings
    ]

    function stepPage(delta) {
        let at = navOrder.indexOf(Ui.page)
        if (at < 0) at = 0
        for (let i = 0; i < navOrder.length; ++i) {
            at = (at + delta + navOrder.length) % navOrder.length
            const page = navOrder[at]
            if (page === MainWindow.Details && !App.show.exists) continue
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
        if (Ui.pipMode || Ui.fullscreen) return

        if (Ui.maximised) {
            Ui.maximised = false
            restoreGeometry()
        } else {
            saveGeometry()
            Ui.maximised = true
            fillDesktop()
        }
    }

    function toggleFullscreen() {
        if (Ui.pipMode) togglePip()

        if (Ui.fullscreen) {
            Ui.fullscreen = false
            if (Ui.maximised) fillDesktop()
            else                   restoreGeometry()
        } else {
            if (!Ui.maximised) saveGeometry()
            Ui.fullscreen = true
            fillScreen()
        }
    }

    property int pipReturnPage: MainWindow.Player
    property bool pipAuto: false
    property real pipSince: 0
    readonly property real pipMinWidth: Ui.sp(240)
    readonly property real pipMargin: 16
    readonly property real pipAspect: Ui.mpv && Ui.mpv.videoAspect > 0
                                      ? Math.max(0.75, Math.min(2.4, Ui.mpv.videoAspect)) : 16 / 9

    function togglePip(auto) {
        if (Ui.pipMode) {
            pipSettle.stop()
            savePipRect()
            App.setPipWindow(root, false)
            Ui.pipMode = false
            pipAuto = false

            if (Ui.fullscreen)      fillScreen()
            else if (Ui.maximised)  fillDesktop()
            else                         restoreGeometry()
            if (pipReturnPage !== MainWindow.Player) gotoPage(pipReturnPage)
        } else {
            if (!Ui.maximised && !Ui.fullscreen) saveGeometry()
            Ui.fullscreen = false
            Ui.maximised = false
            pipReturnPage = Ui.page
            pipAuto = auto === true
            pipSince = Date.now()
            gotoPage(MainWindow.Player)
            Ui.pipMode = true
            const r = pipRect()
            applyGeometry(r.x, r.y, r.width, r.height)
            // Natively, not through flags: changing flags re-shows the window, taking focus from the app just switched to.
            App.setPipWindow(root, true)
            if (root.active) App.focusPreviousWindow(root)
        }
    }

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
        if (Ui.pipMode)
            App.settings.setValue("pip/rect", [x, y, width, height].map(Math.round).join(","))
    }
    function resizePip(newWidth) {
        const areaW = Screen.desktopAvailableWidth, areaH = Screen.desktopAvailableHeight
        const w = Math.round(Math.max(pipMinWidth, Math.min(newWidth, areaW * 0.8, areaH * 0.8 * pipAspect)) / 4) * 4
        const h = Math.round(w / pipAspect)
        const right = x + width / 2 > areaW / 2, bottom = y + height / 2 > areaH / 2
        applyGeometry(right ? x + width - w : x, bottom ? y + height - h : y, w, h)
    }
    function settlePip() {
        const areaW = Screen.desktopAvailableWidth, areaH = Screen.desktopAvailableHeight
        const snap = Ui.sp(40)
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
    onPipAspectChanged: if (Ui.pipMode) resizePip(width)

    readonly property bool chromeVisible: !(Ui.pipMode || Ui.fullscreen)

    readonly property bool autoPipReady: App.settings.autoPip && !App.quiet && !Ui.pipMode && !bossScreen.visible
                                         && !!Ui.mpv && Ui.mpv.state === MpvPlayer.Playing
    onVisibilityChanged: (visibility) => {
        if (visibility !== Window.Minimized || !autoPipReady) return
        App.restoreWithoutFocus(root)
        togglePip(true)
    }
    Timer {
        interval: 700
        repeat: true
        running: root.autoPipReady && root.visible && !root.active && root.visibility !== Window.Minimized
        onTriggered: if (App.coveredFraction(root) > 0.8) root.togglePip(true)
    }
    onActiveChanged: {
        if (active && Ui.pipMode && pipAuto && Date.now() - pipSince > 1000 && !pipOverlay.hovered)
            togglePip()
    }

    Component.onCompleted: {
        Ui.root = root

        if (App.playlist.playAt(0)) {
            if (!App.quiet) root.requestActivate()
            Ui.page = MainWindow.Player
            history = [MainWindow.Player]
        }

        deferredStartupTimer.start()
    }

    property var history: [MainWindow.Explore]
    property int historyIndex: 0

    property string nowPlayingTitle: ""
    property string nowPlayingEpisode: ""
    readonly property bool nowPlayingActive: !!Ui.mpv
        && (Ui.mpv.state !== MpvPlayer.Stopped || Ui.mpv.isLoading || App.playlist.isLoading)
    Connections {
        target: App.playlist
        function onCurrentItemChanged() {
            root.nowPlayingTitle = App.playlist.currentShowName()
            root.nowPlayingEpisode = App.playlist.currentItemName().replace(/\s*\n\s*/g, "  ")
        }
    }

    function gotoPage(page, isHistory = false) {
        if (Ui.fullscreen || Ui.page === page) return
        if (page === MainWindow.Details && !App.show.exists) return

        if (page === MainWindow.Player)
            Ui.mpv.peek(2000)

        Ui.page = page
        if (!isHistory) {
            history.splice(historyIndex + 1)
            history.push(page)
            historyIndex = history.length - 1
        }
    }

    function goHistory(delta) {
        if (Ui.fullscreen) return
        const at = historyIndex + delta
        if (at < 0 || at >= history.length) return
        historyIndex = at
        gotoPage(history[at], true)
    }

    Connections {
        target: Ui
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
        onPlayerRequested: root.gotoPage(MainWindow.Player)
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

    Timer {
        interval: 3 * 60 * 60 * 1000
        repeat: true
        running: true
        onTriggered: App.library.fetchUnwatchedEpisodes(0, true)
    }

    Connections {
        target: App.library
        function onNewEpisodesFound(shows) {
            if (Ui.page !== MainWindow.Library) Ui.newEpisodeShows += shows.length
        }
    }

    Item {
        z: 1
        anchors.fill: contentArea
        opacity: root.onPlayer ? 0 : 1
        visible: opacity > 0.001
        Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

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
            height: visible ? Ui.sp(44) : 0
            color: Theme.accentSoft

            RowLayout {
                anchors { fill: parent; leftMargin: 16; rightMargin: 8 }
                spacing: 10
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Version %1 is available.").arg(App.updateVersion)
                    color: Theme.textPrimary
                    font.pixelSize: Ui.sp(Theme.bodySize)
                    elide: Text.ElideRight
                }
                AppButton {
                    text: qsTr("See what's new")
                    fontSize: 16
                    radius: 8
                    Layout.preferredHeight: Ui.sp(32)
                    onClicked: Qt.openUrlExternally(App.updateUrl)
                }
                IconButton {
                    iconName: "x"
                    iconSize: 16
                    implicitWidth: Ui.sp(32)
                    implicitHeight: Ui.sp(32)
                    tip: qsTr("Skip this version")
                    onClicked: App.dismissUpdate()
                }
            }
        }

        StackLayout {
            id: pageStack
            anchors { fill: parent; topMargin: updateBanner.height }

            readonly property var pageOrder: [
                MainWindow.Explore, MainWindow.Details, MainWindow.Library,
                MainWindow.Downloads, MainWindow.Logs, MainWindow.Settings, MainWindow.History
            ]
            currentIndex: Math.max(0, pageOrder.indexOf(Ui.page))

            function settingsPage() {
                const loader = pageLoaders.itemAt(pageOrder.indexOf(MainWindow.Settings)) as Loader
                if (!loader) return null
                loader.active = true
                return loader.item as SettingsPage
            }

            Repeater {
                id: pageLoaders
                model: [
                    "Pages/ExplorePage.qml",
                    "Pages/DetailsPage.qml",
                    "Pages/LibraryPage.qml",
                    "Pages/DownloadsPage.qml",
                    "Pages/LogsPage.qml",
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
        readonly property bool cancellablePage: Ui.page === MainWindow.Explore
                                             || Ui.page === MainWindow.Details
                                             || Ui.page === MainWindow.Library
                                             || Ui.page === MainWindow.History
        loading: {
            switch (Ui.page) {
            case MainWindow.Explore:  return App.show.isLoading || (App.playlist.isLoading && !Ui.miniHold)
            case MainWindow.Details:    return App.playlist.isLoading || App.show.isLoading
            case MainWindow.Library:
            case MainWindow.History: return App.show.isLoading || (App.playlist.isLoading && !Ui.miniHold)
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

    property bool miniDismissed: false
    onOnPlayerChanged: if (onPlayer) miniDismissed = false
    // Loading counts: an episode change passes through Stopped, and the video should not blink out.
    readonly property bool miniPlayer: App.settings.miniPlayer && !onPlayer && !Ui.pipMode && !miniDismissed
                                       && nowPlayingActive
    readonly property real miniMargin: Ui.sp(16)
    readonly property real miniRadius: Ui.sp(12)
    readonly property real miniAspect: Ui.mpv && Ui.mpv.videoAspect > 0
                                       ? Math.max(0.75, Math.min(2.4, Ui.mpv.videoAspect)) : 16 / 9
    readonly property real miniWidth: Math.round(Math.max(Ui.sp(220),
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

    readonly property bool anyLoading: !!Ui.mpv && (Ui.mpv.isLoading || App.playlist.isLoading)
    onAnyLoadingChanged: if (!anyLoading) Qt.callLater(() => { Ui.miniHold = false })

    PlayerPage {
        id: mpvPage
        enabled: root.onPlayer || Ui.pipMode
        compact: root.miniPlayer
        z: compact ? 2 : 0
        x: compact ? root.miniX : contentArea.x
        y: compact ? root.miniY : contentArea.y
        width: compact ? root.miniWidth : contentArea.width
        height: compact ? root.miniHeight : contentArea.height
        Behavior on x { enabled: root.miniSnapping; NumberAnimation { duration: 240; easing.type: Easing.OutCubic } }
        Behavior on y { enabled: root.miniSnapping; NumberAnimation { duration: 240; easing.type: Easing.OutCubic } }
        // Rounded by the player itself: as a layer it could not show the video under the window.
        videoRadius: compact ? root.miniRadius : 0
    }

    RectangularShadow {
        visible: root.miniPlayer
        z: 1.5
        x: mpvPage.x
        y: mpvPage.y
        width: mpvPage.width
        height: mpvPage.height
        radius: root.miniRadius
        offset.y: Ui.sp(6)
        blur: Ui.sp(28)
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
        readonly property bool busy: !!Ui.mpv && (Ui.mpv.isLoading || App.playlist.isLoading)

        HoverHandler { id: miniHover }

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
                if (!root.miniDragging) { root.gotoPage(MainWindow.Player); return }
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
            height: Math.min(parent.height * 0.45, Ui.sp(52))
            radius: root.miniRadius
            opacity: miniOverlay.active ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 150 } }
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.alpha("black", 0.72) }
                GradientStop { position: 1.0; color: "transparent" }
            }
            Text {
                anchors { left: parent.left; right: miniWindowButtons.left; top: parent.top
                          leftMargin: root.miniBottom && root.miniRight ? Ui.sp(30) : Ui.sp(12)
                          rightMargin: 6; topMargin: Ui.sp(9) }
                text: root.nowPlayingEpisode !== "" ? root.nowPlayingTitle + "  ·  " + root.nowPlayingEpisode
                                                    : root.nowPlayingTitle
                color: Theme.onOverlay
                font.pixelSize: Ui.sp(Theme.compactSize)
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
            Row {
                id: miniWindowButtons
                anchors { right: parent.right; top: parent.top; margins: 4
                          rightMargin: root.miniBottom && !root.miniRight ? Ui.sp(28) : 4 }
                IconButton {
                    iconName: "maximize"
                    iconSize: 15
                    tip: qsTr("Back to the player")
                    variant: "overlay"
                    onClicked: root.gotoPage(MainWindow.Player)
                }
                IconButton {
                    iconName: "x"
                    iconSize: 15
                    tip: qsTr("Close and pause")
                    variant: "overlay"
                    onClicked: {
                        if (Ui.mpv && Ui.mpv.state === MpvPlayer.Playing) Ui.mpv.pause()
                        root.miniDismissed = true
                    }
                }
            }
        }

        Row {
            anchors.centerIn: parent
            spacing: Ui.sp(10)
            opacity: miniOverlay.active && !miniOverlay.busy ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 150 } }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                iconName: "skip-back"
                iconSize: 18
                tip: qsTr("Previous episode")
                variant: "overlay"
                onClicked: { Ui.miniHold = true; App.playlist.stepItem(-1) }
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: Ui.sp(48)
                implicitHeight: Ui.sp(48)
                boxRadius: implicitWidth / 2
                iconName: Ui.mpv && Ui.mpv.state === MpvPlayer.Playing ? "pause" : "play"
                tip: Ui.mpv && Ui.mpv.state === MpvPlayer.Playing ? qsTr("Pause") : qsTr("Play")
                iconSize: 24
                variant: "accent"
                onClicked: Ui.mpv.togglePlayPause()
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                iconName: "skip-forward"
                iconSize: 18
                tip: qsTr("Next episode")
                variant: "overlay"
                onClicked: { Ui.miniHold = true; App.playlist.stepItem(1) }
            }
        }

        AppSpinner {
            anchors.centerIn: parent
            visible: miniOverlay.busy
        }

        Item {
            id: miniProgress
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom
                      leftMargin: root.miniRadius * 0.6; rightMargin: root.miniRadius * 0.6 }
            height: Ui.sp(14)
            readonly property real fraction: Ui.mpv && Ui.mpv.duration > 0
                                             ? Math.min(1, Ui.mpv.time / Ui.mpv.duration) : 0
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
                    if (Ui.mpv && Ui.mpv.duration > 0)
                        Ui.mpv.seek(Ui.mpv.duration * Math.max(0, Math.min(1, mouse.x / width)))
                }
            }
        }

        Item {
            width: Ui.sp(26)
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
        visible: Ui.pipMode
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
        onSearched: (query) => Ui.searchProviders(query)
        onOpenChanged: if (open) settingsSource = pageStack.settingsPage()
        property SettingsPage settingsSource: null
        findSettings: q => settingsSource ? settingsSource.find(q) : []
        onShowRequested: (link, provider, title) => App.openShowInfo(link, provider, title)
        onSettingRequested: label => {
            root.gotoPage(MainWindow.Settings)
            const page = pageStack.settingsPage()
            if (page) page.reveal(label)
        }
        onActionRequested: (id) => {
            switch (id) {
            case "markWatched":      App.playlist.markCurrentWatched(); break
            case "toggleDanmaku":
                App.settings.danmakuEnabled = !App.settings.danmakuEnabled
                if (Ui.mpv) Ui.mpv.showText(App.settings.danmakuEnabled
                                                      ? qsTr("Danmaku on") : qsTr("Danmaku off"))
                break
            case "danmakuSettings":
                root.gotoPage(MainWindow.Player)
                mpvPage.openDanmakuTab()
                break
            case "migrate":          root.gotoPage(MainWindow.Details); break
            case "openMpvConf":      Qt.openUrlExternally("file:///" + App.settings.dataDir + "/mpv/mpv.conf"); break
            case "openSettings":     Qt.openUrlExternally(App.settings.path); break
            case "gotoLogs":         root.gotoPage(MainWindow.Logs); break
            case "gotoDownloads":    root.gotoPage(MainWindow.Downloads); break
            case "connectTracker":   root.gotoPage(MainWindow.Settings); break
            case "reloadEpisode":    App.playlist.reload(); break
            }
        }
    }

    Toast {
        id: toast
    }
    function offerUndo(message, undo, commit) { toast.offerUndo(message, undo, commit) }
    // A text field undoes its own typing first: it takes Ctrl+Z before a shortcut sees it.
    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: toast.undoAction !== null && !root.onPlayer
        onActivated: toast.undo()
    }

    Notifier {
        id: notifier
        onLogsRequested: root.gotoPage(MainWindow.Logs)
        onClosed: {
            if (mpvPage.visible) mpvPage.forceActiveFocus()
            else root.focusCurrentPage()
        }
    }

    // Loaded only while shown, at the window's size: a hidden Image still decodes its 4K source at startup and keeps it.
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
        target: MainWindow
        function onErrorReported(message, header) { notifier.show(message, header) }
        function onInfoReported(message, header)  { toast.show(message, header) }
        function onNavigateRequested(page) {
            if (page === MainWindow.Player && Ui.miniHold) return
            root.gotoPage(page)
        }
        function onHistoryStepRequested(delta)    { root.goHistory(delta) }
    }

    Item {
        anchors.fill: parent
        z: -1
        TapHandler {
            acceptedButtons: Qt.BackButton | Qt.ForwardButton
            onTapped: (eventPoint, button) => root.goHistory(button === Qt.ForwardButton ? 1 : -1)
        }
    }

    readonly property var keys: Ui.recordingKey
        ? Object.keys(App.shortcuts.bindings).reduce((none, action) => { none[action] = []; return none }, {})
        : App.shortcuts.bindings
    Shortcut { sequences: root.keys.forward; onActivated: root.goHistory(1) }
    Shortcut { sequences: root.keys.back;    onActivated: root.goHistory(-1) }
    Shortcut { sequences: root.keys.nextPage;     enabled: !Ui.pipMode; onActivated: root.stepPage(1) }
    Shortcut { sequences: root.keys.previousPage; enabled: !Ui.pipMode; onActivated: root.stepPage(-1) }

    Shortcut { sequences: root.keys.close; onActivated: root.close() }
    Shortcut {
        sequences: root.keys.reload
        enabled: Ui.page === MainWindow.Details && App.show.exists
        onActivated: App.reloadShow()
    }
    Shortcut { sequences: root.keys.quickSearch; onActivated: quickSearch.open = !quickSearch.open }
    Shortcut { sequences: root.keys.pageSearch;    onActivated: root.gotoPage(MainWindow.Explore) }
    Shortcut { sequences: root.keys.pageInfo;      onActivated: root.gotoPage(MainWindow.Details) }
    Shortcut { sequences: root.keys.pageLibrary;   onActivated: root.gotoPage(MainWindow.Library) }
    Shortcut { sequences: root.keys.pagePlayer;    onActivated: root.gotoPage(MainWindow.Player) }
    Shortcut { sequences: root.keys.pageDownloads; onActivated: root.gotoPage(MainWindow.Downloads) }
    Shortcut { sequences: root.keys.pageLogs;      onActivated: root.gotoPage(MainWindow.Logs) }

    Shortcut {
        sequences: root.keys.bossKey
        onActivated: {
            if (Ui.pipMode) root.togglePip()
            if (Ui.maximised) root.toggleMaximised()
            if (Ui.fullscreen) root.toggleFullscreen()
            bossScreen.visible = true
            root.lower()
            root.showMinimized()
            if (Ui.mpv) Ui.mpv.pause()
        }
    }
    Shortcut {
        sequences: root.keys.bossScreen
        onActivated: bossScreen.visible = !bossScreen.visible
    }
}
