pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import App

Rectangle {
    id: sideBar
    property alias treeView: tree
    property string showName: ""
    property string showLink: ""
    property string showProvider: ""
    property int    epCount: 0
    property string filterText: ""
    property bool   treeHasExpanded: false
    property int    dragFrom: -1
    property int    dropAt: -1
    property real   dropY: 0
    signal hideRequested()
    signal editorReleased()
    color: Theme.surfaceDeep

    onFilterTextChanged: {
        tree.forceLayout()
        if (filterText.length > 0) { tree.contentY = 0; tree.returnToBounds() }
        else scrollToIndex(selection.currentIndex)
    }

    function anyExpanded() {
        for (let r = 0; r < tree.rows; r++)
            if (tree.isExpanded(r)) return true
        return false
    }

    function toggleExpand() {
        if (anyExpanded()) { tree.collapseRecursively(); tree.contentY = 0 }
        else tree.expandRecursively()
        tree.forceLayout()
        treeHasExpanded = anyExpanded()
    }

    function dragTo(point) {
        const last = App.playlist.rowCount() - 1
        const cell = tree.cellAtPosition(point.x, point.y)
        const viewRow = cell.y >= 0 ? cell.y : tree.rowAtIndex(App.playlist.index(point.y < 0 ? 0 : last, 0))
        const item = tree.itemAtCell(Qt.point(0, viewRow))
        if (!item) return
        dropAt = tree.index(viewRow, 0).row
        dropY = tree.contentItem.mapToItem(sideBar, 0, dropAt > dragFrom ? item.y + item.height : item.y).y - 1
    }

    function cancelDrag() {
        if (dragFrom >= 0) endDrag(false)
    }

    function endDrag(drop) {
        const from = dragFrom, to = dropAt
        dragFrom = -1
        dropAt = -1
        tree.forceLayout()
        if (drop && from >= 0 && to >= 0 && from !== to) App.playlist.moveRoot(from, to)
    }

    Rectangle {
        id: header
        anchors { top: parent.top; left: parent.left; right: parent.right }
        height: 92
        gradient: Gradient {
            GradientStop { position: 0.0; color: Theme.surface }
            GradientStop { position: 1.0; color: Theme.surfaceDeep }
        }

        Column {
            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; leftMargin: 14; rightMargin: 8 }
            spacing: 10

            Item {
                width: parent.width
                height: 36

                MarqueeText {
                    anchors { left: parent.left; right: closeBtn.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
                    height: 26
                    text: sideBar.showName.length > 0 ? sideBar.showName : qsTr("Playlist")
                    fontSize: Theme.bodySize
                    color: showNameHover.hovered && sideBar.showLink.length > 0 ? Theme.textAccent : Theme.textPrimary
                    horizontalAlignment: Text.AlignLeft

                    HoverHandler {
                        id: showNameHover
                        enabled: sideBar.showLink.length > 0
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        enabled: sideBar.showLink.length > 0
                        onTapped: App.openShowInfo(sideBar.showLink, sideBar.showProvider, sideBar.showName)
                    }
                }

                IconButton {
                    id: closeBtn
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    iconName: "chevron-right"
                    iconSize: 24
                    boxRadius: 10
                    tip: Ui.tipWithKey(qsTr("Close playlist"), "playlist")
                    onClicked: sideBar.hideRequested()
                }
            }

            Item {
                width: parent.width
                height: 36

                Rectangle {
                    visible: sideBar.epCount > 0
                    anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                    width: Math.min(countText.implicitWidth + 22, parent.width - headerBtns.width - 16)
                    height: 32
                    radius: 16
                    color: Qt.alpha(Theme.accent, 0.12)
                    border.color: Qt.alpha(Theme.accent, 0.30)
                    border.width: 1
                    Text {
                        id: countText
                        anchors { fill: parent; leftMargin: 11; rightMargin: 11 }
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                        text: sideBar.epCount === 1 ? qsTr("1 episode") : qsTr("%1 episodes").arg(sideBar.epCount)
                        color: Theme.textAccent
                        font.pixelSize: Ui.sp(Theme.compactSize)
                        font.weight: Font.Medium
                    }
                }

                Row {
                    id: headerBtns
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    spacing: 4
                    IconButton { iconName: "locate-fixed"; tip: qsTr("Scroll to current"); onClicked: sideBar.scrollToIndex(selection.currentIndex) }
                    IconButton {
                        iconName: sideBar.treeHasExpanded ? "list-chevrons-down-up" : "list-chevrons-up-down"
                        tip: sideBar.treeHasExpanded ? qsTr("Collapse all") : qsTr("Expand all")
                        onClicked: sideBar.toggleExpand()
                    }
                    IconButton { iconName: "list-x"; tip: qsTr("Clear playlist"); onClicked: App.playlist.clear() }
                }
            }
        }

        Rectangle {
            anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
            height: 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 0.5; color: Qt.alpha(Theme.accent, 0.55) }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }
    }

    Connections {
        target: App.playlist
        function onRowsInserted() { sideBar.cancelDrag() }
        function onRowsRemoved() { sideBar.cancelDrag() }
        function onRowsMoved() { sideBar.cancelDrag() }
        function onModelReset() { sideBar.cancelDrag() }
        function onCurrentItemChanged(index) {
            selection.clear()
            selection.setCurrentIndex(index, ItemSelectionModel.NoUpdate)
            let cur = index
            while (cur.valid) {
                selection.select(cur, ItemSelectionModel.Select)
                cur = cur.parent
            }
            tree.collapseRecursively()
            sideBar.scrollToIndex(index)
            sideBar.showName = App.playlist.currentShowName()
            sideBar.epCount = App.playlist.currentShowEpisodeCount()
            sideBar.showLink = App.playlist.currentShowLink()
            sideBar.showProvider = App.playlist.currentShowProvider()
        }
    }

    property var pendingScrollIndex: null

    Timer {
        id: scrollTimer
        interval: 50
        repeat: true
        property int tries: 0
        property real lastWidth: -1
        function begin() { tries = 0; lastWidth = -1; restart() }
        onTriggered: {
            if (!sideBar.pendingScrollIndex) { stop(); return }
            const settled = (sideBar.width === lastWidth)
            lastWidth = sideBar.width
            if (settled && sideBar.applyScroll(sideBar.pendingScrollIndex)) {
                sideBar.pendingScrollIndex = null
                stop()
            } else if (++tries > 40) {
                stop()
            }
        }
    }

    onVisibleChanged: {
        if (visible) scrollToIndex(pendingScrollIndex ? pendingScrollIndex : selection.currentIndex)
        else cancelDrag()
    }

    function applyScroll(index) {
        if (!visible || width <= 0 || tree.height <= 0) return false
        tree.expandToIndex(index)
        tree.forceLayout()
        const row = tree.rowAtIndex(index)
        if (row < 0) return false
        tree.positionViewAtRow(row, TableView.AlignVCenter)
        treeHasExpanded = anyExpanded()
        return true
    }

    function scrollToIndex(index) {
        if (index === undefined || index === null || !index.valid) return
        if (filterText.length > 0 && App.playlist.isFilteredOut(index, filterText))
            filterField.text = ""
        pendingScrollIndex = index
        if (applyScroll(index)) pendingScrollIndex = null
        else if (visible) scrollTimer.begin()
    }

    Rectangle {
        id: filterRow
        anchors { top: header.bottom; left: parent.left; right: parent.right }
        height: 42
        color: Theme.surfaceDeep

        AppTextField {
            id: filterField
            anchors { fill: parent; leftMargin: 8; rightMargin: 8; topMargin: 5; bottomMargin: 5 }
            placeholderText: qsTr("Filter episodes…")
            color: Theme.textPrimary
            placeholderTextColor: Theme.textMuted
            fontSize: Theme.compactSize
            onTextChanged: sideBar.filterText = text
            onUnfocused: sideBar.editorReleased()
        }
    }

    AppMenu {
        id: rowMenu
        modal: true
        minWidth: Ui.sp(150)
        property var index: null
        property bool list: false
        property bool top: false
        property int row: -1
        property bool local: false
        property bool watched: false
        property bool deletable: false
        property string link: ""
        property string pageUrl: ""
        property string showLink: ""
        property string showProvider: ""
        property string showTitle: ""

        function openFor(index, list, depth, local, watched, deletable, link) {
            rowMenu.index = index
            rowMenu.list = list
            rowMenu.top = depth === 0
            rowMenu.row = index.row
            rowMenu.local = local
            rowMenu.watched = watched
            rowMenu.deletable = deletable
            rowMenu.link = link
            pageUrl = App.playlist.pageUrlAt(index)
            showLink = App.playlist.showLinkAt(index)
            showProvider = App.playlist.showProviderAt(index)
            showTitle = App.playlist.showTitleAt(index)
            popup()
        }

        Connections {
            target: App.playlist
            function onRowsInserted() { rowMenu.close() }
            function onRowsRemoved() { rowMenu.close() }
            function onRowsMoved() { rowMenu.close() }
            function onModelReset() { rowMenu.close() }
            function onLayoutChanged() { rowMenu.close() }
        }

        // Menu.removeAction destroys an action, so hide items by shown (AppMenu)
        Action {
            text: qsTr("Play")
            icon.name: "play"
            onTriggered: { Ui.mpv.pause(); App.playlist.loadIndex(rowMenu.index) }
        }
        Action {
            property bool shown: rowMenu.showLink.length > 0
            text: qsTr("Show details")
            icon.name: "details"
            onTriggered: App.openShowInfo(rowMenu.showLink, rowMenu.showProvider, rowMenu.showTitle)
        }
        Action {
            property bool shown: !rowMenu.list && !rowMenu.local && rowMenu.showLink.length > 0
            text: rowMenu.watched ? qsTr("Mark unwatched") : qsTr("Mark watched")
            icon.name: rowMenu.watched ? "eye-off" : "eye"
            onTriggered: App.markPlaylistEpisode(rowMenu.index, !rowMenu.watched)
        }
        Action {
            property bool shown: rowMenu.top && rowMenu.row > 0
            text: qsTr("Move to top")
            icon.name: "arrow-up"
            onTriggered: App.playlist.moveRoot(rowMenu.row, 0)
        }
        Action {
            property bool shown: rowMenu.top && rowMenu.row >= 0 && rowMenu.row < App.playlist.rowCount() - 1
            text: qsTr("Move to bottom")
            icon.name: "arrow-down"
            onTriggered: App.playlist.moveRoot(rowMenu.row, App.playlist.rowCount() - 1)
        }
        Action {
            property bool shown: rowMenu.pageUrl !== ""
            text: qsTr("Copy link")
            icon.name: "link"
            onTriggered: App.copyToClipboard(rowMenu.pageUrl)
        }
        Action {
            property bool shown: rowMenu.local
            text: qsTr("Show in folder")
            icon.name: "folder-open"
            onTriggered: App.downloads.showInFolder(rowMenu.link)
        }
        TextMenu.Separator { shown: rowMenu.deletable }
        Action {
            property bool shown: rowMenu.deletable
            property bool destructive: true
            text: qsTr("Remove")
            icon.name: "x"
            onTriggered: App.playlist.remove(rowMenu.index)
        }
        TextMenu.Separator { shown: rowMenu.top }
        Action {
            property bool shown: rowMenu.top
            property bool destructive: true
            text: qsTr("Remove watched shows")
            icon.name: "list-x"
            onTriggered: App.playlist.removeWatched()
        }
    }

    TreeView {
        id: tree
        model: App.playlist
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        boundsMovement: Flickable.StopAtBounds
        keyNavigationEnabled: false
        smooth: false
        pointerNavigationEnabled: false
        selectionBehavior: TableView.SelectRows

        rowHeightProvider: function(row) {
            if (sideBar.dragFrom >= 0 && tree.depth(row) > 0) return 0
            if (sideBar.filterText.length === 0) return -1
            return App.playlist.isFilteredOut(tree.index(row, 0), sideBar.filterText) ? 0 : -1
        }

        anchors {
            top: filterRow.bottom
            right: parent.right
            left: parent.left
            bottom: parent.bottom
            topMargin: 2
        }

        ScrollBar.vertical: AppScrollBar { width: 9; minimumSize: 0.06 }

        selectionModel: ItemSelectionModel {
            id: selection
            model: App.playlist
        }

        delegate: Item {
            id: del
            implicitWidth: sideBar.width
            implicitHeight: card.height + 4
            clip: true
            visible: !filteredOut

            readonly property real indent: 20
            readonly property real markerX: 8 + depth * indent
            readonly property real markerSize: Ui.sp(18)
            readonly property color markerColor: selected ? Theme.textAccent : isLocal ? Theme.textSecondary : Theme.accent
            readonly property bool filteredOut: App.playlist.isFilteredOut(del.index, sideBar.filterText)
            readonly property bool dragged: depth === 0 && sideBar.dragFrom === index.row

            required property bool isCurrentIndex
            required property bool isLoading
            required property string display
            required property bool isDeletable
            required property bool isWatched
            required property bool isLocal
            required property string link
            required property real progress
            required property TreeView treeView
            required property bool expanded
            required property bool hasChildren
            required property int depth
            required property int row
            required property bool selected
            required property var index

            function activate() {
                if (!del.hasChildren) {
                    Ui.mpv.pause()
                    App.playlist.loadIndex(del.index)
                    return
                }
                if (del.treeView.isExpanded(del.row))
                    del.treeView.collapse(del.row)
                else {
                    del.treeView.expand(del.row)
                    sideBar.scrollToIndex(App.playlist.currentChild(del.index))
                }
                sideBar.treeHasExpanded = sideBar.anyExpanded()
            }
            function openMenu() {
                rowMenu.openFor(del.index, del.hasChildren, del.depth, del.isLocal, del.isWatched, del.isDeletable, del.link)
            }

            TapHandler {
                enabled: del.depth > 0
                acceptedButtons: Qt.LeftButton
                acceptedModifiers: Qt.NoModifier
                onTapped: (point) => {
                    if (!delBtn.visible || !delBtn.contains(delBtn.mapFromItem(del, point.position)))
                        del.activate()
                }
            }
            TapHandler {
                enabled: del.depth > 0
                acceptedButtons: Qt.RightButton
                onTapped: del.openMenu()
            }

            Rectangle {
                id: card
                anchors {
                    left: parent.left
                    right: parent.right
                    verticalCenter: parent.verticalCenter
                    leftMargin: 4
                    rightMargin: 4
                }
                height: Math.max(del.hasChildren ? 34 : 40, itemText.height + 16)
                radius: 6
                color: del.dragged ? Qt.alpha(Theme.accent, 0.22)
                     : del.selected ? Qt.alpha(Theme.accent, 0.14)
                     : del.isLoading ? Qt.alpha(Theme.accent, 0.1)
                     : cardHover.hovered ? Qt.alpha(Theme.accent, 0.08) : "transparent"
                border.color: del.selected || del.dragged ? Qt.alpha(Theme.accent, 0.35)
                            : del.hasChildren ? Theme.border : "transparent"
                border.width: (del.selected || del.hasChildren) ? 1 : 0
                Behavior on color { ColorAnimation { duration: 130 } }

                HoverHandler { id: cardHover }

                // preventStealing keeps the TreeView from flicking
                MouseArea {
                    anchors.fill: parent
                    enabled: del.depth === 0
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    preventStealing: true
                    cursorShape: sideBar.dragFrom >= 0 ? Qt.ClosedHandCursor : Qt.ArrowCursor
                    property real startY: 0
                    property bool moved: false
                    onPressed: (mouse) => { startY = mouse.y; moved = false }
                    onPositionChanged: (mouse) => {
                        if (!pressed || !(pressedButtons & Qt.LeftButton)) return
                        if (sideBar.dragFrom < 0) {
                            if (Math.abs(mouse.y - startY) < Ui.sp(8)) return
                            moved = true
                            sideBar.dragFrom = del.index.row
                            tree.forceLayout()
                        }
                        sideBar.dragTo(mapToItem(tree.contentItem, mouse.x, mouse.y))
                    }
                    onReleased: if (sideBar.dragFrom >= 0) sideBar.endDrag(true)
                    onCanceled: sideBar.cancelDrag()
                    onClicked: (mouse) => {
                        if (moved) return
                        if (mouse.button === Qt.RightButton) del.openMenu()
                        else del.activate()
                    }
                }

                AccentBar { visible: (del.selected || del.isLoading) && !del.hasChildren }

                AppIcon {
                    visible: del.hasChildren && !del.isLocal
                    x: del.markerX
                    anchors.verticalCenter: parent.verticalCenter
                    name: "chevron-right"
                    size: del.markerSize
                    rotation: del.expanded ? 90 : 0
                    color: del.markerColor
                    Behavior on rotation { NumberAnimation { duration: 120 } }
                }
                AppIcon {
                    visible: del.isLocal
                    x: del.markerX
                    anchors.verticalCenter: parent.verticalCenter
                    name: del.expanded ? "folder-open" : "folder"
                    size: del.markerSize
                    color: del.markerColor
                }

                Text {
                    id: itemText
                    x: del.markerX + del.markerSize + Ui.sp(6)
                    width: card.width - x - 38
                    anchors.verticalCenter: parent.verticalCenter
                    maximumLineCount: 2
                    text: del.display
                    elide: Text.ElideRight
                    wrapMode: Text.WordWrap
                    readonly property bool scrolls: cardHover.hovered && truncated && !Theme.reduceMotion
                    opacity: scrolls ? 0 : 1
                    font.pixelSize: Ui.sp(Theme.bodySize)
                    color: del.selected       ? Theme.textAccent
                         : del.isCurrentIndex ? Theme.textSuccess
                         : del.isWatched      ? Theme.textMuted
                         : Theme.textSecondary
                }

                MarqueeText {
                    x: itemText.x
                    width: itemText.width
                    anchors.verticalCenter: parent.verticalCenter
                    visible: itemText.scrolls
                    text: del.display
                    fontSize: Theme.bodySize
                    color: itemText.color
                    horizontalAlignment: Text.AlignLeft
                    alwaysScroll: false
                    active: true
                    spacing: 30
                    marqueeSpeed: 50
                }

                Rectangle {
                    visible: !del.hasChildren && del.progress > 0.02 && del.progress < 0.97
                    anchors { left: parent.left; bottom: parent.bottom; leftMargin: 6; bottomMargin: 2 }
                    width: (card.width - 12) * del.progress
                    height: 2
                    radius: 1
                    color: Qt.alpha(Theme.accent, del.isCurrentIndex ? 0.9 : 0.55)
                }

                AppIcon {
                    visible: del.isWatched && !del.hasChildren && !del.isDeletable && !del.isCurrentIndex
                    name: "eye"
                    size: 16
                    color: Theme.textMuted
                    anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
                }

                IconButton {
                    id: delBtn
                    visible: !del.selected && del.isDeletable
                    implicitWidth: 28
                    implicitHeight: 28
                    boxRadius: 7
                    iconName: "x"
                    iconSize: 16
                    destructive: true
                    tip: qsTr("Remove")
                    anchors {
                        right: parent.right
                        rightMargin: 2
                        verticalCenter: parent.verticalCenter
                    }
                    onClicked: App.playlist.remove(del.index)
                }
            }
        }
    }

    Rectangle {
        visible: sideBar.dragFrom >= 0 && sideBar.dropAt >= 0 && sideBar.dropAt !== sideBar.dragFrom
        x: 8
        y: sideBar.dropY
        z: 2
        width: sideBar.width - 16
        height: 2
        radius: 1
        color: Theme.accent
    }
}
