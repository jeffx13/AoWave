import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: toast

    property var undoAction: null
    property var commitAction: null
    function offerUndo(message, undo, commit) {
        show(message, "")
        undoAction = undo
        commitAction = commit || null
        hideTimer.interval = 6000
        hideTimer.restart()
    }
    function settle() {
        const commit = commitAction
        undoAction = null
        commitAction = null
        if (commit) commit()
    }

    function show(message, header) {
        settle()
        headerText.text = header
        messageText.text = message
        hideTimer.interval = Math.min(9000, 2500 + message.length * 45)
        hideTimer.restart()
        open()
        body.Accessible.announce(body.Accessible.name)
    }

    modal: false
    focus: false
    closePolicy: Popup.NoAutoClose
    x: Math.round((parent.width - width) / 2)
    y: parent.height - height - Ui.sp(28)
    width: Math.min(Ui.sp(560), parent.width - Ui.sp(48))
    padding: Ui.sp(14)

    Timer {
        id: hideTimer
        onTriggered: toast.close()
    }

    background: Rectangle {
        radius: 12
        color: Theme.surfaceAlt
        border.color: Qt.alpha(Theme.accent, 0.35)
        border.width: 1
        // No click-to-dismiss while an Undo is on offer: closing settles the action, committing what Undo would take back.
        MouseArea {
            anchors.fill: parent
            onClicked: if (toast.undoAction === null) toast.close()
        }
        HoverHandler {
            onHoveredChanged: hovered ? hideTimer.stop() : (toast.opened && hideTimer.restart())
        }
    }

    function undo() {
        const undo = toast.undoAction
        if (undo === null) return
        toast.undoAction = null
        toast.commitAction = null
        undo()
        toast.close()
    }

    contentItem: ColumnLayout {
        id: body
        spacing: Ui.sp(4)
        Accessible.role: Accessible.AlertMessage
        Accessible.name: headerText.text === "" ? messageText.text : headerText.text + ". " + messageText.text
        Text {
            id: headerText
            Layout.fillWidth: true
            visible: text !== ""
            color: Theme.textAccent
            font.pixelSize: Ui.sp(Theme.bodySize)
            font.bold: true
            elide: Text.ElideRight
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Ui.sp(12)
            Text {
                id: messageText
                Layout.fillWidth: true
                color: Theme.textPrimary
                font.pixelSize: Ui.sp(Theme.bodySize)
                wrapMode: Text.Wrap
            }
            AppButton {
                visible: toast.undoAction !== null
                text: qsTr("Undo")
                fontSize: Theme.bodySize
                radius: 8
                Layout.preferredHeight: Ui.sp(32)
                onClicked: toast.undo()
            }
        }
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.reduceMotion ? 0 : 160 }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.reduceMotion ? 0 : 200 }
    }

    onClosed: {
        settle()
        MainWindow.notificationClosed()
    }
}
