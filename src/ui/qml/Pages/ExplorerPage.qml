pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../Components"
import App
import ".."

Item {
    id: explorerPage
    focus: true

    // For the toolbar's active-state highlight.
    property int browseMode: 0
    // What the last search asked for, for the no-results message.
    property string searchedFor: ""

    // The search field owns the focus here.
    component ToolbarButton: AppButton {
        fontSize: Theme.bodySize
        radius: 10
        focusPolicy: Qt.NoFocus
        focus: false
        activeFocusOnTab: false
        Layout.fillHeight: true
        leftPadding: 16
        rightPadding: 16
    }

    component ToolbarComboBox: AppComboBox {
        fontSize: Theme.bodySize
        focus: false
        activeFocusOnTab: false
        Layout.fillHeight: true
    }

    function search() {
        let q = searchTextField.text.trim()
        if (q.length > 0)
            App.settings.prependToHistory("search/history", q)
        historyPopup.close()
        explorerPage.forceActiveFocus()
        searchedFor = q
        if (q.length > 0) { browseMode = 2; App.search(q, Globals.explorerFilters) }
        else              { browseMode = 1; App.browse(false, Globals.explorerFilters) }
    }
    function browse(latest) {
        explorerPage.forceActiveFocus()
        browseMode = latest ? 0 : 1
        App.browse(latest, Globals.explorerFilters)
    }
    // Whatever was showing, asked of the provider or type just picked.
    function rerun() {
        if (browseMode === 2 && searchTextField.text.trim().length > 0) search()
        else browse(browseMode === 0)
    }

    readonly property var filterKinds: [
        { key: "genre", label: qsTr("Genre") },
        { key: "year", label: qsTr("Year") },
        { key: "status", label: qsTr("Status") }
    ].filter(kind => (App.providers.filterOptions[kind.key] || []).length > 0)
    readonly property bool filtered: Object.keys(Globals.explorerFilters).length > 0
    // A search the provider cannot narrow runs as typed.
    readonly property bool filtersIgnored: filtered && browseMode === 2 && !App.providers.filtersSearch

    function optionLabel(key, option) {
        if (key !== "status") return option.label
        return option.value === "airing" ? qsTr("Airing") : option.value === "finished" ? qsTr("Finished") : qsTr("Upcoming")
    }
    function setFilter(key, value) {
        const next = Object.assign({}, Globals.explorerFilters)
        if (value) next[key] = value
        else delete next[key]
        applyFilters(next)
    }
    function applyFilters(filters) {
        Globals.explorerFilters = filters
        // Words stay where the provider narrows them; otherwise the filters browse.
        const q = searchTextField.text.trim()
        if (browseMode === 2 && q.length > 0 && App.providers.filtersSearch) App.search(q, filters)
        else browse(browseMode === 0)
    }
    // Switches to the show's provider first; its options may still be on their way. A genre the
    // provider cannot filter by is searched for as words, as before.
    function applyPendingGenre() {
        const pending = Globals.pendingGenre
        if (!pending) return
        const index = App.providers.indexOf(pending.provider)
        if (index >= 0 && index !== App.providers.currentIndex) { App.providers.currentIndex = index; return }
        if (index >= 0 && !App.providers.filterOptionsReady) return
        Globals.pendingGenre = null
        const option = index >= 0 ? Globals.genreOption(App.providers.filterOptions.genre, pending.genre) : null
        if (!option) {
            searchTextField.text = pending.genre
            search()
            return
        }
        if (browseMode === 2) browseMode = 1
        setFilter("genre", Globals.explorerFilters.genre === option.value ? "" : option.value)
    }
    // Later, not now: switching provider clears the filters in a handler that runs after these.
    Connections {
        target: Globals
        function onPendingGenreChanged() { Qt.callLater(explorerPage.applyPendingGenre) }
    }
    Connections {
        target: App.providers
        function onFilterOptionsChanged() { Qt.callLater(explorerPage.applyPendingGenre) }
    }
    Component.onCompleted: Qt.callLater(applyPendingGenre)

    HoverHandler {
        cursorShape: Qt.ArrowCursor
    }

    Component.onDestruction: {
        Globals.lastSearch = searchTextField.text
        Globals.explorerLastContentY = gridView.contentY
    }

    AppPopup {
        id: historyPopup
        parent: Overlay.overlay
        modal: false
        padding: 6
        margins: 0
        backgroundRadius: 10

        property var historyItems: []

        contentItem: Column {
            spacing: 0

            ListView {
                id: historyList
                model: historyPopup.historyItems
                width: parent.width
                height: implicitHeight
                implicitHeight: Math.min(count * (rowHeight + spacing), 320)
                readonly property int rowHeight: Globals.controlHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                spacing: 2

                delegate: Rectangle {
                    id: historyRow
                    required property string modelData
                    width: historyList.width
                    height: historyList.rowHeight
                    radius: 8
                    color: rowHover.hovered ? Theme.hoverFill : "transparent"
                    Behavior on color { ColorAnimation { duration: 80 } }

                    HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }
                    // The X sits outside this area.
                    TapHandler {
                        onTapped: {
                            searchTextField.text = historyRow.modelData
                            historyPopup.close()
                            explorerPage.search()
                        }
                    }

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
                        font.pixelSize: Globals.sp(Theme.compactSize)
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
                        onClicked: {
                            App.settings.removeFromHistory("search/history", historyRow.modelData)
                            const h = App.settings.value("search/history", [])
                            if (h.length === 0) { historyPopup.close(); return }
                            historyPopup.historyItems = h
                            // The tap blurred the field, which would close the popup.
                            searchTextField.forceActiveFocus()
                        }
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
                height: 32
                focusPolicy: Qt.NoFocus

                background: Rectangle {
                    radius: 6
                    color: clearAllBtn.hovered ? Theme.hoverFill : "transparent"
                    Behavior on color { ColorAnimation { duration: 80 } }
                }

                contentItem: Text {
                    text: qsTr("Clear History")
                    color: clearAllBtn.hovered ? Theme.danger : Theme.textMuted
                    font.pixelSize: Globals.sp(16)
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    Behavior on color { ColorAnimation { duration: 80 } }
                }

                onClicked: {
                    App.settings.clearHistory("search/history")
                    historyPopup.close()
                }
            }
        }
    }

    Card {
        id: searchBarCard
        height: Globals.toolbarHeight
        radius: 14
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
                text: Globals.lastSearch
                fontSize: Theme.bodySize
                showClearButton: true
                focusPolicy: Qt.NoFocus
                focus: false
                activeFocusOnTab: false
                Layout.fillHeight: true
                Layout.fillWidth: true
                Layout.preferredWidth: 5
                onAccepted: explorerPage.search()
                onActiveFocusChanged: {
                    if (activeFocus) {
                        let h = App.settings.value("search/history", [])
                        if (h.length > 0) {
                            historyPopup.historyItems = h
                            let p = searchTextField.mapToItem(Overlay.overlay, 0, searchTextField.height)
                            historyPopup.x = p.x
                            historyPopup.y = p.y + 4
                            historyPopup.width = searchTextField.width
                            historyPopup.open()
                        }
                    } else {
                        // Deferred: a click on the popup blurs the field, so close only if focus
                        // does not come back.
                        Qt.callLater(function() {
                            if (!searchTextField.activeFocus) historyPopup.close()
                        })
                    }
                }
            }

            // Only the list on show is lit: search results, Latest or Popular.
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
                // One width for every provider, so switching does not reflow the bar.
                Layout.preferredWidth: Globals.sp(150)
                Layout.minimumWidth: Layout.preferredWidth
                onActivated: (index) => { App.providers.currentTypeIndex = index; explorerPage.rerun() }
            }
        }
    }

    Row {
        id: filterRow
        visible: explorerPage.filterKinds.length > 0
        height: visible ? Globals.sp(34) : 0
        anchors { top: searchBarCard.bottom; left: parent.left; topMargin: visible ? 8 : 0; leftMargin: 14 }
        spacing: 8

        Repeater {
            model: explorerPage.filterKinds
            delegate: Rectangle {
                id: filterChip
                required property var modelData
                readonly property string value: Globals.explorerFilters[modelData.key] || ""
                readonly property var options: App.providers.filterOptions[modelData.key] || []
                readonly property string valueLabel: {
                    const option = options.find(option => option.value === value)
                    return option ? explorerPage.optionLabel(modelData.key, option) : value
                }
                height: filterRow.height
                width: chipRow.implicitWidth + 26
                radius: height / 2
                color: value ? Theme.accentMuted : chipHover.hovered || optionsPopup.visible ? Theme.hoverFill : Theme.surface
                border.color: value ? Qt.alpha(Theme.accent, 0.6) : Theme.border
                border.width: 1
                Behavior on color { ColorAnimation { duration: 100 } }

                Row {
                    id: chipRow
                    anchors.centerIn: parent
                    spacing: 6
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: filterChip.value ? filterChip.modelData.label + ":  " + filterChip.valueLabel : filterChip.modelData.label
                        color: filterChip.value ? Theme.textAccent : Theme.textSecondary
                        font.pixelSize: Globals.sp(Theme.compactSize)
                        font.weight: filterChip.value ? Font.DemiBold : Font.Normal
                    }
                    AppIcon {
                        anchors.verticalCenter: parent.verticalCenter
                        name: filterChip.value ? "x" : "chevron-down"
                        size: 14
                        color: filterChip.value ? Theme.textAccent : Theme.textMuted
                    }
                }

                HoverHandler { id: chipHover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    // The x clears; the rest of a set chip picks again.
                    onTapped: point => {
                        if (filterChip.value && point.position.x > filterChip.width - 30) explorerPage.setFilter(filterChip.modelData.key, "")
                        else optionsPopup.toggle()
                    }
                }

                AppPopup {
                    id: optionsPopup
                    toggledByParent: true
                    y: filterChip.height + 6
                    width: Math.min(560, explorerPage.width - filterChip.mapToItem(explorerPage, 0, 0).x - 16)
                    padding: 10
                    backgroundRadius: 12

                    contentItem: Flickable {
                        implicitHeight: Math.min(optionFlow.implicitHeight, 340)
                        contentHeight: optionFlow.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: AppScrollBar {}

                        Flow {
                            id: optionFlow
                            width: parent.width
                            spacing: 6
                            Repeater {
                                model: [{ value: "", label: qsTr("Any") }].concat(filterChip.options)
                                delegate: Rectangle {
                                    id: optionPill
                                    required property var modelData
                                    readonly property bool chosen: modelData.value === filterChip.value
                                    width: optionText.implicitWidth + 22
                                    height: Globals.sp(30)
                                    radius: height / 2
                                    color: chosen ? Theme.accent : optionHover.hovered ? Theme.hoverFill : Theme.surfaceAlt
                                    Text {
                                        id: optionText
                                        anchors.centerIn: parent
                                        text: optionPill.modelData.value === "" ? optionPill.modelData.label
                                              : explorerPage.optionLabel(filterChip.modelData.key, optionPill.modelData)
                                        color: optionPill.chosen ? Theme.onAccent : Theme.textPrimary
                                        font.pixelSize: Globals.sp(Theme.compactSize)
                                    }
                                    HoverHandler { id: optionHover; cursorShape: Qt.PointingHandCursor }
                                    TapHandler {
                                        onTapped: {
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
        }

        Text {
            visible: explorerPage.filtered
            height: parent.height
            verticalAlignment: Text.AlignVCenter
            leftPadding: 4
            text: qsTr("Clear")
            color: clearHover.hovered ? Theme.textPrimary : Theme.textMuted
            font.pixelSize: Globals.sp(Theme.compactSize)
            font.underline: clearHover.hovered
            HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
            TapHandler {
                onTapped: explorerPage.applyFilters({})
            }
        }

        Text {
            visible: explorerPage.filtersIgnored
            height: parent.height
            verticalAlignment: Text.AlignVCenter
            leftPadding: 8
            text: qsTr("%1 filters Latest and Popular, not a search.").arg(providerComboBox.currentText)
            color: Theme.textMuted
            font.pixelSize: Globals.sp(Theme.compactSize)
            font.italic: true
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
            rightMargin: Globals.sp(Theme.scrollbarGutter)
            topMargin: 6
        }
        imageAspectRatio: Globals.imageAspectRatio
        Component.onCompleted: contentY = Globals.explorerLastContentY

        readonly property real fetchThreshold: height * 1.0

        function tryFetchMore() {
            if (!App.explorer.canFetchMore()) return
            if (gridView.height <= 0) return
            // Hidden or minimised, the view stops laying out, so contentHeight never catches up
            // and every page that lands would ask for the next one.
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
            anchors { top: gridView.top; left: gridView.right; bottom: gridView.bottom
                      leftMargin: (Globals.sp(Theme.scrollbarGutter) - width) / 2 }
            width: 6
            barOpacity: 0.4
        }

        onImageAspectRatioChanged: {
            let lastContentY = contentY
            App.explorer.reset()
            contentY = lastContentY
        }

        delegate: ShowItem {
            id: showTile
            required property string title
            required property string link
            required property string cover
            required property string latestTxt
            required property int index

            showTitle: title
            showCover: cover
            badgeText: latestTxt
            width: gridView.cellWidth
            height: gridView.cellHeight
            aspectRatio: Globals.imageAspectRatio

            // Library membership has no per-row notifier.
            property int libraryRevision: 0
            libraryType: showTile.libraryRevision >= 0 ? App.library.libraryTypeOf(showTile.link) : -1

            Connections {
                target: App.library
                function onLibraryChanged() { showTile.libraryRevision++ }
                function onModelReset()     { showTile.libraryRevision++ }
            }

            onImageLoaded: (sourceAspectRatio) => {
                if (index !== 0) return
                if (Math.abs(Globals.imageAspectRatio - sourceAspectRatio) < 0.01) return
                Globals.imageAspectRatio = sourceAspectRatio
            }

            onImageClicked: (mouse) => {
                explorerPage.forceActiveFocus()
                if (mouse.button === Qt.LeftButton) {
                    App.loadShow(index, false)
                } else if (mouse.button === Qt.RightButton) {
                    contextMenu.index = index
                    contextMenu.libraryType = App.library.libraryTypeOf(link)
                    contextMenu.link = link
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
            NumberAnimation {
                property: "opacity"
                from: 0
                to: 1.0
                duration: 400
            }
            NumberAnimation {
                property: "scale"
                from: 0
                to: 1.0
                duration: 400
            }
        }

        displaced: Transition {
            NumberAnimation {
                properties: "x,y"
                duration: 400
                easing.type: Easing.OutBounce
            }
            NumberAnimation {
                property: "opacity"
                to: 1.0
            }
            NumberAnimation {
                property: "scale"
                to: 1.0
            }
        }
    }

    // After the grid: an empty GridView still takes the presses the button needs.
    EmptyState {
        anchors.centerIn: gridView
        visible: App.explorer.ran && !App.explorer.isLoading && gridView.count === 0
        readonly property bool failed: App.explorer.failure !== ""
        readonly property string provider: providerComboBox.displayText
        icon: failed ? "server" : "search"
        title: failed ? qsTr("%1 did not answer").arg(provider)
             : explorerPage.browseMode === 2 && explorerPage.searchedFor !== ""
               ? qsTr("No results for \"%1\" on %2").arg(explorerPage.searchedFor).arg(provider)
               : explorerPage.filtered ? qsTr("Nothing on %1 matches these filters").arg(provider)
               : qsTr("%1 has nothing to show here").arg(provider)
        hint: failed ? App.explorer.failure : qsTr("Try other words, or another provider.")
        actionText: !failed && explorerPage.filtered ? qsTr("Clear filters") : qsTr("Try again")
        onActionTriggered: failed || !explorerPage.filtered ? App.explorer.reload() : explorerPage.applyFilters({})
    }

    AppMenu {
        id: contextMenu
        modal: true
        property int index
        property int libraryType
        property string link

        Action {
            text: qsTr("Play")
            onTriggered: App.appendToPlaylists(contextMenu.index, false, true)
        }

        Action {
            text: qsTr("Queue")
            onTriggered: App.appendToPlaylists(contextMenu.index, false, false)
        }

        LibraryTypeMenu {
            currentType: contextMenu.libraryType
            onPicked: (type) => App.addToLibrary(contextMenu.index, type)
        }

        Action {
            text: qsTr("Remove")
            enabled: contextMenu.libraryType !== -1
            onTriggered: App.library.remove(contextMenu.link)
        }

        // Only for a provider whose page address a link can say.
        property string pageUrl: ""
        Action {
            property bool shown: contextMenu.pageUrl !== ""
            text: qsTr("Open on site")
            enabled: shown
            onTriggered: Qt.openUrlExternally(contextMenu.pageUrl)
        }
        Action {
            property bool shown: contextMenu.pageUrl !== ""
            text: qsTr("Copy link")
            enabled: shown
            onTriggered: App.copyToClipboard(contextMenu.pageUrl)
        }
    }

    // A card's "+": the lists straight away, and a way out for a show already in one.
    LibraryTypeMenu {
        id: addMenu
        modal: true
        property int index
        property string link
        onPicked: (type) => App.addToLibrary(addMenu.index, type)
        Action {
            property bool shown: addMenu.currentType !== -1
            text: qsTr("Remove from library")
            enabled: shown
            onTriggered: App.library.remove(addMenu.link)
        }
    }

    TapHandler {
        onTapped: explorerPage.forceActiveFocus()
    }

    Keys.enabled: true
    Keys.onPressed: event => {
        if (App.shortcuts.actionFor(App.shortcuts.keyText(event.key, event.modifiers)) === "reload") {
            App.explorer.reload()
        } else if (!(event.modifiers & Qt.ControlModifier)) {
            switch (event.key) {
                case Qt.Key_Escape:
                case Qt.Key_Alt:
                if (searchTextField.activeFocus) explorerPage.forceActiveFocus()
                break

                case Qt.Key_Tab:
                providerComboBox.popup.close()
                App.providers.cycle()
                event.accepted = true
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
