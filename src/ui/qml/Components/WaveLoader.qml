import QtQuick
import ".."

// The loading overlay's motif.
ShaderEffect {
    id: wave

    property color colorA: Theme.accent
    property color colorB: Theme.accent2
    property bool running: true

    // Named as wave.frag declares them. `flow` stays 0: an overlay that leans looks like a fault.
    property real flow: 0
    property real foam: 1.0

    property real time: 0
    readonly property real aspect: height > 0 ? width / height : 1
    readonly property vector4d tintA: Qt.vector4d(colorA.r, colorA.g, colorA.b, colorA.a)
    readonly property vector4d tintB: Qt.vector4d(colorB.r, colorB.g, colorB.b, colorB.a)

    fragmentShader: "qrc:/App/src/ui/shaders/wave.frag.qsb"
    blending: true

    FrameAnimation {
        running: wave.visible && wave.running && !Theme.reduceMotion
        onTriggered: wave.time = (wave.time + frameTime) % 3600
    }
}
