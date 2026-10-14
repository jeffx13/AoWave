import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import App

Popup {
    id: notifier

    signal logsRequested()

    property var errors: []
    property int current: 0
    readonly property var shown: errors.length > 0 ? errors[Math.min(current, errors.length - 1)] : ({ header: "", message: "" })

    property bool closing: false
    onAboutToHide: closing = true
    onAboutToShow: closing = false
    function show(message, header) {
        if (closing || !visible) {
            errors = []
            current = 0
        }
        if (errors.some(e => e.header === header && e.message === message)) return
        errors = errors.concat([{ header: header, message: message }]).slice(-20)
        if (!visible || closing) {
            open()
            closing = false
        }
        column.Accessible.announce(column.Accessible.name)
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
        Accessible.role: Accessible.AlertMessage
        Accessible.name: headerText.text + ". " + notifierMessage.text

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Text {
                id: headerText
                text: notifier.shown.header || qsTr("Error")
                color: Theme.danger
                font { pixelSize: Ui.sp(24); bold: true }
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            IconButton {
                visible: notifier.errors.length > 1
                iconName: "chevron-left"
                iconSize: 18
                tip: qsTr("Previous")
                enabled: notifier.current > 0
                implicitWidth: Ui.sp(32)
                implicitHeight: Ui.sp(32)
                onClicked: notifier.current -= 1
            }
            Text {
                visible: notifier.errors.length > 1
                text: qsTr("%1 of %2").arg(notifier.current + 1).arg(notifier.errors.length)
                color: Theme.textMuted
                font.pixelSize: Ui.sp(Theme.compactSize)
            }
            IconButton {
                visible: notifier.errors.length > 1
                iconName: "chevron-right"
                iconSize: 18
                tip: qsTr("Next")
                enabled: notifier.current < notifier.errors.length - 1
                implicitWidth: Ui.sp(32)
                implicitHeight: Ui.sp(32)
                onClicked: notifier.current += 1
            }
        }

        ScrollView {
            id: messageScroll
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(220, Math.max(80, implicitHeight))
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical: AppScrollBar {}

            Text {
                id: notifierMessage
                width: messageScroll.availableWidth
                text: notifier.shown.message || qsTr("An error has occurred.")
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                opacity: 0.9
                font.pixelSize: Ui.sp(Theme.bodySize)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            AppButton {
                text: qsTr("View logs")
                secondary: true
                fontSize: Theme.bodySize
                onClicked: { notifier.close(); notifier.logsRequested() }
            }

            AppButton {
                text: qsTr("Copy")
                secondary: true
                fontSize: Theme.bodySize
                onClicked: App.copyToClipboard(headerText.text + "\n" + notifierMessage.text)
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
    onClosed: {
        errors = []
        current = 0
        MainWindow.notificationClosed()
    }
}
