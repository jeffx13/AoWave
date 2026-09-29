import QtQuick
import QtQuick.Controls
import ".."

ScrollBar {
    id: bar

    property color barColor: Theme.accent
    property real  barOpacity: 0.45
    property bool  showTrack: true

    policy: ScrollBar.AsNeeded
    width: 8

    // These replace the style's own, which hid themselves when everything fits.
    contentItem: Rectangle {
        visible: bar.size < 1.0
        radius: width / 2
        color: bar.barColor
        opacity: bar.pressed ? 1.0 : (bar.hovered ? Math.min(1.0, bar.barOpacity + 0.25) : bar.barOpacity)
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }

    background: Rectangle {
        visible: bar.showTrack && bar.size < 1.0
        radius: width / 2
        color: Theme.surfaceAlt
    }
}
