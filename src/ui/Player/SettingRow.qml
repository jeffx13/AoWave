import QtQuick
import QtQuick.Layouts

Rectangle {
    id: row
    property alias label: rowLabel.text
    property alias sublabel: rowSub.text
    property alias labelColor: rowLabel.color
    default property alias content: rowSlot.data

    Layout.fillWidth: true
    implicitHeight: Math.max(48, rowContent.implicitHeight + 16)
    radius: 10
    color: rowHover.hovered ? Theme.overlayLine : "transparent"
    Behavior on color { ColorAnimation { duration: 100 } }

    HoverHandler { id: rowHover }

    RowLayout {
        id: rowContent
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
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
                HoverHandler { id: subHover }
                AppToolTip { text: rowSub.text; visible: subHover.hovered && rowSub.truncated }
            }
        }

        Item {
            id: rowSlot
            Layout.preferredWidth: childrenRect.width
            Layout.preferredHeight: 0
            Layout.fillHeight: true
        }
    }
}
