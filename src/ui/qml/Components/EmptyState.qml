import QtQuick
import QtQuick.Layouts
import ".."

// What a page would show, and the way to get some.
ColumnLayout {
    id: root

    property string icon: ""
    property string title: ""
    property string hint: ""
    property string actionText: ""
    property color titleColor: Theme.textSecondary
    property color hintColor: Theme.textMuted
    signal actionTriggered()

    spacing: Globals.sp(10)

    AppIcon {
        Layout.alignment: Qt.AlignHCenter
        visible: root.icon !== ""
        name: root.icon
        size: Globals.sp(44)
        color: root.hintColor
    }
    Text {
        Layout.alignment: Qt.AlignHCenter
        text: root.title
        color: root.titleColor
        font.pixelSize: Globals.sp(22)
    }
    Text {
        Layout.alignment: Qt.AlignHCenter
        Layout.maximumWidth: Globals.sp(560)
        visible: root.hint !== ""
        text: root.hint
        color: root.hintColor
        font.pixelSize: Globals.sp(Theme.bodySize)
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
    }
    AppButton {
        Layout.alignment: Qt.AlignHCenter
        Layout.topMargin: Globals.sp(6)
        visible: root.actionText !== ""
        text: root.actionText
        onClicked: root.actionTriggered()
    }
}
