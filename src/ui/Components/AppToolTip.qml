import QtQuick
import QtQuick.Controls

ToolTip {
    id: tip
    delay: 450
    padding: 7

    contentItem: Text {
        text: tip.text
        color: Theme.textPrimary
        font.family: Ui.fontFamily
        font.pixelSize: Ui.sp(16)
    }

    background: Rectangle {
        radius: 7
        color: Theme.surfaceAlt
        border.color: Theme.border
        border.width: 1
    }
}
