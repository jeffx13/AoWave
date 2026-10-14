pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQml.Models
import App

Rectangle {
    id: quickSearch

    property bool open: false
    signal searched(string query)
    signal actionRequested(string id)
    signal showRequested(string link, string provider, string title)
    signal settingRequested(string label)
    property var findSettings: null

    color: Theme.scrim
    visible: opacity > 0.01
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: Theme.dur(120) } }
    onOpenChanged: if (open) { qsField.text = ""; selected = 0; ++openCount; qsField.forceActiveFocus() }

    property int selected: 0
    property int openCount: 0
    readonly property int visibleRows: 8
    readonly property int rowHeight: Ui.sp(38 / 1.15)
    readonly property int effectiveSelected: selected >= 0 ? selected : matches.length - 1
    property point mouseAt
    function reveal() { resultList.positionViewAtIndex(effectiveSelected, ListView.Contain) }

    // built before Ui.mpv exists; a throwing binding stays undefined
    readonly property bool hasPlayback: Ui.mpv !== null && Ui.mpv !== undefined
                                        && Ui.mpv.duration > 0
    readonly property string currentShowLink: (App.show && App.show.link) ? App.show.link : ""
    readonly property bool canMigrate: {
        void quickSearch.openCount
        return currentShowLink !== "" && App.library.entryForLink(currentShowLink).valid
    }

    readonly property var actions: [
        { id: "markWatched",  icon: "check",         label: qsTr("Mark current episode watched"),
          keys: "seen complete", when: quickSearch.hasPlayback },
        { id: "toggleDanmaku", icon: App.settings.danmakuEnabled ? "captions-off" : "captions", label: qsTr("Toggle danmaku"),
          keys: "comments bullet", when: true },
        { id: "danmakuSettings", icon: "settings",   label: qsTr("Danmaku settings"),
          keys: "comments opacity speed", when: true },
        { id: "migrate",      icon: "server",        label: qsTr("Migrate this show to another provider"),
          keys: "switch source", when: quickSearch.canMigrate },
        { id: "openMpvConf",  icon: "file",          label: qsTr("Open mpv.conf"),
          keys: "config player", when: true },
        { id: "openSettings", icon: "file",          label: qsTr("Open settings.ini"),
          keys: "config ini", when: true },
        { id: "gotoExplore",  icon: "search",        label: qsTr("Go to Explore"), shortcut: "pageSearch",
          keys: "browse search latest popular", when: true, page: MainWindow.Explore },
        { id: "gotoDetails",  icon: "details",       label: qsTr("Go to Details"), shortcut: "pageInfo",
          keys: "episodes info", when: App.show.exists, page: MainWindow.Details },
        { id: "gotoLibrary",  icon: "library",       label: qsTr("Go to Library"), shortcut: "pageLibrary",
          keys: "watching planned completed", when: true, page: MainWindow.Library },
        { id: "gotoPlayer",   icon: "tv",            label: qsTr("Go to Player"), shortcut: "pagePlayer",
          keys: "video watch", when: Ui.mpv !== null && Ui.mpv !== undefined, page: MainWindow.Player },
        { id: "gotoDownloads", icon: "download",     label: qsTr("Go to Downloads"), shortcut: "pageDownloads",
          keys: "queue", when: true, page: MainWindow.Downloads },
        { id: "gotoHistory",  icon: "history",       label: qsTr("Go to History"),
          keys: "recent watched", when: true, page: MainWindow.History },
        { id: "gotoLogs",     icon: "list-checks",   label: qsTr("Go to Logs"), shortcut: "pageLogs",
          keys: "errors warnings debug", when: true, page: MainWindow.Logs },
        { id: "gotoSettings", icon: "settings",      label: qsTr("Go to Settings"),
          keys: "options preferences", when: true, page: MainWindow.Settings },
        { id: "connectTracker", icon: "star",        label: qsTr("Connect a watch list"),
          keys: "anilist mal trakt sign in", when: true },
        { id: "reloadEpisode", icon: "refresh-cw",   label: qsTr("Reload current episode"), shortcut: "reload",
          keys: "refresh restart", when: quickSearch.hasPlayback }
    ]

    readonly property var commonActions: ["gotoLibrary", "gotoDownloads", "gotoHistory", "gotoSettings",
                                          "toggleDanmaku", "markWatched", "reloadEpisode"]
    DelegateModel {
        id: recentShows
        model: App.library.historyModel
        delegate: Item {}
    }
    function toMatch(a) {
        return { kind: "action", id: a.id, icon: a.icon, label: a.label, tag: a.shortcut ? Ui.keyFor(a.shortcut) : "",
                 page: a.page }
    }

    readonly property var matches: {
        const text = qsField.text.trim()
        const q = text.toLowerCase()
        if (q.length === 0) {
            void quickSearch.openCount
            const recent = []
            for (let i = 0; i < Math.min(5, recentShows.count); ++i) {
                const show = recentShows.items.get(i).model
                recent.push({ kind: "show", icon: "history", label: show.title,
                              tag: show.playedAt > 0 ? Ui.ago(show.playedAt) : "", link: show.link, provider: show.provider })
            }
            return recent.concat((quickSearch.actions || [])
                .filter(a => a.when && quickSearch.commonActions.indexOf(a.id) >= 0).map(quickSearch.toMatch))
        }
        const rank = label => {
            const name = label.toLowerCase()
            if (name.startsWith(q)) return 0
            if (name.split(/[\s\-:\u00b7]+/).some(word => word.startsWith(q))) return 1
            return name.indexOf(q) >= 0 ? 2 : 3
        }
        const found = (quickSearch.actions || [])
            .filter(a => a.when && (a.label.toLowerCase().indexOf(q) >= 0 || (a.keys || "").indexOf(q) >= 0))
            .map(quickSearch.toMatch)
        for (const show of App.library.findShows(text, 5))
            found.push({ kind: "show", icon: show.libraryType >= 0 ? "library" : "history", label: show.title,
                         tag: show.libraryType >= 0 ? Ui.libraryTypeNames[show.libraryType] : qsTr("History"),
                         link: show.link, provider: show.provider })
        for (const setting of (quickSearch.findSettings ? quickSearch.findSettings(q) : []).slice(0, 4))
            found.push({ kind: "setting", icon: "settings", label: setting.label, tag: setting.card })
        found.sort((a, b) => rank(a.label) - rank(b.label))
        found.push({ kind: "search", icon: "search", label: qsTr("Search providers for \u201c%1\u201d").arg(text), tag: "" })
        return found
    }

    function run(index) {
        const match = matches[index]
        if (!match) return
        quickSearch.open = false
        if (match.page !== undefined) Ui.gotoPage(match.page)
        else if (match.kind === "action") quickSearch.actionRequested(match.id)
        else if (match.kind === "show") quickSearch.showRequested(match.link, match.provider, match.label)
        else if (match.kind === "setting") quickSearch.settingRequested(match.label)
        else quickSearch.searched(qsField.text)
    }

    MouseArea { anchors.fill: parent; onClicked: quickSearch.open = false }

    Column {
        anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: parent.height * 0.18 }
        width: Math.min(Ui.sp(560 / 1.15), quickSearch.width - 80)
        spacing: 8

        Card {
            width: parent.width
            height: Ui.sp(58 / 1.15)
            border.color: Theme.accent

            MouseArea { anchors.fill: parent }

            AppIcon {
                anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 16 }
                name: "search"
                color: Theme.textMuted
                size: 20
            }
            AppTextField {
                id: qsField
                anchors { fill: parent; leftMargin: 50; rightMargin: 16 }
                leftPadding: 0; rightPadding: 0
                placeholderText: qsTr("Search or type a command…")
                background: null
                onTextChanged: quickSearch.selected = 0
                onAccepted: quickSearch.run(quickSearch.selected >= 0 ? quickSearch.selected : quickSearch.matches.length - 1)
                Keys.onDownPressed: if (quickSearch.matches.length > 0) {
                    quickSearch.selected = Math.min(quickSearch.effectiveSelected + 1, quickSearch.matches.length - 1)
                    quickSearch.reveal()
                }
                Keys.onUpPressed: {
                    quickSearch.selected = Math.max(quickSearch.effectiveSelected - 1, qsField.text.trim() === "" ? 0 : -1)
                    quickSearch.reveal()
                }
                onUnfocused: quickSearch.open = false
            }
        }

        Rectangle {
            width: parent.width
            visible: quickSearch.matches.length > 0
            height: visible ? resultList.height + 10 : 0
            radius: 12
            color: Theme.surface
            border.color: Theme.border
            border.width: 1
            MouseArea { anchors.fill: parent }

            ListView {
                id: resultList
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 5 }
                height: Math.min(count, quickSearch.visibleRows) * quickSearch.rowHeight
                        + Math.max(0, Math.min(count, quickSearch.visibleRows) - 1) * spacing
                spacing: 1
                clip: true
                interactive: contentHeight > height
                boundsBehavior: Flickable.StopAtBounds
                model: quickSearch.matches
                ScrollBar.vertical: AppScrollBar {}

                delegate: Rectangle {
                    id: actionItem
                    required property var modelData
                    required property int index
                    width: resultList.width
                    height: quickSearch.rowHeight
                    radius: 8
                    color: quickSearch.effectiveSelected === actionItem.index ? Qt.alpha(Theme.accent, 0.16)
                         : actionHover.hovered ? Qt.alpha(Theme.accent, 0.08) : "transparent"

                    HoverHandler {
                        id: actionHover
                        cursorShape: Qt.PointingHandCursor
                        onPointChanged: {
                            const at = point.scenePosition
                            if (hovered && (at.x !== quickSearch.mouseAt.x || at.y !== quickSearch.mouseAt.y))
                                quickSearch.selected = actionItem.index
                            quickSearch.mouseAt = at
                        }
                    }
                    TapHandler { onTapped: quickSearch.run(actionItem.index) }

                    Row {
                        anchors { fill: parent; leftMargin: 12; rightMargin: resultList.interactive ? 20 : 12 }
                        spacing: 10
                        AppIcon {
                            id: rowIcon
                            anchors.verticalCenter: parent.verticalCenter
                            name: actionItem.modelData.icon
                            size: Ui.sp(16 / 1.15)
                            color: quickSearch.effectiveSelected === actionItem.index ? Theme.textAccent : Theme.textMuted
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - rowIcon.width - tagText.width - 2 * parent.spacing
                            elide: Text.ElideRight
                            text: actionItem.modelData.label
                            color: quickSearch.effectiveSelected === actionItem.index ? Theme.textAccent : Theme.textSecondary
                            font.pixelSize: Ui.sp(Theme.bodySize)
                        }
                        Text {
                            id: tagText
                            anchors.verticalCenter: parent.verticalCenter
                            text: actionItem.modelData.tag
                            horizontalAlignment: Text.AlignRight
                            color: quickSearch.effectiveSelected === actionItem.index ? Theme.textSecondary : Theme.textMuted
                            font.pixelSize: Ui.sp(Theme.captionSize)
                        }
                    }
                }
            }
        }

        Text {
            width: parent.width
            visible: quickSearch.matches.length > 0
            text: qsTr("Enter to open · ↑↓ to choose")
            color: Theme.textMuted
            font.pixelSize: Ui.sp(Theme.captionSize)
            horizontalAlignment: Text.AlignHCenter
        }
    }
}
