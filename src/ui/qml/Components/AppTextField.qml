pragma ComponentBehavior: Bound
import QtQuick.Controls
import QtQuick
import ".."

TextField {
    id: field

    property color checkedColor: Theme.border
    property int fontSize: Theme.bodySize
    property bool showClearButton: false
    // Masked until the eye button reveals it.
    property bool secret: false
    property bool revealed: false

    font.family: Globals.fontFamily
    font.pixelSize: Globals.sp(fontSize)
    color: Theme.textPrimary
    placeholderTextColor: Theme.textMuted
    selectionColor: Theme.accent
    selectedTextColor: Theme.onAccent
    hoverEnabled: true
    echoMode: secret && !revealed ? TextInput.Password : TextInput.Normal
    leftPadding: 14
    rightPadding: trailing.visible ? trailing.width + 8 : 14
    implicitHeight: Globals.controlHeight
    // TextField's own implicitWidth ignores the placeholder.
    implicitWidth: Math.max(160, placeholderMetrics.advanceWidth + leftPadding + rightPadding + 4)

    // Built on the first right click, in place of Qt's unstyled menu.
    ContextMenu.menu: null
    ContextMenu.onRequested: (position) => { textMenu.active = true; (textMenu.item as TextMenu).popup(position) }
    Loader { id: textMenu; active: false; sourceComponent: TextMenu { target: field } }

    TextMetrics {
        id: placeholderMetrics
        font: field.font
        text: field.placeholderText
    }

    // Esc unfocuses and is consumed here; Enter commits through accepted/editingFinished.
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Escape) {
            event.accepted = true
            field.focus = false
        }
    }
    onAccepted: Qt.callLater(() => field.focus = false)
    onActiveFocusChanged: if (!activeFocus) field.unfocused()
    signal unfocused()

    background: FieldBackground {
        focused: field.activeFocus
        hovered: field.hovered
        restingBorder: field.checkedColor

        Rectangle {
            visible: field.activeFocus
            anchors {
                fill: parent
                margins: -3
            }
            radius: parent.radius + 3
            color: "transparent"
            border.color: Theme.focusRing
            border.width: 2
            opacity: field.activeFocus ? 1.0 : 0.0
            Behavior on opacity {
                NumberAnimation { duration: 200 }
            }
        }
    }

    Row {
        id: trailing
        // The buttons' own conditions: their `visible` reads false while this row is hidden, so
        // a field that started empty never showed its clear button.
        visible: field.secret || (field.showClearButton && field.text.length > 0)
        spacing: 2
        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter

        IconButton {
            id: eyeBtn
            visible: field.secret
            implicitWidth: 26
            implicitHeight: 26
            iconName: field.revealed ? "eye-off" : "eye"
            iconSize: 15
            boxRadius: 7
            tip: field.revealed ? qsTr("Hide") : qsTr("Show")
            onClicked: field.revealed = !field.revealed
        }

        IconButton {
            id: clearBtn
            visible: field.showClearButton && field.text.length > 0
            implicitWidth: 26
            implicitHeight: 26
            iconName: "x"
            iconSize: 14
            boxRadius: 7
            destructive: true
            tip: qsTr("Clear")
            onClicked: field.clear()
        }
    }

    MouseArea {
        anchors.fill: parent
        anchors.rightMargin: trailing.visible ? trailing.width + 6 : 0
        cursorShape: Qt.IBeamCursor
        acceptedButtons: Qt.NoButton
    }
}
