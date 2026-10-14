import QtQuick
import App

Item {
    id: cover
    clip: true

    property url source
    property int decodeWidth: 360
    // True for a moment when a clearance arrives and the picture was refused, so it is asked for again.
    property bool retrying: false
    readonly property alias status: picture.status
    readonly property real shape: picture.status === Image.Ready && picture.implicitWidth > 0
                                  ? picture.implicitHeight / picture.implicitWidth : 0
    readonly property bool fills: shape <= 0 || width <= 0 || height <= 0
                                  || Math.abs(shape / (height / width) - 1) < 0.2

    Image {
        anchors.fill: parent
        visible: !cover.fills && picture.status === Image.Ready
        source: visible ? picture.source : ""
        sourceSize.width: 24
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: true
        opacity: 0.45
    }

    Image {
        id: picture
        anchors.centerIn: parent
        width: cover.fills && cover.shape > 0 ? Math.max(cover.width, cover.height / cover.shape) : cover.width
        height: cover.fills && cover.shape > 0 ? Math.max(cover.height, cover.width * cover.shape) : cover.height
        source: cover.retrying ? "" : cover.source
        sourceSize.width: cover.decodeWidth
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        cache: true
    }

    Connections {
        target: Clearances
        function onCountChanged() { if (picture.status === Image.Error || picture.status === Image.Loading) cover.retrying = true }
    }

    Timer {
        interval: 1
        running: cover.retrying
        onTriggered: cover.retrying = false
    }
}
