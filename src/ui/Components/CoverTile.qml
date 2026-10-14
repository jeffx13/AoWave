import QtQuick
import App

Rectangle {
    id: tile

    property url source
    // True for a moment when a clearance arrives and the picture was refused, so it is asked for again.
    property bool retrying: false

    radius: 6
    clip: true
    color: Theme.surfaceDeep

    Image {
        id: art
        anchors.fill: parent
        source: tile.retrying ? "" : tile.source
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: true
        sourceSize: Qt.size(width * 2, height * 2)
    }

    Connections {
        target: Clearances
        function onCountChanged() { if (art.status === Image.Error || art.status === Image.Loading) tile.retrying = true }
    }

    Timer {
        interval: 1
        running: tile.retrying
        onTriggered: tile.retrying = false
    }
}
