pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ComboBox {
    id: comboBox

    property color currentIndexColor: overlay ? Theme.overlayFill : Theme.surfaceAlt
    property int fontSize: Theme.bodySize
    readonly property bool overlay: Ui.inOverlay(comboBox)

    property string text: ""
    property string placeholderText: ""
    property string displayFormat: ""
    readonly property string shownText: displayFormat !== "" ? displayFormat.arg(displayText) : displayText
    readonly property string rowLabel: Ui.labelOf(comboBox)
    Accessible.name: rowLabel || shownText
    Accessible.description: rowLabel ? shownText : ""

    textRole: comboBox.text && comboBox.text.length > 0 ? comboBox.text : ""

    readonly property int sidePadding: 12
    readonly property int indicatorSpace: 30

    readonly property real longestEntry: {
        void entryMetrics.font.pixelSize;
        void comboBox.model;
        let widest = entryMetrics.advanceWidth(comboBox.placeholderText);
        for (let i = 0; i < comboBox.count; ++i)
            widest = Math.max(widest, entryMetrics.advanceWidth(comboBox.displayFormat !== ""
                                                                 ? comboBox.displayFormat.arg(comboBox.textAt(i))
                                                                 : comboBox.textAt(i)));
        return widest;
    }
    FontMetrics {
        id: entryMetrics
        font.family: Ui.fontFamily
        font.pixelSize: Ui.sp(comboBox.fontSize)
    }

    implicitWidth: Math.ceil(longestEntry) + sidePadding * 2 + indicatorSpace
    implicitHeight: Ui.controlHeight
    focusPolicy: Qt.TabFocus
    Layout.minimumWidth: Math.min(implicitWidth, Ui.sp(100))
    font.family: Ui.fontFamily
    font.pixelSize: Ui.sp(fontSize)
    enabled: count > 0
    opacity: enabled ? 1 : 0.55

    delegate: ItemDelegate {
        id: itemDel
        required property int index
        required property var model

        width: ListView.view ? ListView.view.width : comboBox.width
        height: Math.max(Ui.controlHeight, implicitContentHeight + 12)
        enabled: typeof itemDel.model.disabled === "undefined" || !itemDel.model.disabled
        opacity: enabled ? 1.0 : 0.5

        readonly property bool isCurrent: itemDel.index === comboBox.currentIndex

        contentItem: Text {
            leftPadding: 12
            rightPadding: 12
            text: comboBox.textAt(itemDel.index)
            elide: Text.ElideRight
            color: comboBox.highlightedIndex === itemDel.index ? (comboBox.overlay ? Theme.onOverlay : Theme.textPrimary)
                 : itemDel.isCurrent ? (comboBox.overlay ? Theme.onOverlayAccent : Theme.textAccent)
                 : comboBox.overlay ? Theme.onOverlayMuted : Theme.textPrimary
            font.weight: itemDel.isCurrent ? Font.Medium : Font.Normal
            font.family: Ui.fontFamily
            font.pixelSize: Ui.sp(comboBox.fontSize)
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignHCenter
        }

        background: Rectangle {
            color: {
                if (comboBox.highlightedIndex === itemDel.index)
                    return comboBox.overlay ? Theme.overlayFillHover : Theme.accentMuted;
                return itemDel.isCurrent ? comboBox.currentIndexColor : "transparent";
            }
            radius: 8
            Behavior on color {
                ColorAnimation {
                    duration: Theme.dur(90)
                }
            }
        }
    }

    indicator: AppIcon {
        x: comboBox.width - width - 12
        anchors.verticalCenter: parent.verticalCenter
        name: "chevron-down"
        size: Ui.sp(14)
        color: comboBox.down ? (comboBox.overlay ? Theme.onOverlayAccent : Theme.textAccent)
                             : comboBox.overlay ? Theme.onOverlayDim : Theme.textMuted
        rotation: comboBox.down ? 180 : 0
        Behavior on rotation {
            NumberAnimation {
                duration: Theme.dur(150)
                easing.type: Easing.OutCubic
            }
        }
    }

    contentItem: Text {
        width: comboBox.width - comboBox.sidePadding - comboBox.indicatorSpace
        height: comboBox.height
        x: comboBox.sidePadding
        text: comboBox.currentIndex < 0 && comboBox.placeholderText.length > 0 ? comboBox.placeholderText : comboBox.shownText
        elide: Text.ElideRight
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        font.family: Ui.fontFamily
        font.pixelSize: Ui.sp(comboBox.fontSize)
        color: comboBox.currentIndex < 0 && comboBox.placeholderText.length > 0
               ? (comboBox.overlay ? Theme.onOverlayDim : Theme.textMuted)
               : comboBox.overlay ? Theme.onOverlay : Theme.textPrimary
    }

    background: FieldBackground {
        focused: comboBox.activeFocus || comboBox.down
        hovered: comboBox.hovered
        FocusRing { shown: comboBox.visualFocus; radius: parent.radius + 3 }
    }

    popup: Popup {
        id: listPopup
        readonly property real hostWidth: Math.max(comboBox.width, Ui.appWidth)
        readonly property real hostHeight: Math.max(Ui.controlHeight, Ui.appHeight)
        readonly property real windowX: {
            void listPopup.visible
            return comboBox.mapToItem(null, 0, 0).x
        }

        parent: comboBox
        x: Math.min(0, hostWidth - 12 - windowX - width)
        y: comboBox.height + 4
        width: Math.min(Math.max(comboBox.width, comboBox.implicitWidth + 8), Math.max(comboBox.width, hostWidth - 24))
        implicitHeight: Math.min(contentItem.implicitHeight + padding * 2, Math.max(80, hostHeight * 0.6))
        padding: 6
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

        enter: Transition {
            ParallelAnimation {
                NumberAnimation {
                    property: "opacity"
                    from: 0
                    to: 1
                    duration: Theme.dur(150)
                    easing.type: Easing.OutCubic
                }
                NumberAnimation {
                    property: "scale"
                    from: 0.95
                    to: 1.0
                    duration: Theme.dur(150)
                    easing.type: Easing.OutCubic
                }
            }
        }
        exit: Transition {
            NumberAnimation {
                property: "opacity"
                from: 1
                to: 0
                duration: Theme.dur(100)
            }
        }

        contentItem: ListView {
            implicitHeight: contentHeight
            model: comboBox.popup.visible ? comboBox.delegateModel : null
            clip: true
            currentIndex: comboBox.highlightedIndex
            spacing: 2
            ScrollBar.vertical: AppScrollBar {}
        }

        background: Rectangle {
            color: comboBox.overlay ? Theme.overlayScrim : Theme.surfaceRaised
            border.color: comboBox.overlay ? Theme.overlayFillActive : Theme.borderStrong
            border.width: 1
            radius: 12
        }
    }
}
