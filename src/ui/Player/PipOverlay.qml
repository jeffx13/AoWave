pragma ComponentBehavior: Bound
import QtQuick
import App

Item {
    id: pip

    readonly property MpvPlayer player: Ui.mpv
    readonly property bool playing: !!player && player.state === MpvPlayer.Playing
    readonly property bool busy: !!player && (player.isLoading || player.buffering || App.playlist.isLoading)
    readonly property bool hovered: hover.hovered
    property bool awake: false
    readonly property bool shown: (hovered && awake) || grip.pressed || seekArea.pressed || mover.active || !playing
    readonly property int fade: Theme.dur(150)

    property int stepRevision: 0
    readonly property bool canPrevious: stepRevision >= 0 && App.playlist.canStep(-1)
    readonly property bool canNext: stepRevision >= 0 && App.playlist.canStep(1)
    Connections {
        target: App.playlist
        function onCurrentItemChanged() { pip.stepRevision++ }
        function onRowsInserted() { pip.stepRevision++ }
        function onRowsRemoved() { pip.stepRevision++ }
        function onRowsMoved() { pip.stepRevision++ }
        function onModelReset() { pip.stepRevision++ }
    }

    HoverHandler {
        id: hover
        onPointChanged: { pip.awake = true; sleep.restart() }
        onHoveredChanged: if (!hovered) pip.awake = false
    }
    Timer { id: sleep; interval: 2200; onTriggered: pip.awake = false }

    // a drag from anywhere, a button too, is Windows' own move; the window settles when it ends
    DragHandler {
        id: mover
        target: null
        grabPermissions: PointerHandler.CanTakeOverFromAnything
        onActiveChanged: active ? Ui.root.movePip() : Ui.root.settlePip()
    }

    WheelHandler {
        acceptedModifiers: Qt.ControlModifier
        onWheel: (event) => Ui.root.resizePip(Ui.root.width * (event.angleDelta.y > 0 ? 1.1 : 1 / 1.1))
    }

    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: 0
        NumberAnimation on opacity { id: reveal; running: false; from: 1; to: 0; duration: Theme.dur(260); easing.type: Easing.OutCubic }
        Connections {
            target: Ui
            function onPipModeChanged() { if (Ui.pipMode && !Theme.reduceMotion) reveal.restart() }
        }
    }

    Item {
        anchors.fill: parent
        opacity: pip.shown ? 1 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: pip.fade; easing.type: Easing.OutCubic } }

        Rectangle {
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: Math.min(parent.height * 0.45, Ui.sp(64))
            gradient: Gradient {
                GradientStop { position: 0.0; color: Theme.overlayScrimMid }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }
        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: Math.min(parent.height * 0.5, Ui.sp(72))
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: Theme.overlayScrimMid }
            }
        }

        Column {
            anchors { left: parent.left; right: windowButtons.left; top: parent.top
                      leftMargin: gripBox.onRight && gripBox.onBottom ? Ui.sp(30) : Ui.sp(12)
                      rightMargin: 6; topMargin: Ui.sp(9) }
            spacing: 1
            Text {
                width: parent.width
                text: Ui.root ? Ui.root.nowPlayingTitle : ""
                color: Theme.onOverlay
                font.pixelSize: Ui.sp(Theme.compactSize)
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                visible: text !== "" && pip.height > Ui.sp(180)
                text: Ui.root ? Ui.root.nowPlayingEpisode : ""
                color: Theme.onOverlayMuted
                font.pixelSize: Ui.sp(Theme.captionSize)
                elide: Text.ElideRight
            }
        }

        Row {
            id: windowButtons
            anchors { right: parent.right; top: parent.top; margins: 4
                      rightMargin: !gripBox.onRight && gripBox.onBottom ? Ui.sp(28) : 4 }
            IconButton {
                iconName: "maximize"
                iconSize: 15
                tip: Ui.tipWithKey(qsTr("Back to the player"), "pip")
                variant: "overlay"
                onClicked: Ui.togglePip()
            }
            IconButton {
                iconName: "x"
                iconSize: 15
                tip: qsTr("Close and pause")
                variant: "overlay"
                onClicked: {
                    if (pip.playing) pip.player.pause()
                    Ui.togglePip()
                }
            }
        }

        Row {
            anchors.centerIn: parent
            spacing: Ui.sp(10)
            visible: !pip.busy
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: pip.width > Ui.sp(260)
                iconName: "skip-back"
                iconSize: 18
                tip: Ui.tipWithKey(qsTr("Previous episode"), "previousEpisode")
                variant: "overlay"
                enabled: pip.canPrevious
                opacity: enabled ? 1 : 0.4
                onClicked: App.playlist.stepItem(-1)
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: Ui.sp(48)
                implicitHeight: Ui.sp(48)
                boxRadius: implicitWidth / 2
                iconName: pip.playing ? "pause" : "play"
                tip: Ui.tipWithKey(pip.playing ? qsTr("Pause") : qsTr("Play"), "playPause")
                iconSize: 24
                variant: "accent"
                scale: pressed ? 0.92 : 1
                Behavior on scale { NumberAnimation { duration: Theme.dur(90) } }
                onClicked: pip.player.togglePlayPause()
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: pip.width > Ui.sp(260)
                iconName: "skip-forward"
                iconSize: 18
                tip: Ui.tipWithKey(qsTr("Next episode"), "nextEpisode")
                variant: "overlay"
                enabled: pip.canNext
                opacity: enabled ? 1 : 0.4
                onClicked: App.playlist.stepItem(1)
            }
        }

        TimeCode {
            anchors { left: parent.left; bottom: parent.bottom; bottomMargin: Ui.sp(18)
                      leftMargin: gripBox.onRight && !gripBox.onBottom ? Ui.sp(30) : Ui.sp(12) }
            visible: !!pip.player && pip.player.duration > 0
            text: pip.player ? Ui.timeText(seekArea.pressed ? seekArea.time : pip.player.time)
                               + " / " + Ui.timeText(pip.player.duration) : ""
            color: Theme.onOverlay
            pixelSize: Ui.sp(Theme.captionSize)
        }
        IconButton {
            anchors { right: parent.right; bottom: parent.bottom; bottomMargin: Ui.sp(14)
                      rightMargin: !gripBox.onRight && !gripBox.onBottom ? Ui.sp(28) : 4 }
            implicitWidth: Ui.sp(28)
            implicitHeight: Ui.sp(28)
            iconName: !pip.player || pip.player.muted || pip.player.volume === 0 ? "volume-x"
                    : pip.player.volume < 50 ? "volume-1" : "volume-2"
            iconSize: 15
            tip: Ui.tipWithKey(pip.player && pip.player.muted ? qsTr("Unmute") : qsTr("Mute"), "mute")
            variant: "overlay"
            onClicked: pip.player.muted = !pip.player.muted
        }
    }

    AppSpinner {
        anchors.centerIn: parent
        visible: pip.busy
    }

    MouseArea {
        id: seekArea
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom
                  leftMargin: Ui.sp(10); rightMargin: Ui.sp(10) }
        height: Ui.sp(16)
        enabled: pip.shown && !!pip.player && pip.player.duration > 0
        hoverEnabled: true
        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        readonly property real fraction: !pip.player || pip.player.duration <= 0 ? 0
            : pressed ? Math.max(0, Math.min(1, mouseX / width))
            : Math.min(1, pip.player.time / pip.player.duration)
        readonly property real time: fraction * (pip.player ? pip.player.duration : 0)
        // a drag that goes sideways first scrubs; one that goes up or down first moves the window
        preventStealing: true
        property point pressedAt
        property bool scrubbing: false
        onPressed: (mouse) => { pressedAt = Qt.point(mouse.x, mouse.y); scrubbing = false; preventStealing = true }
        onPositionChanged: (mouse) => {
            if (!pressed || scrubbing || !preventStealing) return
            const slop = Application.styleHints.startDragDistance
            if (Math.abs(mouse.x - pressedAt.x) > slop) scrubbing = true
            else if (Math.abs(mouse.y - pressedAt.y) > slop) preventStealing = false
        }
        onCanceled: preventStealing = true
        onReleased: { pip.player.seek(time); preventStealing = true }

        Rectangle {
            id: track
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; bottomMargin: 4 }
            height: seekArea.containsMouse || seekArea.pressed ? 5 : pip.shown ? 3 : 2
            radius: height / 2
            color: Theme.overlayFillActive
            Behavior on height { NumberAnimation { duration: Theme.dur(120) } }
            Rectangle {
                width: parent.width * seekArea.fraction
                height: parent.height
                radius: parent.radius
                color: Theme.accent
            }
            Rectangle {
                visible: seekArea.containsMouse || seekArea.pressed
                x: parent.width * seekArea.fraction - width / 2
                anchors.verticalCenter: parent.verticalCenter
                width: 12; height: 12; radius: 6
                color: Theme.onOverlay
            }
        }
    }

    Item {
        id: gripBox
        readonly property bool onRight: {
            if (!Ui.root) return false
            const area = Ui.root.area()
            return Ui.root.x + Ui.root.width / 2 > area.x + area.width / 2
        }
        readonly property bool onBottom: {
            if (!Ui.root) return false
            const area = Ui.root.area()
            return Ui.root.y + Ui.root.height / 2 > area.y + area.height / 2
        }
        width: Ui.sp(26)
        height: width
        x: onRight ? 0 : parent.width - width
        y: onBottom ? 0 : parent.height - height
        opacity: pip.shown ? 1 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: pip.fade } }
        AppIcon {
            anchors.centerIn: parent
            name: "arrow-up-left"
            size: 14
            color: Theme.onOverlay
            rotation: gripBox.onRight ? (gripBox.onBottom ? 0 : 270) : (gripBox.onBottom ? 90 : 180)
        }
        MouseArea {
            id: grip
            anchors.fill: parent
            preventStealing: true
            cursorShape: gripBox.onRight === gripBox.onBottom ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
            property real startX
            property real startWidth
            onPressed: (mouse) => {
                startX = mapToGlobal(mouse.x, mouse.y).x
                startWidth = Ui.root.width
            }
            onPositionChanged: (mouse) => {
                if (!pressed) return
                const dx = mapToGlobal(mouse.x, mouse.y).x - startX
                Ui.root.resizePip(startWidth + (gripBox.onRight ? -dx : dx))
            }
            onReleased: Ui.root.savePipRect()
        }
    }
}
