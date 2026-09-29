pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import "./../Components"
import QtQuick.Layouts
import App
import ".."
import QtQuick.Effects

Item {
    id: infoPage
    focus: true
    readonly property var currentShow: App.show
    // The show's page on its site, where a link can say it.
    readonly property string pageUrl: currentShow.link !== "" ? App.showUrl(-1, false) : ""
    // The download controls, folded away until asked for.
    property bool downloadsOpen: false

    HoverHandler {
        cursorShape: Qt.ArrowCursor
    }

    component MetaChip: Rectangle {
        id: chip
        property string iconName
        property string chipValue
        property bool   pill: true
        property color  iconColor:  Theme.textSecondary
        property color  valueColor: Theme.textSecondary

        visible: chipValue.length > 0
        // A Flow places items along a row, so clamping to its width lets a mid-row chip overrun.
        readonly property real cap: parent ? parent.width * 0.6 : implicitWidth
        implicitWidth: chipRow.implicitWidth + 16
        width: Math.min(implicitWidth, cap)
        height: Globals.sp(pill ? 32 : 28)
        radius: pill ? height / 2 : 6
        color: Theme.surfaceAlt
        border.color: Theme.border
        border.width: 1

        RowLayout {
            id: chipRow
            anchors { fill: parent; leftMargin: 8; rightMargin: 8 }
            spacing: 5
            AppIcon {
                Layout.alignment: Qt.AlignVCenter
                name: chip.iconName
                size: chip.pill ? 16 : 14
                color: chip.iconColor
            }
            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                // Bounds the row's implicit width, which the chip sizes against.
                Layout.maximumWidth: Math.max(0, chip.cap - 32 - chipRow.spacing)
                text: chip.chipValue
                color: chip.valueColor
                font.pixelSize: Globals.sp(Theme.metadataSize)
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }

        }
    }

    function correctIndex(index) {
        return App.show.episodes.sourceIndex(index)
    }

    // Providers fill gaps with placeholders; a chip reading '?' says nothing.
    function knownScore(score) {
        const s = (score ?? "").trim()
        return /^(\?|n\/?a|-+|0+(\.0+)?)$/i.test(s) ? "" : s
    }

    function airedSince(dates) {
        const d = (dates ?? "").trim()
        const open = d.match(/^(.*\S)\s+to\s+\?$/)
        return open ? qsTr("Since %1").arg(open[1]) : d
    }

    // Playback moves episode progress while this page is out of sight.
    onVisibleChanged: if (visible) App.show.episodes.refreshProgress()

    // Bumped whenever an episode's download state may have moved, so the rows ask again.
    property int downloadRevision: 0
    Connections {
        target: App.downloads
        function onRowsInserted() { infoPage.downloadRevision++ }
        function onRowsRemoved() { infoPage.downloadRevision++ }
        function onFinishedChanged() { infoPage.downloadRevision++ }
    }
    Connections {
        target: App.settings
        function onDownloadDirChanged() { infoPage.downloadRevision++ }
    }

    Connections {
        target: App.show
        function onShowChanged() { epFilterField.text = ""; libraryComboBox.rebuildModel() }
    }
    Connections {
        target: App.library
        function onLibraryChanged() { libraryComboBox.rebuildModel() }
    }

    Image {
        id: bgImage
        anchors.fill: parent
        source: infoPage.currentShow.coverUrl
        fillMode: Image.PreserveAspectCrop
        visible: false
    }
    // No padding: the blur fills the page edge to edge instead of fading out at the borders.
    MultiEffect {
        anchors.fill: parent
        source: bgImage
        autoPaddingEnabled: false
        blurEnabled: true
        blurMax: 64
        blur: 1.0
    }
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.alpha(Theme.background, 0.82) }
            GradientStop { position: 0.4; color: Qt.alpha(Theme.background, 0.94) }
            GradientStop { position: 1.0; color: Theme.background }
        }
    }

    Card {
        id: episodePanel
        width: Math.min(parent.width * 0.32, 420)
        anchors {
            right: parent.right
            top: parent.top
            bottom: parent.bottom
            topMargin: 12
            rightMargin: 12
            bottomMargin: 12
        }
        radius: 16
        clip: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                Layout.margins: 8
                Layout.bottomMargin: 0
                spacing: 6

                // "13 EPISODES", the number in the accent colour; "3 MATCHES" while filtering.
                Text {
                    readonly property int shown: episodeListView.count
                    readonly property string number: "<font color='" + Theme.textAccent + "'>" + shown + "</font>"
                    text: shown === 0 ? qsTr("EPISODES")
                        : epFilterField.text.length > 0 ? (shown === 1 ? qsTr("%1 MATCH") : qsTr("%1 MATCHES")).arg(number)
                        : (shown === 1 ? qsTr("%1 EPISODE") : qsTr("%1 EPISODES")).arg(number)
                    textFormat: Text.StyledText
                    color: Theme.textMuted
                    Layout.fillWidth: true
                    font {
                        pixelSize: Globals.sp(20)
                        bold: true
                        letterSpacing: 1.5
                    }
                }

                IconButton {
                    visible: episodeListView.lastWatchedIndex >= 0
                    Layout.preferredWidth: 26
                    Layout.preferredHeight: 26
                    iconName: "locate-fixed"
                    iconSize: 18
                    boxRadius: 6
                    iconColor: Theme.textMuted
                    iconHoverColor: Theme.textAccent
                    tip: qsTr("Go to the current episode")
                    onClicked: episodeListView.positionViewAtIndex(episodeListView.lastWatchedIndex, ListView.Center)
                }

                IconButton {
                    Layout.preferredWidth: 26
                    Layout.preferredHeight: 26
                    iconName: "arrow-up-down"
                    iconSize: 18
                    boxRadius: 6
                    iconColor: Theme.textMuted
                    iconHoverColor: Theme.textAccent
                    tip: App.show.episodes.reversed ? qsTr("Oldest first") : qsTr("Newest first")
                    onClicked: App.show.episodes.reversed = !App.show.episodes.reversed
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                Layout.leftMargin: 10
                Layout.rightMargin: 10
                color: Theme.border
            }

            AppTextField {
                id: epFilterField
                Layout.fillWidth: true
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                Layout.topMargin: 2
                placeholderText: qsTr("Filter episodes...")
                color: Theme.textPrimary
                placeholderTextColor: Theme.textMuted
                fontSize: Theme.bodySize
                onTextChanged: App.show.episodes.filterText = text
            }

            // A show with more than one season can be narrowed to one.
            Flow {
                visible: App.show.episodes.seasons.length > 0
                Layout.fillWidth: true
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                Layout.topMargin: 6
                spacing: 6
                Repeater {
                    model: App.show.episodes.seasons.length > 0 ? [0].concat(App.show.episodes.seasons) : []
                    delegate: Rectangle {
                        id: seasonPill
                        required property int modelData
                        readonly property bool chosen: App.show.episodes.season === modelData
                        width: seasonText.implicitWidth + 20
                        height: Globals.sp(28)
                        radius: height / 2
                        color: chosen ? Theme.accent : seasonHover.hovered ? Theme.hoverFill : Theme.surfaceAlt
                        Text {
                            id: seasonText
                            anchors.centerIn: parent
                            text: seasonPill.modelData === 0 ? qsTr("All") : qsTr("S%1").arg(seasonPill.modelData)
                            color: seasonPill.chosen ? Theme.onAccent : Theme.textSecondary
                            font.pixelSize: Globals.sp(Theme.compactSize)
                        }
                        HoverHandler { id: seasonHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: App.show.episodes.season = seasonPill.modelData }
                    }
                }
            }

            ListView {
                id: episodeListView
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 4
                clip: true
                model: App.show.episodes
                spacing: 2
                boundsBehavior: Flickable.StopAtBounds

                property int lastWatchedIndex: -1

                function centerLastWatched() {
                    if (lastWatchedIndex >= 0 && lastWatchedIndex < count)
                        positionViewAtIndex(lastWatchedIndex, ListView.Center)
                }

                function syncLastWatched() {
                    lastWatchedIndex = App.show.episodes.visibleIndex(App.show.lastWatchedIndex)
                    Qt.callLater(centerLastWatched)
                }

                Component.onCompleted: Qt.callLater(syncLastWatched)

                Connections {
                    target: App.show
                    function onLastWatchedIndexChanged() { episodeListView.syncLastWatched() }
                    function onShowChanged() { episodeListView.syncLastWatched() }
                }

                Connections {
                    target: App.show.episodes
                    function onReversedChanged() { episodeListView.syncLastWatched() }
                    function onFilterTextChanged() { episodeListView.syncLastWatched() }
                    function onSeasonChanged() { episodeListView.syncLastWatched() }
                }

                ScrollBar.vertical: AppScrollBar { width: 9; minimumSize: 0.06 }

                delegate: Rectangle {
                    id: ep
                    required property string title
                    required property real episodeNumber
                    required property int seasonNumber
                    required property int index
                    required property real progress
                    required property string thumbnail
                    required property real airedAt
                    required property bool preview
                    readonly property int downloadState: infoPage.downloadRevision >= 0
                                                         ? App.episodeDownloadState(infoPage.correctIndex(index)) : 0
                    readonly property bool pictured: thumbnail !== ""
                    // Specials, trailers and extras come without a number, and show none.
                    readonly property bool numbered: episodeNumber >= 0
                    readonly property string numberText: Math.floor(episodeNumber) === episodeNumber
                                                         ? Math.floor(episodeNumber).toString() : episodeNumber.toFixed(1)
                    property bool isCurrent: episodeListView.lastWatchedIndex === index
                    readonly property bool hovered: rowHover.hovered
                    readonly property bool watched: infoPage.correctIndex(index) < App.show.continueIndex
                                                    || progress >= App.settings.watchedPercent / 100
                    readonly property bool started: progress > 0 && !watched

                    width: episodeListView.width - Globals.sp(Theme.scrollbarGutter)
                    // The same for every row, whatever it has to say.
                    height: ep.pictured ? Globals.sp(66) : Globals.sp(52)
                    radius: 10
                    color: isCurrent ? Qt.alpha(Theme.accent, 0.14) : (hovered ? Qt.alpha(Theme.accent, 0.08) : "transparent")
                    border.color: isCurrent ? Qt.alpha(Theme.accent, 0.35) : "transparent"
                    border.width: isCurrent ? 1 : 0
                    Behavior on color { ColorAnimation { duration: 80 } }

                    // A handler, not the MouseArea: it keeps the row hovered while over its buttons.
                    HoverHandler { id: rowHover }

                    Rectangle {
                        visible: ep.isCurrent
                        width: 3
                        radius: 1.5
                        color: Theme.accent
                        anchors {
                            left: parent.left
                            top: parent.top
                            bottom: parent.bottom
                            leftMargin: 1
                            topMargin: 8
                            bottomMargin: 8
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.AllButtons
                        cursorShape: Qt.PointingHandCursor
                        onClicked: (mouse) => {
                            const ci = infoPage.correctIndex(ep.index)
                            if (mouse.button === Qt.RightButton) episodeMenu.show(ci, ep.watched, ep.preview, ep.downloadState)
                            else App.playFromEpisodeList(ci, false)
                        }
                    }

                    RowLayout {
                        anchors {
                            fill: parent
                            leftMargin: 8
                            rightMargin: 6
                        }
                        spacing: 8

                        // The number, on the episode's picture where the provider has one.
                        Item {
                            Layout.preferredWidth: ep.pictured ? Globals.sp(96) : Math.max(Globals.sp(36), epNumText.implicitWidth + 14)
                            Layout.preferredHeight: ep.pictured ? Globals.sp(54) : Globals.sp(36)

                            Image {
                                anchors.fill: parent
                                visible: ep.pictured
                                source: ep.thumbnail
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                sourceSize.width: 200
                            }
                            Rectangle {
                                anchors.fill: parent
                                visible: ep.pictured
                                color: "transparent"
                                border.color: ep.isCurrent ? Theme.accent : Theme.border
                                border.width: ep.isCurrent ? 2 : 1
                            }
                            // How far you got, along the picture's foot and in its corner.
                            Rectangle {
                                visible: ep.pictured && ep.started
                                anchors { left: parent.left; bottom: parent.bottom; margins: 1 }
                                width: (parent.width - 2) * Math.min(1, ep.progress)
                                height: 3
                                color: Theme.accent
                            }
                            Rectangle {
                                visible: ep.pictured && ep.started
                                anchors { right: parent.right; bottom: parent.bottom; margins: 4 }
                                width: percentText.implicitWidth + 10
                                height: Globals.sp(20)
                                radius: 5
                                color: Qt.alpha("black", 0.72)
                                Text {
                                    id: percentText
                                    anchors.centerIn: parent
                                    text: epMeta.percent + "%"
                                    color: "white"
                                    font.pixelSize: Globals.sp(13)
                                    font.bold: true
                                }
                            }
                            // An unnumbered episode keeps its box in a plain list, so the titles line
                            // up, with a mark in place of a number; on a picture it needs none.
                            Rectangle {
                                visible: !ep.pictured || ep.numbered
                                x: ep.pictured ? 4 : 0
                                y: ep.pictured ? parent.height - height - 4 : 0
                                width: ep.pictured ? epNumText.implicitWidth + 10 : parent.width
                                height: ep.pictured ? Globals.sp(20) : parent.height
                                radius: ep.pictured ? 5 : 10
                                color: ep.isCurrent ? Theme.accent : ep.pictured ? Qt.alpha("black", 0.72) : Theme.surfaceDeep
                                border.color: ep.isCurrent ? Theme.accentLight : Theme.border
                                border.width: ep.pictured ? 0 : 1

                                Text {
                                    id: epNumText
                                    anchors.centerIn: parent
                                    visible: ep.numbered
                                    text: ep.numberText
                                    color: ep.isCurrent ? Theme.onAccent : ep.pictured ? "white" : Theme.textMuted
                                    font {
                                        pixelSize: ep.pictured ? Globals.sp(13) : Globals.sp(18)
                                        bold: true
                                    }
                                }
                                AppIcon {
                                    anchors.centerIn: parent
                                    visible: !ep.numbered
                                    name: ep.preview ? "play" : "star"
                                    size: 16
                                    color: ep.isCurrent ? Theme.onAccent : Theme.textMuted
                                }

                                PulseRing {
                                    visible: ep.isCurrent && !ep.pictured
                                    running: visible
                                    anchors.fill: parent
                                    radius: parent.radius
                                }
                            }
                        }

                        // The title leads. The number is in the box already, so only an untitled
                        // episode spells it out.
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 2

                            MarqueeText {
                                Layout.fillWidth: true
                                Layout.preferredHeight: implicitHeight
                                horizontalAlignment: Text.AlignLeft
                                text: (ep.seasonNumber > 0 ? qsTr("S%1").arg(ep.seasonNumber) + "  \u00b7  " : "")
                                      + (ep.title.trim().length > 0 ? ep.title.trim()
                                         : ep.numbered ? qsTr("Episode %1").arg(ep.numberText)
                                         : ep.preview ? qsTr("Trailer") : qsTr("Extra"))
                                fontSize: Theme.bodySize
                                fontWeight: Font.DemiBold
                                color: ep.isCurrent ? Theme.textAccent : ep.watched ? Theme.textSecondary : Theme.textPrimary
                                spacing: 30
                                marqueeSpeed: 50
                            }

                            // One line, never wrapped: when it aired, and how far you got.
                            RowLayout {
                                id: epMeta
                                readonly property string aired: ep.airedAt > 0 ? Qt.formatDate(new Date(ep.airedAt * 1000), "d MMM yyyy") : ""
                                readonly property int percent: Math.round(ep.progress * 100)
                                visible: aired !== "" || ep.watched || ep.started
                                Layout.fillWidth: true
                                spacing: 4
                                AppIcon {
                                    visible: ep.watched
                                    name: "check"
                                    size: 13
                                    color: Theme.success
                                }
                                Text {
                                    Layout.fillWidth: true
                                    // A picture has the percentage in its corner already.
                                    text: ep.watched ? (epMeta.aired !== "" ? epMeta.aired : qsTr("Watched"))
                                        : ep.started && epMeta.aired === "" ? qsTr("%1% watched").arg(epMeta.percent)
                                        : ep.started && !ep.pictured ? epMeta.aired + "  \u00b7  " + epMeta.percent + "%"
                                        : epMeta.aired
                                    elide: Text.ElideRight
                                    maximumLineCount: 1
                                    font.pixelSize: Globals.sp(Theme.compactSize)
                                    color: ep.watched && epMeta.aired === "" ? Theme.success : Theme.textMuted
                                }
                            }
                        }

                        IconButton {
                            Layout.preferredWidth: 32
                            Layout.preferredHeight: 32
                            // A trailer is not an episode to have watched.
                            visible: !ep.isCurrent && !ep.watched && !ep.preview
                            // On hover only: a tick on every row reads as 'watched'.
                            opacity: ep.hovered ? 1 : 0
                            enabled: ep.hovered
                            iconName: "check"
                            tip: qsTr("Mark watched")
                            iconSize: 18
                            boxRadius: 8
                            iconHoverColor: Theme.textSecondary
                            onClicked: {
                                const sourceIndex = infoPage.correctIndex(ep.index)
                                App.show.markWatched(sourceIndex)
                            }
                        }

                        IconButton {
                            id: dlBtn
                            // 0 not downloaded, 1 queued or running, 2 on disk: DownloadQueue::EpisodeState.
                            readonly property bool taken: ep.downloadState > 0
                            Layout.preferredWidth: 32
                            Layout.preferredHeight: 32
                            iconName: taken ? "check" : "download"
                            iconSize: 18
                            boxRadius: 8
                            active: taken
                            hoverColor: taken ? Qt.alpha(ep.downloadState === 2 ? Theme.success : Theme.accent, 0.15) : Theme.border
                            iconColor: ep.downloadState === 2 ? Theme.success : ep.downloadState === 1 ? Theme.textAccent : Theme.textMuted
                            iconHoverColor: taken ? iconColor : Theme.textSecondary
                            tip: ep.downloadState === 2 ? qsTr("Downloaded") : ep.downloadState === 1 ? qsTr("Queued") : qsTr("Download")
                            onClicked: {
                                if (taken) Globals.gotoPage(AppShell.Download)
                                else App.downloadCurrentShow(infoPage.correctIndex(ep.index))
                            }
                        }
                    }

                    Rectangle {
                        visible: !ep.pictured && ep.started
                        anchors { left: parent.left; bottom: parent.bottom; leftMargin: 10; bottomMargin: 3 }
                        width: (parent.width - 20) * Math.min(1, ep.progress)
                        height: 3
                        radius: 1.5
                        color: Theme.accent
                    }
                }
            }
        }
    }

    Flickable {
        id: infoFlickable
        anchors {
            top: parent.top
            left: parent.left
            right: episodePanel.left
            bottom: parent.bottom
            topMargin: 12
            leftMargin: 12
            rightMargin: 10
            bottomMargin: 12
        }
        contentHeight: infoCol.implicitHeight + 24
        contentWidth: width
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: infoCol
            width: infoFlickable.width
            spacing: 14

            RowLayout {
                Layout.fillWidth: true
                spacing: 16

                Rectangle {
                    Layout.preferredWidth: Math.min(infoFlickable.width * 0.28, 220)
                    Layout.preferredHeight: Layout.preferredWidth * 1.42
                    radius: 14
                    color: Theme.surfaceDeep
                    clip: true
                    border.color: Theme.border
                    border.width: 1

                    Image {
                        id: poster
                        anchors {
                            fill: parent
                            margins: 2
                        }
                        source: infoPage.currentShow.coverUrl
                        fillMode: Image.PreserveAspectCrop
                    }

                    // Reassigning poster.source would kill its binding.
                    Image {
                        anchors {
                            fill: parent
                            margins: 2
                        }
                        fillMode: Image.PreserveAspectCrop
                        visible: poster.status === Image.Error
                        source: visible ? "qrc:/App/resources/images/error_image.png" : ""
                    }

                    Rectangle {
                        anchors.fill: parent
                        radius: parent.radius
                        color: posterHover.containsMouse ? Qt.alpha(Theme.accent, 0.18) : "transparent"
                        Behavior on color { ColorAnimation { duration: 150 } }
                    }

                    MouseArea {
                        id: posterHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: coverPopup.open()
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Text {
                        text: infoPage.currentShow.title
                        font {
                            pixelSize: Globals.sp(28)
                            bold: true
                        }
                        color: Theme.textPrimary
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true

                        // Left: search for it. Middle: the web. Right: the menu with both and more.
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
                            cursorShape: Qt.PointingHandCursor
                            onClicked: (mouse) => {
                                if (mouse.button === Qt.LeftButton) Globals.searchProviders(infoPage.currentShow.title)
                                else if (mouse.button === Qt.MiddleButton) titleMenu.searchWeb()
                                else titleMenu.popup()
                            }
                        }
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: 8

                        Rectangle {
                            visible: (infoPage.currentShow.status ?? "").length > 0
                            width: statusText.implicitWidth + 20
                            height: 30
                            radius: 15
                            color: {
                                let s = (infoPage.currentShow.status ?? "").toLowerCase()
                                if (s.includes("air") || s.includes("ongoing")) return Theme.success
                                if (s.includes("finish") || s.includes("complete")) return Theme.accent
                                return Theme.textMuted
                            }
                            Text {
                                id: statusText
                                anchors.centerIn: parent
                                text: infoPage.currentShow.status ?? ""
                                color: Theme.inkOn(parent.color)
                                font {
                                    pixelSize: Globals.sp(20)
                                    bold: true
                                }
                            }
                        }

                        MetaChip {
                            iconName:   "star"
                            chipValue:  infoPage.knownScore(infoPage.currentShow.rating)
                            iconColor:  Theme.warning
                            valueColor: Theme.textPrimary
                        }

                        MetaChip {
                            iconName:  "eye"
                            chipValue: infoPage.currentShow.views ?? ""
                        }

                        Rectangle {
                            visible: infoPage.currentShow.provider?.name?.length > 0
                            width: provText.implicitWidth + 16
                            height: 30
                            radius: 15
                            color: Theme.surfaceAlt
                            border.color: Theme.border
                            border.width: 1
                            Text {
                                id: provText
                                anchors.centerIn: parent
                                text: infoPage.currentShow.provider?.name ?? ""
                                color: Theme.textAccent
                                font.pixelSize: Globals.sp(Theme.bodySize)
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: Qt.openUrlExternally(infoPage.currentShow.provider?.hostUrl ?? '#')
                            }
                        }
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: 6
                        MetaChip {
                            pill: false
                            iconName: "calendar"
                            chipValue: infoPage.airedSince(infoPage.currentShow.releaseDate)
                            iconColor: Theme.textMuted
                            valueColor: Theme.textMuted
                        }
                        MetaChip {
                            pill: false
                            iconName: "history"
                            chipValue: infoPage.currentShow.updateTime ?? ""
                            iconColor: Theme.textMuted
                            valueColor: Theme.textMuted
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        AppButton {
                            visible: App.show.continueText.length > 0
                            text: ""
                            radius: height / 2
                            Layout.preferredHeight: 42
                            Layout.preferredWidth: Math.min(260, infoFlickable.width * 0.45)
                            leftPadding: 0
                            rightPadding: 0
                            backgroundDefaultColor: Theme.accent
                            contentItem: Item {
                                anchors {
                                    fill: parent
                                    leftMargin: 16
                                    rightMargin: 16
                                }
                                RowLayout {
                                    anchors.fill: parent
                                    spacing: 8
                                    AppIcon {
                                        name: "play"
                                        size: 18
                                        color: Theme.onAccent
                                    }
                                    MarqueeText {
                                        Layout.fillWidth: true
                                        color: Theme.onAccent
                                        text: App.show.continueText
                                        fontSize: Theme.bodySize
                                        spacing: 30
                                        marqueeSpeed: 50
                                    }
                                }
                            }
                            onClicked: App.continueWatching()
                        }

                        AppComboBox {
                            id: libraryComboBox
                            text: "text"
                            Layout.preferredHeight: 42
                            Layout.preferredWidth: Math.min(180, infoFlickable.width * 0.28)
                            focus: false
                            activeFocusOnTab: false
                            placeholderText: qsTr("+ Library")
                            currentIndex: -1
                            // Kept by rebuildModel, which runs on every library change.
                            property int libraryType: -1

                            function rebuildModel() {
                                libraryTypeModel.clear()
                                const types = Globals.libraryTypeNames
                                const lt = App.library.libraryTypeOf(infoPage.currentShow.link)
                                libraryType = lt
                                if (lt === -1) {
                                    for (let i = 0; i < types.length; i++)
                                        libraryTypeModel.append({ text: types[i], disabled: false })
                                    placeholderText = qsTr("+ Library")
                                    currentIndex = -1
                                } else {
                                    libraryTypeModel.append({ text: qsTr("Remove"), disabled: false })
                                    for (let i = 0; i < types.length; i++)
                                        libraryTypeModel.append({ text: types[i], disabled: i === lt })
                                    placeholderText = ""
                                    currentIndex = lt + 1
                                }
                            }

                            Component.onCompleted: rebuildModel()

                            onActivated: (index) => {
                                const lt = App.library.libraryTypeOf(infoPage.currentShow.link)
                                if (lt === -1) {
                                    App.addToLibrary(-1, index)
                                } else if (index === 0) {
                                    App.library.remove(infoPage.currentShow.link)
                                } else {
                                    let t = index - 1
                                    if (t !== lt) App.addToLibrary(-1, t)
                                }
                                rebuildModel()
                            }

                            model: ListModel { id: libraryTypeModel }
                        }

                        IconButton {
                            visible: libraryComboBox.libraryType !== -1
                            Layout.preferredHeight: 42
                            Layout.preferredWidth: 42
                            iconName: "server"
                            iconSize: 20
                            boxRadius: 10
                            tip: qsTr("Migrate provider")
                            onClicked: infoMigrateDialog.openForLink(infoPage.currentShow.link)
                        }

                        IconButton {
                            visible: infoPage.pageUrl !== ""
                            Layout.preferredHeight: 42
                            Layout.preferredWidth: 42
                            iconName: "external-link"
                            iconSize: 20
                            boxRadius: 10
                            tip: qsTr("Open on %1").arg(infoPage.currentShow.provider?.name ?? "")
                            onClicked: Qt.openUrlExternally(infoPage.pageUrl)
                        }

                        IconButton {
                            visible: episodeListView.count > 0
                            Layout.preferredHeight: 42
                            Layout.preferredWidth: 42
                            iconName: "download"
                            iconSize: 20
                            boxRadius: 10
                            active: infoPage.downloadsOpen
                            tip: infoPage.downloadsOpen ? qsTr("Hide download options") : qsTr("Download episodes")
                            onClicked: infoPage.downloadsOpen = !infoPage.downloadsOpen
                        }

                        Item { Layout.fillWidth: true }
                    }

                    // Where you are in the show, and when it continues.
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        spacing: 6
                        visible: App.show.watchedCount > 0 || nextEpisode.visible

                        Text {
                            visible: App.show.watchedCount > 0
                            text: App.show.watchedCount >= App.show.episodeCount
                                  ? qsTr("All %1 watched").arg(App.show.episodeCount)
                                  : qsTr("%1 of %2 watched").arg(App.show.watchedCount).arg(App.show.episodeCount)
                            color: Theme.textSecondary
                            font.pixelSize: Globals.sp(Theme.bodySize)
                        }

                        Rectangle {
                            visible: App.show.watchedCount > 0
                            Layout.preferredWidth: Math.min(Globals.sp(360), parent.width)
                            Layout.preferredHeight: 4
                            radius: 2
                            color: Theme.surfaceAlt
                            Rectangle {
                                width: parent.width * Math.min(1, App.show.watchedCount / Math.max(1, App.show.episodeCount))
                                height: parent.height
                                radius: 2
                                color: Theme.accent
                            }
                        }

                        Text {
                            id: nextEpisode
                            property real now: Date.now()
                            readonly property real due: App.show.nextEpisodeAt ? App.show.nextEpisodeAt.getTime() : NaN
                            visible: due > now
                            text: {
                                const minutes = Math.floor((due - now) / 60000)
                                const d = Math.floor(minutes / 1440)
                                const h = Math.floor(minutes % 1440 / 60)
                                const m = minutes % 60
                                const left = d > 0 ? qsTr("%1d %2h").arg(d).arg(h)
                                           : h > 0 ? qsTr("%1h %2m").arg(h).arg(m) : qsTr("%1m").arg(m)
                                return qsTr("Next episode in %1 · %2").arg(left)
                                    .arg(Qt.formatDateTime(App.show.nextEpisodeAt, "ddd d MMM, HH:mm"))
                            }
                            color: Theme.textAccent
                            font.pixelSize: Globals.sp(Theme.bodySize)
                            Timer {
                                interval: 60000
                                repeat: true
                                running: nextEpisode.visible
                                onTriggered: nextEpisode.now = Date.now()
                            }
                        }
                    }
                }
            }

            // Two rows: one fits beside the episode list at the smallest window. Folded away until
            // the download button above opens them.
            RowLayout {
                visible: infoPage.downloadsOpen && episodeListView.count > 0
                Layout.fillWidth: true
                spacing: 8

                AppIcon {
                    name: "download"
                    size: 18
                    color: Theme.textMuted
                }

                AppComboBox {
                    readonly property var heights: [0, 1080, 720, 480]
                    model: [qsTr("Best quality"), "1080p", "720p", "480p"]
                    currentIndex: Math.max(0, heights.indexOf(App.settings.downloadMaxHeight))
                    onActivated: index => App.settings.downloadMaxHeight = heights[index]
                    Layout.preferredHeight: 38
                }

                AppButton {
                    // Within the chosen season.
                    readonly property int first: Math.max(App.show.continueIndex, App.show.episodes.seasonFirst)
                    readonly property int last: Math.min(first + 2, App.show.episodes.seasonLast, App.show.episodeCount - 1)
                    visible: App.show.continueIndex >= 0 && first <= last && App.show.watchedCount < App.show.episodeCount
                    secondary: true
                    text: last > first ? qsTr("Next %1 unwatched").arg(last - first + 1) : qsTr("Next unwatched")
                    Layout.preferredHeight: 38
                    onClicked: App.downloadCurrentShow(first, last)
                }

                Item { Layout.fillWidth: true }
            }

            RowLayout {
                id: downloadRange
                visible: infoPage.downloadsOpen && episodeListView.count > 0
                Layout.fillWidth: true
                Layout.leftMargin: 26
                spacing: 8

                // From the next episode to watch, not the one last played, to the latest, within
                // the chosen season; set afresh whenever the show, its progress or the season
                // moves. Later, so `to` has moved first.
                function resetRange() {
                    const episodes = App.show.episodes
                    const first = Math.max(episodes.seasonFirst, 0)
                    const last = Math.min(episodes.seasonLast + 1, endSpinBox.to) - 1
                    endSpinBox.value = last + 1
                    startSpinBox.value = Math.min(Math.max(App.show.continueIndex, first), last) + 1
                }
                Connections {
                    target: App.show
                    function onShowChanged() { Qt.callLater(downloadRange.resetRange) }
                    function onLastWatchedIndexChanged() { Qt.callLater(downloadRange.resetRange) }
                }
                Connections {
                    target: App.show.episodes
                    function onRangeChanged() { Qt.callLater(downloadRange.resetRange) }
                }
                Component.onCompleted: resetRange()

                AppSpinBox {
                    id: startSpinBox
                    Layout.preferredWidth: 110
                    from: 1
                    // Not the list's count, which the episode filter narrows; nor a trailing trailer.
                    to: Math.max(1, App.show.episodeCount)
                    onValueModified: {
                        if (value > endSpinBox.value)
                            endSpinBox.value = value
                    }
                }

                AppIcon {
                    name: "arrow-right"
                    size: 18
                    color: Theme.textMuted
                }

                AppSpinBox {
                    id: endSpinBox
                    Layout.preferredWidth: 110
                    from: 1
                    to: startSpinBox.to
                    onValueModified: {
                        if (value < startSpinBox.value)
                            startSpinBox.value = value
                    }
                }

                AppButton {
                    text: qsTr("Download")
                    Layout.preferredHeight: 38
                    onClicked: App.downloadCurrentShow(startSpinBox.value - 1, endSpinBox.value - 1)
                }

                Item { Layout.fillWidth: true }
            }

            Flow {
                visible: (infoPage.currentShow.genresString ?? "").length > 0
                Layout.fillWidth: true
                spacing: 6

                Repeater {
                    model: (infoPage.currentShow.genresString ?? "").split(",").map(s => s.trim()).filter(s => s.length > 0)
                    delegate: Rectangle {
                        id: genreChip
                        required property string modelData
                        // Explorer's own genre, when Explorer is on this show's provider.
                        readonly property var option: {
                            const provider = infoPage.currentShow.provider
                            return provider && App.providers.currentIndex === App.providers.indexOf(provider.name)
                                   ? Globals.genreOption(App.providers.filterOptions.genre, modelData) : null
                        }
                        readonly property bool active: option !== null && Globals.explorerFilters.genre === option.value
                        width: chipText.implicitWidth + 20
                        height: 32
                        radius: 16
                        color: active ? Theme.accentMuted : chipMa.containsMouse ? Qt.alpha(Theme.accent, 0.18) : Theme.surfaceAlt
                        border.color: active || chipMa.containsMouse ? Theme.accent : Theme.border
                        border.width: 1
                        Behavior on color { ColorAnimation { duration: 120 } }
                        Behavior on border.color { ColorAnimation { duration: 120 } }

                        Text {
                            id: chipText
                            anchors.centerIn: parent
                            text: genreChip.modelData
                            color: genreChip.active || chipMa.containsMouse ? Theme.textAccent : Theme.textMuted
                            font.pixelSize: Globals.sp(Theme.bodySize)
                            Behavior on color { ColorAnimation { duration: 120 } }
                        }

                        MouseArea {
                            id: chipMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                const provider = infoPage.currentShow.provider
                                Globals.pendingGenre = { provider: provider ? provider.name : "", genre: genreChip.modelData }
                                Globals.gotoPage(AppShell.Search)
                            }
                        }
                        AppToolTip {
                            visible: chipMa.containsMouse
                            text: genreChip.active ? qsTr("Stop browsing by this genre")
                                                   : qsTr("Browse %1 by this genre").arg(infoPage.currentShow.provider
                                                                                         ? infoPage.currentShow.provider.name : "")
                        }
                    }
                }
            }

            Card {
                Layout.fillWidth: true
                implicitHeight: descCol.implicitHeight + 24
                radius: 14

                ColumnLayout {
                    id: descCol
                    anchors {
                        fill: parent
                        margins: 12
                    }
                    spacing: 8

                    Text {
                        text: qsTr("SYNOPSIS")
                        color: Theme.textMuted
                        font {
                            pixelSize: Globals.sp(20)
                            bold: true
                            letterSpacing: 1.5
                        }
                    }

                    Item {
                        id: synopsis
                        property bool expanded: false
                        readonly property real lineHeight: descText.lineCount > 0 ? descText.implicitHeight / descText.lineCount : 0
                        readonly property bool clipped: descText.lineCount > 6
                        Layout.fillWidth: true
                        Layout.maximumWidth: Globals.sp(820)
                        implicitHeight: clipped && !expanded ? lineHeight * 5 : descText.implicitHeight
                        clip: true
                        Behavior on implicitHeight { NumberAnimation { duration: Theme.reduceMotion ? 0 : 180; easing.type: Easing.OutCubic } }

                        Connections {
                            target: App.show
                            function onShowChanged() { synopsis.expanded = false }
                        }

                        RichText {
                            id: descText
                            width: parent.width
                            text: infoPage.currentShow.description.length > 0
                                  ? infoPage.currentShow.description : qsTr("No description")
                        }
                        Rectangle {
                            visible: synopsis.clipped && !synopsis.expanded
                            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                            height: synopsis.lineHeight * 1.5
                            gradient: Gradient {
                                GradientStop { position: 0.0; color: Qt.alpha(Theme.surface, 0) }
                                GradientStop { position: 1.0; color: Theme.surface }
                            }
                        }
                    }

                    Text {
                        visible: synopsis.clipped
                        // At the text's right edge, which stops short of a wide card's.
                        Layout.alignment: Qt.AlignRight
                        Layout.rightMargin: descCol.width - synopsis.width
                        text: synopsis.expanded ? qsTr("Less") : qsTr("More")
                        color: moreHover.hovered ? Theme.accentLight : Theme.accent
                        font.pixelSize: Globals.sp(Theme.compactSize)
                        font.weight: Font.DemiBold
                        HoverHandler { id: moreHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: synopsis.expanded = !synopsis.expanded }
                    }
                }
            }

            Card {
                id: notesCard
                // Saved when the note loses focus, a star is picked, or another show opens.
                property int rating: 0
                property string loadedFor: ""
                Layout.fillWidth: true
                implicitHeight: notesCol.implicitHeight + 24
                radius: 14

                function load() {
                    const note = App.library.note(infoPage.currentShow.link)
                    rating = note.rating
                    notesField.text = note.text
                    loadedFor = infoPage.currentShow.link
                }
                function save() {
                    if (loadedFor.length > 0) App.library.setNote(loadedFor, rating, notesField.text)
                }
                Component.onCompleted: load()
                Connections {
                    target: App.show
                    function onShowChanged() {
                        if (notesCard.loadedFor === infoPage.currentShow.link) return
                        notesCard.save()
                        notesCard.load()
                    }
                }

                ColumnLayout {
                    id: notesCol
                    anchors {
                        fill: parent
                        margins: 12
                    }
                    spacing: 8

                    RowLayout {
                        spacing: 2
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("YOUR NOTES")
                            color: Theme.textMuted
                            font {
                                pixelSize: Globals.sp(20)
                                bold: true
                                letterSpacing: 1.5
                            }
                        }
                        Repeater {
                            model: 5
                            delegate: AppIcon {
                                id: star
                                required property int index
                                name: "star"
                                size: 22
                                color: index < notesCard.rating ? Theme.warning : Theme.textMuted
                                Accessible.role: Accessible.Button
                                Accessible.name: qsTr("Rate %1 of 5").arg(index + 1)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                // The same star again clears the rating.
                                TapHandler {
                                    onTapped: {
                                        notesCard.rating = notesCard.rating === star.index + 1 ? 0 : star.index + 1
                                        notesCard.save()
                                    }
                                }
                            }
                        }
                    }

                    TextArea {
                        id: notesField
                        ContextMenu.menu: null
                        ContextMenu.onRequested: (position) => { notesMenu.active = true; (notesMenu.item as TextMenu).popup(position) }
                        Loader { id: notesMenu; active: false; sourceComponent: TextMenu { target: notesField } }
                        Layout.fillWidth: true
                        wrapMode: TextEdit.Wrap
                        placeholderText: qsTr("Your thoughts, who recommended it, what to watch for...")
                        placeholderTextColor: Theme.textMuted
                        color: Theme.textPrimary
                        selectionColor: Theme.accent
                        selectedTextColor: Theme.onAccent
                        font.family: Globals.fontFamily
                        font.pixelSize: Globals.sp(Theme.bodySize)
                        leftPadding: 14
                        rightPadding: 14
                        hoverEnabled: true
                        background: FieldBackground { focused: notesField.activeFocus; hovered: notesField.hovered }
                        onEditingFinished: notesCard.save()
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Text {
                    text: qsTr("LINKS")
                    color: Theme.textMuted
                    font {
                        pixelSize: Globals.sp(20)
                        bold: true
                    }
                }

                Repeater {
                    model: [
                        { icon: "qrc:/App/resources/images/links/anilist.png",
                          url: "https://anilist.co/search/anime?search=" },
                        { icon: "qrc:/App/resources/images/links/mal.png",
                          url: "https://myanimelist.net/search/all?q=" },
                        { icon: "qrc:/App/resources/images/links/imdb.png",
                          url: "https://www.imdb.com/find?q=" },
                        { icon: "qrc:/App/resources/images/links/douban.png",
                          url: "https://movie.douban.com/subject_search?search_text=" }
                    ]
                    delegate: Image {
                        id: linkButton
                        required property var modelData
                        source: linkButton.modelData.icon
                        fillMode: Image.PreserveAspectFit
                        Layout.preferredWidth: 36
                        Layout.preferredHeight: 36

                        scale: linkArea.pressed ? 0.92 : (linkArea.containsMouse ? 1.06 : 1.0)
                        Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }

                        MouseArea {
                            id: linkArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Qt.openUrlExternally(
                                linkButton.modelData.url + encodeURIComponent(infoPage.currentShow.title))
                        }
                    }
                }

                Item { Layout.fillWidth: true }
            }

            TrackerPanel {
                showLink: infoPage.currentShow.link
                showTitle: infoPage.currentShow.title
            }

            Item { Layout.preferredHeight: 12 }
        }
    }

    Popup {
        id: coverPopup
        modal: true
        focus: true
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        anchors.centerIn: parent
        height: Math.min(Globals.appHeight > 0 ? Globals.appHeight * 0.85 : 880, 880)
        width: height * 0.705
        Overlay.modal: Rectangle { color: Theme.scrim }
        background: Rectangle { color: "transparent" }
        contentItem: Rectangle {
            color: "transparent"
            radius: 12
            clip: true
            Image {
                anchors.fill: parent
                source: infoPage.currentShow.coverUrl
                fillMode: Image.PreserveAspectFit
                asynchronous: true
            }
            TapHandler { onTapped: coverPopup.close() }
        }
    }

    Keys.enabled: true
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Space)
            App.continueWatching()
        else if (event.key === Qt.Key_Escape)
            infoPage.forceActiveFocus()
    }

    MigrateDialog { id: infoMigrateDialog }

    // An episode row's right click.
    AppMenu {
        id: episodeMenu
        modal: true
        property int index: -1
        property bool watched: false
        property bool preview: false
        property int downloadState: 0

        function show(index, watched, preview, downloadState) {
            episodeMenu.index = index
            episodeMenu.watched = watched
            episodeMenu.preview = preview
            episodeMenu.downloadState = downloadState
            popup()
        }

        Action { text: qsTr("Play"); onTriggered: App.playFromEpisodeList(episodeMenu.index, false) }
        Action { text: qsTr("Queue"); onTriggered: App.playFromEpisodeList(episodeMenu.index, true) }
        Action {
            text: qsTr("Download")
            enabled: episodeMenu.downloadState === 0
            onTriggered: App.downloadCurrentShow(episodeMenu.index)
        }
        Action {
            // A trailer is not an episode to have watched.
            text: episodeMenu.watched ? qsTr("Mark unwatched") : qsTr("Mark watched")
            enabled: !episodeMenu.preview
            onTriggered: episodeMenu.watched ? App.show.markUnwatched(episodeMenu.index)
                                             : App.show.markWatched(episodeMenu.index)
        }
    }

    // The title's right click.
    AppMenu {
        id: titleMenu
        modal: true
        readonly property string showTitle: infoPage.currentShow.title
        readonly property string site: infoPage.currentShow.provider?.name ?? ""

        function searchWeb() { Qt.openUrlExternally("https://www.google.com/search?q=" + encodeURIComponent(showTitle)) }

        Action { text: qsTr("Copy title"); onTriggered: App.copyToClipboard(titleMenu.showTitle) }
        Action {
            property bool shown: infoPage.pageUrl !== ""
            text: qsTr("Copy link")
            enabled: shown
            onTriggered: App.copyToClipboard(infoPage.pageUrl)
        }
        Action {
            property bool shown: infoPage.pageUrl !== ""
            text: qsTr("Open on %1").arg(titleMenu.site)
            enabled: shown
            onTriggered: Qt.openUrlExternally(infoPage.pageUrl)
        }
        TextMenu.Separator {}
        Action { text: qsTr("Search for this title"); onTriggered: Globals.searchProviders(titleMenu.showTitle) }
        Action { text: qsTr("Search the web"); onTriggered: titleMenu.searchWeb() }
        TextMenu.Separator {}
        Action { text: qsTr("Check for new episodes"); onTriggered: App.reloadShow() }
    }

    Component.onCompleted: infoPage.forceActiveFocus()
}
