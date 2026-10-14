import QtQuick

Rectangle {
    id: row

    property bool current: false
    property bool hovered: false

    radius: 10
    color: current ? Theme.accentSoft : hovered ? Theme.overlayLine : "transparent"
    border.color: current ? Theme.accentMuted : "transparent"
    border.width: current ? 1 : 0
    Behavior on color { ColorAnimation { duration: 100 } }

    AccentBar {
        visible: row.current
        anchors.leftMargin: 2
    }
}
