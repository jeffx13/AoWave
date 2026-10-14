import QtQuick
import QtQuick.Effects

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
    // Colorization scales by the tint's alpha, so Brightness whitens the black strokes and the tint stays opaque.
    MultiEffect {
        anchors.fill: img
        source: img
        brightness: 1.0
        colorization: 1.0
        colorizationColor: Qt.rgba(root.color.r, root.color.g, root.color.b, 1)
        opacity: root.color.a
    }
}
