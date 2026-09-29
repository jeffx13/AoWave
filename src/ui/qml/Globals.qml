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

    property int page: AppShell.Search
    // Shows with an episode found since the library was last opened.
    property int newEpisodeShows: 0
    // While Settings waits for a new shortcut key, the app's own shortcuts stand aside.
    property bool recordingKey: false
    // An episode the mini player's own buttons loaded plays on there: the jump to the player
    // that loading makes is skipped until this load is over.
    property bool miniHold: false
    onPageChanged: if (page === AppShell.Library) newEpisodeShows = 0

    property var root: null
    property var mpv: null

    property real libraryLastContentY: 0
    property real explorerLastContentY: 0
    property string lastSearch: ""
    // The library's lists, in Library::LibraryType order.
    readonly property var libraryTypeNames: [qsTr("Watching"), qsTr("Planned"), qsTr("Paused"),
                                             qsTr("Dropped"), qsTr("Completed")]
    // Explorer's genre, year and status picks, {"genre": value, ...}; each provider and type has its own.
    property var explorerFilters: ({})
    property Connections filterReset: Connections {
        target: App.providers
        function onCurrentTypeIndexChanged() { Globals.explorerFilters = ({}) }
    }
    // A genre picked on the Info page, {provider, genre}, for Explorer to toggle once that
    // provider's options are in.
    property var pendingGenre: null
    // The first key bound to a shortcut action, as the user has it now; "" when it has none.
    function keyFor(action) {
        const keys = App.shortcuts.bindings[action] ?? []
        return keys.length > 0 ? keys[0] : ""
    }
    // A tooltip with the action's key as bound now, if it has one.
    function tipWithKey(label, action) {
        const key = keyFor(action)
        return key === "" ? label : label + " (" + key + ")"
    }
    // The option among `options` that a show's genre names: sites spell one genre "Sci-Fi" in
    // one place and "Sci Fi" in another.
    function genreOption(options, genre) {
        const key = text => String(text).toLowerCase().replace(/[\s\-_'.·]+/g, "")
        const wanted = key(genre)
        return (options || []).find(option => key(option.label) === wanted || key(option.value) === wanted) || null
    }
    property real imageAspectRatio: 319 / 225

    // Screen is an attached property of Item, and this is a singleton.
    readonly property int minWidth: 960
    readonly property int minHeight: 600

    readonly property int defaultWidth:  App.settings.windowWidth
    readonly property int defaultHeight: App.settings.windowHeight

    // Anything larger than the screen is filtered out.
    readonly property var sizePresets: [
        { w: 1280, h: 720 },  { w: 1366, h: 768 },  { w: 1440, h: 900 },
        { w: 1600, h: 900 },  { w: 1680, h: 1050 }, { w: 1920, h: 1080 }
    ]

    property real uiScale: 1.0

    // Quick Controls styles apply their own default font.
    readonly property string fontFamily: App.settings.appFont || Application.font.family

    // The comfortable baseline is baked in, so the scale multiplies it.
    readonly property real baseScale: 1.15

    function sp(n) {
        return Math.round(n * uiScale * baseScale)
    }

    // Shared control sizes, so toolbars and fields agree.
    readonly property int controlHeight: sp(40)
    readonly property int toolbarHeight: controlHeight + 12

    function gotoPage(page)     { if (root) root.gotoPage(page) }
    function searchProviders(query) {
        lastSearch = query
        App.search(query, explorerFilters)
        gotoPage(AppShell.Search)
    }
    function offerUndo(message, undo, commit) { if (root) root.offerUndo(message, undo, commit) }
    // How long ago a unix time was, short: "5 min ago", or the date past a day.
    function ago(seconds) {
        const elapsed = Math.max(0, Date.now() / 1000 - seconds)
        if (elapsed < 60) return qsTr("Just now")
        if (elapsed < 3600) return qsTr("%1 min ago").arg(Math.floor(elapsed / 60))
        if (elapsed < 86400) return qsTr("%1 h ago").arg(Math.floor(elapsed / 3600))
        return Qt.formatDate(new Date(seconds * 1000), "d MMM")
    }
    function togglePip()        { if (root) root.togglePip() }
    function toggleFullscreen() { if (root) root.toggleFullscreen() }
}
