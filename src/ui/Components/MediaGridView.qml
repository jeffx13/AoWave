pragma ComponentBehavior: Bound
import QtQuick
import App

GridView {
    id: grid

    property real imageAspectRatio: Ui.coverAspect
    property bool subtitles: false
    property bool rows: false
    readonly property real minCellWidth: (imageAspectRatio < 1 ? 300 : 190) * App.settings.cardSize / 100
    readonly property int columns: rows ? 1 : width > 0 ? Math.max(1, Math.floor(width / minCellWidth)) : 1
    property real spacing: 12

    cellWidth: Math.max(1, Math.floor(width / columns))
    cellHeight: rows ? Math.round(Ui.sp(Theme.bodySize) * 2 + 44)
                     : Math.max(1, (cellWidth - 14) * imageAspectRatio + Ui.sp(Theme.bodySize) * 3 + 12
                                       + (subtitles ? Ui.sp(Theme.metadataSize) * 1.4 : 0))

    reuseItems: true
    cacheBuffer: Math.round(cellHeight * 2)

    boundsBehavior: Flickable.StopAtBounds
    boundsMovement: Flickable.StopAtBounds
    clip: true

    currentIndex: -1
    highlightFollowsCurrentItem: true
    highlightMoveDuration: 130
    highlight: Rectangle {
        z: 2
        visible: grid.currentIndex >= 0
        color: "transparent"
        border.color: Theme.accent
        border.width: 2
        radius: 12
    }

    anchors.margins: spacing

    function moveCursor(key) {
        if (grid.count <= 0) return false
        const step = key === Qt.Key_Up || key === Qt.Key_Down ? grid.columns : 1
        const back = key === Qt.Key_Left || key === Qt.Key_Up
        if (!back && key !== Qt.Key_Right && key !== Qt.Key_Down) return false
        grid.currentIndex = grid.currentIndex < 0 ? 0
                          : back                  ? Math.max(0, grid.currentIndex - step)
                                                  : Math.min(grid.count - 1, grid.currentIndex + step)
        return true
    }
}
