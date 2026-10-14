pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

SpinBox {
    id: spin

    implicitHeight: Ui.controlHeight
    implicitWidth: 110
    hoverEnabled: true
    readonly property bool overlay: Ui.inOverlay(spin)
    editable: true
    Accessible.name: Ui.labelOf(spin)
    inputMethodHints: Qt.ImhDigitsOnly
    leftPadding: 34
    rightPadding: 34
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
        font.family: Ui.fontFamily
        font.pixelSize: Ui.sp(Theme.bodySize)
        font.weight: Font.Medium
        color: spin.overlay ? Theme.onOverlay : Theme.textPrimary
        selectionColor: Theme.accent
        selectedTextColor: Theme.accentInk
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: Text.AlignHCenter
    }

    down.indicator: Rectangle {
        implicitWidth: 30
        implicitHeight: parent.height
        anchors.left: parent.left
        radius: 10
        color: spin.down.pressed ? Theme.accent
             : spin.down.hovered ? (spin.overlay ? Theme.overlayFillHover : Theme.hoverFill) : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.dur(80) } }

        AppIcon {
            anchors.centerIn: parent
            name: "minus"
            size: 15
            color: spin.down.pressed ? Theme.accentInk : spin.overlay ? Theme.onOverlayMuted : Theme.textSecondary
        }
    }

    up.indicator: Rectangle {
        implicitWidth: 30
        implicitHeight: parent.height
        anchors.right: parent.right
        radius: 10
        color: spin.up.pressed ? Theme.accent
             : spin.up.hovered ? (spin.overlay ? Theme.overlayFillHover : Theme.hoverFill) : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.dur(80) } }

        AppIcon {
            anchors.centerIn: parent
            name: "plus"
            size: 15
            color: spin.up.pressed ? Theme.accentInk : spin.overlay ? Theme.onOverlayMuted : Theme.textSecondary
        }
    }

    background: FieldBackground {
        focused: spin.activeFocus
        hovered: spin.hovered
        FocusRing { shown: spin.visualFocus; radius: parent.radius + 3 }
    }
}
