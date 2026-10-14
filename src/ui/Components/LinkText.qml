import QtQuick

Text {
    id: link
    signal clicked()

    color: hover.hovered ? Theme.textPrimary : Theme.textMuted
    font.pixelSize: Ui.sp(Theme.compactSize)
    font.underline: hover.hovered
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: link.clicked() }
}
