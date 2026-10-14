import QtQuick

// the one pill: neutral by default, tinted, filled or checked; over video it takes the overlay look
Rectangle {
    id: chip

    property alias label: chipLabel.text
    property string iconName: ""
    property color tint: overlay ? Theme.accent : "transparent"
    property bool filled: false
    property bool checked: false
    property bool interactive: false
    readonly property bool overlay: Ui.inOverlay(chip)
    readonly property bool hovered: hover.hovered
    readonly property color hue: checked ? Theme.accent : tint
    readonly property bool solid: filled || checked
    readonly property bool neutral: !solid && tint.a === 0
    readonly property color ink: solid ? Theme.inkOn(hue)
                               : overlay ? (neutral ? Theme.onOverlayMuted : Theme.onOverlayAccent)
                               : interactive && hovered ? Theme.textPrimary : Theme.textSecondary
    signal clicked()

    implicitHeight: overlay ? Ui.sp(17) : Ui.sp(30)
    implicitWidth: content.implicitWidth + (overlay ? 12 : Ui.sp(22))
    radius: height / 2
    color: solid ? hue
         : neutral ? (overlay ? Theme.overlayFill : interactive && hovered ? Theme.surfaceRaised : Theme.surfaceAlt)
         : Qt.alpha(hue, interactive && hovered ? 0.22 : 0.14)
    border.width: 1
    border.color: solid ? Qt.alpha(hue, 0.6)
                : neutral ? (overlay ? Theme.overlayLine : interactive && hovered ? Theme.borderStrong : Theme.border)
                : Qt.alpha(hue, 0.45)
    Behavior on color { ColorAnimation { duration: Theme.dur(120) } }

    activeFocusOnTab: interactive
    Accessible.role: interactive ? Accessible.Button : Accessible.StaticText
    Accessible.name: chipLabel.text
    Keys.onPressed: event => {
        if (!interactive || event.isAutoRepeat) return
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
            event.accepted = true
            chip.clicked()
        }
    }

    HoverHandler { id: hover; cursorShape: chip.interactive ? Qt.PointingHandCursor : Qt.ArrowCursor }
    TapHandler { enabled: chip.interactive; onTapped: chip.clicked() }
    FocusRing { shown: chip.interactive && chip.activeFocus; radius: chip.radius + 3 }

    Row {
        id: content
        anchors.centerIn: parent
        spacing: Ui.sp(5)
        AppIcon {
            visible: chip.iconName !== ""
            anchors.verticalCenter: parent.verticalCenter
            name: chip.iconName
            size: Math.round(chipLabel.font.pixelSize * 0.95)
            color: chip.ink
        }
        Text {
            id: chipLabel
            anchors.verticalCenter: parent.verticalCenter
            color: chip.ink
            font.pixelSize: chip.overlay ? Ui.sp(Theme.captionSize) : Ui.sp(Theme.metadataSize)
            font.weight: chip.overlay ? Font.Bold : Font.Medium
        }
    }
}
