import QtQuick

Row {
    id: mini

    property alias label: miniLabel.text
    property alias checked: miniSwitch.checked
    signal toggled()

    spacing: 6

    Text {
        id: miniLabel
        color: Theme.textMuted
        font.pixelSize: Ui.sp(17)
        anchors.verticalCenter: parent.verticalCenter
    }
    AppSwitch {
        id: miniSwitch
        onToggled: mini.toggled()
    }
}
