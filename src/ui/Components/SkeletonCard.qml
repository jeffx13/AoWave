pragma ComponentBehavior: Bound
import QtQuick

Item {
    id: card
    property real aspectRatio: Ui.coverAspect

    Rectangle {
        id: body
        anchors { fill: parent; margins: 5 }
        color: Theme.surface
        radius: 9
        clip: true

        Rectangle {
            id: cover
            anchors { top: parent.top; left: parent.left; right: parent.right; margins: 2 }
            height: width * card.aspectRatio
            radius: 8
            color: Theme.surfaceAlt
        }
        Repeater {
            model: [0.85, 0.5]
            Rectangle {
                required property real modelData
                required property int index
                x: 8
                y: cover.y + cover.height + 10 + index * Ui.sp(Theme.bodySize) * 1.2
                width: (body.width - 16) * modelData
                height: Ui.sp(Theme.bodySize) * 0.6
                radius: height / 2
                color: Theme.surfaceAlt
            }
        }

        Rectangle {
            width: parent.width * 0.6
            height: parent.height
            rotation: 12
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 0.5; color: Qt.alpha(Theme.textPrimary, 0.07) }
                GradientStop { position: 1.0; color: "transparent" }
            }
            NumberAnimation on x {
                from: -card.width * 0.6
                to: card.width
                duration: 1150
                loops: Animation.Infinite
                running: card.visible && !Theme.reduceMotion
            }
        }
    }
}
