import QtQuick

Rectangle {
    id: bg

    property bool focused: false
    property bool hovered: false
    property color restingBorder: Theme.border
    readonly property bool overlay: Ui.inOverlay(bg)

    radius: 10
    color: overlay ? (focused ? Theme.overlayFillHover : Theme.overlayFill)
                   : (focused ? Theme.surfaceAlt : Theme.surface)
    border.color: focused ? Theme.accent
                : hovered ? (overlay ? Theme.overlayFillActive : Qt.lighter(Theme.accent, 1.3))
                : overlay ? Theme.overlayFillHover : restingBorder
    border.width: 1
    Behavior on border.color { ColorAnimation { duration: Theme.dur(150) } }
    Behavior on color { ColorAnimation { duration: Theme.dur(150) } }

    Rectangle {
        anchors { bottom: parent.bottom; horizontalCenter: parent.horizontalCenter; bottomMargin: -1 }
        height: 2
        radius: 1
        width: bg.focused ? parent.width - 20 : 0
        color: Theme.accent
        opacity: bg.focused ? 1.0 : 0.0
        Behavior on width { NumberAnimation { duration: Theme.dur(250); easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: Theme.dur(200) } }
    }
}
