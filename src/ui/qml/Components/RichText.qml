pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import ".."

TextEdit {
    id: richText
    ContextMenu.onRequested: (position) => { textMenu.active = true; (textMenu.item as TextMenu).popup(position) }
    Loader { id: textMenu; active: false; sourceComponent: TextMenu { target: richText } }
    readOnly: true
    selectByMouse: true
    textFormat: TextEdit.RichText
    wrapMode: TextEdit.Wrap
    color: Theme.textSecondary
    font.pixelSize: Globals.sp(Theme.bodySize)

    onLinkActivated: (link) => Qt.openUrlExternally(link)
    HoverHandler {
        enabled: parent.hoveredLink.length > 0
        cursorShape: Qt.PointingHandCursor
    }
}
