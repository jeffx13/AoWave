import QtQuick
import QtQuick.Effects
import ".."

// Active: accent gradient tile. Otherwise the glyph alone, muted.
Item {
    id: root

    property string glyph: ""
    property bool   active: false
    property bool   hovered: false
    property int    size: 36

    implicitWidth: size
    implicitHeight: size

    scale: active ? 1.0 : hovered ? 1.06 : 1.0
    Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutBack } }

    Rectangle {
        id: tile
        anchors.fill: parent
        radius: root.size * 0.3
        color: root.hovered && !root.active ? Theme.hoverFill : "transparent"
        Behavior on color { ColorAnimation { duration: 140 } }

        Rectangle {
            id: fill
            anchors.fill: parent
            radius: parent.radius
            opacity: root.active ? 1.0 : 0.0
            Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
            gradient: Gradient {
                orientation: Gradient.Vertical
                GradientStop { position: 0.0; color: Theme.accentLight }
                GradientStop { position: 1.0; color: Theme.accent2 }
            }
            Rectangle {
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 }
                height: parent.height * 0.45
                radius: parent.radius - 1
                gradient: Gradient {
                    GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, 0.22) }
                    GradientStop { position: 1.0; color: "transparent" }
                }
            }
        }
    }

    // Behind the tile, sized from it.
    MultiEffect {
        anchors.fill: tile
        source: fill
        z: -1
        shadowEnabled: true
        shadowBlur: 14 / blurMax
        shadowVerticalOffset: 2
        shadowColor: Qt.alpha(Theme.accent, 0.55)
        opacity: root.active ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 200 } }
    }

    AppIcon {
        anchors.centerIn: parent
        name: root.glyph
        size: Math.round(root.size * 0.5)
        color: root.active ? Theme.inkOn(Theme.accent)
             : root.hovered ? Theme.textPrimary : Theme.textMuted
        Behavior on color { ColorAnimation { duration: 160 } }
    }
}
