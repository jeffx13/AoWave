import QtQuick
import QtQuick.Effects
import ".."

Item {
    id: root
    property string name: ""
    property color color: Theme.textSecondary
    property int size: 20
    implicitWidth: size
    implicitHeight: size

    Image {
        id: img
        anchors.fill: parent
        source: root.name ? "qrc:/App/resources/icons/" + root.name + ".svg" : ""
        sourceSize: Qt.size(root.size * 2, root.size * 2)
        fillMode: Image.PreserveAspectFit
        smooth: true
        visible: false
    }
    // Brightness lifts the black currentColor strokes to white so colorization can paint them.
    // Colorization scales by the tint's alpha instead of fading, hence opaque plus opacity.
    MultiEffect {
        anchors.fill: img
        source: img
        brightness: 1.0
        colorization: 1.0
        colorizationColor: Qt.rgba(root.color.r, root.color.g, root.color.b, 1)
        opacity: root.color.a
    }
}
