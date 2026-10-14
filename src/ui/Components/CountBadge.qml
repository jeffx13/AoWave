import QtQuick

// counts are good news (new episodes, active downloads): accent, one pulse when the count rises
Rectangle {
    id: badge

    property int count: 0
    property int cap: 99
    property color tint: Theme.accent
    // a reused delegate changes key and count together: only a rise for the same key pulses
    property string key: ""
    property int seen: 0
    property string seenKey: ""

    visible: count > 0
    implicitHeight: Ui.sp(15)
    implicitWidth: Math.max(height, countText.implicitWidth + Math.round(height * 0.5))
    radius: height / 2
    color: tint
    Accessible.role: Accessible.StaticText
    Accessible.name: countText.text

    onCountChanged: Qt.callLater(settle)
    onKeyChanged: Qt.callLater(settle)
    function settle() {
        if (key === seenKey && count > seen && !Theme.reduceMotion) pulse.restart()
        seen = count
        seenKey = key
    }

    Text {
        id: countText
        anchors.centerIn: parent
        text: badge.count > badge.cap ? badge.cap + "+" : badge.count
        color: Theme.inkOn(badge.tint)
        font.pixelSize: Math.max(9, Math.round(badge.height * 0.66))
        font.bold: true
    }

    SequentialAnimation {
        id: pulse
        NumberAnimation { target: badge; property: "scale"; to: 1.25; duration: Theme.dur(120); easing.type: Easing.OutCubic }
        NumberAnimation { target: badge; property: "scale"; to: 1.0; duration: Theme.dur(200); easing.type: Easing.OutCubic }
    }
}
