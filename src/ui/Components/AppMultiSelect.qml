pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

Control {
    id: picker

    property var model: []
    property var selected: []
    property string placeholderText: qsTr("None")
    property int fontSize: Theme.bodySize
    signal toggled(string code, bool on)

    readonly property string summary: {
        const names = []
        for (const entry of model) if (selected.indexOf(entry.code) >= 0) names.push(entry.name)
        return names.join(", ")
    }

    implicitWidth: Ui.sp(200)
    implicitHeight: Ui.controlHeight
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    Keys.onPressed: event => {
        if (event.key !== Qt.Key_Space && event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter
                && event.key !== Qt.Key_Down) return
        event.accepted = true
        if (!listPopup.visible) listPopup.open()
    }
    Accessible.role: Accessible.ComboBox
    Accessible.name: Ui.labelOf(picker) || placeholderText
    Accessible.description: summary
    font.family: Ui.fontFamily
    font.pixelSize: Ui.sp(fontSize)

    background: FieldBackground {
        focused: listPopup.visible
        hovered: picker.hovered
        FocusRing { shown: picker.visualFocus; radius: parent.radius + 3 }
    }

    contentItem: Text {
        leftPadding: 12
        rightPadding: 30
        text: picker.summary.length > 0 ? picker.summary : picker.placeholderText
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: Text.AlignHCenter
        font: picker.font
        color: picker.summary.length > 0 ? Theme.textPrimary : Theme.textMuted
    }

    AppIcon {
        x: picker.width - width - 12
        anchors.verticalCenter: parent.verticalCenter
        name: "chevron-down"
        size: Ui.sp(14)
        color: listPopup.visible ? Theme.textAccent : Theme.textMuted
        rotation: listPopup.visible ? 180 : 0
        Behavior on rotation { NumberAnimation { duration: Theme.dur(150); easing.type: Easing.OutCubic } }
    }

    TapHandler {
        onTapped: listPopup.visible ? listPopup.close() : listPopup.open()
    }

    Popup {
        id: listPopup
        parent: picker
        x: 0
        y: picker.height + 4
        width: picker.width
        implicitHeight: Math.min(contentItem.implicitHeight + padding * 2, Math.max(120, Ui.appHeight * 0.5))
        padding: 6
        modal: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

        enter: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.dur(150); easing.type: Easing.OutCubic }
                NumberAnimation { property: "scale"; from: 0.95; to: 1.0; duration: Theme.dur(150); easing.type: Easing.OutCubic }
            }
        }
        exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.dur(100) } }

        contentItem: ListView {
            implicitHeight: contentHeight
            model: listPopup.visible ? picker.model : null
            clip: true
            spacing: 2
            ScrollBar.vertical: AppScrollBar { width: 6 }

            delegate: Rectangle {
                id: row
                required property var modelData
                readonly property bool on: picker.selected.indexOf(row.modelData.code) >= 0

                width: ListView.view ? ListView.view.width : picker.width
                height: Ui.controlHeight - 4
                radius: 8
                color: rowHover.hovered ? Theme.accentMuted : row.on ? Theme.surfaceAlt : "transparent"
                Behavior on color { ColorAnimation { duration: Theme.dur(90) } }

                HoverHandler { id: rowHover }

                AppCheckBox {
                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; leftMargin: 10; rightMargin: 10 }
                    text: row.modelData.name
                    fontSize: picker.fontSize
                    onClicked: picker.toggled(row.modelData.code, !row.on)
                    Binding on checked { value: row.on }
                }
            }
        }

        background: Rectangle {
            color: Theme.surfaceRaised
            border.color: Theme.borderStrong
            border.width: 1
            radius: 12
        }
    }
}
