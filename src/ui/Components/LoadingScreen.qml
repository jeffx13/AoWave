pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

Item {
    id: loadingScreen
    visible: loading

    signal cancelled()
    property bool cancellable: false
    property bool loading: false
    property bool cancelling: false

    Rectangle {
        anchors.fill: parent
        color: Theme.isLight ? Theme.surface : Theme.scrim
        opacity: Theme.isLight ? 0.55 : 0.5
    }
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onWheel: (wheel) => wheel.accepted = true
    }

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 6

        WaveLoader {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: Ui.sp(150)
            Layout.preferredHeight: Ui.sp(96)
            running: loadingScreen.loading
        }

        Text {
            Layout.alignment: Qt.AlignHCenter
            text: (loadingScreen.cancelling ? qsTr("Cancelling") : qsTr("Loading")) + dots.trail
            color: Theme.textPrimary
            font.pixelSize: Ui.sp(Theme.bodySize)
            font.weight: Font.Medium
        }

        AppButton {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 6
            text: qsTr("Cancel")
            secondary: true
            visible: loadingScreen.cancellable
            onClicked: loadingScreen.cancel()
        }
    }

    function cancel() {
        loadingScreen.cancelling = true
        loadingScreen.cancelled()
    }
    Shortcut {
        sequence: "Esc"
        enabled: loadingScreen.visible && loadingScreen.cancellable && !loadingScreen.cancelling
                 && !(loadingScreen.Window.activeFocusItem instanceof TextInput)
        onActivated: loadingScreen.cancel()
    }

    Timer {
        id: dots
        property int count: 0
        readonly property string trail: ".".repeat(count)
        interval: 420
        repeat: true
        running: loadingScreen.loading
        onTriggered: count = (count + 1) % 4
    }

    onLoadingChanged: if (loading) { cancelling = false; dots.count = 0 }
}
