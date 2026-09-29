pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import ".."

Item {
    id: overlay
    visible: loading

    signal cancelled()
    property bool cancellable: false   // opt in, so a call site without onCancelled cannot show a dead button
    property bool loading: false
    property bool cancelling: false

    // Scrim and label flip together.
    Rectangle {
        anchors.fill: parent
        color: Theme.isLight ? "#FFFFFF" : "#000000"
        opacity: Theme.isLight ? 0.55 : 0.3
    }

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 6

        WaveLoader {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: Globals.sp(150)
            Layout.preferredHeight: Globals.sp(96)
            running: overlay.loading
        }

        Text {
            Layout.alignment: Qt.AlignHCenter
            text: (overlay.cancelling ? qsTr("Cancelling") : qsTr("Loading")) + dots.trail
            color: Theme.textPrimary
            font.pixelSize: Globals.sp(Theme.bodySize)
            font.weight: Font.Medium
        }

        AppButton {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 6
            text: qsTr("Cancel")
            secondary: true
            visible: overlay.cancellable
            onClicked: {
                overlay.cancelling = true
                overlay.cancelled()
            }
        }
    }

    Timer {
        id: dots
        property int count: 0
        readonly property string trail: ".".repeat(count)
        interval: 420
        repeat: true
        running: overlay.loading
        onTriggered: count = (count + 1) % 4
    }

    onLoadingChanged: if (loading) { cancelling = false; dots.count = 0 }
}
