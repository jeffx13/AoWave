import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import ".."

Popup {
    id: notifier

    signal logsRequested()

    function show(message, header) {
        notifierMessage.text = message
        headerText.text = header
        open()
    }

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 20
    width: Math.min(520, Math.round(parent.width * 0.9))
    implicitHeight: column.implicitHeight + padding * 2
    anchors.centerIn: parent

    Overlay.modal: Rectangle { color: Theme.scrim }

    background: Item {
        implicitWidth: 400
        implicitHeight: 220
        Card {
            id: bgCard
            anchors.fill: parent
            radius: 14
            border.color: Qt.alpha(Theme.accent, 0.2)
        }
        MultiEffect {
            anchors.fill: bgCard
            source: bgCard
            shadowEnabled: true
            shadowBlur: 24 / blurMax
            shadowVerticalOffset: 12
            shadowColor: Theme.scrim
        }
    }

    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 160; easing.type: Easing.OutCubic }
            NumberAnimation { property: "scale"; from: 0.94; to: 1.0; duration: 180; easing.type: Easing.OutBack }
        }
    }
    exit: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 140; easing.type: Easing.InCubic }
            NumberAnimation { property: "scale"; from: 1.0; to: 0.96; duration: 140; easing.type: Easing.InCubic }
        }
    }

    contentItem: ColumnLayout {
        id: column
        spacing: 14

        Text {
            id: headerText
            text: qsTr("Error")
            color: Theme.danger
            font { pixelSize: Globals.sp(24); bold: true }
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        ScrollView {
            id: messageScroll
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(220, Math.max(80, implicitHeight))
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            Text {
                id: notifierMessage
                // A ScrollView gives its content no width, so without this nothing wraps.
                width: messageScroll.availableWidth
                text: qsTr("An error has occurred.")
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                opacity: 0.9
                font.pixelSize: Globals.sp(Theme.bodySize)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            AppButton {
                text: qsTr("View Logs")
                secondary: true
                fontSize: Theme.bodySize
                onClicked: { notifier.close(); notifier.logsRequested() }
            }

            Item { Layout.fillWidth: true }

            AppButton {
                id: okButton
                text: qsTr("OK")
                fontSize: Theme.bodySize
                onClicked: notifier.close()
            }
        }
    }

    onOpened: okButton.forceActiveFocus()
    onClosed: AppShell.notificationClosed()
}
