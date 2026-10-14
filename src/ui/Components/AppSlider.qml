import QtQuick
import QtQuick.Controls

Slider {
    id: slider

    property int   decimals: 0
    property string unitSuffix: ""

    implicitHeight: Ui.sp(26)
    implicitWidth: 140
    hoverEnabled: true
    readonly property bool overlay: Ui.inOverlay(slider)
    focusPolicy: Qt.TabFocus
    Accessible.name: Ui.labelOf(slider)
    live: true

    readonly property bool active: hovered || pressed

    background: Rectangle {
        x: slider.leftPadding
        y: slider.topPadding + slider.availableHeight / 2 - height / 2
        width: slider.availableWidth
        height: slider.active ? 8 : 5
        radius: height / 2
        color: slider.overlay ? Theme.overlayFillHover : Theme.border
        Behavior on height { NumberAnimation { duration: Theme.dur(130); easing.type: Easing.OutCubic } }

        Rectangle {
            width: slider.visualPosition * parent.width
            height: parent.height
            radius: parent.radius
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: Theme.accentLight }
                GradientStop { position: 1.0; color: Theme.accent }
            }
        }
    }

    handle: Rectangle {
        x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
        y: slider.topPadding + slider.availableHeight / 2 - height / 2
        width: Ui.sp(17); height: width; radius: width / 2
        gradient: Gradient {
            GradientStop { position: 0.0; color: Theme.accentLight }
            GradientStop { position: 1.0; color: Theme.accent }
        }
        border.color: Theme.accentInk
        border.width: 2
        scale: slider.pressed ? 1.22 : (slider.hovered ? 1.1 : 1.0)
        FocusRing { shown: slider.visualFocus; radius: parent.radius + 3 }
        Behavior on scale { NumberAnimation { duration: Theme.dur(110); easing.type: Easing.OutCubic } }

        Rectangle {
            anchors.centerIn: parent
            width: 6; height: 6; radius: 3
            color: Theme.accentInk
        }

        Rectangle {
            anchors.centerIn: parent
            width: parent.width + 14; height: width; radius: width / 2
            color: "transparent"
            border.color: Theme.accent
            border.width: 2
            opacity: slider.active ? 0.4 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.dur(130) } }
        }

        Rectangle {
            visible: slider.active || slider.visualFocus
            anchors.horizontalCenter: parent.horizontalCenter
            y: -height - 8
            width: bubbleText.implicitWidth + 16
            height: 24
            radius: 7
            color: Theme.accent

            Text {
                id: bubbleText
                anchors.centerIn: parent
                text: (slider.decimals > 0 ? Number(slider.value).toFixed(slider.decimals)
                                           : Math.round(slider.value)) + slider.unitSuffix
                color: Theme.accentInk
                font.pixelSize: Ui.sp(Theme.metadataSize)
                font.weight: Font.Medium
            }

            Rectangle {
                anchors { top: parent.bottom; horizontalCenter: parent.horizontalCenter; topMargin: -3 }
                width: 8; height: 8; rotation: 45
                color: Theme.accent
            }
        }
    }
}
