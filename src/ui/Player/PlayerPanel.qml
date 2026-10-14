pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App

Popup {
    id: panel
    required property MpvPlayer player
    visible: false
    modal: false
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
    signal editorReleased()

    readonly property int trackPage: 0
    readonly property var allGroups: [
        { id: "source", label: qsTr("Source"), pages: [
            { id: "servers", label: qsTr("Servers"), model: App.playlist.serverList, page: 3 },
            { id: "video",   label: qsTr("Quality"), model: panel.player.videoList,  page: panel.trackPage } ] },
        { id: "audio", label: qsTr("Audio"), pages: [
            { id: "audio", label: qsTr("Audio"), model: panel.player.audioList, page: panel.trackPage } ] },
        { id: "subtitles", label: qsTr("Subtitles"), pages: [
            { id: "subs",      label: qsTr("Tracks"), model: panel.player.subtitleList, page: panel.trackPage },
            { id: "subsearch", label: qsTr("Search"), model: null, page: 4 },
            { id: "substyle",  label: qsTr("Style"),  model: null, page: 6 },
            { id: "danmaku",   label: qsTr("Danmaku"), model: null, page: 5 } ] },
        { id: "picture", label: qsTr("Picture"), pages: [
            { id: "picture", label: qsTr("Picture"), model: null, page: 7 } ] },
        { id: "playback", label: qsTr("Playback"), pages: [
            { id: "general", label: qsTr("General"), model: null, page: 1 },
            { id: "skip",    label: qsTr("Skip"),    model: null, page: 2 } ] }
    ]

    function pageShown(page) {
        if (page.id === "danmaku")
            return panel.player.danmakuHeat.length > 0 || (panel.player.hasDanmaku && !App.settings.danmakuEnabled)
        if (page.id === "picture") return App.settings.pictureTab
        if (!page.model || page.id === "subs") return true
        return page.model.count > (page.id === "servers" ? 1 : 0)
    }
    readonly property var visibleGroups: allGroups
        .map(group => ({ id: group.id, label: group.label, pages: group.pages.filter(panel.pageShown) }))
        .filter(group => group.pages.length > 0)

    property string activeTabId: "servers"
    readonly property int activeGroupIndex: visibleGroups.findIndex(group => group.pages.some(page => page.id === activeTabId))
    readonly property var activeGroup: activeGroupIndex >= 0 ? visibleGroups[activeGroupIndex] : null
    readonly property var activeTab: activeGroup ? activeGroup.pages.find(page => page.id === activeTabId) : null
    property var lastPageOfGroup: ({})

    property bool tuning: false
    readonly property real tuningOpacity: tuning && opened ? 0.25 : 1

    function sliderAt(point) {
        let item = contentItem, x = point.x, y = point.y
        while (item) {
            if (item instanceof Slider) return true
            const child = item.childAt(x, y)
            if (!child) return false
            const local = item.mapToItem(child, x, y)
            item = child; x = local.x; y = local.y
        }
        return false
    }

    function groupCount(group) {
        return group.id === "audio" || group.id === "subtitles" ? group.pages[0].model.count : -1
    }
    function groupOf(pageId) {
        return allGroups.find(group => group.pages.some(page => page.id === pageId))
    }
    function openGroup(group) {
        const last = lastPageOfGroup[group.id]
        activeTabId = group.pages.some(page => page.id === last) ? last : group.pages[0].id
    }
    function shownPage(pageId) {
        if (visibleGroups.some(shown => shown.pages.some(page => page.id === pageId))) return pageId
        const group = groupOf(pageId)
        const sibling = group ? visibleGroups.find(shown => shown.id === group.id) : null
        const video = visibleGroups.find(shown => shown.pages.some(page => page.id === "video"))
        return sibling ? sibling.pages[0].id : video ? "video"
             : visibleGroups.length > 0 ? visibleGroups[0].pages[0].id : ""
    }
    function syncActiveTab() {
        if (!activeTab) activeTabId = shownPage(activeTabId)
    }
    onVisibleGroupsChanged: syncActiveTab()
    onActiveTabIdChanged: {
        const group = groupOf(activeTabId)
        if (group) lastPageOfGroup[group.id] = activeTabId
        if (activeTabId === "subsearch") refreshSearch()
    }

    background: Rectangle {
        radius: 18
        color: Theme.overlayScrim
        border.color: Theme.overlayFillHover
        border.width: 1
        opacity: panel.tuningOpacity
        Behavior on opacity { NumberAnimation { duration: Theme.dur(120); easing.type: Easing.OutCubic } }

    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.dur(160); easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.dur(120); easing.type: Easing.InCubic }
    }

    function refreshSearch() {
        const search = subtitleSearchLoader.item as SubtitleSearchTab
        if (search) search.refresh()
    }
    function centerLists() {
        const tracks = trackLoader.item as TrackTab
        const servers = serverLoader.item as ServerTab
        if (tracks) tracks.centerCurrent()
        if (servers) servers.centerCurrent()
    }
    onOpened: {
        tuning = false
        syncActiveTab()
        Qt.callLater(centerLists)
        if (activeTabId === "subsearch") refreshSearch()
    }
    Connections {
        target: App.playlist
        function onCurrentItemChanged() { if (panel.opened && panel.activeTabId === "subsearch") panel.refreshSearch() }
    }

    contentItem: ColumnLayout {
        readonly property bool overlaySurface: true
        spacing: 0
        opacity: panel.tuningOpacity
        Behavior on opacity { NumberAnimation { duration: Theme.dur(120); easing.type: Easing.OutCubic } }

        PointHandler {
            acceptedButtons: Qt.LeftButton
            onActiveChanged: panel.tuning = active && panel.sliderAt(point.position)
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 42
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            Layout.topMargin: 10

            Rectangle {
                anchors.fill: parent
                radius: 12
                color: Theme.overlayFillSoft

                Row {
                    id: tabRow
                    anchors {
                        fill: parent
                        margins: 3
                    }
                    spacing: 2

                    FontMetrics { id: tabFm; font.pixelSize: Ui.sp(Theme.compactSize); font.bold: true }

                    readonly property var tabWidths: {
                        const n = panel.visibleGroups.length
                        if (n <= 0 || width <= 0) return []
                        const avail = width - (n - 1) * spacing
                        let natural = [], total = 0
                        for (let i = 0; i < n; i++) {
                            const entry = panel.visibleGroups[i]
                            const w = tabFm.advanceWidth(entry.label) + 16 + (panel.groupCount(entry) >= 0 ? 28 : 0)
                            natural.push(w)
                            total += w
                        }
                        if (total <= 0) return []
                        if (total <= avail) {
                            const extra = (avail - total) / n
                            return natural.map(function(w) { return w + extra })
                        }
                        return natural.map(function(w) { return w * avail / total })
                    }


                    Repeater {
                        model: panel.visibleGroups
                        delegate: AbstractButton {
                            id: tab
                            required property var modelData
                            required property int index
                            readonly property bool isActive: panel.activeGroupIndex === tab.index
                            readonly property int itemCount: panel.groupCount(tab.modelData)

                            width: tabRow.tabWidths[tab.index] || 0
                            height: tabRow.height
                            focusPolicy: Qt.NoFocus
                            text: tab.modelData.label
                            Accessible.role: Accessible.PageTab
                            Accessible.selected: tab.isActive
                            onClicked: panel.openGroup(tab.modelData)

                            background: Rectangle {
                                radius: 10
                                color: tab.isActive ? Theme.accent : (tab.hovered ? Theme.overlayFill : "transparent")
                                Behavior on color { ColorAnimation { duration: Theme.dur(120) } }

                                Rectangle {
                                    visible: tab.isActive
                                    anchors {
                                        top: parent.top
                                        left: parent.left
                                        right: parent.right
                                        topMargin: 1
                                        leftMargin: 8
                                        rightMargin: 8
                                    }
                                    height: 1
                                    radius: 1
                                    color: Theme.overlayFillActive
                                }
                            }

                            contentItem: Item {
                                Row {
                                    anchors.centerIn: parent
                                    spacing: 5
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: tab.modelData.label
                                        font.pixelSize: Ui.sp(Theme.compactSize)
                                        font.bold: tab.isActive
                                        color: tab.isActive ? Theme.accentInk : (tab.hovered ? Theme.onOverlayDim : Theme.onOverlayFaint)
                                        elide: Text.ElideRight
                                        width: Math.min(implicitWidth, tab.width - 14 - (tabCount.visible ? tabCount.width + 5 : 0))
                                        Behavior on color { ColorAnimation { duration: Theme.dur(120) } }
                                    }
                                    Rectangle {
                                        id: tabCount
                                        visible: tab.itemCount >= 0
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: Math.max(20, tabCountText.implicitWidth + 8)
                                        height: 18
                                        radius: 9
                                        color: tab.isActive ? Theme.overlayFillActive : Theme.overlayFill
                                        Text {
                                            id: tabCountText
                                            anchors.centerIn: parent
                                            text: tab.itemCount
                                            font.pixelSize: Ui.sp(Theme.captionSize)
                                            font.weight: Font.Medium
                                            color: tab.isActive ? Theme.accentInk : Theme.onOverlayFaint
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 10
            spacing: 8
            visible: !!panel.activeGroup && panel.activeGroup.pages.length > 1

            Repeater {
                model: panel.activeGroup ? panel.activeGroup.pages : []
                delegate: AbstractButton {
                    id: subTab
                    required property var modelData
                    readonly property bool selected: !!panel.activeTab && panel.activeTab.id === modelData.id

                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    focusPolicy: Qt.NoFocus
                    hoverEnabled: true
                    text: modelData.label
                    Accessible.role: Accessible.PageTab
                    Accessible.selected: selected
                    onClicked: panel.activeTabId = subTab.modelData.id

                    background: Rectangle {
                        radius: 9
                        color: subTab.selected ? Qt.alpha(Theme.accent, 0.22)
                             : subTab.hovered  ? Theme.overlayFill
                                               : "transparent"
                        Behavior on color { ColorAnimation { duration: Theme.dur(120) } }
                    }

                    contentItem: Text {
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: subTab.modelData.model && subTab.modelData.model.count > 0
                              ? subTab.modelData.label + "  " + subTab.modelData.model.count : subTab.modelData.label
                        color: subTab.selected ? Theme.onOverlayAccent : Theme.onOverlayMuted
                        font.pixelSize: Ui.sp(Theme.compactSize)
                        font.bold: subTab.selected
                    }

                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: 8
            currentIndex: panel.activeTab ? panel.activeTab.page : panel.trackPage

            component Lazy: Loader {
                active: false
                property bool shown: panel.visible && StackLayout.isCurrentItem
                onShownChanged: if (shown) active = true
            }

            Lazy {
                id: trackLoader
                sourceComponent: TrackTab {
                    readonly property var tab: (panel.activeTab && panel.activeTab.page === panel.trackPage)
                                               ? panel.activeTab : null
                    player: panel.player
                    kind: tab ? tab.id : ""
                    trackModel: tab ? tab.model : null
                }
                onLoaded: Qt.callLater(panel.centerLists)
            }

            Lazy { sourceComponent: PlaybackTab { player: panel.player } }

            Lazy { sourceComponent: SkipTab { player: panel.player; onEditorReleased: panel.editorReleased() } }

            Lazy {
                id: serverLoader
                sourceComponent: ServerTab {}
                onLoaded: Qt.callLater(panel.centerLists)
            }

            Lazy {
                id: subtitleSearchLoader
                sourceComponent: SubtitleSearchTab { onEditorReleased: panel.editorReleased() }
                onLoaded: if (panel.activeTabId === "subsearch") panel.refreshSearch()
            }

            Lazy { sourceComponent: DanmakuTab { player: panel.player } }

            Lazy { sourceComponent: SubStyleTab {} }

            Lazy { sourceComponent: PictureTab {} }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 6
            Layout.bottomMargin: 10
            spacing: 8
            visible: panel.activeTab && panel.activeTab.id === "subs"
                     && (panel.player.primarySubId !== 0 || panel.player.secondarySubId !== 0)

            Repeater {
                model: [1, 2]
                delegate: Rectangle {
                    id: slotChip
                    required property int modelData
                    readonly property int slotId: modelData === 1 ? panel.player.primarySubId
                                                                  : panel.player.secondarySubId
                    visible: slotId !== 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    radius: 8
                    color: Qt.alpha(Theme.accent, 0.14)

                    RowLayout {
                        anchors { fill: parent; leftMargin: 9; rightMargin: 6 }
                        spacing: 6

                        Text {
                            text: slotChip.modelData
                            color: Theme.onOverlayAccent
                            font.pixelSize: Ui.sp(Theme.captionSize)
                            font.bold: true
                        }
                        Text {
                            Layout.fillWidth: true
                            text: panel.player.subNameForId(slotChip.slotId)
                            color: Theme.onOverlayMuted
                            font.pixelSize: Ui.sp(Theme.captionSize)
                            elide: Text.ElideMiddle
                        }
                        IconButton {
                            implicitWidth: Ui.sp(24)
                            implicitHeight: Ui.sp(24)
                            boxRadius: 6
                            variant: "overlay"
                            iconName: "x"
                            iconSize: Ui.sp(13)
                            iconColor: Theme.onOverlayDim
                            iconHoverColor: Theme.danger
                            focusPolicy: Qt.NoFocus
                            tip: slotChip.modelData === 1 ? qsTr("Turn off primary subtitle") : qsTr("Turn off secondary subtitle")
                            onClicked: slotChip.modelData === 1 ? panel.player.setPrimarySub(0)
                                                                : panel.player.setSecondarySub(0)
                        }
                    }
                }
            }

            AbstractButton {
                id: clearBoth
                visible: panel.player.primarySubId !== 0 && panel.player.secondarySubId !== 0
                Layout.fillHeight: true
                leftPadding: 6
                rightPadding: 6
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                text: qsTr("Clear")
                Accessible.description: qsTr("Turn off both subtitles")
                onClicked: panel.player.clearSubs()
                contentItem: Text {
                    verticalAlignment: Text.AlignVCenter
                    text: clearBoth.text
                    color: clearBoth.hovered ? Theme.onOverlayAccent : Theme.onOverlayDim
                    font.pixelSize: Ui.sp(Theme.captionSize)
                }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
            }
        }
    }
}
