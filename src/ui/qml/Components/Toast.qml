import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

// A confirmation that gets out of the way: no focus, no scrim, gone after a few seconds or a click.
Popup {
    id: toast

    // Undo on offer: `undo` puts it back; `commit`, if given, is what goes ahead when the offer lapses.
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
        // A new toast ends an earlier offer.
        settle()
        headerText.text = header
        messageText.text = message
        // Long enough to read: a base, plus time per character, capped.
        hideTimer.interval = Math.min(9000, 2500 + message.length * 45)
        hideTimer.restart()
        open()
    }

    modal: false
    focus: false
    closePolicy: Popup.NoAutoClose
    x: Math.round((parent.width - width) / 2)
    y: parent.height - height - Globals.sp(28)
    width: Math.min(Globals.sp(560), parent.width - Globals.sp(48))
    padding: Globals.sp(14)

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
            onClicked: toast.close()
        }
    }

    contentItem: ColumnLayout {
        spacing: Globals.sp(4)
        Text {
            id: headerText
            Layout.fillWidth: true
            visible: text !== ""
            color: Theme.textAccent
            font.pixelSize: Globals.sp(Theme.bodySize)
            font.bold: true
            elide: Text.ElideRight
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Globals.sp(12)
            Text {
                id: messageText
                Layout.fillWidth: true
                color: Theme.textPrimary
                font.pixelSize: Globals.sp(Theme.bodySize)
                wrapMode: Text.Wrap
            }
            AppButton {
                visible: toast.undoAction !== null
                text: qsTr("Undo")
                fontSize: Theme.bodySize
                radius: 8
                Layout.preferredHeight: Globals.sp(32)
                onClicked: {
                    const undo = toast.undoAction
                    toast.undoAction = null
                    toast.commitAction = null
                    undo()
                    toast.close()
                }
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
        AppShell.notificationClosed()
    }
}
