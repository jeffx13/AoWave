import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: toast

    property var undoAction: null
    property var commitAction: null
    property string actionText: ""
    property var action: null
    property var waiting: []

    function offerUndo(message, undo, commit) {
        present({ message: message, header: "", undo: undo, commit: commit || null })
    }
    function offerAction(message, header, text, action) {
        present({ message: message, header: header, actionText: text, action: action })
    }
    function show(message, header) {
        present({ message: message, header: header })
    }
    function settle() {
        const commit = commitAction
        undoAction = null
        commitAction = null
        if (commit) commit()
    }

    function present(entry) {
        if (undoAction !== null && !entry.undo) {
            waiting.push(entry)
            return
        }
        settle()
        headerText.text = entry.header || ""
        messageText.text = entry.message
        undoAction = entry.undo || null
        commitAction = entry.commit || null
        actionText = entry.actionText || ""
        action = entry.action || null
        hideTimer.interval = undoAction !== null ? 6000
                           : Math.min(9000, (action !== null ? 4500 : 2500) + entry.message.length * 45)
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

    Connections {
        target: Qt.application
        function onAboutToQuit() { toast.settle() }
    }
    Timer {
        id: hideTimer
        onTriggered: toast.close()
    }

    background: Rectangle {
        radius: 12
        color: Theme.surfaceAlt
        border.color: Qt.alpha(Theme.accent, 0.35)
        border.width: 1
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

    function act() {
        const action = toast.action
        toast.action = null
        toast.close()
        if (action) action()
    }

    contentItem: RowLayout {
        id: body
        spacing: Ui.sp(12)
        Accessible.role: Accessible.AlertMessage
        Accessible.name: headerText.text === "" ? messageText.text : headerText.text + ". " + messageText.text
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Ui.sp(4)
            Text {
                id: headerText
                Layout.fillWidth: true
                visible: text !== ""
                color: Theme.textAccent
                font.pixelSize: Ui.sp(Theme.bodySize)
                font.bold: true
                elide: Text.ElideRight
            }
            Text {
                id: messageText
                Layout.fillWidth: true
                color: Theme.textPrimary
                font.pixelSize: Ui.sp(Theme.bodySize)
                wrapMode: Text.Wrap
            }
        }
        AppButton {
            visible: toast.undoAction !== null || toast.action !== null
            text: toast.undoAction !== null ? qsTr("Undo") : toast.actionText
            fontSize: Theme.bodySize
            radius: 8
            Layout.preferredHeight: Ui.sp(32)
            onClicked: toast.undoAction !== null ? toast.undo() : toast.act()
        }
        IconButton {
            iconName: "x"
            iconSize: 16
            boxRadius: 8
            tip: qsTr("Close")
            implicitWidth: Ui.sp(30)
            implicitHeight: Ui.sp(30)
            onClicked: toast.close()
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
        if (waiting.length > 0) Qt.callLater(() => { if (!toast.opened && toast.waiting.length > 0) toast.present(toast.waiting.shift()) })
    }
}
