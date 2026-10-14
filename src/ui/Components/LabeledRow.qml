import QtQuick
import QtQuick.Layouts

RowLayout {
    id: row

    property string label: ""
    property string sublabel: ""
    property string reason: ""

    Layout.fillWidth: true
    spacing: 8

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 0

        Text {
            text: row.label
            color: Theme.textSecondary
            font.pixelSize: Ui.sp(Theme.bodySize)
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        Text {
            id: sublabelText
            text: row.sublabel
            visible: row.sublabel !== ""
            color: Theme.textMuted
            font.pixelSize: Ui.sp(15)
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
            Layout.fillWidth: true

            HoverHandler { id: sublabelHover }
            AppToolTip { text: row.sublabel; visible: sublabelHover.hovered && sublabelText.truncated }
        }
        Text {
            text: row.reason
            visible: !row.enabled && row.reason !== ""
            color: Theme.textMuted
            font.pixelSize: Ui.sp(15)
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
    }
}
