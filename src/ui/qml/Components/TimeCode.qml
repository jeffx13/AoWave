pragma ComponentBehavior: Bound
import QtQuick
import ".."

// Each digit sits in a cell as wide as the widest one, so a running clock does not jitter.
Row {
    id: root

    property string text: ""
    property color color: Theme.textPrimary
    property real pixelSize: Globals.sp(Theme.bodySize)
    property int weight: Font.Normal

    FontMetrics {
        id: metrics
        font.family: Globals.fontFamily
        font.pixelSize: root.pixelSize
        font.weight: root.weight
    }
    readonly property real digitWidth: Math.max(...Array.from("0123456789", (d) => metrics.advanceWidth(d)))

    // Counting characters, not the string: the delegates survive every tick.
    Repeater {
        model: root.text.length
        Text {
            required property int index
            readonly property string character: root.text.charAt(index)
            width: character >= "0" && character <= "9" ? root.digitWidth : implicitWidth
            horizontalAlignment: Text.AlignHCenter
            text: character
            color: root.color
            font.family: Globals.fontFamily
            font.pixelSize: root.pixelSize
            font.weight: root.weight
        }
    }
}
