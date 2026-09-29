import QtQuick
import ".."

Item {
    id: marqueeText
    property string text: ""
    property color color: Theme.textSecondary
    property int fontSize: Theme.bodySize
    property int spacing: 30
    property real marqueeSpeed: 80
    readonly property bool isOverflow: primaryText.paintedWidth > Math.ceil(marqueeText.width) + 1
    property int horizontalAlignment: Text.AlignHCenter
    property int textFormat: Text.PlainText
    property int fontWeight: Font.Normal
    readonly property real contentWidth: primaryText.paintedWidth

    clip: true
    implicitHeight: Math.max(staticText.implicitHeight, primaryText.implicitHeight)
    visible: text.length > 0

    // setPaused() warns when the animation is not running.
    function updatePaused(hovered) {
        if (scrollAnim.running && scrollAnim.paused !== hovered) scrollAnim.paused = hovered
    }

    onWidthChanged: {
        if (!scrollAnim.paused) {
            primaryText.x = 0
            if (scrollAnim.running) scrollAnim.restart()
        }
    }

    Text {
        id: staticText
        // Needs full width or horizontalAlignment has nothing to work against.
        width: marqueeText.width
        text: marqueeText.text
        textFormat: marqueeText.textFormat
        font.family: Globals.fontFamily
        font.pixelSize: Globals.sp(marqueeText.fontSize)
        font.weight: marqueeText.fontWeight
        color: marqueeText.color
        elide: Text.ElideRight
        wrapMode: Text.NoWrap
        horizontalAlignment: marqueeText.horizontalAlignment
        anchors.verticalCenter: parent.verticalCenter
        visible: !marqueeText.isOverflow || Theme.reduceMotion
    }

    Text {
        id: primaryText
        text: marqueeText.text
        textFormat: marqueeText.textFormat
        font.family: Globals.fontFamily
        font.pixelSize: Globals.sp(marqueeText.fontSize)
        font.weight: marqueeText.fontWeight
        color: marqueeText.color
        elide: Text.ElideNone
        wrapMode: Text.NoWrap
        anchors.verticalCenter: parent.verticalCenter
        x: 0
        visible: marqueeText.isOverflow && !Theme.reduceMotion
        onPaintedWidthChanged: if (scrollAnim.running && !scrollAnim.paused) scrollAnim.restart()
        onTextChanged: {
            x = 0
            if (scrollAnim.running) scrollAnim.restart()
        }
    }

    Text {
        text: primaryText.text
        textFormat: primaryText.textFormat
        font.family: Globals.fontFamily
        font.pixelSize: primaryText.font.pixelSize
        font.weight: primaryText.font.weight
        color: primaryText.color
        wrapMode: Text.NoWrap
        anchors.verticalCenter: parent.verticalCenter
        x: primaryText.x + primaryText.paintedWidth + marqueeText.spacing
        visible: marqueeText.isOverflow && !Theme.reduceMotion
    }

    NumberAnimation {
        id: scrollAnim
        target: primaryText
        property: "x"
        from: 0
        to: -(primaryText.paintedWidth + marqueeText.spacing)
        duration: ((primaryText.paintedWidth + marqueeText.spacing) / marqueeText.marqueeSpeed) * 1000
        easing.type: Easing.Linear
        loops: Animation.Infinite
        running: marqueeText.isOverflow && marqueeText.visible && !Theme.reduceMotion
        onRunningChanged: marqueeText.updatePaused(hover.hovered)
    }

    HoverHandler {
        id: hover
        onHoveredChanged: marqueeText.updatePaused(hover.hovered)
    }
}

