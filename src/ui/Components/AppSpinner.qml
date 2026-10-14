pragma ComponentBehavior: Bound
import QtQuick

Item {
    id: spinner

    property color color: Theme.accent
    property real radius: 14
    property int duration: 1600
    property bool running: true

    implicitWidth: radius * 2 + 6
    implicitHeight: implicitWidth

    readonly property real size: Math.min(width, height)

    component Eddy: Canvas {
        id: eddy
        property int arms: 4
        property real turns: 1.15
        property real reach: 0.88
        property real weight: 0.05
        property int period: spinner.duration

        anchors.centerIn: parent
        antialiasing: true
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        Connections {
            target: spinner
            function onColorChanged() { eddy.requestPaint() }
        }

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const lw = Math.max(1, width * weight)
            const cx = width / 2, cy = height / 2
            const outer = Math.min(width, height) / 2 - lw
            const steps = 72
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            ctx.strokeStyle = spinner.color
            ctx.lineWidth = lw
            for (let k = 0; k < arms; ++k) {
                const phase = k * 2 * Math.PI / arms
                ctx.beginPath()
                for (let i = 0; i <= steps; ++i) {
                    const t = i / steps
                    const r = outer * (1 - reach * t)
                    const a = phase + t * turns * 2 * Math.PI
                    if (i === 0) ctx.moveTo(cx + r * Math.cos(a), cy + r * Math.sin(a))
                    else ctx.lineTo(cx + r * Math.cos(a), cy + r * Math.sin(a))
                }
                ctx.stroke()
            }
            ctx.globalCompositeOperation = "destination-in"
            const fade = ctx.createRadialGradient(cx, cy, 0, cx, cy, outer + lw)
            fade.addColorStop(0, "rgba(0, 0, 0, 1)")
            fade.addColorStop(0.15, "rgba(0, 0, 0, 1)")
            fade.addColorStop(1, "rgba(0, 0, 0, 0)")
            ctx.fillStyle = fade
            ctx.fillRect(0, 0, width, height)
        }

        NumberAnimation on rotation {
            from: 360
            to: 0
            duration: eddy.period
            loops: Animation.Infinite
            running: spinner.running && spinner.visible
        }
    }

    Eddy {
        width: spinner.size
        height: spinner.size
    }
    Eddy {
        width: spinner.size * 0.5
        height: spinner.size * 0.5
        arms: 3
        turns: 1
        reach: 0.8
        weight: 0.07
        period: spinner.duration * 0.55
    }
    Rectangle {
        anchors.centerIn: parent
        width: Math.max(2, spinner.size * 0.08)
        height: width
        radius: width / 2
        color: spinner.color
        SequentialAnimation on opacity {
            running: spinner.running && spinner.visible
            loops: Animation.Infinite
            NumberAnimation { from: 1; to: 0.4; duration: spinner.duration / 2; easing.type: Easing.InOutSine }
            NumberAnimation { from: 0.4; to: 1; duration: spinner.duration / 2; easing.type: Easing.InOutSine }
        }
    }
}
