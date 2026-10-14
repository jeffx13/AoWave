pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App

Rectangle {
    id: libraryPage
    color: "transparent"

    function dragIndex(source, propertyName) {
        if (!source)
            return -1;
        const value = source[propertyName];
        return typeof value === "number" ? value : -1;
    }

    property var selection: []
    property string anchorLink: ""

    function clearSelection() {
        selection = []
        anchorLink = ""
    }

    function toggleSelected(link) {
        const at = selection.indexOf(link)
        selection = at >= 0 ? selection.filter(picked => picked !== link) : selection.concat([link])
        anchorLink = link
    }

    function selectTo(link) {
        const range = App.libraryModel.linksBetween(anchorLink, link)
        if (range.length === 0) return toggleSelected(link)
        selection = selection.concat(range.filter(picked => selection.indexOf(picked) < 0))
    }

    function picked() { return App.libraryModel.inViewOrder(selection) }

    function selectAll() {
        const links = []
        for (let i = 0; i < libraryGridView.count; ++i) links.push(App.libraryModel.linkAt(i))
        selection = links
        anchorLink = links.length > 0 ? links[links.length - 1] : ""
    }

    function moveSelected(type) {
        for (const link of picked()) App.library.changeLibraryType(link, type)
        clearSelection()
    }

    function queueSelected() {
        for (const link of picked()) App.appendToPlaylists(App.library.indexOf(link), true, false)
        clearSelection()
    }

    function removeSelected() {
        let rows = []
        for (const link of picked()) rows = rows.concat(App.library.remove(link))
        clearSelection()
        if (rows.length > 0)
            Ui.offerUndo(rows.length === 1 ? qsTr("Removed “%1” from the library.").arg(rows[0].title)
                                           : qsTr("Removed %1 shows from the library.").arg(rows.length),
                         () => App.library.restoreRows("shows", rows))
    }

    function clearFilters() {
        App.libraryModel.titleFilter = ""
        App.libraryModel.typeFilter = 0
        App.libraryModel.hasUnwatchedEpisodesOnly = false
        App.libraryModel.useRegex = false
        App.libraryModel.caseSensitive = false
    }

    Connections {
        target: App.libraryModel
        function onTypeFilterChanged() { libraryPage.clearSelection() }
        function onTitleFilterChanged() { libraryPage.clearSelection() }
        function onHasUnwatchedEpisodesOnlyChanged() { libraryPage.clearSelection() }
        function onModelReset() { libraryPage.clearSelection() }
        function onRowsRemoved() { libraryPage.selection = libraryPage.picked() }
    }
    Connections {
        target: App.library
        function onLibraryTypeChanged() { libraryPage.clearSelection() }
    }

    component UnwatchedBadge: Rectangle {
        id: unwatchedBadge
        property int count
        property bool compact: false
        visible: count > 0
        width: Math.max(height, unwatchedText.implicitWidth + 14)
        height: compact ? 28 : 34
        radius: height / 2

        gradient: Gradient {
            GradientStop {
                position: 0.0
                color: Qt.lighter(Theme.danger, 1.1)
            }
            GradientStop {
                position: 1.0
                color: Qt.darker(Theme.danger, 1.15)
            }
        }
        border.color: Qt.darker(Theme.danger, 1.4)
        border.width: 1

        Text {
            id: unwatchedText
            anchors.centerIn: parent
            text: unwatchedBadge.count
            color: Theme.inkOn(Theme.danger)
            font {
                pixelSize: Ui.sp(unwatchedBadge.compact ? 17 : 20)
                bold: true
            }
        }

        Rectangle {
            anchors.centerIn: parent
            width: parent.width + 6
            height: parent.height + 6
            radius: height / 2
            color: "transparent"
            border.color: Qt.alpha(Theme.danger, 0.25)
            border.width: 2
        }

        PulseRing {
            anchors.centerIn: parent
            width: parent.width
            height: parent.height
            radius: height / 2
            border.width: 2
            ringColor: Theme.danger
            peakOpacity: 0.6
            peakScale: 1.6
            period: 1600
        }
    }

    HoverHandler {
        cursorShape: Qt.ArrowCursor
    }

    Keys.onPressed: event => {
        if (App.shortcuts.actionFor(App.shortcuts.keyText(event.key, event.modifiers)) === "reload") {
            App.library.refreshAllUnwatched();
            return;
        }
        if ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_A) {
            if (!titleFilterTextField.activeFocus) {
                libraryPage.selectAll();
                event.accepted = true;
            }
            return;
        }
        if (event.modifiers & Qt.ControlModifier) return;
        switch (event.key) {
        case Qt.Key_Delete:
            if (libraryPage.selection.length > 0 && !titleFilterTextField.activeFocus) {
                libraryPage.removeSelected();
                event.accepted = true;
            }
            break;
        case Qt.Key_Slash:
            titleFilterTextField.forceActiveFocus();
            event.accepted = true;
            break;
        case Qt.Key_Escape:
            if (libraryPage.selection.length > 0) {
                libraryPage.clearSelection();
                event.accepted = true;
            }
            break;
        case Qt.Key_Space:
            if (libraryGridView.currentIndex >= 0) {
                libraryPage.toggleSelected(App.libraryModel.linkAt(libraryGridView.currentIndex));
                event.accepted = true;
            }
            break;
        case Qt.Key_Tab:
            event.accepted = true;
            libraryTypeComboBox.popup.close();
            App.library.cycleDisplayLibraryType();
            break;
        case Qt.Key_Left:
        case Qt.Key_Right:
        case Qt.Key_Up:
        case Qt.Key_Down:
            libraryGridView.moveCursor(event.key);
            event.accepted = true;
            break;
        case Qt.Key_Enter:
        case Qt.Key_Return:
            if (libraryGridView.currentIndex >= 0) {
                App.loadShow(App.libraryModel.mapToAbsoluteIndex(libraryGridView.currentIndex), true);
                event.accepted = true;
            }
            break;
        }
    }

    Card {
        id: topBarCard
        height: Ui.toolbarHeight
        radius: 14
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            topMargin: 10
            leftMargin: 8
            rightMargin: 8
        }

        RowLayout {
            anchors.fill: parent
            anchors.margins: 6
            spacing: 6

            Item {
                Layout.fillHeight: true
                implicitWidth: libraryTypeComboBox.implicitWidth
                HoverHandler { id: listHover }
                AppToolTip { text: qsTr("Searching all lists"); visible: App.library.searchAllLists && listHover.hovered }

                AppComboBox {
                    id: libraryTypeComboBox
                    anchors.fill: parent
                    fontSize: Theme.compactSize
                    focusPolicy: Qt.NoFocus
                    currentIndex: App.library.libraryType
                    enabled: !App.library.searchAllLists
                    onActivated: index => App.library.libraryType = index
                    text: ""
                    model: Ui.libraryTypeNames
                }
            }

            AppComboBox {
                Layout.fillHeight: true
                fontSize: Theme.compactSize
                focusPolicy: Qt.NoFocus
                currentIndex: App.libraryModel.typeFilter
                onActivated: index => App.libraryModel.typeFilter = index
                text: ""
                model: [qsTr("All types"), qsTr("Anime"), qsTr("Movies"), qsTr("TV series"), qsTr("Variety shows"), qsTr("Documentaries")]
            }

            AppComboBox {
                Layout.fillHeight: true
                fontSize: Theme.compactSize
                focusPolicy: Qt.NoFocus
                currentIndex: App.libraryModel.sortMode
                onActivated: index => App.libraryModel.sortMode = index
                text: ""
                model: [qsTr("My order"), qsTr("A-Z"), qsTr("Most unwatched"), qsTr("Recently watched")]
                displayFormat: qsTr("Sort: %1")
            }

            Item {
                Layout.fillHeight: true
                Layout.fillWidth: true
                Layout.preferredWidth: 3.2

                AppTextField {
                    id: titleFilterTextField
                    fontSize: Theme.compactSize
                    rightPadding: filterToggles.width + 16
                    anchors.fill: parent
                    checkedColor: Theme.accent
                    color: Theme.textPrimary
                    placeholderText: qsTr("Search all lists")
                    placeholderTextColor: Theme.textMuted
                    focusPolicy: Qt.NoFocus
                    text: App.libraryModel.titleFilter
                    Binding {
                        target: App.libraryModel
                        property: "titleFilter"
                        value: titleFilterTextField.text
                    }
                    Binding {
                        target: App.library
                        property: "searchAllLists"
                        value: titleFilterTextField.text.length > 0
                    }
                }

                Row {
                    id: filterToggles
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 6 }
                    spacing: 2
                    Repeater {
                        model: [
                            { label: ".*", tip: qsTr("Regular expression"), on: App.libraryModel.useRegex },
                            { label: "Aa", tip: qsTr("Match case"), on: App.libraryModel.caseSensitive }
                        ]
                        delegate: Rectangle {
                            id: toggle
                            required property var modelData
                            required property int index
                            width: toggleLabel.implicitWidth + 12
                            height: Ui.controlHeight - 14
                            radius: 6
                            color: toggle.modelData.on ? Theme.accentMuted : toggleHover.hovered ? Theme.hoverFill : "transparent"
                            Behavior on color { ColorAnimation { duration: 100 } }
                            Text {
                                id: toggleLabel
                                anchors.centerIn: parent
                                text: toggle.modelData.label
                                font.pixelSize: Ui.sp(Theme.metadataSize)
                                font.weight: Font.DemiBold
                                color: toggle.modelData.on ? Theme.textAccent : Theme.textMuted
                            }
                            HoverHandler { id: toggleHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler {
                                onTapped: {
                                    if (toggle.index === 0) App.libraryModel.useRegex = !App.libraryModel.useRegex
                                    else App.libraryModel.caseSensitive = !App.libraryModel.caseSensitive
                                }
                            }
                            AppToolTip { text: toggle.modelData.tip; visible: toggleHover.hovered }
                        }
                    }
                }
            }

            AppButton {
                text: qsTr("Unwatched")
                fontSize: Theme.compactSize
                secondary: !App.libraryModel.hasUnwatchedEpisodesOnly
                Layout.fillHeight: true
                leftPadding: 12
                rightPadding: 12
                onClicked: App.libraryModel.hasUnwatchedEpisodesOnly = !App.libraryModel.hasUnwatchedEpisodesOnly
            }
            IconButton {
                id: refreshButton
                iconName: "refresh-cw"
                iconSize: 19
                implicitWidth: Ui.controlHeight
                implicitHeight: Ui.controlHeight
                boxRadius: 10
                tip: qsTr("Refresh unwatched counts") + (App.shortcuts.bindings.reload.length > 0 ? " (" + App.shortcuts.bindings.reload[0] + ")" : "")
                onClicked: App.library.refreshAllUnwatched()

                RotationAnimator {
                    target: refreshButton.contentItem
                    from: 0
                    to: 360
                    duration: 900
                    loops: Animation.Infinite
                    running: App.library.busy && !Theme.reduceMotion
                    onRunningChanged: if (!running) refreshButton.contentItem.rotation = 0
                }
            }
            IconButton {
                iconName: App.settings.libraryRows ? "layout-grid" : "list"
                iconSize: 19
                implicitWidth: Ui.controlHeight
                implicitHeight: Ui.controlHeight
                boxRadius: 10
                tip: App.settings.libraryRows ? qsTr("Show as grid") : qsTr("Show as list")
                onClicked: App.settings.libraryRows = !App.settings.libraryRows
            }
            IconButton {
                id: scheduleButton
                iconName: "calendar"
                iconSize: 19
                implicitWidth: Ui.controlHeight
                implicitHeight: Ui.controlHeight
                boxRadius: 10
                tip: qsTr("Airing schedule")
                onClicked: schedulePopup.toggle()

                AppPopup {
                    id: schedulePopup
                    toggledByParent: true
                    property var shows: []
                    x: scheduleButton.width - width
                    y: scheduleButton.height + 6
                    width: 380
                    padding: 10
                    backgroundRadius: 10
                    onAboutToShow: shows = App.library.airingSchedule()

                    function dayOf(at) {
                        const days = Math.round((new Date(at).setHours(0, 0, 0, 0) - new Date().setHours(0, 0, 0, 0)) / 86400000)
                        return days === 0 ? qsTr("Today") : days === 1 ? qsTr("Tomorrow") : Qt.formatDate(at, "dddd d MMM")
                    }

                    contentItem: Flickable {
                        implicitHeight: Math.min(scheduleColumn.implicitHeight, 420)
                        contentHeight: scheduleColumn.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: AppScrollBar {}

                        Column {
                            id: scheduleColumn
                            width: parent.width
                            spacing: 2

                            Text {
                                visible: schedulePopup.shows.length === 0
                                width: parent.width
                                wrapMode: Text.Wrap
                                padding: 6
                                text: qsTr("No shows scheduled. Shows in Watching appear after the next refresh, if "
                                           + "their provider publishes airing times.")
                                color: Theme.textMuted
                                font.pixelSize: Ui.sp(Theme.compactSize)
                            }
                            Repeater {
                                model: schedulePopup.shows
                                delegate: Column {
                                    id: scheduleEntry
                                    required property var modelData
                                    required property int index
                                    readonly property string day: schedulePopup.dayOf(modelData.at)
                                    width: scheduleColumn.width

                                    Text {
                                        visible: scheduleEntry.index === 0
                                                 || schedulePopup.dayOf(schedulePopup.shows[scheduleEntry.index - 1].at) !== scheduleEntry.day
                                        text: scheduleEntry.day
                                        topPadding: scheduleEntry.index === 0 ? 2 : 10
                                        bottomPadding: 4
                                        leftPadding: 6
                                        color: Theme.textMuted
                                        font.pixelSize: Ui.sp(Theme.compactSize)
                                        font.bold: true
                                    }
                                    Rectangle {
                                        id: scheduleRow
                                        width: parent.width
                                        height: Ui.controlHeight
                                        radius: 8
                                        color: scheduleHover.hovered ? Theme.hoverFill : "transparent"
                                        function open() {
                                            schedulePopup.close()
                                            App.openShowInfo(scheduleEntry.modelData.link, scheduleEntry.modelData.provider,
                                                             scheduleEntry.modelData.title)
                                        }
                                        activeFocusOnTab: true
                                        Keys.onSpacePressed: open()
                                        Keys.onReturnPressed: open()
                                        Keys.onEnterPressed: open()
                                        Accessible.role: Accessible.Button
                                        Accessible.name: scheduleEntry.modelData.title
                                        Accessible.onPressAction: open()
                                        FocusRing { shown: scheduleRow.activeFocus; outset: 0; radius: 8 }
                                        HoverHandler { id: scheduleHover; cursorShape: Qt.PointingHandCursor }
                                        TapHandler { onTapped: scheduleRow.open() }
                                        RowLayout {
                                            anchors { fill: parent; leftMargin: 6; rightMargin: 8 }
                                            spacing: 10
                                            Text {
                                                text: Qt.formatTime(scheduleEntry.modelData.at, "HH:mm")
                                                color: Theme.accent
                                                font.pixelSize: Ui.sp(Theme.compactSize)
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                text: scheduleEntry.modelData.title
                                                elide: Text.ElideRight
                                                color: Theme.textPrimary
                                                font.pixelSize: Ui.sp(Theme.compactSize)
                                            }
                                            Text {
                                                text: qsTr("E%1").arg(scheduleEntry.modelData.episode)
                                                color: Theme.textMuted
                                                font.pixelSize: Ui.sp(Theme.compactSize)
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
                text: libraryGridView.count === 1 ? qsTr("1 show") : qsTr("%1 shows").arg(libraryGridView.count)
                font.pixelSize: Ui.sp(Theme.compactSize)
                color: Theme.textMuted
                verticalAlignment: Qt.AlignVCenter
                Layout.fillHeight: true
                Layout.leftMargin: 4
                Layout.rightMargin: 6
            }
        }
    }

    Timer {
        id: autoScrollTimer
        interval: 16
        repeat: true
        property int direction: 0

        onTriggered: {
            if (direction < 0 && !libraryGridView.atYBeginning)
                libraryGridView.contentY = Math.max(0, libraryGridView.contentY - 8);
            else if (direction > 0 && !libraryGridView.atYEnd)
                libraryGridView.contentY = Math.min(libraryGridView.contentHeight - libraryGridView.height, libraryGridView.contentY + 8);
        }
    }

    MediaGridView {
        id: libraryGridView
        focusPolicy: Qt.NoFocus

        signal contextMenuRequested(int index, int libraryType, string title, string provider)
        rows: App.settings.libraryRows

        anchors {
            left: parent.left
            top: topBarCard.bottom
            bottom: parent.bottom
            right: parent.right
            rightMargin: Ui.sp(Theme.scrollbarGutter)
        }
        footer: Item { width: 1; height: scrollNav.visible || selectionBar.visible ? Math.max(scrollNav.height, selectionBar.height) + 28 : 0 }

        onMovementStarted: scrollNavigation.stop()
        Component.onDestruction: Ui.libraryLastContentY = contentY
        Component.onCompleted: contentY = Ui.libraryLastContentY

        property real savedContentY: 0
        property bool restorePending: false

        function restoreScroll() {
            if (!restorePending)
                return;
            restorePending = false;
            forceLayout();
            contentY = savedContentY;
            returnToBounds();
        }

        Connections {
            target: App.libraryModel
            function onModelAboutToBeReset() {
                libraryGridView.savedContentY = libraryGridView.contentY;
            }
            function onModelReset() {
                libraryGridView.restorePending = true;
                Qt.callLater(libraryGridView.restoreScroll);
            }
        }

        Connections {
            target: App.library
            function onLibraryTypeChanged() {
                libraryGridView.restorePending = false;
                libraryGridView.contentY = 0;
            }
        }

        onContextMenuRequested: (index, libraryType, title, provider) => {
            contextMenu.index = index;
            contextMenu.libraryType = libraryType;
            contextMenu.showTitle = title;
            contextMenu.provider = provider;
            contextMenu.pageUrl = App.showUrl(index, true);
            contextMenu.popup();
        }

        displaced: Transition {
            NumberAnimation {
                properties: "x,y"
                duration: 200
                easing.type: Easing.OutCubic
            }
        }

        ScrollBar.vertical: AppScrollBar {
            parent: libraryGridView.parent
            anchors {
                top: libraryGridView.top
                left: libraryGridView.right
                bottom: libraryGridView.bottom
                leftMargin: (Ui.sp(Theme.scrollbarGutter) - width) / 2
                bottomMargin: scrollNav.visible ? scrollNav.height + 28 : 0
            }
            width: 6
            barOpacity: 0.6
        }

        model: DelegateModel {
            id: visualModel
            model: App.libraryModel

            delegate: DropArea {
                id: dropCell
                keys: ["app/library-card"]
                width: libraryGridView.cellWidth
                height: libraryGridView.cellHeight
                required property string title
                required property string cover
                required property int index
                required property int unwatchedEpisodes
                required property int totalEpisodes
                required property string provider
                required property int libraryType
                required property string link

                property int visualIndex: DelegateModel.itemsIndex
                readonly property bool selected: libraryPage.selection.indexOf(dropCell.link) >= 0

                function activate(mouse) {
                    const at = App.libraryModel.mapToAbsoluteIndex(dropCell.index);
                    if (mouse.button === Qt.LeftButton && (mouse.modifiers & Qt.ShiftModifier))
                        libraryPage.selectTo(dropCell.link);
                    else if (mouse.button === Qt.LeftButton && ((mouse.modifiers & Qt.ControlModifier) || libraryPage.selection.length > 0))
                        libraryPage.toggleSelected(dropCell.link);
                    else if (mouse.button === Qt.LeftButton)
                        App.loadShow(at, true);
                    else if (mouse.button === Qt.RightButton)
                        libraryGridView.contextMenuRequested(at, dropCell.libraryType, dropCell.title, dropCell.provider);
                    else if (mouse.button === Qt.MiddleButton)
                        App.appendToPlaylists(at, true, false);
                }

                onEntered: function (drag) {
                    let from = libraryPage.dragIndex(drag.source, "visualIndex");
                    let to = dropCell.visualIndex;
                    if (from >= 0 && from !== to)
                        visualModel.items.move(from, to);
                }

                onDropped: function (drop) {
                    let fromAbsolute = libraryPage.dragIndex(drop.source, "dragStartAbsoluteIndex");
                    let toAbsolute = App.libraryModel.mapToAbsoluteIndex(dropCell.visualIndex);
                    if (fromAbsolute < 0 || fromAbsolute === toAbsolute)
                        return;
                    let y = libraryGridView.contentY;
                    App.library.move(fromAbsolute, toAbsolute);
                    libraryGridView.contentY = y;
                }

                Item {
                    id: dragBox
                    width: dropCell.width
                    height: dropCell.height

                    property int visualIndex: dropCell.visualIndex
                    property int dragStartAbsoluteIndex: -1
                    readonly property Item handle: !face.item ? null : libraryGridView.rows ? face.item as Item : (face.item as ShowItem).image

                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.verticalCenter: parent.verticalCenter

                    Drag.active: dragHandle.drag.active
                    Drag.source: dragBox
                    Drag.keys: ["app/library-card"]
                    Drag.hotSpot.x: width / 2
                    Drag.hotSpot.y: height / 2

                    Loader {
                        id: face
                        anchors.fill: parent
                        sourceComponent: libraryGridView.rows ? rowFace : cardFace
                    }

                    Rectangle {
                        anchors { fill: parent; margins: 2 }
                        visible: dropCell.selected
                        radius: 12
                        color: Qt.alpha(Theme.accent, 0.14)
                        border { width: 2; color: Theme.accent }
                    }

                    Component {
                        id: cardFace
                        ShowItem {
                            id: cardItem
                            showTitle: dropCell.title
                            showCover: dropCell.cover
                            showAddAction: false
                            badgeText: dropCell.provider
                            libraryType: App.library.searchAllLists ? dropCell.libraryType : -1

                            onImageClicked: mouse => dropCell.activate(mouse)
                            onPlayClicked: App.appendToPlaylists(App.libraryModel.mapToAbsoluteIndex(dropCell.index), true, true)

                            UnwatchedBadge {
                                parent: cardItem.image
                                count: dropCell.unwatchedEpisodes
                                anchors {
                                    top: parent.top
                                    topMargin: 6
                                    left: App.library.searchAllLists ? parent.left : undefined
                                    right: App.library.searchAllLists ? undefined : parent.right
                                    leftMargin: 6
                                    rightMargin: 6
                                }
                            }
                        }
                    }

                    Component {
                        id: rowFace
                        Rectangle {
                            id: row
                            radius: 10
                            color: rowHover.hovered ? Theme.hoverFill : "transparent"
                            Behavior on color { ColorAnimation { duration: 120 } }

                            HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                                onClicked: mouse => dropCell.activate(mouse)
                            }

                            RowLayout {
                                anchors { fill: parent; margins: 6; rightMargin: 12 }
                                spacing: 12

                                Rectangle {
                                    Layout.fillHeight: true
                                    Layout.preferredWidth: height / libraryGridView.imageAspectRatio
                                    radius: 6
                                    clip: true
                                    color: Theme.surface
                                    Image {
                                        anchors.fill: parent
                                        source: dropCell.cover
                                        fillMode: Image.PreserveAspectCrop
                                        asynchronous: true
                                        sourceSize: Qt.size(360, Math.round(360 * libraryGridView.imageAspectRatio))
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 3
                                    Text {
                                        Layout.fillWidth: true
                                        text: dropCell.title
                                        elide: Text.ElideRight
                                        color: Theme.textPrimary
                                        font.pixelSize: Ui.sp(Theme.bodySize)
                                        font.weight: Font.DemiBold
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        color: Theme.textMuted
                                        font.pixelSize: Ui.sp(Theme.compactSize)
                                        text: {
                                            const parts = [dropCell.provider]
                                            if (App.library.searchAllLists)
                                                parts.push(Ui.libraryTypeNames[dropCell.libraryType])
                                            const total = dropCell.totalEpisodes, left = dropCell.unwatchedEpisodes
                                            if (total > 0 && left >= 0)
                                                parts.push(left === 0 ? qsTr("All %1 watched").arg(total)
                                                         : left === total ? qsTr("%1 episodes").arg(total)
                                                                          : qsTr("%1 of %2 watched").arg(total - left).arg(total))
                                            return parts.filter(part => part).join("  ·  ")
                                        }
                                    }
                                }

                                UnwatchedBadge { count: dropCell.unwatchedEpisodes; compact: true }

                                Rectangle {
                                    implicitWidth: Ui.controlHeight
                                    implicitHeight: Ui.controlHeight
                                    radius: 10
                                    opacity: rowHover.hovered ? 1 : 0
                                    color: playArea.containsMouse ? Theme.accent : Theme.hoverFill
                                    AppIcon {
                                        anchors.centerIn: parent
                                        name: "play"
                                        size: 18
                                        color: playArea.containsMouse ? Theme.onAccent : Theme.textPrimary
                                    }
                                    MouseArea {
                                        id: playArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        onClicked: App.appendToPlaylists(App.libraryModel.mapToAbsoluteIndex(dropCell.index), true, true)
                                    }
                                    AppToolTip { text: qsTr("Play"); visible: playArea.containsMouse }
                                }
                            }
                        }
                    }

                    MouseArea {
                        id: dragHandle
                        readonly property Item handle: dragBox.handle
                        x: handle ? handle.x : 0
                        y: handle ? handle.y : 0
                        width: handle ? handle.width : 0
                        height: handle ? handle.height : 0
                        propagateComposedEvents: true
                        drag.target: App.libraryModel.sortMode !== LibraryProxyModel.Manual || App.library.searchAllLists ? null : dragBox
                        cursorShape: drag.active ? Qt.ClosedHandCursor : Qt.PointingHandCursor

                        onPressed: {
                            dragBox.dragStartAbsoluteIndex = App.libraryModel.mapToAbsoluteIndex(dropCell.index);
                        }

                        onReleased: {
                            autoScrollTimer.direction = 0;
                            autoScrollTimer.stop();
                            dragBox.Drag.drop();
                        }

                        onPositionChanged: mouse => {
                            if (!drag.active)
                                return;
                            let posInGrid = libraryGridView.mapFromItem(dragHandle, mouse.x, mouse.y);

                            if (posInGrid.y < libraryGridView.height * 0.1) {
                                autoScrollTimer.direction = -1;
                                if (!autoScrollTimer.running)
                                    autoScrollTimer.start();
                            } else if (posInGrid.y > libraryGridView.height * 0.9) {
                                autoScrollTimer.direction = 1;
                                if (!autoScrollTimer.running)
                                    autoScrollTimer.start();
                            } else {
                                autoScrollTimer.direction = 0;
                                autoScrollTimer.stop();
                            }
                        }
                    }

                    states: [
                        State {
                            when: dragBox.Drag.active

                            AnchorChanges {
                                target: dragBox
                                anchors.horizontalCenter: undefined
                                anchors.verticalCenter: undefined
                            }

                            ParentChange {
                                target: dragBox
                                parent: libraryPage
                            }
                        }
                    ]
                }

                states: [
                    State {
                        when: dropCell.containsDrag && dropCell.drag.source != dragBox
                        PropertyChanges {
                            dropCell.opacity: 0.7
                        }
                    }
                ]
            }
        }
    }

    EmptyState {
        anchors.centerIn: libraryGridView
        visible: libraryGridView.count === 0
        readonly property bool filtered: App.library.displayedCount > 0
        icon: "library"
        title: filtered ? qsTr("No shows match these filters.") : qsTr("No shows in this list.")
        hint: filtered ? "" : qsTr("Add a show from its page with + Library.")
        actionText: filtered ? qsTr("Clear filters") : qsTr("Browse shows")
        onActionTriggered: filtered ? libraryPage.clearFilters() : Ui.gotoPage(MainWindow.Explore)
    }

    Card {
        id: selectionBar
        visible: libraryPage.selection.length > 0
        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 14 }
        width: Math.min(selectionRow.implicitWidth + 16, parent.width - 2 * (scrollNav.width + 24))
        height: Ui.toolbarHeight
        radius: 14

        RowLayout {
            id: selectionRow
            anchors { fill: parent; margins: 6 }
            spacing: 6

            Text {
                text: qsTr("%1 selected").arg(libraryPage.selection.length)
                font.pixelSize: Ui.sp(Theme.compactSize)
                color: Theme.textPrimary
                verticalAlignment: Qt.AlignVCenter
                Layout.fillHeight: true
                Layout.leftMargin: 8
                Layout.rightMargin: 4
            }
            AppButton {
                id: moveButton
                text: qsTr("Move to list")
                fontSize: Theme.compactSize
                secondary: true
                Layout.fillHeight: true
                onClicked: moveMenu.popup(moveButton, 0, -moveMenu.implicitHeight - 6)
            }
            AppButton {
                text: qsTr("Queue")
                fontSize: Theme.compactSize
                secondary: true
                Layout.fillHeight: true
                onClicked: libraryPage.queueSelected()
            }
            AppButton {
                text: qsTr("Remove")
                fontSize: Theme.compactSize
                secondary: true
                Layout.fillHeight: true
                onClicked: libraryPage.removeSelected()
            }
            IconButton {
                iconName: "x"
                iconSize: 19
                implicitWidth: Ui.controlHeight
                implicitHeight: Ui.controlHeight
                boxRadius: 10
                tip: qsTr("Clear selection")
                onClicked: libraryPage.clearSelection()
            }
        }
    }

    LibraryTypeMenu {
        id: moveMenu
        modal: true
        currentType: App.library.searchAllLists ? -1 : App.library.libraryType
        onPicked: type => libraryPage.moveSelected(type)
    }

    NumberAnimation {
        id: scrollNavigation
        target: libraryGridView
        property: "contentY"
        duration: Theme.reduceMotion ? 0 : 480
        easing.type: Easing.InOutCubic
    }
    IconButton {
        id: scrollNav
        readonly property real bottomY: libraryGridView.originY + Math.max(0, libraryGridView.contentHeight - libraryGridView.height)
        readonly property bool nearBottom: libraryGridView.contentY >= bottomY - 36
        anchors {
            right: parent.right
            bottom: parent.bottom
            rightMargin: 10
            bottomMargin: 14
        }
        visible: libraryGridView.contentHeight > libraryGridView.height
        implicitWidth: 38
        implicitHeight: 38
        boxRadius: 19
        variant: "accent"
        iconName: nearBottom ? "arrow-up" : "arrow-down"
        iconSize: 18
        tip: nearBottom ? qsTr("Scroll to top") : qsTr("Scroll to bottom")
        onClicked: {
            scrollNavigation.stop();
            scrollNavigation.to = nearBottom ? libraryGridView.originY : bottomY;
            scrollNavigation.start();
        }
    }

    AppMenu {
        id: contextMenu
        modal: true
        property int index
        property int libraryType
        property string showTitle: ""
        property string provider: ""
        property string pageUrl: ""

        Action {
            icon.name: "play"
            text: qsTr("Play")
            onTriggered: App.appendToPlaylists(contextMenu.index, true, true)
        }

        Action {
            icon.name: "list-plus"
            text: qsTr("Queue")
            onTriggered: App.appendToPlaylists(contextMenu.index, true, false)
        }

        AppMenu {
            title: qsTr("Move")
            iconName: "arrow-up-down"
            shown: !App.library.searchAllLists
            Action {
                icon.name: "arrow-up"
                text: qsTr("Move to top")
                onTriggered: {
                    App.library.move(contextMenu.index, 0);
                    libraryGridView.contentY = 0;
                }
            }
            Action {
                icon.name: "arrow-down"
                text: qsTr("Move to bottom")
                onTriggered: {
                    App.library.move(contextMenu.index, App.library.count() - 1);
                    libraryGridView.positionViewAtEnd();
                }
            }
        }

        LibraryTypeMenu {
            currentType: contextMenu.libraryType
            onPicked: type => App.library.changeLibraryTypeAt(contextMenu.index, type, -1)
        }

        Action {
            icon.name: "server"
            text: qsTr("Migrate provider")
            onTriggered: migrateDialog.openFor(contextMenu.index)
        }

        SearchOnMenu {
            showTitle: contextMenu.showTitle
            ownProvider: contextMenu.provider
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

        TextMenu.Separator {}

        Action {
            property bool destructive: true
            icon.name: "trash-2"
            text: qsTr("Remove")
            onTriggered: {
                const rows = App.library.removeAt(contextMenu.index)
                if (rows.length > 0)
                    Ui.offerUndo(qsTr("Removed “%1” from the library.").arg(rows[0].title),
                                      () => App.library.restoreRows("shows", rows))
            }
        }
    }

    MigrateDialog {
        id: migrateDialog
    }
}
