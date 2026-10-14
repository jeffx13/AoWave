import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App

AppPopup {
    id: notifier

    signal logsRequested()

    property var errors: []
    property int current: 0
    readonly property var shown: errors.length > 0 ? errors[Math.min(current, errors.length - 1)] : ({ header: "", message: "" })

    property bool closing: false
    onAboutToHide: closing = true
    onAboutToShow: closing = false
    // errors that came in picture-in-picture, shown once the window is back
    property bool held: false
    function show(message, header) {
        if ((closing || !visible) && !held) {
            errors = []
            current = 0
        }
        if (errors.some(e => e.header === header && e.message === message)) return
        errors = errors.concat([{ header: header, message: message }]).slice(-20)
        if (Ui.pipMode && !visible) {
            held = true
            if (Ui.mpv) Ui.mpv.showText(qsTr("%1. Leave picture-in-picture for details.").arg(header || qsTr("Error")), "info")
            return
        }
        if (!visible || closing) {
            open()
            closing = false
        }
        column.Accessible.announce(column.Accessible.name)
    }

    Connections {
        target: Ui
        function onPipModeChanged() {
            if (Ui.pipMode || !notifier.held) return
            notifier.held = false
            notifier.open()
        }
    }

    modal: true
    focus: true
    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    width: Math.min(520, parent ? parent.width - 60 : 520)
    padding: 18

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
                color: Theme.textDanger
                font.pixelSize: Ui.sp(Theme.bodySize)
                font.weight: Font.DemiBold
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
            Layout.preferredHeight: Math.min(220, implicitHeight)
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
                onClicked: { notifier.close(); notifier.logsRequested() }
            }

            AppButton {
                text: qsTr("Copy")
                secondary: true
                onClicked: App.copyToClipboard(headerText.text + "\n" + notifierMessage.text)
            }

            Item { Layout.fillWidth: true }

            AppButton {
                id: okButton
                text: qsTr("OK")
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
