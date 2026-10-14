import QtQuick
import QtQuick.Controls

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
    property bool alwaysScroll: true
    property bool active: false
    readonly property bool scrolling: isOverflow && !Theme.reduceMotion && (alwaysScroll || hover.hovered || active)

    clip: true
    implicitHeight: Math.max(staticText.implicitHeight, primaryText.implicitHeight)
    visible: text.length > 0

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
        width: marqueeText.width
        text: marqueeText.text
        textFormat: marqueeText.textFormat
        font.family: Ui.fontFamily
        font.pixelSize: Ui.sp(marqueeText.fontSize)
        font.weight: marqueeText.fontWeight
        color: marqueeText.color
        elide: Text.ElideRight
        wrapMode: Text.NoWrap
        horizontalAlignment: marqueeText.horizontalAlignment
        anchors.verticalCenter: parent.verticalCenter
        visible: !marqueeText.scrolling
    }

    Text {
        id: primaryText
        text: marqueeText.text
        textFormat: marqueeText.textFormat
        font.family: Ui.fontFamily
        font.pixelSize: Ui.sp(marqueeText.fontSize)
        font.weight: marqueeText.fontWeight
        color: marqueeText.color
        elide: Text.ElideNone
        wrapMode: Text.NoWrap
        anchors.verticalCenter: parent.verticalCenter
        x: 0
        visible: marqueeText.scrolling
        onPaintedWidthChanged: if (scrollAnim.running && !scrollAnim.paused) scrollAnim.restart()
        onTextChanged: {
            x = 0
            if (scrollAnim.running) scrollAnim.restart()
        }
    }

    Text {
        text: primaryText.text
        textFormat: primaryText.textFormat
        font.family: Ui.fontFamily
        font.pixelSize: primaryText.font.pixelSize
        font.weight: primaryText.font.weight
        color: primaryText.color
        wrapMode: Text.NoWrap
        anchors.verticalCenter: parent.verticalCenter
        x: primaryText.x + primaryText.paintedWidth + marqueeText.spacing
        visible: marqueeText.scrolling
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
        running: marqueeText.scrolling && marqueeText.visible
        onRunningChanged: {
            if (!running) primaryText.x = 0
            marqueeText.updatePaused(marqueeText.alwaysScroll && hover.hovered)
        }
    }

    HoverHandler {
        id: hover
        onHoveredChanged: marqueeText.updatePaused(marqueeText.alwaysScroll && hover.hovered)
    }
    ToolTip.visible: hover.hovered && isOverflow && !alwaysScroll
    ToolTip.text: text
    ToolTip.delay: 600
}

