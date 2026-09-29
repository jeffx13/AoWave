pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import ".."

// Dropdown keeping several entries ticked.
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

    implicitWidth: Globals.sp(200)
    implicitHeight: Globals.controlHeight
    hoverEnabled: true
    font.family: Globals.fontFamily
    font.pixelSize: Globals.sp(fontSize)

    background: FieldBackground {
        radius: 12
        focused: listPopup.visible
        hovered: picker.hovered
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
        size: 16
        color: listPopup.visible ? Theme.textAccent : Theme.textMuted
        rotation: listPopup.visible ? 180 : 0
        Behavior on rotation { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
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
        implicitHeight: Math.min(contentItem.implicitHeight + padding * 2, Math.max(120, Globals.appHeight * 0.5))
        padding: 6
        modal: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

        enter: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 150; easing.type: Easing.OutCubic }
                NumberAnimation { property: "scale"; from: 0.95; to: 1.0; duration: 150; easing.type: Easing.OutCubic }
            }
        }
        exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 100 } }

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
                height: Globals.controlHeight - 4
                radius: 8
                color: rowHover.hovered ? Theme.accentMuted : row.on ? Theme.surfaceAlt : "transparent"
                Behavior on color { ColorAnimation { duration: 90 } }

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
