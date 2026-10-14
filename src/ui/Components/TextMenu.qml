import QtQuick
import QtQuick.Controls

AppMenu {
    id: textMenu
    required property var target

    // Focus stays in the text, so its selection survives and fields that commit on losing focus are left alone.
    focus: false

    readonly property bool editable: !target.readOnly
    readonly property bool masked: target.echoMode !== undefined && target.echoMode !== TextInput.Normal
    readonly property bool selected: target.selectedText.length > 0
    readonly property string query: target.selectedText.trim().replace(/\s+/g, " ")

    component Separator: MenuSeparator {
        property bool shown: true
        visible: shown
        height: shown ? implicitHeight : 0
        topPadding: 4
        bottomPadding: 4
        leftPadding: 8
        rightPadding: 8
        contentItem: Rectangle { implicitHeight: 1; color: Theme.border }
        background: null
    }

    Action {
        property bool shown: textMenu.editable
        property string hint: "Ctrl+Z"
        text: qsTr("Undo")
        enabled: shown && textMenu.target.canUndo
        onTriggered: textMenu.target.undo()
    }
    Action {
        property bool shown: textMenu.editable
        property string hint: "Ctrl+Y"
        text: qsTr("Redo")
        enabled: shown && textMenu.target.canRedo
        onTriggered: textMenu.target.redo()
    }
    Separator { shown: textMenu.editable }
    Action {
        property bool shown: textMenu.editable
        property string hint: "Ctrl+X"
        text: qsTr("Cut")
        enabled: shown && textMenu.selected && !textMenu.masked
        onTriggered: textMenu.target.cut()
    }
    Action {
        property bool shown: !textMenu.masked
        property string hint: "Ctrl+C"
        text: qsTr("Copy")
        enabled: shown && textMenu.selected
        onTriggered: textMenu.target.copy()
    }
    Action {
        property bool shown: textMenu.editable
        property string hint: "Ctrl+V"
        text: qsTr("Paste")
        enabled: shown && textMenu.target.canPaste
        onTriggered: textMenu.target.paste()
    }
    Action {
        property bool shown: textMenu.editable
        property string hint: "Del"
        text: qsTr("Delete")
        enabled: shown && textMenu.selected
        onTriggered: textMenu.target.remove(textMenu.target.selectionStart, textMenu.target.selectionEnd)
    }
    Separator {}
    Action {
        property string hint: "Ctrl+A"
        text: qsTr("Select all")
        enabled: textMenu.target.length > 0
        onTriggered: textMenu.target.selectAll()
    }
    Action {
        property bool shown: textMenu.editable
        text: qsTr("Clear")
        enabled: shown && textMenu.target.length > 0
        onTriggered: textMenu.target.remove(0, textMenu.target.length)
    }
    Separator { shown: !textMenu.masked && textMenu.query.length > 0 }
    Action {
        property bool shown: !textMenu.masked && textMenu.query.length > 0
        text: qsTr("Search providers for “%1”")
              .arg(textMenu.query.length > 28 ? textMenu.query.slice(0, 27) + "…" : textMenu.query)
        enabled: shown
        onTriggered: Ui.searchProviders(textMenu.query)
    }
}
