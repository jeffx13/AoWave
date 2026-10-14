import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Flickable {
    id: scroll
    default property alias content: column.data
    property alias spacing: column.spacing

    ScrollBar.vertical: AppScrollBar { barOpacity: 0.4 }
    anchors { fill: parent; margins: 12 }
    contentHeight: column.implicitHeight
    clip: true
    boundsBehavior: Flickable.StopAtBounds

    ColumnLayout {
        id: column
        width: scroll.width
        spacing: 6
    }
}
