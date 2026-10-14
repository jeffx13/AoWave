import QtQuick
import QtQuick.Controls

AbstractButton {
    id: btn

    property string iconName: ""
    property string tip: ""
    property int    iconSize: 20
    property real   boxRadius: 9
    property string variant: ""
    property color  hoverColor: variant === "overlay" ? Theme.overlayFillHover
                              : variant === "accent" ? (hovered ? Theme.accent : Qt.alpha(Theme.accent, 0.85))
                              : Theme.hoverFill
    property color  iconColor: variant === "overlay" ? Theme.onOverlay
                             : variant === "accent" ? Theme.onAccent : Theme.textSecondary
    property color  iconHoverColor: variant === "overlay" ? Theme.onOverlay
                                  : variant === "accent" ? Theme.onAccent : Theme.textPrimary
    property bool   active: variant === "accent"
    property bool   destructive: false
    property string accessibleName: ""

    implicitWidth: 36
    implicitHeight: 36
    focusPolicy: Qt.TabFocus
    hoverEnabled: true
    Accessible.name: accessibleName || tip
    Accessible.description: accessibleName ? tip : ""
    Keys.onPressed: event => Ui.clickOnEnter(event, btn)

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

    property bool tipDismissed: false
    onHoveredChanged: if (!hovered) tipDismissed = false
    onPressedChanged: if (pressed) tipDismissed = true
    AppToolTip { text: btn.tip; visible: btn.tip !== "" && btn.hovered && !btn.tipDismissed }
    HoverHandler { cursorShape: btn.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
    FocusRing { shown: btn.visualFocus; radius: btn.boxRadius + 3 }
}
