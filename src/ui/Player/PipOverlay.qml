pragma ComponentBehavior: Bound
import QtQuick
import App

// Only buttons and the seek bar take the pointer; a press elsewhere reaches the video (drag, click, double click).
Item {
    id: pip

    readonly property MpvPlayer player: Ui.mpv
    readonly property bool playing: !!player && player.state === MpvPlayer.Playing
    readonly property bool busy: !!player && (player.isLoading || player.buffering || App.playlist.isLoading)
    readonly property bool hovered: hover.hovered
    property bool awake: false
    readonly property bool shown: (hovered && awake) || grip.pressed || seekArea.pressed || !playing
    readonly property int fade: Theme.reduceMotion ? 0 : 150

    function clock(seconds) {
        seconds = Math.max(0, Math.floor(seconds))
        const h = Math.floor(seconds / 3600), m = Math.floor(seconds % 3600 / 60), s = seconds % 60
        const pad = n => (n < 10 ? "0" : "") + n
        return (h > 0 ? h + ":" + pad(m) : m) + ":" + pad(s)
    }

    HoverHandler {
        id: hover
        onPointChanged: { pip.awake = true; sleep.restart() }
        onHoveredChanged: if (!hovered) pip.awake = false
    }
    Timer { id: sleep; interval: 2200; onTriggered: pip.awake = false }

    WheelHandler {
        acceptedModifiers: Qt.ControlModifier
        onWheel: (event) => Ui.root.resizePip(Ui.root.width * (event.angleDelta.y > 0 ? 1.1 : 1 / 1.1))
    }

    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: 0
        NumberAnimation on opacity { id: reveal; running: false; from: 1; to: 0; duration: 260; easing.type: Easing.OutCubic }
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
                GradientStop { position: 0.0; color: Qt.alpha("black", 0.75) }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }
        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: Math.min(parent.height * 0.5, Ui.sp(72))
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: Qt.alpha("black", 0.8) }
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
                font.pixelSize: Ui.sp(13)
                elide: Text.ElideRight
            }
        }

        Row {
            id: windowButtons
            anchors { right: parent.right; top: parent.top; margins: 4
                      rightMargin: !gripBox.onRight && gripBox.onBottom ? Ui.sp(26) : 4 }
            IconButton {
                iconName: "maximize"
                iconSize: 15
                tip: qsTr("Back to the full window")
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
            spacing: Ui.sp(14)
            visible: !pip.busy
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: pip.width > Ui.sp(260)
                iconName: "skip-back"
                iconSize: 18
                tip: qsTr("Previous episode")
                variant: "overlay"
                onClicked: App.playlist.stepItem(-1)
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: Ui.sp(52)
                implicitHeight: Ui.sp(52)
                boxRadius: implicitWidth / 2
                iconName: pip.playing ? "pause" : "play"
                tip: pip.playing ? qsTr("Pause") : qsTr("Play")
                iconSize: 24
                variant: "accent"
                scale: pressed ? 0.92 : 1
                Behavior on scale { NumberAnimation { duration: 90 } }
                onClicked: pip.player.togglePlayPause()
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: pip.width > Ui.sp(260)
                iconName: "skip-forward"
                iconSize: 18
                tip: qsTr("Next episode")
                variant: "overlay"
                onClicked: App.playlist.stepItem(1)
            }
        }

        Text {
            anchors { left: parent.left; bottom: parent.bottom; bottomMargin: Ui.sp(18)
                      leftMargin: gripBox.onRight && !gripBox.onBottom ? Ui.sp(30) : Ui.sp(12) }
            visible: !!pip.player && pip.player.duration > 0
            text: pip.player ? pip.clock(seekArea.pressed ? seekArea.time : pip.player.time)
                               + " / " + pip.clock(pip.player.duration) : ""
            color: Theme.onOverlay
            font.pixelSize: Ui.sp(13)
        }
        IconButton {
            anchors { right: parent.right; bottom: parent.bottom; bottomMargin: Ui.sp(14)
                      rightMargin: !gripBox.onRight && !gripBox.onBottom ? Ui.sp(26) : 4 }
            implicitWidth: Ui.sp(28)
            implicitHeight: Ui.sp(28)
            iconName: !pip.player || pip.player.muted || pip.player.volume === 0 ? "volume-x"
                    : pip.player.volume < 50 ? "volume-1" : "volume-2"
            iconSize: 15
            tip: pip.player && pip.player.muted ? qsTr("Unmute") : qsTr("Mute")
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
        onReleased: pip.player.seek(time)

        Rectangle {
            id: track
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; bottomMargin: 4 }
            height: seekArea.containsMouse || seekArea.pressed ? 5 : pip.shown ? 3 : 2
            radius: height / 2
            color: Qt.alpha("white", pip.shown ? 0.28 : 0.18)
            Behavior on height { NumberAnimation { duration: 120 } }
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
        readonly property bool onRight: Ui.root && Ui.root.x + Ui.root.width / 2 > Screen.desktopAvailableWidth / 2
        readonly property bool onBottom: Ui.root && Ui.root.y + Ui.root.height / 2 > Screen.desktopAvailableHeight / 2
        width: Ui.sp(24)
        height: width
        x: onRight ? 0 : parent.width - width
        y: onBottom ? 0 : parent.height - height
        opacity: pip.shown && pip.playing || grip.pressed ? 1 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: pip.fade } }
        AppIcon {
            anchors.centerIn: parent
            name: "arrow-up-left"
            size: 13
            color: Theme.onOverlay
            rotation: gripBox.onRight ? (gripBox.onBottom ? 0 : 270) : (gripBox.onBottom ? 90 : 180)
        }
        MouseArea {
            id: grip
            anchors.fill: parent
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
