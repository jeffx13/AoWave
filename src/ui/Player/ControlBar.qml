pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App

Item {
    id: controlBar

    property bool overlaySurface: true
    implicitHeight: btnSize + 6 + 28 + 8

    readonly property bool hovered: hoverHandler.hovered || sliderHovered || volumeArea.open || trackPopup.visible
    readonly property bool sliderHovered: seekHoverArea.containsMouse || timeSlider.pressed
    readonly property real hoveredTime: Math.max(0, Math.min(duration,
        (seekHoverArea.mouseX - timeSlider.leftPadding) / Math.max(1, timeSlider.availableWidth) * duration))

    required property MpvPlayer player

    readonly property bool isPlaying: player.state === MpvPlayer.Playing
    readonly property bool idle: player.state === MpvPlayer.Stopped && !player.isLoading && !App.playlist.isLoading
    readonly property int time: player.time
    readonly property int duration: player.duration
    readonly property int volume: player.volume

    property int stepRevision: 0
    readonly property bool canPrevious: stepRevision >= 0 && App.playlist.canStep(-1)
    readonly property bool canNext: stepRevision >= 0 && App.playlist.canStep(1)
    readonly property bool canResume: stepRevision >= 0 && App.playlist.canResumeStopped()

    Connections {
        target: App.playlist
        function onCurrentItemChanged() { controlBar.stepRevision++ }
        function onRowsInserted() { controlBar.stepRevision++ }
        function onRowsRemoved() { controlBar.stepRevision++ }
        function onRowsMoved() { controlBar.stepRevision++ }
        function onModelReset() { controlBar.stepRevision++ }
    }

    signal playlistRequested()
    signal panelRequested(string tab)
    signal openFileRequested()

    onTimeChanged: if (!timeSlider.pressed) timeSlider.value = time

    readonly property var markRanges: {
        const p = controlBar.player
        const d = controlBar.duration
        if (!p || d <= 0) return []
        const ranges = []
        const add = (from, to, color) => {
            from = Math.max(0, from)
            to = Math.min(d, to)
            if (to > from) ranges.push({ from: from / d, to: to / d, color: color })
        }
        const chapter = Qt.alpha(Theme.onOverlay, 0.3)
        if (p.hasOP) add(p.aniOPStart, p.aniOPStart + p.aniOPLength, chapter)
        else if (p.skipOP) add(p.skipOPStart, p.skipOPStart + p.skipOPLength, chapter)
        if (p.hasED) add(d - p.aniEDLength, d, chapter)
        else if (p.skipED) add(d - p.skipEDLength, d, chapter)
        if (p.loopA >= 0) add(p.loopA, p.loopB >= 0 ? p.loopB : p.loopA + d / 300, Qt.alpha(Theme.onOverlayAccent, 0.6))
        return ranges
    }

    function chapterTitleAt(seconds) {
        let title = ""
        for (const chapter of player.chapters) {
            if (chapter.time > seconds) break
            title = chapter.title
        }
        return title
    }

    readonly property int btnSize: Math.min(Ui.sp(38), Math.max(28, width / 24))

    component CtrlBtn: Item {
        id: cb
        property string icon: ""
        property string tip: ""
        property int iconSize: Ui.sp(19)
        signal clicked()
        Layout.preferredWidth: controlBar.btnSize
        Layout.preferredHeight: controlBar.btnSize
        opacity: enabled ? 1 : 0.4
        Accessible.role: Accessible.Button
        Accessible.name: cb.tip
        Rectangle {
            anchors { fill: parent; margins: 2 }
            radius: 9
            color: cbArea.pressed ? Theme.overlayFillActive : cbHover.hovered ? Theme.overlayFillHover : "transparent"
            Behavior on color { ColorAnimation { duration: Theme.dur(110) } }
        }
        AppIcon {
            anchors.centerIn: parent
            name: cb.icon
            size: cb.iconSize
            color: cbHover.hovered ? Theme.onOverlay : Theme.onOverlayMuted
            Behavior on color { ColorAnimation { duration: Theme.dur(120) } }
            scale: cbArea.pressed ? 0.9 : (cbHover.hovered ? 1.08 : 1.0)
            Behavior on scale { NumberAnimation { duration: Theme.dur(110); easing.type: Easing.OutCubic } }
        }
        property bool tipDismissed: false
        HoverHandler { id: cbHover; onHoveredChanged: if (!hovered) cb.tipDismissed = false }
        MouseArea {
            id: cbArea; anchors.fill: parent; cursorShape: Qt.PointingHandCursor
            onPressed: cb.tipDismissed = true
            onClicked: cb.clicked()
        }
        AppToolTip { text: cb.tip; visible: cb.tip !== "" && cbHover.hovered && !cb.tipDismissed }
    }

    component Divider: Rectangle {
        Layout.preferredWidth: 1; Layout.preferredHeight: 18
        Layout.leftMargin: 5; Layout.rightMargin: 5
        color: Theme.overlayFillHover
    }

    Rectangle {
        anchors.fill: parent

        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 0.15; color: Theme.overlayScrimSoft }
            GradientStop { position: 0.5; color: Theme.overlayScrimMid }
            GradientStop { position: 1.0; color: Theme.overlayScrim }
        }

        Rectangle {
            anchors { top: parent.top; left: parent.left; right: parent.right }
            height: 1
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 0.3; color: Theme.overlayFill }
                GradientStop { position: 0.7; color: Theme.overlayFill }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }

        HoverHandler {
            id: hoverHandler
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        }

        Slider {
            id: timeSlider
            visible: !controlBar.idle
            from: 0
            to: controlBar.duration
            focusPolicy: Qt.NoFocus
            hoverEnabled: true
            live: true
            enabled: controlBar.duration > 0 && !App.playlist.isLoading
            z: 1
            anchors {
                left: parent.left; right: parent.right
                bottom: buttonRow.top
                leftMargin: 10; rightMargin: 10
            }
            height: 28
            onPressedChanged: if (!pressed && enabled) controlBar.player.seek(value)

            MouseArea {
                id: seekHoverArea
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
            }

            background: WaterProgress {
                x: timeSlider.leftPadding
                width: timeSlider.availableWidth
                anchors.verticalCenter: parent.verticalCenter
                height: controlBar.sliderHovered ? 10 : 6
                progress: timeSlider.visualPosition
                buffered: controlBar.duration > 0 ? controlBar.player.bufferedTime / controlBar.duration : 0
                animating: controlBar.isPlaying && !controlBar.player.buffering
                waterOpacity: 0.92
                trackColor: Theme.overlayFillHover
                bubbles: 1.0
                bubbleScale: controlBar.sliderHovered ? 2.6 : 3.2
                Behavior on height { NumberAnimation { duration: Theme.dur(150); easing.type: Easing.OutCubic } }

                Row {
                    id: heatRow
                    anchors.fill: parent
                    visible: heatRow.buckets.length > 0 && App.settings.danmakuEnabled
                    readonly property var buckets: controlBar.player.danmakuHeat
                    Repeater {
                        model: heatRow.buckets
                        delegate: Item {
                            id: heatCell
                            required property real modelData
                            required property int index
                            width: heatRow.width / Math.max(1, heatRow.buckets.length)
                            height: heatRow.height
                            Rectangle {
                                anchors.centerIn: parent
                                width: parent.width + 0.5
                                height: parent.height * (0.28 + 0.72 * heatCell.modelData)
                                radius: height / 2
                                readonly property bool played:
                                    (heatCell.index + 0.5) / Math.max(1, heatRow.buckets.length)
                                    <= timeSlider.visualPosition
                                color: Qt.alpha(Theme.onOverlay, heatCell.modelData * (played ? 0.30 : 0.13))
                            }
                        }
                    }
                }

                Repeater {
                    model: controlBar.markRanges
                    delegate: Rectangle {
                        required property var modelData
                        x: parent.width * modelData.from
                        width: Math.max(3, parent.width * (modelData.to - modelData.from))
                        height: parent.height
                        radius: height / 2
                        color: modelData.color
                    }
                }

                Repeater {
                    model: controlBar.duration > 0 ? controlBar.player.chapters : []
                    delegate: Rectangle {
                        required property var modelData
                        visible: modelData.time > 0.5 && modelData.time < controlBar.duration - 0.5
                        x: parent.width * modelData.time / controlBar.duration - width / 2
                        width: 2
                        height: parent.height
                        color: Theme.overlayScrim
                    }
                }
            }

            handle: Rectangle {
                width: controlBar.sliderHovered ? 16 : 12
                height: width; radius: width / 2
                x: timeSlider.leftPadding + timeSlider.visualPosition * (timeSlider.availableWidth - width)
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.onOverlay
                Behavior on width { NumberAnimation { duration: Theme.dur(130); easing.type: Easing.OutCubic } }
                scale: timeSlider.pressed ? 1.25 : 1.0
                Behavior on scale { NumberAnimation { duration: Theme.dur(120); easing.type: Easing.OutCubic } }

                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width * 0.42; height: width; radius: width / 2
                    color: Theme.accent
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width + 10; height: width; radius: width / 2
                    color: "transparent"; border.color: Qt.alpha(Theme.accent, 0.5); border.width: 2
                    visible: controlBar.sliderHovered
                }
            }

            Rectangle {
                id: seekPreview
                visible: controlBar.sliderHovered && controlBar.duration > 0
                z: 5
                readonly property bool showImage: App.settings.seekPreviewsEnabled && thumb.available
                readonly property real frameWidth: Math.min(Ui.sp(232), timeSlider.width - 16)
                width: showImage ? frameWidth + 8
                                 : Math.max(seekTipText.implicitWidth, chapterTip.visible ? chapterTip.width : 0) + 22
                height: (showImage ? frameWidth / thumb.frameAspect + 8 : 0) + Ui.sp(30)
                        + (chapterTip.visible ? chapterTip.height + 2 : 0)
                radius: 10
                color: Theme.overlayScrim
                border.color: Theme.overlayFillActive
                border.width: 1
                y: -height - 10
                x: Math.max(0, Math.min(timeSlider.width - width, seekHoverArea.mouseX - width / 2))
                Behavior on width  { enabled: seekPreview.visible; NumberAnimation { duration: Theme.dur(120); easing.type: Easing.OutCubic } }
                Behavior on height { enabled: seekPreview.visible; NumberAnimation { duration: Theme.dur(120); easing.type: Easing.OutCubic } }

                Rectangle {
                    anchors { top: parent.top; left: parent.left; right: parent.right; margins: 4 }
                    height: seekPreview.frameWidth / thumb.frameAspect
                    visible: seekPreview.showImage
                    radius: 6
                    color: Theme.overlayFillSoft
                    clip: true

                    SeekPreview {
                        id: thumb
                        anchors.fill: parent
                        player: controlBar.player
                        time: controlBar.hoveredTime
                        active: controlBar.visible && controlBar.sliderHovered && App.settings.seekPreviewsEnabled
                        opacity: hasFrame ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: Theme.dur(140) } }
                    }
                    Rectangle {
                        id: shimmer
                        visible: !thumb.hasFrame && thumb.loading
                        width: shimmer.parent.width * 0.45
                        height: shimmer.parent.height
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: "transparent" }
                            GradientStop { position: 0.5; color: Theme.overlayFillHover }
                            GradientStop { position: 1.0; color: "transparent" }
                        }
                        NumberAnimation on x {
                            from: -shimmer.parent.width * 0.45; to: shimmer.parent.width
                            duration: 900; loops: Animation.Infinite
                            running: shimmer.visible && !Theme.reduceMotion
                        }
                    }
                }
                Text {
                    id: chapterTip
                    readonly property string title: controlBar.chapterTitleAt(controlBar.hoveredTime)
                    visible: title !== ""
                    anchors { bottom: seekTipText.top; horizontalCenter: parent.horizontalCenter; bottomMargin: 2 }
                    width: Math.min(implicitWidth, seekPreview.showImage ? seekPreview.frameWidth : Ui.sp(260))
                    text: title
                    color: Theme.onOverlayDim
                    font.pixelSize: Ui.sp(Theme.compactSize)
                    elide: Text.ElideRight
                }
                TimeCode {
                    id: seekTipText
                    anchors { bottom: parent.bottom; horizontalCenter: parent.horizontalCenter; bottomMargin: 6 }
                    text: Ui.timeText(controlBar.hoveredTime)
                    color: Theme.onOverlay
                    pixelSize: Ui.sp(Theme.compactSize)
                    weight: Font.Medium
                }
            }

        }

        RowLayout {
            id: buttonRow
            anchors {
                left: parent.left; right: parent.right; bottom: parent.bottom
                leftMargin: 10; rightMargin: 10; bottomMargin: 6
            }
            height: controlBar.btnSize
            spacing: 2

            CtrlBtn {
                icon: "skip-back"
                tip: Ui.tipWithKey(qsTr("Previous episode"), "previousEpisode")
                enabled: controlBar.canPrevious
                onClicked: App.playlist.stepItem(-1)
            }
            CtrlBtn {
                icon: controlBar.isPlaying ? "pause" : "play"
                tip: Ui.tipWithKey(controlBar.isPlaying ? qsTr("Pause") : qsTr("Play"), "playPause")
                visible: !controlBar.idle || controlBar.canResume
                onClicked: {
                    if (controlBar.idle && App.playlist.resumeStopped()) return
                    controlBar.player.togglePlayPause()
                }
            }
            CtrlBtn {
                icon: "skip-forward"
                tip: Ui.tipWithKey(qsTr("Next episode"), "nextEpisode")
                enabled: controlBar.canNext
                onClicked: App.playlist.stepItem(1)
            }
            CtrlBtn {
                icon: "square"
                tip: qsTr("Stop")
                visible: !controlBar.idle
                iconSize: Ui.sp(17)
                onClicked: App.playlist.stop()
            }

            Divider {}

            // hovering the speaker slides a slider out beside it; no popup
            Item {
                id: volumeArea
                objectName: "volumeArea"
                readonly property string muteTip: Ui.tipWithKey(controlBar.player.muted ? qsTr("Unmute") : qsTr("Mute"), "mute")
                readonly property bool open: volHover.hovered || volSlider.pressed || volLinger.running
                property real reveal: open ? 1 : 0
                Behavior on reveal { NumberAnimation { duration: Theme.dur(160); easing.type: Easing.OutCubic } }
                Layout.preferredWidth: controlBar.btnSize + (volSlider.width + volLevel.width + 6) * reveal
                Layout.preferredHeight: controlBar.btnSize
                clip: true

                HoverHandler {
                    id: volHover
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onHoveredChanged: if (!hovered) volLinger.restart()
                }
                Timer { id: volLinger; interval: 400 }

                WheelHandler {
                    property real rest: 0
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: (event) => {
                        if (event.angleDelta.y === 0) return
                        rest += event.angleDelta.y
                        const steps = Math.trunc(rest / 120)
                        if (steps === 0) return
                        rest -= steps * 120
                        controlBar.player.stepVolume(steps * App.settings.volumeStep)
                    }
                }

                Rectangle {
                    anchors { fill: parent; margins: 2 }
                    radius: 9
                    color: muteTap.pressed ? Theme.overlayFillActive
                         : volumeArea.open ? Theme.overlayFillHover : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.dur(110) } }
                }

                Item {
                    width: controlBar.btnSize
                    height: parent.height
                    Accessible.role: Accessible.Button
                    Accessible.name: volumeArea.muteTip
                    AppIcon {
                        anchors.centerIn: parent
                        size: Ui.sp(19)
                        name: controlBar.player.muted || controlBar.volume === 0 ? "volume-x"
                            : controlBar.volume < 50  ? "volume-1" : "volume-2"
                        color: volumeArea.open ? Theme.onOverlay : Theme.onOverlayMuted
                        Behavior on color { ColorAnimation { duration: Theme.dur(120) } }
                    }
                    HoverHandler { id: muteHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { id: muteTap; onTapped: controlBar.player.muted = !controlBar.player.muted }
                    AppToolTip { text: volumeArea.muteTip; visible: muteHover.hovered }
                }

                Slider {
                    id: volSlider
                    x: controlBar.btnSize
                    visible: volumeArea.reveal > 0
                    width: Ui.sp(84)
                    height: parent.height
                    padding: 0
                    leftPadding: 4
                    rightPadding: 8
                    from: 0; to: 200; stepSize: 1
                    value: controlBar.volume
                    focusPolicy: Qt.NoFocus
                    live: true
                    opacity: volumeArea.reveal
                    Accessible.name: qsTr("Volume")
                    onMoved: controlBar.player.volume = value
                    onPressedChanged: if (!pressed) volLinger.restart()

                    // 100% is the middle tick; past it the fill turns amber as a boost warning
                    background: Item {
                        x: volSlider.leftPadding
                        y: volSlider.topPadding + (volSlider.availableHeight - height) / 2
                        width: volSlider.availableWidth
                        height: 4
                        Rectangle { anchors.fill: parent; radius: 2; color: Theme.overlayFillActive }
                        Rectangle {
                            width: parent.width * Math.min(volSlider.visualPosition, 0.5)
                            height: parent.height
                            radius: 2
                            color: Theme.accent
                        }
                        Rectangle {
                            visible: volSlider.visualPosition > 0.5
                            x: parent.width * 0.5
                            width: parent.width * (volSlider.visualPosition - 0.5)
                            height: parent.height
                            radius: 2
                            color: Theme.onOverlayWarning
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            width: 1
                            height: parent.height + 6
                            color: Theme.onOverlayDim
                        }
                    }

                    handle: Rectangle {
                        x: volSlider.leftPadding + volSlider.visualPosition * (volSlider.availableWidth - width)
                        y: volSlider.topPadding + (volSlider.availableHeight - height) / 2
                        width: 12
                        height: 12
                        radius: 6
                        color: Theme.onOverlay
                        scale: volSlider.pressed ? 1.25 : 1.0
                        Behavior on scale { NumberAnimation { duration: Theme.dur(120); easing.type: Easing.OutCubic } }
                        Rectangle {
                            anchors.centerIn: parent
                            width: parent.width * 0.42
                            height: width
                            radius: width / 2
                            color: controlBar.volume > 100 ? Theme.onOverlayWarning : Theme.accent
                        }
                    }
                }

                Item {
                    id: volLevel
                    anchors { left: volSlider.right; verticalCenter: parent.verticalCenter }
                    width: volText.digitWidth * 3
                    height: parent.height
                    opacity: volumeArea.reveal
                    TimeCode {
                        id: volText
                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                        text: String(controlBar.volume)
                        color: controlBar.volume > 100 ? Theme.onOverlayWarning : Theme.onOverlayMuted
                        pixelSize: Ui.sp(Theme.compactSize)
                    }
                }
            }

            Rectangle {
                id: timeBox
                property bool remaining: String(App.settings.value("player/timeRemaining", false)) === "true"
                visible: !controlBar.idle
                Layout.preferredHeight: 30
                Layout.preferredWidth: timeText.implicitWidth + 18
                radius: 15
                color: timeHover.hovered ? Theme.overlayFillHover : Theme.overlayFill
                border.color: Theme.overlayLine; border.width: 1
                TimeCode {
                    id: timeText
                    anchors.centerIn: parent
                    text: (timeBox.remaining ? "-" + Ui.timeText(Math.max(0, controlBar.duration - controlBar.time))
                                             : Ui.timeText(controlBar.time))
                          + " / " + Ui.timeText(controlBar.duration)
                    color: Theme.onOverlay
                }
                HoverHandler { id: timeHover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    onTapped: {
                        timeBox.remaining = !timeBox.remaining
                        App.settings.setValue("player/timeRemaining", timeBox.remaining)
                    }
                }
                AppToolTip {
                    visible: timeHover.hovered
                    text: timeBox.remaining ? qsTr("Show time played") : qsTr("Show time left")
                }
            }

            Rectangle {
                visible: !controlBar.idle && Math.abs(controlBar.player.speed - 1) > 0.001
                Layout.leftMargin: 4
                Layout.preferredHeight: 30
                Layout.preferredWidth: speedText.implicitWidth + 18
                radius: 15
                color: speedHover.hovered ? Theme.overlayFillHover : Theme.overlayFill
                border.color: Theme.overlayLine; border.width: 1
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Reset speed")
                TimeCode {
                    id: speedText
                    anchors.centerIn: parent
                    text: Math.round(controlBar.player.speed * 100) / 100 + "x"
                    color: Theme.onOverlay
                }
                HoverHandler { id: speedHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: controlBar.player.setSpeed(1.0) }
                AppToolTip { visible: speedHover.hovered; text: qsTr("Reset speed") }
            }

            Item { Layout.fillWidth: true }

            Row {
                spacing: 8
                visible: App.playlist.isLoading || controlBar.player.isLoading || controlBar.player.buffering
                Layout.alignment: Qt.AlignHCenter
                Text { text: App.playlist.isLoading ? qsTr("Resolving…") : controlBar.player.buffering ? qsTr("Buffering…") : qsTr("Loading…"); color: Theme.onOverlayAccent; font.pixelSize: Ui.sp(Theme.compactSize); anchors.verticalCenter: parent.verticalCenter }
            }

            Item { Layout.fillWidth: true }

            CtrlBtn {
                id: tracksButton
                icon: "list-checks"
                tip: qsTr("Audio and subtitles")
                visible: controlBar.player.audioList.count > 1 || controlBar.player.subtitleList.count > 0
                onClicked: trackPopup.toggle()

                AppPopup {
                    id: trackPopup
                    toggledByParent: true
                    x: tracksButton.width - width
                    y: -height - 10
                    width: Ui.sp(300)
                    padding: 8

                    component TrackRow: Rectangle {
                        id: trackRow
                        property string label
                        property bool current
                        signal picked()
                        width: parent ? parent.width : 0
                        height: Ui.sp(34)
                        radius: 8
                        color: rowHover.hovered ? Theme.overlayFillHover : "transparent"
                        HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: { trackRow.picked(); trackPopup.close() } }
                        AppIcon {
                            id: tick
                            anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }
                            name: "check"
                            size: Ui.sp(14)
                            color: Theme.onOverlayAccent
                            opacity: trackRow.current ? 1 : 0
                        }
                        Text {
                            anchors { left: tick.right; leftMargin: 8; right: parent.right; rightMargin: 8
                                      verticalCenter: parent.verticalCenter }
                            text: trackRow.label
                            elide: Text.ElideRight
                            color: trackRow.current ? Theme.onOverlay : Theme.onOverlayMuted
                            font.pixelSize: Ui.sp(Theme.compactSize)
                            font.weight: trackRow.current ? Font.DemiBold : Font.Normal
                        }
                    }
                    component SectionLabel: Text {
                        leftPadding: 8
                        topPadding: 6
                        bottomPadding: 2
                        color: Theme.onOverlayDim
                        font.pixelSize: Ui.sp(Theme.captionSize)
                        font.bold: true
                    }

                    contentItem: Flickable {
                        implicitHeight: Math.min(trackColumn.implicitHeight, Ui.sp(420))
                        contentHeight: trackColumn.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: AppScrollBar {}

                        Column {
                            id: trackColumn
                            width: parent.width

                            SectionLabel {
                                visible: controlBar.player.audioList.count > 1
                                text: qsTr("Audio")
                            }
                            Repeater {
                                model: controlBar.player.audioList.count > 1 ? controlBar.player.audioList : null
                                delegate: TrackRow {
                                    required property string name
                                    required property int index
                                    label: name
                                    current: index === controlBar.player.audioList.currentIndex
                                    onPicked: controlBar.player.setAudioIndex(index)
                                }
                            }
                            SectionLabel {
                                visible: controlBar.player.subtitleList.count > 0
                                text: qsTr("Subtitles")
                            }
                            TrackRow {
                                visible: controlBar.player.subtitleList.count > 0
                                label: qsTr("Off")
                                current: controlBar.player.primarySubId === 0 && controlBar.player.secondarySubId === 0
                                onPicked: controlBar.player.clearSubs()
                            }
                            Repeater {
                                model: controlBar.player.subtitleList
                                delegate: TrackRow {
                                    required property string name
                                    required property int index
                                    label: name
                                    current: controlBar.player.primarySubId !== 0
                                             && index === controlBar.player.subtitleList.currentIndex
                                    onPicked: controlBar.player.setSubIndex(index)
                                }
                            }
                        }
                    }
                }
            }
            CtrlBtn {
                visible: !controlBar.idle
                icon: controlBar.player.subVisible ? "captions" : "captions-off"
                tip: Ui.tipWithKey(controlBar.player.subVisible ? qsTr("Hide subtitles") : qsTr("Show subtitles"), "subtitles")
                onClicked: controlBar.player.subVisible = !controlBar.player.subVisible
            }
            CtrlBtn {
                icon: "server"
                tip: qsTr("Servers")
                visible: !controlBar.idle && App.playlist.serverList.count > 1
                onClicked: controlBar.panelRequested("servers")
            }
            CtrlBtn {
                icon: "list-video"
                tip: Ui.tipWithKey(qsTr("Playlist"), "playlist")
                onClicked: controlBar.playlistRequested()
            }

            Divider {}

            CtrlBtn {
                icon: "folder"
                tip: Ui.tipWithKey(qsTr("Open file"), "openFile")
                onClicked: controlBar.openFileRequested()
            }
            CtrlBtn {
                icon: "settings"
                tip: Ui.tipWithKey(qsTr("Settings"), "panel")
                onClicked: controlBar.panelRequested("general")
            }

            Divider {}

            CtrlBtn {
                visible: !controlBar.idle || Ui.pipMode
                icon: Ui.pipMode ? "picture-in-picture-2" : "picture-in-picture"
                tip: Ui.tipWithKey(Ui.pipMode ? qsTr("Exit picture-in-picture") : qsTr("Picture-in-picture"), "pip")
                onClicked: Ui.togglePip()
            }
            CtrlBtn {
                icon: Ui.fullscreen ? "minimize" : "maximize"
                tip: Ui.tipWithKey(Ui.fullscreen ? qsTr("Exit fullscreen") : qsTr("Fullscreen"), "fullscreen")
                onClicked: Ui.toggleFullscreen()
            }
        }
    }
}
