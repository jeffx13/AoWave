import QtQuick
import ".."

// Liquid in a capsule, drawn by water.frag: title pill and player pipe share it.
ShaderEffect {
    id: water

    property real progress: 0
    property real buffered: 0
    property bool turbulentFront: false
    property bool animating: true
    property color waterColor: Theme.accent
    property color trackColor: "transparent"
    property real waterOpacity: 1
    // Sized in item heights, so a 6px pipe and a 32px pill need different scales.
    property real bubbles: 1
    property real bubbleScale: 1

    // Named as water.frag declares them.
    property real time: 0
    readonly property real aspect: height > 0 ? width / height : 1
    readonly property real turbulent: turbulentFront ? 1 : 0
    readonly property real intensity: waterOpacity
    readonly property vector4d waterRgba: Qt.vector4d(waterColor.r, waterColor.g, waterColor.b, waterColor.a)
    readonly property vector4d trackRgba: Qt.vector4d(trackColor.r, trackColor.g, trackColor.b, trackColor.a)

    fragmentShader: "qrc:/App/src/ui/shaders/water.frag.qsb"
    blending: true

    FrameAnimation {
        running: water.visible && water.animating && water.progress > 0 && !Theme.reduceMotion
        onTriggered: water.time = (water.time + frameTime) % 3600
    }
}
