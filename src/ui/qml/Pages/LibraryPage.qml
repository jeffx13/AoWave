pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../Components"
import App
import ".."

Rectangle {
    id: libraryPage
    color: "transparent"

    function dragIndex(source, propertyName) {
        if (!source)
            return -1;
        const value = source[propertyName];
        return typeof value === "number" ? value : -1;
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
                pixelSize: Globals.sp(unwatchedBadge.compact ? 17 : 20)
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
        if (event.modifiers & Qt.ControlModifier) return;
        switch (event.key) {
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
        height: Globals.toolbarHeight
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

            AppComboBox {
                id: libraryTypeComboBox
                Layout.fillHeight: true
                fontSize: Theme.compactSize
                focusPolicy: Qt.NoFocus
                currentIndex: App.library.libraryType
                // A search covers every list; the cards say which.
                enabled: !App.library.searchAllLists
                onActivated: index => App.library.libraryType = index
                text: ""
                model: Globals.libraryTypeNames
            }

            AppComboBox {
                Layout.fillHeight: true
                fontSize: Theme.compactSize
                focusPolicy: Qt.NoFocus
                currentIndex: App.libraryModel.typeFilter
                onActivated: index => App.libraryModel.typeFilter = index
                text: ""
                // Order matches ShowData::ShowType.
                model: [qsTr("All types"), qsTr("Anime"), qsTr("Movies"), qsTr("TV Series"), qsTr("Variety Shows"), qsTr("Documentaries")]
            }

            AppComboBox {
                Layout.fillHeight: true
                fontSize: Theme.compactSize
                focusPolicy: Qt.NoFocus
                currentIndex: App.libraryModel.sortMode
                onActivated: index => App.libraryModel.sortMode = index
                text: ""
                // "My order" is the one you drag cards into.
                model: [qsTr("My order"), qsTr("A-Z"), qsTr("Most unwatched")]
                displayText: qsTr("Sort: %1").arg(currentText)
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

                // Regex and case toggles live inside the field.
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
                            height: Globals.controlHeight - 14
                            radius: 6
                            color: toggle.modelData.on ? Theme.accentMuted : toggleHover.hovered ? Theme.hoverFill : "transparent"
                            Behavior on color { ColorAnimation { duration: 100 } }
                            Text {
                                id: toggleLabel
                                anchors.centerIn: parent
                                text: toggle.modelData.label
                                font.pixelSize: Globals.sp(Theme.metadataSize)
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
                iconName: "refresh-cw"
                iconSize: 19
                implicitWidth: Globals.controlHeight
                implicitHeight: Globals.controlHeight
                boxRadius: 10
                tip: qsTr("Refresh unwatched counts") + (App.shortcuts.bindings.reload.length > 0 ? " (" + App.shortcuts.bindings.reload[0] + ")" : "")
                onClicked: App.library.refreshAllUnwatched()
            }
            IconButton {
                iconName: App.settings.libraryRows ? "layout-grid" : "list"
                iconSize: 19
                implicitWidth: Globals.controlHeight
                implicitHeight: Globals.controlHeight
                boxRadius: 10
                tip: App.settings.libraryRows ? qsTr("Show as grid") : qsTr("Show as list")
                onClicked: App.settings.libraryRows = !App.settings.libraryRows
            }
            IconButton {
                id: scheduleButton
                iconName: "calendar"
                iconSize: 19
                implicitWidth: Globals.controlHeight
                implicitHeight: Globals.controlHeight
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
                                text: qsTr("Nothing scheduled. Airing times come from Watching shows whose provider publishes them, after the next refresh.")
                                color: Theme.textMuted
                                font.pixelSize: Globals.sp(Theme.compactSize)
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
                                        font.pixelSize: Globals.sp(Theme.compactSize)
                                        font.bold: true
                                    }
                                    Rectangle {
                                        width: parent.width
                                        height: Globals.controlHeight
                                        radius: 8
                                        color: scheduleHover.hovered ? Theme.hoverFill : "transparent"
                                        HoverHandler { id: scheduleHover; cursorShape: Qt.PointingHandCursor }
                                        TapHandler {
                                            onTapped: {
                                                schedulePopup.close()
                                                App.openShowInfo(scheduleEntry.modelData.link, scheduleEntry.modelData.provider,
                                                                 scheduleEntry.modelData.title)
                                            }
                                        }
                                        RowLayout {
                                            anchors { fill: parent; leftMargin: 6; rightMargin: 8 }
                                            spacing: 10
                                            Text {
                                                text: Qt.formatTime(scheduleEntry.modelData.at, "HH:mm")
                                                color: Theme.accent
                                                font.pixelSize: Globals.sp(Theme.compactSize)
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                text: scheduleEntry.modelData.title
                                                elide: Text.ElideRight
                                                color: Theme.textPrimary
                                                font.pixelSize: Globals.sp(Theme.compactSize)
                                            }
                                            Text {
                                                text: qsTr("E%1").arg(scheduleEntry.modelData.episode)
                                                color: Theme.textMuted
                                                font.pixelSize: Globals.sp(Theme.compactSize)
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
                font.pixelSize: Globals.sp(Theme.compactSize)
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
        interval: 16  // ~60fps
        repeat: true
        property int direction: 0  // -1 up, +1 down, 0 stop

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

        signal contextMenuRequested(int index, int libraryType)
        rows: App.settings.libraryRows

        anchors {
            left: parent.left
            top: topBarCard.bottom
            bottom: parent.bottom
            right: parent.right
            rightMargin: Globals.sp(Theme.scrollbarGutter)
        }
        // Room under the last row for the floating button.
        footer: Item { width: 1; height: scrollNav.visible ? scrollNav.height + 28 : 0 }

        onMovementStarted: scrollNavigation.stop()
        Component.onDestruction: Globals.libraryLastContentY = contentY
        Component.onCompleted: contentY = Globals.libraryLastContentY

        property real savedContentY: 0
        property bool restorePending: false

        // Deferred: the view clears contentY while handling the model reset.
        function restoreScroll() {
            if (!restorePending)
                return;
            restorePending = false;
            forceLayout();
            contentY = savedContentY;
            returnToBounds();
        }

        // A migrate resets the model, which would snap the grid.
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

        onContextMenuRequested: (index, libraryType) => {
            contextMenu.index = index;
            contextMenu.libraryType = libraryType;
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
                leftMargin: (Globals.sp(Theme.scrollbarGutter) - width) / 2
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
                // Without a key an external file drag lands here too.
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

                property int visualIndex: DelegateModel.itemsIndex

                function activate(mouse) {
                    const at = App.libraryModel.mapToAbsoluteIndex(dropCell.index);
                    if (mouse.button === Qt.LeftButton)
                        App.loadShow(at, true);
                    else if (mouse.button === Qt.RightButton)
                        libraryGridView.contextMenuRequested(at, dropCell.libraryType);
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
                    // What the pointer drags by: a card's cover, or a whole row.
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

                    Component {
                        id: cardFace
                        ShowItem {
                            showTitle: dropCell.title
                            showCover: dropCell.cover
                            showAddAction: false
                            badgeText: dropCell.provider
                            libraryType: App.library.searchAllLists ? dropCell.libraryType : -1

                            onImageClicked: mouse => dropCell.activate(mouse)
                            onPlayClicked: App.appendToPlaylists(App.libraryModel.mapToAbsoluteIndex(dropCell.index), true, true)

                            UnwatchedBadge {
                                count: dropCell.unwatchedEpisodes
                                anchors {
                                    top: parent.top
                                    topMargin: 6
                                    // The list badge has the right corner.
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
                                        // The card's decode size, so both views share one cached image.
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
                                        font.pixelSize: Globals.sp(Theme.bodySize)
                                        font.weight: Font.DemiBold
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        color: Theme.textMuted
                                        font.pixelSize: Globals.sp(Theme.compactSize)
                                        text: {
                                            const parts = [dropCell.provider]
                                            if (App.library.searchAllLists)
                                                parts.push(Globals.libraryTypeNames[dropCell.libraryType])
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

                                // A MouseArea, not a button: the drag handle over the row passes clicks only to those.
                                Rectangle {
                                    implicitWidth: Globals.controlHeight
                                    implicitHeight: Globals.controlHeight
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
                        // Order is per list, and only the default sort shows it.
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

    // After the grid: an empty GridView still takes the presses the button needs.
    EmptyState {
        anchors.centerIn: libraryGridView
        visible: libraryGridView.count === 0
        readonly property bool filtered: App.library.displayedCount > 0
        icon: "library"
        title: filtered ? qsTr("No shows match these filters.") : qsTr("Nothing in this list yet.")
        hint: filtered ? "" : qsTr("Add a show from its page with + Library.")
        actionText: filtered ? "" : qsTr("Browse shows")
        onActionTriggered: Globals.gotoPage(AppShell.Search)
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
        active: true
        hoverColor: hovered ? Theme.accent : Qt.alpha(Theme.accent, 0.85)
        iconColor: Theme.onAccent
        iconHoverColor: Theme.onAccent
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

        Action {
            text: qsTr("Play")
            onTriggered: App.appendToPlaylists(contextMenu.index, true, true)
        }

        Action {
            text: qsTr("Queue")
            onTriggered: App.appendToPlaylists(contextMenu.index, true, false)
        }

        AppMenu {
            title: qsTr("Move")
            // Order is per list.
            enabled: !App.library.searchAllLists
            Action {
                text: qsTr("Move to Top")
                onTriggered: {
                    App.library.move(contextMenu.index, 0);
                    libraryGridView.contentY = 0;
                }
            }
            Action {
                text: qsTr("Move to Bottom")
                onTriggered: {
                    App.library.move(contextMenu.index, App.library.count() - 1);
                    libraryGridView.contentY = libraryGridView.contentHeight - libraryGridView.height;
                }
            }
        }

        Action {
            text: qsTr("Migrate Provider")
            onTriggered: migrateDialog.openFor(contextMenu.index)
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

        LibraryTypeMenu {
            currentType: contextMenu.libraryType
            onPicked: type => App.library.changeLibraryTypeAt(contextMenu.index, type, -1)
        }

        Action {
            text: qsTr("Remove")
            onTriggered: {
                const rows = App.library.removeAt(contextMenu.index)
                if (rows.length > 0)
                    Globals.offerUndo(qsTr("Removed \u201c%1\u201d from the library").arg(rows[0].title),
                                      () => App.library.restoreRows("shows", rows))
            }
        }
    }

    MigrateDialog {
        id: migrateDialog
    }
}
