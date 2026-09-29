pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../Components"
import App
import ".."

Popup {
    id: panel
    required property MpvPlayer player
    visible: false
    // Not modal, and not closed by a press in the player: the control bar's Servers and Settings
    // switch its page or close it, and MpvPage closes it on a click on the video.
    modal: false
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

    // A few groups along the top, their pages in a second row when there is more than one.
    // `page` indexes the StackLayout; a page with a model lists it, and hides while it is empty.
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
            { id: "substyle",  label: qsTr("Style"),  model: null, page: 6 } ] },
        { id: "picture", label: qsTr("Picture"), pages: [
            { id: "picture", label: qsTr("Picture"), model: null, page: 7 } ] },
        { id: "playback", label: qsTr("Playback"), pages: [
            { id: "general", label: qsTr("General"), model: null, page: 1 },
            { id: "skip",    label: qsTr("Skip"),    model: null, page: 2 },
            { id: "danmaku", label: qsTr("Danmaku"), model: null, page: 5 } ] }
    ]

    function pageShown(page) {
        // Most episodes have no comments at all.
        if (page.id === "danmaku") return panel.player.danmakuHeat.length > 0
        if (page.id === "picture") return App.settings.pictureTab
        // Subtitles stay with no tracks: that is when you go looking.
        if (!page.model || page.id === "subs") return true
        return page.model.count > (page.id === "servers" ? 1 : 0)
    }
    readonly property var visibleGroups: allGroups
        .map(group => ({ id: group.id, label: group.label, pages: group.pages.filter(panel.pageShown) }))
        .filter(group => group.pages.length > 0)

    // A page id; MpvPage sets it to open the panel at a page.
    property string activeTabId: "servers"
    readonly property int activeGroupIndex: visibleGroups.findIndex(group => group.pages.some(page => page.id === activeTabId))
    readonly property var activeGroup: activeGroupIndex >= 0 ? visibleGroups[activeGroupIndex] : null
    readonly property var activeTab: activeGroup ? activeGroup.pages.find(page => page.id === activeTabId) : null
    // Each group reopens on the page last used in it.
    property var lastPageOfGroup: ({})

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
    // A hidden page gives way to its group's next one, then the quality list, then any.
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
        if (activeTabId === "subsearch") subtitleSearchTab.refresh()
    }

    background: Rectangle {
        radius: 18
        color: Theme.overlayScrim
        border.color: Theme.overlayFillHover
        border.width: 1

        component EdgeGlow: Rectangle {
            id: glow
            property color tint: Theme.accentStrong
            height: 1
            radius: 1
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 0.5; color: glow.tint }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }

        EdgeGlow {
            anchors { top: parent.top; left: parent.left; right: parent.right
                      topMargin: 1; leftMargin: 20; rightMargin: 20 }
        }
        EdgeGlow {
            tint: Theme.accentSoft
            anchors { bottom: parent.bottom; left: parent.left; right: parent.right
                      bottomMargin: 1; leftMargin: 40; rightMargin: 40 }
        }
    }

    // Opacity-only: a scale animation re-rasterises the panel over live video.
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 160; easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 120; easing.type: Easing.InCubic }
    }

    onOpened: {
        syncActiveTab()
        Qt.callLater(() => { trackTab.centerCurrent(); serverTab.centerCurrent() })
    }

    contentItem: ColumnLayout {
        spacing: 0

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

                    // Measured bold even when inactive, or widths shuffle on every click.
                    FontMetrics { id: tabFm; font.pixelSize: Globals.sp(18); font.bold: true }

                    // Uniform tabs elide the long labels long before the short ones need room.
                    readonly property var tabWidths: {
                        const n = panel.visibleGroups.length
                        if (n <= 0 || width <= 0) return []
                        const avail = width - (n - 1) * spacing
                        let natural = [], total = 0
                        for (let i = 0; i < n; i++) {
                            const entry = panel.visibleGroups[i]
                            // 16 for padding, 28 for a count badge and its gap.
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

                    // The Repeater stacks itself into children[].
                    function tabStart(index) {
                        let x = 0
                        for (let k = 0; k < index && k < tabWidths.length; k++) x += tabWidths[k] + spacing
                        return x
                    }

                    Repeater {
                        // The array, not its length: indexing reads past the end while delegates lag.
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
                            onClicked: panel.openGroup(tab.modelData)

                            background: Rectangle {
                                radius: 10
                                color: tab.isActive ? Theme.accent : (tab.hovered ? Theme.overlayFill : "transparent")
                                Behavior on color { ColorAnimation { duration: 120 } }

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
                                        font.pixelSize: Globals.sp(18)
                                        font.bold: tab.isActive
                                        color: tab.isActive ? Theme.onOverlay : (tab.hovered ? Theme.onOverlayDim : Theme.onOverlayFaint)
                                        elide: Text.ElideRight
                                        width: Math.min(implicitWidth, tab.width - 14 - (tabCount.visible ? tabCount.width + 5 : 0))
                                        Behavior on color { ColorAnimation { duration: 120 } }
                                    }
                                    Rectangle {
                                        id: tabCount
                                        visible: tab.itemCount >= 0
                                        anchors.verticalCenter: parent.verticalCenter
                                        // Min width fits two digits.
                                        width: Math.max(20, tabCountText.implicitWidth + 8)
                                        height: 18
                                        radius: 9
                                        color: tab.isActive ? Theme.overlayFillActive : Theme.overlayFill
                                        Text {
                                            id: tabCountText
                                            anchors.centerIn: parent
                                            text: tab.itemCount
                                            font.pixelSize: Globals.sp(14)
                                            font.weight: Font.Medium
                                            color: tab.isActive ? Theme.onOverlay : Theme.onOverlayFaint
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {
                height: 3
                radius: 1.5
                color: Theme.accent
                y: parent.height - 1

                // Follows the measured row.
                readonly property real slotWidth: tabRow.tabWidths[panel.activeGroupIndex] || 0
                x: tabRow.x + tabRow.tabStart(panel.activeGroupIndex) + 12
                width: Math.max(0, slotWidth - 24)

                Behavior on x     { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
                Behavior on width { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }

                Rectangle {
                    anchors {
                        horizontalCenter: parent.horizontalCenter
                        top: parent.top
                    }
                    width: parent.width * 0.6
                    height: 6
                    radius: 3
                    color: Theme.accentMuted
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
                delegate: Rectangle {
                    id: subTab
                    required property var modelData
                    readonly property bool selected: !!panel.activeTab && panel.activeTab.id === modelData.id

                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    radius: 9
                    color: selected            ? Qt.alpha(Theme.accent, 0.22)
                         : subTabHover.hovered ? Qt.alpha(Theme.onOverlay, 0.07)
                                               : "transparent"
                    Behavior on color { ColorAnimation { duration: 120 } }

                    Text {
                        anchors.centerIn: parent
                        text: subTab.modelData.model && subTab.modelData.model.count > 0
                              ? subTab.modelData.label + "  " + subTab.modelData.model.count : subTab.modelData.label
                        color: subTab.selected ? Theme.accent : Theme.onOverlayMuted
                        font.pixelSize: Globals.sp(18)
                        font.bold: subTab.selected
                    }

                    HoverHandler { id: subTabHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: panel.activeTabId = subTab.modelData.id }
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: 8
            currentIndex: panel.activeTab ? panel.activeTab.page : panel.trackPage

            // One list for all three track kinds.
            TrackTab {
                id: trackTab
                readonly property var tab: (panel.activeTab && panel.activeTab.page === panel.trackPage)
                                           ? panel.activeTab : null
                player: panel.player
                kind: tab ? tab.id : ""
                trackModel: tab ? tab.model : null
            }

            PlaybackTab { player: panel.player }

            SkipTab { player: panel.player }

            ServerTab { id: serverTab }

            SubtitleSearchTab { id: subtitleSearchTab }

            DanmakuTab { player: panel.player }

            SubStyleTab {}

            PictureTab {}
        }

        // Otherwise a fetched subtitle holds a slot with no row to clear it.
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
                            color: Theme.accent
                            font.pixelSize: Globals.sp(13)
                            font.bold: true
                        }
                        Text {
                            Layout.fillWidth: true
                            text: panel.player.subNameForId(slotChip.slotId)
                            color: Theme.onOverlayMuted
                            font.pixelSize: Globals.sp(14)
                            elide: Text.ElideMiddle
                        }
                        AppIcon {
                            name: "x"
                            size: 13
                            color: clearOne.hovered ? Theme.danger : Theme.onOverlayDim
                            HoverHandler { id: clearOne; cursorShape: Qt.PointingHandCursor }
                            TapHandler {
                                onTapped: slotChip.modelData === 1 ? panel.player.setPrimarySub(0)
                                                                   : panel.player.setSecondarySub(0)
                            }
                        }
                    }
                }
            }

            Text {
                text: qsTr("Clear")
                color: clearBoth.hovered ? Theme.accent : Theme.onOverlayDim
                font.pixelSize: Globals.sp(14)
                visible: panel.player.primarySubId !== 0 && panel.player.secondarySubId !== 0
                HoverHandler { id: clearBoth; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: panel.player.clearSubs() }
            }
        }
    }
}
