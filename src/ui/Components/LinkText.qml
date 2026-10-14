import QtQuick

Text {
    id: link
    signal clicked()
    readonly property bool hovered: hover.hovered

    color: hovered ? Theme.textPrimary : Theme.textMuted
    font.pixelSize: Ui.sp(Theme.compactSize)
    font.underline: hovered
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.onPressAction: link.clicked()
    Keys.onPressed: event => {
        if (event.isAutoRepeat) return
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
            event.accepted = true
            link.clicked()
        }
    }
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: link.clicked() }
    FocusRing { shown: link.activeFocus; radius: 4 }
}
