import QtQuick
import QtQuick.Controls
import ".."

CheckBox {
    id: control

    property int fontSize: Theme.bodySize
    readonly property int boxSize: Globals.sp(22)
    readonly property bool marked: checkState !== Qt.Unchecked
    readonly property bool hasText: text.length > 0

    implicitWidth: boxSize + (hasText ? spacing + Math.ceil(label.implicitWidth) : 0) + leftPadding + rightPadding
    implicitHeight: Math.max(boxSize, hasText ? label.implicitHeight : 0) + 8
    leftPadding: 0
    rightPadding: 0
    spacing: hasText ? 10 : 0
    hoverEnabled: true

    indicator: Rectangle {
        implicitWidth: control.boxSize
        implicitHeight: control.boxSize
        // Alone it centres; beside a label it leads the row.
        x: control.hasText ? control.leftPadding : (control.width - width) / 2
        anchors.verticalCenter: parent.verticalCenter
        radius: 6
        color: control.marked ? Theme.accent : Theme.surface
        border.color: control.marked ? Qt.lighter(Theme.accent, 1.2)
                    : control.hovered ? Theme.textMuted : Theme.border
        border.width: 1
        opacity: control.enabled ? 1 : 0.5

        Behavior on color { ColorAnimation { duration: 140 } }
        Behavior on border.color { ColorAnimation { duration: 140 } }

        AppIcon {
            anchors.centerIn: parent
            name: control.checkState === Qt.PartiallyChecked ? "minus" : "check"
            size: Math.round(parent.height * 0.62)
            color: Theme.onAccent
            visible: control.marked
            opacity: control.marked ? 1.0 : 0.0
            scale: control.marked ? 1.0 : 0.5
            Behavior on opacity { NumberAnimation { duration: 120 } }
            Behavior on scale { NumberAnimation { duration: 160; easing.type: Easing.OutBack } }
        }
    }

    contentItem: Text {
        id: label
        visible: control.hasText
        leftPadding: control.hasText ? control.boxSize + control.spacing : 0
        text: control.text
        color: control.enabled ? (control.hovered ? Theme.textPrimary : Theme.textSecondary) : Theme.textDisabled
        font.family: Globals.fontFamily
        font.pixelSize: Globals.sp(control.fontSize)
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    HoverHandler { cursorShape: control.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
}
