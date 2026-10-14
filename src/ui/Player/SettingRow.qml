import QtQuick
import QtQuick.Layouts

Rectangle {
    id: row
    property alias label: rowLabel.text
    property alias sublabel: rowSub.text
    property alias labelColor: rowLabel.color
    default property alias content: rowSlot.data

    Layout.fillWidth: true
    implicitHeight: 48
    radius: 10
    color: rowHover.hovered ? Theme.overlayLine : "transparent"
    Behavior on color { ColorAnimation { duration: 100 } }

    HoverHandler { id: rowHover }

    RowLayout {
        anchors { fill: parent; leftMargin: 14; rightMargin: 14 }
        spacing: 10

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Text {
                id: rowLabel
                Layout.fillWidth: true
                color: Theme.onOverlayDim
                font.pixelSize: Ui.sp(Theme.bodySize)
                elide: Text.ElideRight
            }
            Text {
                id: rowSub
                Layout.fillWidth: true
                visible: text !== ""
                color: Theme.onOverlayMuted
                font.pixelSize: Ui.sp(14)
                elide: Text.ElideRight
            }
        }

        Item {
            id: rowSlot
            Layout.preferredWidth: childrenRect.width
            Layout.preferredHeight: parent.height
        }
    }
}
