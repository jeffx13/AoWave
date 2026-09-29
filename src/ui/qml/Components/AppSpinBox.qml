pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import ".."

SpinBox {
    id: spin

    implicitHeight: Globals.controlHeight
    implicitWidth: 110
    hoverEnabled: true
    editable: true
    inputMethodHints: Qt.ImhDigitsOnly
    leftPadding: 34
    rightPadding: 34
    // Episode numbers, seconds and counts: 1182, never the locale's "1,182".
    textFromValue: (value, locale) => String(value)
    valueFromText: (text, locale) => parseInt(text, 10)

    contentItem: TextInput {
        id: spinInput
        ContextMenu.onRequested: (position) => { textMenu.active = true; (textMenu.item as TextMenu).popup(position) }
        Loader { id: textMenu; active: false; sourceComponent: TextMenu { target: spinInput } }
        text: spin.textFromValue(spin.value, spin.locale)
        readOnly: !spin.editable
        selectByMouse: true
        validator: spin.validator
        inputMethodHints: Qt.ImhDigitsOnly
        font.family: Globals.fontFamily
        font.pixelSize: Globals.sp(Theme.bodySize)
        font.weight: Font.Medium
        color: Theme.textPrimary
        selectionColor: Theme.accent
        selectedTextColor: Theme.onAccent
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: Text.AlignHCenter
    }

    down.indicator: Rectangle {
        implicitWidth: 30
        implicitHeight: parent.height
        anchors.left: parent.left
        radius: 10
        color: spin.down.pressed ? Theme.accent
             : spin.down.hovered ? Theme.hoverFill : "transparent"
        Behavior on color { ColorAnimation { duration: 80 } }

        AppIcon {
            anchors.centerIn: parent
            name: "minus"
            size: 15
            color: spin.down.pressed ? Theme.onAccent : Theme.textSecondary
        }
    }

    up.indicator: Rectangle {
        implicitWidth: 30
        implicitHeight: parent.height
        anchors.right: parent.right
        radius: 10
        color: spin.up.pressed ? Theme.accent
             : spin.up.hovered ? Theme.hoverFill : "transparent"
        Behavior on color { ColorAnimation { duration: 80 } }

        AppIcon {
            anchors.centerIn: parent
            name: "plus"
            size: 15
            color: spin.up.pressed ? Theme.onAccent : Theme.textSecondary
        }
    }

    background: Rectangle {
        radius: 10
        color: spin.activeFocus ? Theme.surfaceAlt : Theme.surface
        border.color: spin.activeFocus ? Theme.accent
                    : spin.hovered     ? Theme.textMuted : Theme.border
        border.width: 1
        Behavior on border.color { ColorAnimation { duration: 120 } }
    }
}
