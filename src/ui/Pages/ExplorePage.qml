pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App

Item {
    id: explorerPage
    focus: true
    // a Tab stop of its own, so Tab goes on to the search bar and Shift+Tab comes back to the grid
    activeFocusOnTab: true

    property int browseMode: 0
    property string searchedFor: ""

    component ToolbarButton: AppButton {
        fontSize: Theme.bodySize
        radius: 10
        Layout.fillHeight: true
        leftPadding: 16
        rightPadding: 16
    }

    component ToolbarComboBox: AppComboBox {
        fontSize: Theme.bodySize
        Layout.fillHeight: true
    }

    function search() {
        let q = searchTextField.text.trim()
        if (q.length > 0) {
            historyPopup.release(q)
            App.settings.prependToHistory("search/history", q)
        }
        historyPopup.close()
        explorerPage.forceActiveFocus()
        searchedFor = q
        if (q.length > 0) { browseMode = 2; App.search(q, Ui.explorerFilters) }
        else              { browseMode = 1; App.browse(false, Ui.explorerFilters) }
    }
    function browse(latest) {
        explorerPage.forceActiveFocus()
        browseMode = latest ? 0 : 1
        App.browse(latest, Ui.explorerFilters)
    }
    function applyPendingSearch() {
        const query = Ui.pendingSearch
        if (query === "") return
        Ui.pendingSearch = ""
        searchTextField.text = query
        search()
    }
    function removeFromLibrary(link) {
        const rows = App.library.remove(link)
        if (rows.length > 0)
            Ui.offerUndo(qsTr("Removed “%1” from the library.").arg(rows[0].title),
                         () => App.library.restoreRows("shows", rows))
    }
    function rerun() {
        if (browseMode === 2 && searchTextField.text.trim().length > 0) search()
        else browse(browseMode === 0)
    }

    readonly property var filterKinds: [
        { key: "genre", label: qsTr("Genre") },
        { key: "year", label: qsTr("Year") },
        { key: "status", label: qsTr("Status") }
    ].filter(kind => (App.providers.filterOptions[kind.key] || []).length > 0)
    readonly property bool filtered: Object.keys(Ui.explorerFilters).length > 0
    readonly property bool filtersIgnored: filtered && browseMode === 2 && !App.providers.filtersSearch

    function optionLabel(key, option) {
        if (key !== "status") return option.label
        return option.value === "airing" ? qsTr("Airing") : option.value === "finished" ? qsTr("Finished") : qsTr("Upcoming")
    }
    function setFilter(key, value) {
        const next = Object.assign({}, Ui.explorerFilters)
        if (value) next[key] = value
        else delete next[key]
        applyFilters(next)
    }
    function applyFilters(filters) {
        Ui.explorerFilters = filters
        const q = searchTextField.text.trim()
        if (browseMode === 2 && q.length > 0 && App.providers.filtersSearch) App.search(q, filters)
        else browse(browseMode === 0)
    }
    function applyPendingGenre() {
        const pending = Ui.pendingGenre
        if (!pending) return
        const index = App.providers.indexOf(pending.provider)
        if (index >= 0 && index !== App.providers.currentIndex) { App.providers.currentIndex = index; return }
        if (index >= 0 && !App.providers.filterOptionsReady) return
        Ui.pendingGenre = null
        const option = index >= 0 ? Ui.genreOption(App.providers.filterOptions.genre, pending.genre) : null
        if (!option) {
            searchTextField.text = pending.genre
            search()
            return
        }
        if (browseMode === 2) browseMode = 1
        setFilter("genre", Ui.explorerFilters.genre === option.value ? "" : option.value)
    }
    // later: a provider switch clears the filters in a handler that runs after
    Connections {
        target: Ui
        function onPendingGenreChanged() { Qt.callLater(explorerPage.applyPendingGenre) }
        function onPendingSearchChanged() { explorerPage.applyPendingSearch() }
    }
    Connections {
        target: App.providers
        function onFilterOptionsChanged() { Qt.callLater(explorerPage.applyPendingGenre) }
    }
    Component.onCompleted: {
        Qt.callLater(applyPendingGenre)
        Qt.callLater(applyPendingSearch)
    }

    HoverHandler {
        cursorShape: Qt.ArrowCursor
    }

    Component.onDestruction: {
        Ui.lastSearch = searchTextField.text
        Ui.explorerLastContentY = gridView.contentY
    }

    AppPopup {
        id: historyPopup
        parent: Overlay.overlay
        modal: false
        padding: 6
        margins: 0

        property var stored: []
        property var pendingRemoval: []
        property bool filtering: false
        readonly property string typed: searchTextField.text.trim().toLowerCase()
        readonly property var historyItems: stored.filter(item => !pendingRemoval.includes(item)
                                                          && (!filtering || (item.toLowerCase().includes(typed)
                                                                             && item.toLowerCase() !== typed)))

        function reload() { stored = App.settings.value("search/history", []) }
        function sync() {
            historyList.currentIndex = -1
            if (!searchTextField.activeFocus || historyItems.length === 0) {
                close()
                return
            }
            const p = searchTextField.mapToItem(Overlay.overlay, 0, searchTextField.height)
            x = p.x
            y = p.y + 4
            width = searchTextField.width
            open()
        }
        function step(delta) {
            if (!visible) return
            historyList.currentIndex = Math.max(-1, Math.min(historyItems.length - 1, historyList.currentIndex + delta))
            historyList.positionViewAtIndex(Math.max(0, historyList.currentIndex), ListView.Contain)
        }
        function pick(query) {
            searchTextField.text = query
            close()
            explorerPage.search()
        }
        function release(query) { pendingRemoval = pendingRemoval.filter(item => item !== query) }
        function removeEntry(query) {
            pendingRemoval = pendingRemoval.concat([query])
            searchTextField.forceActiveFocus()
            if (historyItems.length === 0) close()
            Ui.offerUndo(qsTr("Removed “%1” from the search history.").arg(query), () => release(query),
                         () => {
                             // searched again meanwhile: it stays
                             if (pendingRemoval.includes(query)) App.settings.removeFromHistory("search/history", query)
                             release(query)
                             reload()
                         })
        }
        function clearAll() {
            const all = stored.slice()
            pendingRemoval = all
            close()
            Ui.offerUndo(qsTr("Search history cleared."), () => { pendingRemoval = [] },
                         () => {
                             for (const item of all)
                                 if (pendingRemoval.includes(item)) App.settings.removeFromHistory("search/history", item)
                             // a removal made after the clear has its own toast
                             pendingRemoval = pendingRemoval.filter(item => !all.includes(item))
                             reload()
                         })
        }

        contentItem: Column {
            spacing: 0

            ListView {
                id: historyList
                model: historyPopup.historyItems
                width: parent.width
                height: implicitHeight
                implicitHeight: Math.min(count * (rowHeight + spacing), 320)
                readonly property int rowHeight: Ui.controlHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                spacing: 2
                currentIndex: -1

                delegate: Rectangle {
                    id: historyRow
                    required property string modelData
                    width: historyList.width
                    height: historyList.rowHeight
                    radius: 8
                    color: rowHover.hovered || historyRow.ListView.isCurrentItem ? Theme.hoverFill : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.dur(80) } }

                    HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: historyPopup.pick(historyRow.modelData) }

                    AppIcon {
                        id: histIcon
                        anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                        name: "search"
                        size: 16
                        color: Theme.textMuted
                    }
                    Text {
                        anchors {
                            left: histIcon.right; leftMargin: 10
                            right: histClear.left; rightMargin: 8
                            verticalCenter: parent.verticalCenter
                        }
                        text: historyRow.modelData
                        color: Theme.textSecondary
                        font.pixelSize: Ui.sp(Theme.compactSize)
                        elide: Text.ElideRight
                    }
                    IconButton {
                        id: histClear
                        anchors { right: parent.right; rightMargin: 6; verticalCenter: parent.verticalCenter }
                        implicitWidth: 26
                        implicitHeight: 26
                        iconName: "x"
                        iconSize: 13
                        boxRadius: 7
                        destructive: true
                        tip: qsTr("Remove")
                        onClicked: historyPopup.removeEntry(historyRow.modelData)
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.border
                visible: historyPopup.historyItems.length > 0
            }

            AbstractButton {
                id: clearAllBtn
                width: parent.width
                height: historyList.rowHeight
                focusPolicy: Qt.NoFocus

                background: Rectangle {
                    radius: 8
                    color: clearAllBtn.hovered ? Theme.hoverFill : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.dur(80) } }
                }

                contentItem: Text {
                    text: qsTr("Clear history")
                    color: clearAllBtn.hovered ? Theme.danger : Theme.textMuted
                    font.pixelSize: Ui.sp(Theme.metadataSize)
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    Behavior on color { ColorAnimation { duration: Theme.dur(80) } }
                }

                onClicked: historyPopup.clearAll()
            }
        }
    }

    Card {
        id: searchBarCard
        height: Ui.toolbarHeight
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
            topMargin: 10
            leftMargin: 8
            rightMargin: 8
        }

        RowLayout {
            anchors {
                fill: parent
                margins: 6
            }
            spacing: 6

            AppTextField {
                id: searchTextField
                color: Theme.textPrimary
                placeholderText: qsTr("Search %1…").arg(providerComboBox.currentText)
                placeholderTextColor: Theme.textMuted
                text: Ui.lastSearch
                fontSize: Theme.bodySize
                showClearButton: true
                Layout.fillHeight: true
                Layout.fillWidth: true
                Layout.preferredWidth: 5
                Layout.minimumWidth: Ui.sp(120)
                onAccepted: {
                    if (historyPopup.visible && historyList.currentIndex >= 0)
                        historyPopup.pick(historyPopup.historyItems[historyList.currentIndex])
                    else
                        explorerPage.search()
                }
                onTextEdited: {
                    historyPopup.filtering = true
                    historyPopup.sync()
                }
                Keys.onDownPressed: historyPopup.step(1)
                Keys.onUpPressed: historyPopup.step(-1)
                onActiveFocusChanged: {
                    if (activeFocus) {
                        historyPopup.filtering = false
                        historyPopup.reload()
                        historyPopup.sync()
                    } else {
                        // deferred: a click on the popup blurs the field
                        Qt.callLater(function() {
                            if (!searchTextField.activeFocus) historyPopup.close()
                        })
                    }
                }
            }

            ToolbarButton {
                text: qsTr("Search")
                backgroundDefaultColor: explorerPage.browseMode === 2 ? Theme.accent : Theme.surfaceAlt
                onClicked: explorerPage.search()
            }

            ToolbarButton {
                text: qsTr("Latest")
                backgroundDefaultColor: explorerPage.browseMode === 0 ? Theme.accent : Theme.surfaceAlt
                onClicked: explorerPage.browse(true)
            }

            ToolbarButton {
                text: qsTr("Popular")
                backgroundDefaultColor: explorerPage.browseMode === 1 ? Theme.accent : Theme.surfaceAlt
                onClicked: explorerPage.browse(false)
            }

            ToolbarComboBox {
                id: providerComboBox
                text: "text"
                model: App.providers
                currentIndex: App.providers.currentIndex
                onActivated: (index) => { App.providers.currentIndex = index; explorerPage.rerun() }
            }

            ToolbarComboBox {
                text: ""
                model: App.providers.showTypes
                currentIndex: App.providers.currentTypeIndex
                currentIndexColor: Qt.alpha(Theme.accent, 0.25)
                Layout.preferredWidth: Ui.sp(150)
                Layout.minimumWidth: Layout.preferredWidth
                onActivated: (index) => { App.providers.currentTypeIndex = index; explorerPage.rerun() }
            }
        }
    }

    RowLayout {
        id: filterRow
        visible: explorerPage.filterKinds.length > 0
        height: visible ? Ui.sp(34) : 0
        anchors { top: searchBarCard.bottom; left: parent.left; right: parent.right
                  topMargin: visible ? 8 : 0; leftMargin: 14; rightMargin: 14 }
        spacing: 8

        Repeater {
            model: explorerPage.filterKinds
            delegate: Chip {
                id: filterChip
                required property var modelData
                readonly property string value: Ui.explorerFilters[modelData.key] || ""
                readonly property var options: App.providers.filterOptions[modelData.key] || []
                readonly property string valueLabel: {
                    const option = options.find(option => option.value === value)
                    return option ? explorerPage.optionLabel(modelData.key, option) : value
                }
                readonly property string caption: value ? modelData.label + ": " + valueLabel : modelData.label
                // the label sits in its own row so the chevron or clear icon can trail it
                implicitWidth: chipRow.implicitWidth + Ui.sp(22)
                interactive: true
                checked: value !== ""
                onClicked: optionsPopup.toggle()

                Keys.onDeletePressed: if (filterChip.value) explorerPage.setFilter(filterChip.modelData.key, "")
                Keys.onPressed: (event) => {
                    if (event.key !== Qt.Key_Backspace || !filterChip.value) return
                    event.accepted = true
                    explorerPage.setFilter(filterChip.modelData.key, "")
                }
                Accessible.name: caption
                Accessible.onPressAction: optionsPopup.toggle()

                Row {
                    id: chipRow
                    anchors.centerIn: parent
                    spacing: 6
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: filterChip.caption
                        color: filterChip.ink
                        font.pixelSize: Ui.sp(Theme.metadataSize)
                        font.weight: Font.Medium
                    }
                    AppIcon {
                        anchors.verticalCenter: parent.verticalCenter
                        name: filterChip.value ? "x" : "chevron-down"
                        size: 14
                        color: filterChip.ink
                    }
                }

                MouseArea {
                    visible: filterChip.value !== ""
                    anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
                    width: 30
                    cursorShape: Qt.PointingHandCursor
                    onClicked: explorerPage.setFilter(filterChip.modelData.key, "")
                }

                AppPopup {
                    id: optionsPopup
                    toggledByParent: true
                    focus: true
                    y: filterChip.height + 6
                    width: Math.min(560, explorerPage.width - filterChip.mapToItem(explorerPage, 0, 0).x - 16)
                    padding: 10

                    contentItem: Flickable {
                        implicitHeight: Math.min(optionFlow.implicitHeight, 340)
                        contentHeight: optionFlow.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: AppScrollBar {}

                        Flow {
                            id: optionFlow
                            width: parent.width - 12
                            spacing: 6
                            Repeater {
                                model: [{ value: "", label: qsTr("Any") }].concat(filterChip.options)
                                delegate: Chip {
                                    id: optionPill
                                    required property var modelData
                                    label: modelData.value === "" ? modelData.label
                                           : explorerPage.optionLabel(filterChip.modelData.key, modelData)
                                    interactive: true
                                    checked: modelData.value === filterChip.value
                                    onClicked: {
                                        optionsPopup.close()
                                        explorerPage.setFilter(filterChip.modelData.key, optionPill.modelData.value)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        LinkText {
            visible: explorerPage.filtered
            Layout.fillHeight: true
            verticalAlignment: Text.AlignVCenter
            leftPadding: 4
            text: qsTr("Clear")
            onClicked: explorerPage.applyFilters({})
        }

        Item { Layout.fillWidth: true; visible: !ignoredNotice.visible }

        Text {
            id: ignoredNotice
            visible: explorerPage.filtersIgnored
            Layout.fillWidth: true
            Layout.fillHeight: true
            verticalAlignment: Text.AlignVCenter
            leftPadding: 8
            text: qsTr("On %1, filters apply to Latest and Popular only.").arg(providerComboBox.currentText)
            color: Theme.textMuted
            font.pixelSize: Ui.sp(Theme.compactSize)
            elide: Text.ElideRight

            HoverHandler { id: noticeHover }
            AppToolTip { text: ignoredNotice.text; visible: noticeHover.hovered && ignoredNotice.truncated }
        }

    }

    MediaGridView {
        id: gridView
        model: App.explorer
        focus: false
        anchors {
            top: filterRow.bottom
            left: parent.left
            right: parent.right
            bottom: parent.bottom
            rightMargin: Ui.sp(Theme.scrollbarGutter)
            topMargin: 6
        }
        imageAspectRatio: App.explorer.listAspect > 0 ? App.explorer.listAspect : Ui.coverAspect
        subtitles: App.explorer.listSubtitles
        Component.onCompleted: contentY = Ui.explorerLastContentY
        visible: !skeletonGrid.visible

        footer: Item {
            id: gridFooter
            readonly property bool failedMore: App.explorer.moreFailure !== ""
            readonly property bool ended: App.explorer.count > 0 && !App.explorer.isLoading
                                          && !App.explorer.hasMore && !failedMore
            width: gridView.width
            height: App.explorer.isLoadingMore ? gridView.cellHeight : failedMore || ended ? Ui.sp(64) : 0

            Row {
                visible: App.explorer.isLoadingMore
                Repeater {
                    model: gridView.columns
                    SkeletonCard {
                        width: gridView.cellWidth
                        height: gridView.cellHeight
                        aspectRatio: gridView.imageAspectRatio
                    }
                }
            }
            Row {
                anchors.centerIn: parent
                visible: gridFooter.failedMore
                spacing: Ui.sp(12)
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Could not load more from %1.").arg(providerComboBox.displayText)
                    color: Theme.textSecondary
                    font.pixelSize: Ui.sp(Theme.bodySize)
                }
                AppButton {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Retry")
                    secondary: true
                    onClicked: App.explorer.retryMore()
                }
            }
            Text {
                anchors.centerIn: parent
                visible: gridFooter.ended
                text: qsTr("End of results")
                color: Theme.textMuted
                font.pixelSize: Ui.sp(Theme.compactSize)
            }
        }

        readonly property real fetchThreshold: height * 1.0

        function tryFetchMore() {
            if (!App.explorer.canFetchMore()) return
            if (gridView.height <= 0) return
            const window = gridView.Window.window
            if (!window || window.visibility === Window.Hidden || window.visibility === Window.Minimized) return

            let distanceFromBottom = contentHeight - (contentY + height)
            if (contentHeight <= height || distanceFromBottom < fetchThreshold) {
                App.explorer.fetchMore()
            }
        }

        onContentYChanged: tryFetchMore()
        onContentHeightChanged: tryFetchMore()
        onHeightChanged: tryFetchMore()

        Connections {
            target: App.explorer
            function onIsLoadingChanged() {
                if (!App.explorer.isLoading) {
                    fetchDebounce.restart()
                }
            }
        }

        Timer {
            id: fetchDebounce
            interval: 50
            repeat: false
            onTriggered: gridView.tryFetchMore()
        }

        ScrollBar.vertical: AppScrollBar {
            parent: gridView.parent
            visible: gridView.visible
            anchors { top: gridView.top; left: gridView.right; bottom: gridView.bottom
                      leftMargin: (Ui.sp(Theme.scrollbarGutter) - width) / 2 }
            width: 6
            barOpacity: 0.4
        }

        delegate: ShowItem {
            id: showTile
            required property string title
            required property string link
            required property string cover
            required property string latestTxt
            required property string subtitle
            required property int index

            showTitle: title
            showCover: cover
            badgeText: latestTxt
            byline: subtitle
            width: gridView.cellWidth
            height: gridView.cellHeight
            aspectRatio: gridView.imageAspectRatio

            property int libraryRevision: 0
            libraryType: showTile.libraryRevision >= 0 ? App.library.libraryTypeOf(showTile.link) : -1

            Connections {
                target: App.library
                function onLibraryChanged() { showTile.libraryRevision++ }
                function onModelReset()     { showTile.libraryRevision++ }
            }

            onImageClicked: (mouse) => {
                explorerPage.forceActiveFocus()
                if (mouse.button === Qt.LeftButton) {
                    App.loadShow(index, false)
                } else if (mouse.button === Qt.RightButton) {
                    contextMenu.index = index
                    contextMenu.libraryType = App.library.libraryTypeOf(link)
                    contextMenu.link = link
                    contextMenu.showTitle = title
                    contextMenu.pageUrl = App.showUrl(index, false)
                    contextMenu.popup()
                } else if (mouse.button === Qt.MiddleButton) {
                    App.appendToPlaylists(index, false, false)
                }
            }

            onPlayClicked: {
                explorerPage.forceActiveFocus()
                App.appendToPlaylists(index, false, true)
            }
            onAddClicked: {
                explorerPage.forceActiveFocus()
                addMenu.index = index
                addMenu.link = link
                addMenu.currentType = App.library.libraryTypeOf(link)
                addMenu.popup()
            }
        }

        add: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1.0; duration: Theme.dur(160) }
            NumberAnimation { property: "y"; from: ViewTransition.destination.y + 8; duration: Theme.dur(160); easing.type: Easing.OutCubic }
        }

        displaced: Transition {
            NumberAnimation { properties: "x,y"; duration: Theme.dur(200); easing.type: Easing.OutCubic }
            NumberAnimation { property: "opacity"; to: 1.0; duration: Theme.dur(160) }
        }
    }

    GridView {
        id: skeletonGrid
        anchors.fill: gridView
        visible: App.explorer.isLoading && !App.explorer.isLoadingMore
        interactive: false
        cellWidth: gridView.cellWidth
        cellHeight: gridView.cellHeight
        model: visible ? gridView.columns * Math.ceil(height / cellHeight) : 0
        delegate: SkeletonCard {
            width: skeletonGrid.cellWidth
            height: skeletonGrid.cellHeight
            aspectRatio: gridView.imageAspectRatio
        }
    }

    AppButton {
        anchors { horizontalCenter: skeletonGrid.horizontalCenter; bottom: skeletonGrid.bottom; bottomMargin: Ui.sp(24) }
        visible: skeletonGrid.visible && slowLoad.slow
        text: qsTr("Cancel")
        onClicked: App.explorer.cancel()
    }
    Timer {
        id: slowLoad
        property bool slow: false
        interval: 4000
        running: skeletonGrid.visible
        onRunningChanged: slow = false
        onTriggered: slow = true
    }

    EmptyState {
        anchors.centerIn: gridView
        visible: App.explorer.ran && !App.explorer.isLoading && gridView.count === 0
        readonly property bool failed: App.explorer.failure !== ""
        readonly property string provider: providerComboBox.displayText
        icon: failed ? "server" : "search"
        title: failed ? qsTr("Could not load results from %1.").arg(provider)
             : explorerPage.browseMode === 2 && explorerPage.searchedFor !== ""
               ? qsTr("No results for “%1” on %2.").arg(explorerPage.searchedFor).arg(provider)
               : explorerPage.filtered ? qsTr("No results for these filters on %1.").arg(provider)
               : qsTr("No results on %1.").arg(provider)
        hint: failed ? App.explorer.failure : explorerPage.browseMode === 2 ? qsTr("Try other words, or another provider.") : ""
        actionText: !failed && explorerPage.filtered ? qsTr("Clear filters") : qsTr("Try again")
        onActionTriggered: failed || !explorerPage.filtered ? App.explorer.reload() : explorerPage.applyFilters({})
    }

    AppMenu {
        id: contextMenu
        modal: true
        property int index
        property int libraryType
        property string link
        property string showTitle: ""
        property string pageUrl: ""

        Action {
            icon.name: "play"
            text: qsTr("Play")
            onTriggered: App.appendToPlaylists(contextMenu.index, false, true)
        }

        Action {
            icon.name: "list-plus"
            text: qsTr("Queue")
            onTriggered: App.appendToPlaylists(contextMenu.index, false, false)
        }

        LibraryTypeMenu {
            currentType: contextMenu.libraryType
            onPicked: (type) => App.addToLibrary(contextMenu.index, type)
        }

        SearchOnMenu {
            showTitle: contextMenu.showTitle
            ownProvider: providerComboBox.currentText
        }

        Action {
            icon.name: "copy"
            text: qsTr("Copy title")
            onTriggered: App.copyToClipboard(contextMenu.showTitle)
        }

        Action {
            property bool shown: contextMenu.pageUrl !== ""
            icon.name: "external-link"
            text: qsTr("Open on site")
            onTriggered: Qt.openUrlExternally(contextMenu.pageUrl)
        }
        Action {
            property bool shown: contextMenu.pageUrl !== ""
            icon.name: "link"
            text: qsTr("Copy link")
            onTriggered: App.copyToClipboard(contextMenu.pageUrl)
        }

        TextMenu.Separator { shown: contextMenu.libraryType !== -1 }

        Action {
            property bool shown: contextMenu.libraryType !== -1
            property bool destructive: true
            icon.name: "trash-2"
            text: qsTr("Remove from library")
            onTriggered: explorerPage.removeFromLibrary(contextMenu.link)
        }
    }

    LibraryTypeMenu {
        id: addMenu
        modal: true
        property int index
        property string link
        onPicked: (type) => App.addToLibrary(addMenu.index, type)

        TextMenu.Separator { shown: addMenu.currentType !== -1 }

        Action {
            property bool shown: addMenu.currentType !== -1
            property bool destructive: true
            icon.name: "trash-2"
            text: qsTr("Remove from library")
            onTriggered: explorerPage.removeFromLibrary(addMenu.link)
        }
    }

    TapHandler {
        onTapped: explorerPage.forceActiveFocus()
    }

    Keys.enabled: true
    Keys.onPressed: event => {
        if (App.shortcuts.actionFor(App.shortcuts.keyText(event.key, event.modifiers)) === "reload") {
            App.explorer.reload()
        } else if ((event.modifiers & Qt.ControlModifier) && (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp)) {
            providerComboBox.popup.close()
            App.providers.cycle(event.key === Qt.Key_PageDown ? 1 : -1)
            explorerPage.rerun()
            event.accepted = true
        } else if (!(event.modifiers & Qt.ControlModifier)) {
            switch (event.key) {
                case Qt.Key_Escape:
                case Qt.Key_Alt:
                if (!explorerPage.activeFocus) explorerPage.forceActiveFocus()
                break

                case Qt.Key_Enter:
                case Qt.Key_Return:
                if (gridView.currentIndex >= 0 && !searchTextField.activeFocus) {
                    App.loadShow(gridView.currentIndex, false)
                    event.accepted = true
                } else {
                    search()
                }
                break

                case Qt.Key_Slash:
                searchTextField.forceActiveFocus()
                event.accepted = true
                break

                case Qt.Key_P:
                explorerPage.browse(false)
                break

                case Qt.Key_L:
                explorerPage.browse(true)
                break

                case Qt.Key_Left:
                case Qt.Key_Right:
                case Qt.Key_Up:
                case Qt.Key_Down:
                gridView.moveCursor(event.key)
                event.accepted = true
                break
            }
        }
    }
}
