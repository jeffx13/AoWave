pragma ComponentBehavior: Bound
import QtQuick
import ".."
import App

Rectangle {
    id: bar

    property bool   canGoBack: false
    property bool   canGoForward: false
    property string nowPlayingTitle: ""
    property string nowPlayingEpisode: ""

    signal moveRequested()
    signal maximiseToggled()
    signal minimiseRequested()
    signal closeRequested()
    signal historyStep(int delta)
    signal playerRequested()

    visible: height > 0
    focus: false

    gradient: Gradient {
        GradientStop { position: 0.0; color: Theme.surface }
        GradientStop { position: 1.0; color: Theme.surfaceDeep }
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.border
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        onPressed: bar.moveRequested()
        onDoubleClicked: bar.maximiseToggled()
    }

    Row {
        anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 10 }
        spacing: 2

        Repeater {
            model: [{ icon: "chevron-left", forward: false }, { icon: "chevron-right", forward: true }]
            delegate: IconButton {
                required property var modelData
                implicitWidth: 30
                implicitHeight: 30
                boxRadius: 8
                enabled: modelData.forward ? bar.canGoForward : bar.canGoBack
                opacity: enabled ? 1.0 : 0.4
                iconName: modelData.icon
                iconSize: 18
                hoverColor: Qt.alpha(Theme.accent, 0.14)
                iconColor: enabled ? Theme.textSecondary : Theme.textMuted
                iconHoverColor: Theme.textSecondary
                onClicked: bar.historyStep(modelData.forward ? 1 : -1)
                tip: modelData.forward ? Globals.tipWithKey(qsTr("Forward"), "forward")
                                       : Globals.tipWithKey(qsTr("Back"), "back")
            }
        }
    }

    Rectangle {
        id: npPill
        anchors { verticalCenter: parent.verticalCenter; horizontalCenter: parent.horizontalCenter }
        visible: bar.nowPlayingTitle !== ""
        height: Math.min(Globals.sp(32), Math.max(0, bar.height - 8))
        readonly property bool compact: maxWidth < 160
        readonly property int chrome: compact ? 54 : 112
        // Half the bar; the second term only bites on a narrow window.
        readonly property real maxWidth: Math.max(0, Math.min(bar.width * 0.5, bar.width - 180))
        readonly property real maxLabel: Math.max(0, maxWidth - chrome)
        width: Math.min(maxWidth, chrome + Math.ceil(npLabel.contentWidth) + 2)
        radius: height / 2
        color: Theme.surfaceDeep
        border.color: pillHover.hovered ? Qt.alpha(Theme.accent, 0.7) : Qt.alpha(Theme.accent, 0.4)
        border.width: 1
        Behavior on border.color { ColorAnimation { duration: 150 } }

        readonly property bool playing: Globals.mpv && Globals.mpv.state === MpvPlayer.Playing
        readonly property real progress: Globals.mpv && Globals.mpv.duration > 0
                                         ? Globals.mpv.time / Globals.mpv.duration : 0

        function escapeHtml(s) {
            return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
        }

        HoverHandler { id: pillHover }

        // Accent bloom, stronger while playing.
        Rectangle {
            anchors { fill: parent; margins: -3 }
            radius: height / 2
            color: "transparent"
            border.color: Qt.alpha(Theme.accent, npPill.playing ? 0.16 : 0.08)
            border.width: 3
            Behavior on border.color { ColorAnimation { duration: 300 } }
        }

        WaterProgress {
            anchors { fill: parent; margins: 1 }
            progress: npPill.progress
            turbulentFront: true
            animating: npPill.playing
            waterOpacity: Theme.isLight ? 0.5 : 0.62
            trackColor: Theme.surfaceAlt
            // No bubbles: the pill is flat and carries a label.
            bubbles: 0
        }

        // Highlight across the top half, over the water.
        Rectangle {
            anchors { fill: parent; margins: 1; bottomMargin: parent.height / 2 }
            radius: height
            gradient: Gradient {
                GradientStop { position: 0.0; color: Theme.isLight ? Qt.rgba(1, 1, 1, 0.55) : Qt.rgba(1, 1, 1, 0.13) }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }

        component NpBtn: IconButton {
            implicitWidth: 24
            implicitHeight: 24
            boxRadius: 12
            iconSize: 14
            hoverColor: Qt.alpha(Theme.accent, 0.35)
            iconColor: Theme.textPrimary
            iconHoverColor: Theme.textPrimary
        }

        Row {
            id: npRow
            anchors { left: parent.left; leftMargin: 9; verticalCenter: parent.verticalCenter }
            height: parent.height
            spacing: 5
            NpBtn {
                visible: !npPill.compact; anchors.verticalCenter: parent.verticalCenter
                iconName: "skip-back"; tip: qsTr("Previous episode")
                onClicked: App.playlist.stepItem(-1)
            }
            NpBtn {
                anchors.verticalCenter: parent.verticalCenter
                iconName: npPill.playing ? "pause" : "play"
                tip: npPill.playing ? qsTr("Pause") : qsTr("Play")
                onClicked: Globals.mpv.togglePlayPause()
            }
            NpBtn {
                visible: !npPill.compact; anchors.verticalCenter: parent.verticalCenter
                iconName: "skip-forward"; tip: qsTr("Next episode")
                onClicked: App.playlist.stepItem(1)
            }
            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 1; height: 14; color: Qt.alpha(Theme.textPrimary, 0.25) }
            // One label, so title and episode scroll as a unit.
            MarqueeText {
                id: npLabel
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(Math.ceil(contentWidth) + 2, npPill.maxLabel)
                height: npPill.height
                textFormat: Text.StyledText
                fontSize: Theme.compactSize
                fontWeight: Font.Medium
                color: Theme.textPrimary
                horizontalAlignment: Text.AlignLeft
                marqueeSpeed: 40
                text: npPill.escapeHtml(bar.nowPlayingTitle)
                      + (bar.nowPlayingEpisode !== ""
                         ? "  <font color=\"" + Theme.textSecondary + "\">·  " + npPill.escapeHtml(bar.nowPlayingEpisode) + "</font>"
                         : "")
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: bar.playerRequested() }
            }
        }
    }

    NotificationBell {
        anchors { verticalCenter: parent.verticalCenter; right: windowDots.left; rightMargin: 14 }
    }

    Row {
        id: windowDots
        anchors { verticalCenter: parent.verticalCenter; right: parent.right; rightMargin: 12 }
        spacing: 8
        layoutDirection: Qt.RightToLeft
        HoverHandler { id: groupHandler }
        Repeater {
            // Fixed traffic-light colours: these read as window controls.
            model: [
                { dotColor: "#ff5f57", groupColor: "#fa564d", borderColor: "#e0443e" },
                { dotColor: "#febc2e", groupColor: "#ffbf39", borderColor: "#dfa020" },
                { dotColor: "#28c840", groupColor: "#53cb43", borderColor: "#1aab29" }
            ]
            delegate: Rectangle {
                id: windowDot
                required property var modelData
                required property int index
                width: 18
                height: 18
                radius: 9
                HoverHandler { id: dotHover }
                color: dotHover.hovered     ? modelData.dotColor
                     : groupHandler.hovered ? modelData.groupColor
                                            : Theme.surfaceAlt
                border.color: dotHover.hovered ? modelData.borderColor : Theme.border
                border.width: 1
                AppToolTip {
                    visible: dotHover.hovered
                    text: windowDot.index === 0 ? qsTr("Close")
                        : windowDot.index === 1 ? (Globals.maximised ? qsTr("Restore") : qsTr("Maximise"))
                                                : qsTr("Minimise")
                }
                TapHandler {
                    onTapped: {
                        if (windowDot.index === 0)      bar.closeRequested()
                        else if (windowDot.index === 1) bar.maximiseToggled()
                        else                            bar.minimiseRequested()
                    }
                }
            }
        }
    }
}
