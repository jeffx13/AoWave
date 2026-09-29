pragma ComponentBehavior: Bound
import QtQuick
import ".."
import App

// Ctrl+K: commands, then shows from the library and history, then settings, then a search of
// the providers.
Rectangle {
    id: quickSearch

    property bool open: false
    signal searched(string query)
    signal actionRequested(string id)
    signal showRequested(string link, string provider, string title)
    signal settingRequested(string label)
    // main.qml's: settings by name, [{label, card}].
    property var findSettings: null

    color: Theme.scrim
    visible: opacity > 0.01
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 120 } }
    onOpenChanged: if (open) { qsField.text = ""; selected = 0; qsField.forceActiveFocus() }

    property int selected: 0

    // Guarded: this list is built before Globals.mpv exists, and a binding that throws leaves
    // the property undefined, which takes the whole palette down.
    readonly property bool hasPlayback: Globals.mpv !== null && Globals.mpv !== undefined
                                        && Globals.mpv.duration > 0
    readonly property string currentShowLink: (App.show && App.show.link) ? App.show.link : ""

    // `id` is what main.qml switches on; `when` hides an action that cannot run.
    readonly property var actions: [
        { id: "markWatched",  icon: "check",         label: qsTr("Mark current episode watched"),
          keys: "seen complete", when: quickSearch.hasPlayback },
        { id: "toggleDanmaku", icon: "captions",     label: qsTr("Toggle danmaku"),
          keys: "comments bullet", when: true },
        { id: "danmakuSettings", icon: "settings",   label: qsTr("Danmaku settings"),
          keys: "comments opacity speed", when: true },
        { id: "migrate",      icon: "server",        label: qsTr("Migrate this show to another provider"),
          keys: "switch source", when: quickSearch.currentShowLink !== "" },
        { id: "openMpvConf",  icon: "details",       label: qsTr("Open mpv.conf"),
          keys: "config player", when: true },
        { id: "openSettings", icon: "settings",      label: qsTr("Open settings.ini"),
          keys: "config ini", when: true },
        { id: "gotoLogs",     icon: "list-checks",   label: qsTr("Jump to logs"),
          keys: "errors warnings debug", when: true },
        { id: "gotoDownloads", icon: "download",     label: qsTr("Jump to downloads"),
          keys: "queue", when: true },
        { id: "connectTracker", icon: "star",        label: qsTr("Connect a watch-list service"),
          keys: "anilist mal trakt sign in", when: true },
        { id: "reloadEpisode", icon: "refresh-cw",   label: qsTr("Reload current episode"),
          keys: "refresh restart", when: quickSearch.hasPlayback }
    ]

    readonly property var matches: {
        const text = qsField.text.trim()
        const q = text.toLowerCase()
        if (q.length === 0) return []
        // Where the words land: the start of a name, then the start of a word in it, then anywhere
        // in it, and last a command found only by its hidden keywords. Kinds keep their order within.
        const rank = label => {
            const name = label.toLowerCase()
            if (name.startsWith(q)) return 0
            if (name.split(/[\s\-:\u00b7]+/).some(word => word.startsWith(q))) return 1
            return name.indexOf(q) >= 0 ? 2 : 3
        }
        const found = (quickSearch.actions || [])
            .filter(a => a.when && (a.label.toLowerCase().indexOf(q) >= 0 || (a.keys || "").indexOf(q) >= 0))
            .map(a => ({ kind: "action", id: a.id, icon: a.icon, label: a.label, tag: "" }))
        for (const show of App.library.findShows(text, 5))
            found.push({ kind: "show", icon: show.libraryType >= 0 ? "library" : "history", label: show.title,
                         tag: show.libraryType >= 0 ? Globals.libraryTypeNames[show.libraryType] : qsTr("History"),
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
        if (match.kind === "action") quickSearch.actionRequested(match.id)
        else if (match.kind === "show") quickSearch.showRequested(match.link, match.provider, match.label)
        else if (match.kind === "setting") quickSearch.settingRequested(match.label)
        else quickSearch.searched(qsField.text)
    }

    MouseArea { anchors.fill: parent; onClicked: quickSearch.open = false }

    Column {
        anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: parent.height * 0.18 }
        width: Math.min(560, quickSearch.width - 80)
        spacing: 8

        Rectangle {
            width: parent.width
            height: 58
            radius: 14
            color: Theme.surface
            border.color: Theme.accent
            border.width: 1

            MouseArea { anchors.fill: parent }   // swallow clicks so the box stays open

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
                placeholderText: qsTr("Search, or type a command...")
                background: null
                onTextChanged: quickSearch.selected = 0
                // The last row is always the providers' search.
                onAccepted: quickSearch.run(quickSearch.selected >= 0 ? quickSearch.selected : quickSearch.matches.length - 1)
                Keys.onDownPressed: if (quickSearch.matches.length > 0)
                    quickSearch.selected = Math.min(quickSearch.selected + 1, quickSearch.matches.length - 1)
                Keys.onUpPressed: quickSearch.selected = Math.max(quickSearch.selected - 1, -1)
                // AppTextField consumes Escape to drop focus.
                onUnfocused: quickSearch.open = false
            }
        }

        Rectangle {
            width: parent.width
            visible: quickSearch.matches.length > 0
            height: visible ? actionCol.implicitHeight + 10 : 0
            radius: 12
            color: Theme.surface
            border.color: Theme.border
            border.width: 1
            MouseArea { anchors.fill: parent }

            Column {
                id: actionCol
                anchors { fill: parent; margins: 5 }
                spacing: 1

                Repeater {
                    model: quickSearch.matches
                    delegate: Rectangle {
                        id: actionItem
                        required property var modelData
                        required property int index
                        width: actionCol.width
                        height: 38
                        radius: 8
                        color: quickSearch.selected === actionItem.index ? Qt.alpha(Theme.accent, 0.16)
                             : actionHover.hovered ? Qt.alpha(Theme.accent, 0.08) : "transparent"

                        HoverHandler {
                            id: actionHover
                            cursorShape: Qt.PointingHandCursor
                            onHoveredChanged: if (hovered) quickSearch.selected = actionItem.index
                        }
                        TapHandler { onTapped: quickSearch.run(actionItem.index) }

                        Row {
                            anchors { fill: parent; leftMargin: 12; rightMargin: 12 }
                            spacing: 10
                            AppIcon {
                                anchors.verticalCenter: parent.verticalCenter
                                name: actionItem.modelData.icon
                                size: 16
                                color: quickSearch.selected === actionItem.index ? Theme.textAccent : Theme.textMuted
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: Math.min(implicitWidth, actionCol.width - 60 - tagText.implicitWidth)
                                elide: Text.ElideRight
                                text: actionItem.modelData.label
                                color: quickSearch.selected === actionItem.index ? Theme.textAccent : Theme.textSecondary
                                font.pixelSize: Globals.sp(Theme.bodySize)
                            }
                            Text {
                                id: tagText
                                anchors.verticalCenter: parent.verticalCenter
                                text: actionItem.modelData.tag
                                color: Theme.textMuted
                                font.pixelSize: Globals.sp(13)
                            }
                        }
                    }
                }
            }
        }

        Text {
            width: parent.width
            visible: qsField.text.trim().length > 0
            text: qsTr("Enter opens the highlighted row \u00b7 \u2191\u2193 to choose")
            color: Theme.textMuted
            font.pixelSize: Globals.sp(13)
            horizontalAlignment: Text.AlignHCenter
        }
    }
}
