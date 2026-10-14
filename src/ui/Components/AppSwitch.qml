import QtQuick
import QtQuick.Controls

Switch {
    id: sw

    implicitWidth: Ui.sp(42)
    implicitHeight: Ui.sp(23)
    padding: 0
    hoverEnabled: true
    readonly property bool overlay: Ui.inOverlay(sw)
    focusPolicy: Qt.TabFocus
    opacity: enabled ? 1 : 0.5
    Accessible.name: sw.text || Ui.labelOf(sw)
    Keys.onPressed: event => Ui.clickOnEnter(event, sw)

    indicator: Rectangle {
        implicitWidth: sw.implicitWidth
        implicitHeight: sw.implicitHeight
        x: sw.leftPadding
        y: parent.height / 2 - height / 2
        radius: height / 2
        color: sw.overlay ? Theme.overlayFillActive : Theme.border
        border.width: 1
        border.color: sw.checked ? Qt.lighter(Theme.accent, 1.2)
                    : sw.hovered ? (sw.overlay ? Theme.onOverlayDim : Theme.textMuted)
                    : sw.overlay ? Theme.overlayFillActive : Theme.border
        Behavior on border.color { ColorAnimation { duration: Theme.dur(150) } }

        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            opacity: sw.checked ? 1 : 0
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: Theme.accentLight }
                GradientStop { position: 1.0; color: Theme.accent }
            }
            Behavior on opacity { NumberAnimation { duration: Theme.dur(180); easing.type: Easing.OutCubic } }

            Rectangle {
                anchors { left: parent.left; right: parent.right; top: parent.top
                          leftMargin: 8; rightMargin: 8; topMargin: 1 }
                height: 1
                radius: 1
                color: Theme.overlayFillActive
            }
        }

        Rectangle {
            width: parent.height - 6
            height: width
            radius: width / 2
            y: 3
            x: sw.checked ? parent.width - width - 3 : 3
            color: Theme.onOverlay
            border.width: 1
            border.color: sw.checked ? Qt.lighter(Theme.accent, 1.3) : Theme.borderStrong

            Behavior on x { NumberAnimation { duration: Theme.dur(190); easing.type: Easing.OutCubic } }

            scale: sw.down ? 0.9 : (sw.hovered ? 1.05 : 1.0)
            Behavior on scale { NumberAnimation { duration: Theme.dur(120); easing.type: Easing.OutCubic } }

            Rectangle {
                anchors.centerIn: parent
                width: 4; height: 4; radius: 2
                color: sw.checked ? Theme.accent : Theme.textDisabled
                opacity: 0.55
                Behavior on color { ColorAnimation { duration: Theme.dur(150) } }
            }
        }
    }

    FocusRing { shown: sw.visualFocus; radius: sw.height / 2 + 3 }
}
