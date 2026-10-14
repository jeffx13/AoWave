pragma Singleton
import QtQml
import QtQuick
import App

QtObject {

    property bool maximised: false
    property bool fullscreen: false
    property bool pipMode: false

    property real appWidth:  root ? root.width  : 0
    property real appHeight: root ? root.height : 0

    property int page: MainWindow.Explore
    property int newEpisodeShows: 0
    property bool recordingKey: false
    // An episode the mini player's buttons loaded plays on there: the jump to the player is skipped until the load is over.
    property bool miniHold: false
    onPageChanged: if (page === MainWindow.Library) newEpisodeShows = 0

    property var root: null
    property var mpv: null

    property real libraryLastContentY: 0
    property real explorerLastContentY: 0
    property string lastSearch: ""
    readonly property var libraryTypeNames: [qsTr("Watching"), qsTr("Planned"), qsTr("Paused"),
                                             qsTr("Dropped"), qsTr("Completed")]
    property var explorerFilters: ({})
    property Connections filterReset: Connections {
        target: App.providers
        function onCurrentTypeIndexChanged() { Ui.explorerFilters = ({}) }
    }
    property var pendingGenre: null
    function keyFor(action) {
        const keys = App.shortcuts.bindings[action] ?? []
        return keys.length > 0 ? keys[0] : ""
    }
    function tipWithKey(label, action) {
        const key = keyFor(action)
        return key === "" ? label : label + " (" + key + ")"
    }
    function genreOption(options, genre) {
        const key = text => String(text).toLowerCase().replace(/[\s\-_'.·]+/g, "")
        const wanted = key(genre)
        return (options || []).find(option => key(option.label) === wanted || key(option.value) === wanted) || null
    }
    readonly property real coverAspect: 319 / 225

    readonly property int minWidth: 960
    readonly property int minHeight: 600

    readonly property int defaultWidth:  App.settings.windowWidth
    readonly property int defaultHeight: App.settings.windowHeight

    readonly property var sizePresets: [
        { w: 1280, h: 720 },  { w: 1366, h: 768 },  { w: 1440, h: 900 },
        { w: 1600, h: 900 },  { w: 1680, h: 1050 }, { w: 1920, h: 1080 }
    ]

    property real uiScale: 1.0

    readonly property string fontFamily: App.settings.appFont || Application.font.family

    readonly property real baseScale: 1.15

    function sp(n) {
        return Math.round(n * uiScale * baseScale)
    }

    readonly property int controlHeight: sp(40)
    readonly property int toolbarHeight: controlHeight + 12

    function gotoPage(page)     { if (root) root.gotoPage(page) }
    function searchProviders(query) {
        lastSearch = query
        App.search(query, explorerFilters)
        gotoPage(MainWindow.Explore)
    }
    function offerUndo(message, undo, commit) { if (root) root.offerUndo(message, undo, commit) }
    function ago(seconds) {
        const elapsed = Math.max(0, Date.now() / 1000 - seconds)
        if (elapsed < 60) return qsTr("Just now")
        if (elapsed < 3600) return qsTr("%1 min ago").arg(Math.floor(elapsed / 60))
        if (elapsed < 86400) return qsTr("%1 h ago").arg(Math.floor(elapsed / 3600))
        return Qt.formatDate(new Date(seconds * 1000), "d MMM")
    }
    // The label of the row or slider a control sits in: its name for a screen reader.
    function labelOf(item) {
        let owner = item.parent
        for (let depth = 0; owner && depth < 4; ++depth, owner = owner.parent)
            if (typeof owner.label === "string" && owner.label !== "") return owner.label
        return ""
    }
    // Enter presses a focused button as Space does.
    function clickOnEnter(event, button) {
        if (event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter) return
        event.accepted = true
        if (!event.isAutoRepeat) button.click()
    }
    function togglePip()        { if (root) root.togglePip() }
    function toggleFullscreen() { if (root) root.toggleFullscreen() }
}
