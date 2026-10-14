import QtQuick

Rectangle {
    property bool shown: false
    property real outset: 3

    anchors { fill: parent; margins: -outset }
    visible: shown
    color: "transparent"
    border { width: 2; color: Theme.textAccent }
}
