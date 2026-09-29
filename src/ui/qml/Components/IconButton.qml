import QtQuick
import QtQuick.Controls
import ".."

AbstractButton {
    id: btn

    // Not `icon`: AbstractButton declares that final.
    property string iconName: ""
    property string tip: ""
    property int    iconSize: 20
    property real   boxRadius: 9
    property color  hoverColor: Theme.hoverFill
    property color  iconColor: Theme.textSecondary
    property color  iconHoverColor: Theme.textPrimary
    property bool   active: false        // paint hoverColor even when not hovered
    // Destructive: red glyph on a light-red tile.
    property bool   destructive: false

    implicitWidth: 36
    implicitHeight: 36
    focusPolicy: Qt.NoFocus
    hoverEnabled: true

    background: Rectangle {
        radius: btn.boxRadius
        color: btn.destructive ? (btn.hovered ? Theme.dangerSoft : "transparent")
             : btn.active || btn.hovered ? btn.hoverColor : "transparent"
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    contentItem: Item {
        AppIcon {
            anchors.centerIn: parent
            name: btn.iconName
            size: btn.iconSize
            color: btn.destructive ? (btn.hovered ? Theme.danger : Theme.textMuted)
                 : btn.hovered ? btn.iconHoverColor : btn.iconColor
            Behavior on color { ColorAnimation { duration: 120 } }
        }
    }

    // Gone from the press until the pointer leaves: what the click opened or did is what counts.
    property bool tipDismissed: false
    onHoveredChanged: if (!hovered) tipDismissed = false
    onPressedChanged: if (pressed) tipDismissed = true
    AppToolTip { text: btn.tip; visible: btn.tip !== "" && btn.hovered && !btn.tipDismissed }
    HoverHandler { cursorShape: btn.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
}
