pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../Components"
import App
import ".."

Item {
    id: controlBar

    // An open popup keeps the controls up.
    readonly property bool hovered: hoverHandler.hovered || sliderHovered || volPopup.visible || trackPopup.visible
    // seekHoverArea sits on top of the slider; the slider's own `hovered` is intermittent.
    readonly property bool sliderHovered: seekHoverArea.containsMouse || timeSlider.pressed
    readonly property real hoveredTime: Math.max(0, Math.min(duration,
        (seekHoverArea.mouseX - timeSlider.leftPadding) / Math.max(1, timeSlider.availableWidth) * duration))

    required property MpvPlayer player

    readonly property bool isPlaying: player.state === MpvPlayer.Playing
    // Nothing loaded: no position to show or seek to.
    readonly property bool idle: player.state === MpvPlayer.Stopped && !player.isLoading && !App.playlist.isLoading
    readonly property int time: player.time
    readonly property int duration: player.duration
    readonly property int volume: player.volume

    signal playlistRequested()
    signal panelRequested(string tab)
    signal openFileRequested()

    onTimeChanged: if (!timeSlider.pressed) timeSlider.value = time

    function toHHMMSS(seconds) {
        let h = Math.floor(seconds / 3600)
        let m = Math.floor((seconds % 3600) / 60)
        let s = Math.floor(seconds % 60)
        let pad = (n) => n < 10 ? "0" + n : "" + n
        return (h > 0 ? pad(h) + ":" : "") + pad(m) + ":" + pad(s)
    }

    // Fractions of the duration drawn on the seek bar: opening, ending, A-B loop.
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
        const chapter = Qt.rgba(1, 1, 1, 0.3)
        if (p.hasOP) add(p.aniOPStart, p.aniOPStart + p.aniOPLength, chapter)
        else if (p.skipOP) add(p.skipOPStart, p.skipOPStart + p.skipOPLength, chapter)
        if (p.hasED) add(d - p.aniEDLength, d, chapter)
        else if (p.skipED) add(d - p.skipEDLength, d, chapter)
        // A lone A point shows as a sliver.
        if (p.loopA >= 0) add(p.loopA, p.loopB >= 0 ? p.loopB : p.loopA + d / 300, Qt.alpha(Theme.onOverlayAccent, 0.6))
        return ranges
    }

    readonly property int btnSize: Math.min(Globals.sp(38), Math.max(28, width / 24))

    component CtrlBtn: Item {
        id: cb
        property string icon: ""
        property string tip: ""
        property int iconSize: 22
        signal clicked()
        Layout.preferredWidth: controlBar.btnSize
        Layout.preferredHeight: controlBar.btnSize
        Rectangle {
            anchors { fill: parent; margins: 2 }
            radius: 9
            color: cbArea.pressed ? Theme.overlayFillActive : cbHover.hovered ? Theme.overlayFillHover : "transparent"
            Behavior on color { ColorAnimation { duration: 110 } }
        }
        AppIcon {
            anchors.centerIn: parent
            name: cb.icon
            size: cb.iconSize
            color: cbHover.hovered ? Theme.onOverlay : Theme.onOverlayMuted
            Behavior on color { ColorAnimation { duration: 120 } }
            scale: cbArea.pressed ? 0.86 : (cbHover.hovered ? 1.12 : 1.0)
            Behavior on scale { NumberAnimation { duration: 110; easing.type: Easing.OutBack } }
        }
        // As IconButton's: gone from the press until the pointer leaves.
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

    Timer {
        id: volCloseTimer
        interval: 120
        onTriggered: if (!volBtnHover.hovered && !volPopupHover.hovered) volPopup.close()
    }

    Popup {
        id: volPopup
        parent: Overlay.overlay
        width: 68; height: 214
        padding: 0; margins: 0
        modal: false
        closePolicy: Popup.NoAutoClose

        property bool _up: false
        onAboutToShow: {
            _up = false
            const ov = Overlay.overlay
            if (ov && volumeArea.visible) {
                const p = volumeArea.mapToItem(ov, 0, 0)
                x = p.x + (volumeArea.width - width) / 2
                y = p.y - height - 10
            }
            Qt.callLater(() => { _up = true })
        }
        onAboutToHide: _up = false

        background: null

        HoverHandler {
            id: volPopupHover
            onHoveredChanged: {
                if (hovered) volCloseTimer.stop()
                else if (!volBtnHover.hovered) volCloseTimer.restart()
            }
        }

        enter: Transition { NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: 200; easing.type: Easing.OutCubic } }
        exit:  Transition { NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; duration: 170; easing.type: Easing.InCubic } }

        Item {
            id: popupBody
            anchors.fill: parent
            transformOrigin: Item.Bottom
            scale: volPopup._up ? 1.0 : 0.72
            Behavior on scale { NumberAnimation { duration: 290; easing.type: Easing.OutBack; easing.overshoot: 1.45 } }

            readonly property real over: Math.max(0, controlBar.volume - 100) / 100.0
            readonly property color cTop: Qt.lighter(over > 0 ? Theme.warning : Theme.accent, 1.3)
            readonly property color cBottom: over > 0 ? Theme.warning : Theme.accent

            Rectangle {
                anchors.centerIn: parent
                width: parent.width + 22; height: parent.height + 22; radius: width / 2
                color: "transparent"
                border.color: Qt.rgba(popupBody.cBottom.r, popupBody.cBottom.g, popupBody.cBottom.b, 0.18)
                border.width: 11
            }

            Rectangle { anchors.fill: parent; radius: 18; color: Theme.overlayScrim }
            Rectangle { anchors { fill: parent; margins: 1 }
                radius: 17; color: "transparent"; border.color: Theme.overlayFillActive; border.width: 1 }
            Rectangle { anchors { fill: parent; margins: 2 }
                radius: 16; color: "transparent"; border.color: Theme.overlayLine; border.width: 1 }

            Rectangle {
                anchors { top: parent.top; horizontalCenter: parent.horizontalCenter; topMargin: 2 }
                width: parent.width * 0.50; height: 2; radius: 1; color: Theme.overlayFillActive
            }

            Text {
                id: pctLabel
                anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: 10 }
                text: controlBar.volume + "%"
                color: Theme.onOverlay
                font { pixelSize: Globals.sp(16); family: "monospace"; bold: true }
            }

            Slider {
                id: volSlider
                orientation: Qt.Vertical
                from: 0; to: 200; stepSize: 1
                value: controlBar.volume
                focusPolicy: Qt.NoFocus
                live: true
                anchors {
                    horizontalCenter: parent.horizontalCenter
                    top: pctLabel.bottom; bottom: parent.bottom
                    topMargin: 8; bottomMargin: 14
                }
                width: 40
                onMoved: controlBar.player.volume = value

                background: Item {
                    x: volSlider.leftPadding + (volSlider.availableWidth - width) / 2
                    y: volSlider.topPadding
                    implicitWidth: 6; implicitHeight: 130
                    width: 6; height: volSlider.availableHeight

                    Rectangle { anchors.fill: parent; radius: 3; color: Theme.overlayFillActive }

                    Rectangle {
                        id: volFill
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: (1.0 - volSlider.visualPosition) * parent.height
                        radius: 3
                        gradient: Gradient {
                            orientation: Gradient.Vertical
                            GradientStop { position: 0.0; color: popupBody.cTop;    Behavior on color { ColorAnimation { duration: 350 } } }
                            GradientStop { position: 1.0; color: popupBody.cBottom; Behavior on color { ColorAnimation { duration: 350 } } }
                        }

                        Rectangle {
                            visible: volFill.height > 4
                            anchors { top: parent.top; horizontalCenter: parent.horizontalCenter }
                            width: parent.width + 10; height: 10; radius: 5
                            color: Qt.rgba(popupBody.cTop.r, popupBody.cTop.g, popupBody.cTop.b, 0.65)
                            SequentialAnimation on opacity {
                                running: volPopup.visible
                                loops: Animation.Infinite
                                NumberAnimation { to: 0.2; duration: 950; easing.type: Easing.InOutSine }
                                NumberAnimation { to: 1.0; duration: 950; easing.type: Easing.InOutSine }
                            }
                        }
                    }

                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: parent.height * 0.5
                        width: parent.width + 10; height: 1
                        color: Theme.overlayFillActive
                    }
                }

                handle: Rectangle {
                    x: volSlider.leftPadding + (volSlider.availableWidth - width) / 2
                    y: volSlider.topPadding + volSlider.visualPosition * (volSlider.availableHeight - height)
                    width: 18; height: 18; radius: 9
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: Theme.onOverlay }
                        GradientStop { position: 1.0; color: Theme.onOverlayMuted }
                    }
                    border.color: popupBody.cBottom; border.width: 2
                    Behavior on border.color { ColorAnimation { duration: 300 } }

                    scale: volSlider.pressed ? 1.35 : (volSlider.hovered ? 1.15 : 1.0)
                    Behavior on scale { NumberAnimation { duration: 130; easing.type: Easing.OutBack } }

                    Rectangle {
                        anchors.centerIn: parent
                        width: parent.width + 10; height: parent.height + 10; radius: height / 2
                        color: "transparent"
                        border.color: Qt.rgba(popupBody.cBottom.r, popupBody.cBottom.g, popupBody.cBottom.b, 0.45)
                        border.width: 2
                        visible: volSlider.pressed || volSlider.hovered
                        Behavior on border.color { ColorAnimation { duration: 300 } }
                    }
                }
            }
        }
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
                // At 6px, bubbles measured in item heights come out a pixel wide.
                bubbleScale: controlBar.sliderHovered ? 2.6 : 3.2
                Behavior on height { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }

                // Danmaku heat: comment density as a brightness ripple, additive over the water.
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
                                width: parent.width + 0.5   // overlap, or the ripple reads as a comb
                                height: parent.height * (0.28 + 0.72 * heatCell.modelData)
                                radius: height / 2
                                // Only over filled water: ahead of the playhead it would
                                // fight the buffered pane.
                                readonly property bool played:
                                    (heatCell.index + 0.5) / Math.max(1, heatRow.buckets.length)
                                    <= timeSlider.visualPosition
                                color: Qt.rgba(1, 1, 1, heatCell.modelData
                                                        * (played ? 0.30 : 0.13))
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
            }

            handle: Rectangle {
                width: controlBar.sliderHovered ? 16 : 12
                height: width; radius: width / 2
                x: timeSlider.leftPadding + timeSlider.visualPosition * (timeSlider.availableWidth - width)
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.onOverlay
                Behavior on width { NumberAnimation { duration: 130; easing.type: Easing.OutCubic } }
                scale: timeSlider.pressed ? 1.25 : 1.0
                Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutBack } }

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
                // Collapses to the timestamp when previews are off.
                readonly property bool showImage: App.settings.seekPreviewsEnabled && thumb.available
                readonly property real frameWidth: Math.min(Globals.sp(232), timeSlider.width - 16)
                width: showImage ? frameWidth + 8 : seekTipText.implicitWidth + 22
                height: (showImage ? frameWidth / thumb.frameAspect + 8 : 0) + Globals.sp(30)
                radius: 10
                color: Theme.overlayScrim
                border.color: Theme.overlayFillActive
                border.width: 1
                y: -height - 10
                x: Math.max(0, Math.min(timeSlider.width - width, seekHoverArea.mouseX - width / 2))
                Behavior on width  { enabled: seekPreview.visible; NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                Behavior on height { enabled: seekPreview.visible; NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

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
                        Behavior on opacity { NumberAnimation { duration: 140 } }
                    }
                    // A sweep of light stands in while libmpv fetches the frame.
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
                TimeCode {
                    id: seekTipText
                    anchors { bottom: parent.bottom; horizontalCenter: parent.horizontalCenter; bottomMargin: 6 }
                    text: controlBar.toHHMMSS(controlBar.hoveredTime)
                    color: Theme.onOverlay
                    pixelSize: Globals.sp(Theme.compactSize)
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
                tip: qsTr("Previous episode")
                onClicked: App.playlist.stepItem(-1)
            }
            CtrlBtn {
                icon: controlBar.isPlaying ? "pause" : "play"
                tip: controlBar.isPlaying ? qsTr("Pause") : qsTr("Play")
                onClicked: controlBar.player.togglePlayPause()
            }
            CtrlBtn {
                icon: "skip-forward"
                tip: qsTr("Next episode")
                onClicked: App.playlist.stepItem(1)
            }
            CtrlBtn {
                icon: "square"
                tip: qsTr("Stop")
                iconSize: 19
                onClicked: App.playlist.stop()
            }

            Divider {}

            Item {
                id: volumeArea
                Layout.preferredWidth: controlBar.btnSize
                Layout.preferredHeight: controlBar.btnSize

                HoverHandler {
                    id: volBtnHover
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onHoveredChanged: {
                        if (hovered) {
                            volCloseTimer.stop()
                            if (!volPopup.visible) volPopup.open()
                        } else if (!volPopupHover.hovered) {
                            volCloseTimer.restart()
                        }
                    }
                }

                WheelHandler {
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    // MpvPlayer clamps to 0..200.
                    onWheel: (event) => controlBar.player.volume += (event.angleDelta.y > 0 ? 5 : -5)
                }

                AppIcon {
                    anchors.centerIn: parent
                    size: 22
                    name: controlBar.volume === 0 ? "volume-x"
                        : controlBar.volume < 50  ? "volume-1" : "volume-2"
                    color: volBtnHover.hovered ? Theme.onOverlay : Theme.onOverlayMuted
                    Behavior on color { ColorAnimation { duration: 120 } }
                }
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: controlBar.player.muted = !controlBar.player.muted }
            }

            Rectangle {
                id: timeBox
                // Time left instead of time played, flipped by a click and remembered.
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
                    text: (timeBox.remaining ? "-" + controlBar.toHHMMSS(Math.max(0, controlBar.duration - controlBar.time))
                                             : controlBar.toHHMMSS(controlBar.time))
                          + " / " + controlBar.toHHMMSS(controlBar.duration)
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

            Item { Layout.fillWidth: true }

            Row {
                spacing: 8
                visible: App.playlist.isLoading || controlBar.player.isLoading || controlBar.player.buffering
                Layout.alignment: Qt.AlignHCenter
                AppSpinner { width: 24; height: 24; running: parent.visible; anchors.verticalCenter: parent.verticalCenter }
                Text { text: App.playlist.isLoading ? qsTr("Resolving…") : controlBar.player.buffering ? qsTr("Buffering…") : qsTr("Loading…"); color: controlBar.player.buffering ? Theme.warning : Theme.onOverlayDim; font.pixelSize: Globals.sp(Theme.compactSize); anchors.verticalCenter: parent.verticalCenter }
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
                    width: Globals.sp(300)
                    padding: 8
                    backgroundRadius: 12

                    component TrackRow: Rectangle {
                        id: trackRow
                        property string label
                        property bool current
                        signal picked()
                        width: parent ? parent.width : 0
                        height: Globals.sp(34)
                        radius: 8
                        color: rowHover.hovered ? Theme.hoverFill : "transparent"
                        HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: { trackRow.picked(); trackPopup.close() } }
                        AppIcon {
                            id: tick
                            anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }
                            name: "check"
                            size: 16
                            color: Theme.accent
                            opacity: trackRow.current ? 1 : 0
                        }
                        Text {
                            anchors { left: tick.right; leftMargin: 8; right: parent.right; rightMargin: 8
                                      verticalCenter: parent.verticalCenter }
                            text: trackRow.label
                            elide: Text.ElideRight
                            color: trackRow.current ? Theme.textPrimary : Theme.textSecondary
                            font.pixelSize: Globals.sp(Theme.compactSize)
                            font.weight: trackRow.current ? Font.DemiBold : Font.Normal
                        }
                    }
                    component SectionLabel: Text {
                        leftPadding: 8
                        topPadding: 6
                        bottomPadding: 2
                        color: Theme.textMuted
                        font.pixelSize: Globals.sp(13)
                        font.bold: true
                    }

                    contentItem: Flickable {
                        implicitHeight: Math.min(trackColumn.implicitHeight, Globals.sp(420))
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
                                current: controlBar.player.primarySubId === 0
                                onPicked: controlBar.player.setPrimarySub(0)
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
                icon: controlBar.player.subVisible ? "captions" : "captions-off"
                tip: controlBar.player.subVisible ? qsTr("Hide subtitles") : qsTr("Show subtitles")
                onClicked: controlBar.player.subVisible = !controlBar.player.subVisible
            }
            CtrlBtn {
                icon: "server"
                tip: qsTr("Servers")
                onClicked: controlBar.panelRequested("servers")
            }
            CtrlBtn {
                icon: "list-video"
                tip: qsTr("Episodes")
                onClicked: controlBar.playlistRequested()
            }

            Divider {}

            CtrlBtn {
                icon: "folder"
                tip: qsTr("Open file")
                onClicked: controlBar.openFileRequested()
            }
            CtrlBtn {
                icon: "settings"
                tip: qsTr("Settings")
                onClicked: controlBar.panelRequested("general")
            }

            Divider {}

            CtrlBtn {
                icon: Globals.pipMode ? "picture-in-picture-2" : "picture-in-picture"
                tip: Globals.tipWithKey(Globals.pipMode ? qsTr("Exit picture-in-picture") : qsTr("Picture-in-picture"), "pip")
                onClicked: Globals.togglePip()
            }
            CtrlBtn {
                icon: Globals.fullscreen ? "minimize" : "maximize"
                tip: Globals.tipWithKey(Globals.fullscreen ? qsTr("Exit fullscreen") : qsTr("Fullscreen"), "fullscreen")
                onClicked: Globals.toggleFullscreen()
            }
        }
    }
}
