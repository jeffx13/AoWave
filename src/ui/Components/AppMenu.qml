pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import App

Menu {
    id: appMenu
    modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 6
    spacing: 2
    transformOrigin: Item.TopLeft

    property int minWidth: Ui.sp(200)
    // A modal menu swallows the right click that closes it; this hands it on so another row's menu opens at once.
    property real openedAt: 0
    onAboutToShow: openedAt = Date.now()
    onClosed: if (modal) MainWindow.replayRightClick(openedAt)
    // The style sizes a menu by its background, not its items: without this a wide item is cut off.
    property real widestItem: 0
    readonly property int maxHeight: Math.round(Screen.desktopAvailableHeight * 0.6)

    enter: Transition {
        NumberAnimation {
            properties: "opacity"
            from: 0
            to: 1
            duration: 140
            easing.type: Easing.OutCubic
        }
        NumberAnimation {
            properties: "scale"
            from: 0.98
            to: 1.0
            duration: 140
            easing.type: Easing.OutCubic
        }
    }

    exit: Transition {
        NumberAnimation {
            properties: "opacity"
            from: 1
            to: 0
            duration: 100
            easing.type: Easing.InCubic
        }
    }

    font.family: Ui.fontFamily
    font.pixelSize: Ui.sp(Theme.bodySize)

    background: Rectangle {
        implicitWidth: Math.max(Math.max(appMenu.contentItem.implicitWidth, appMenu.widestItem) + appMenu.padding * 2,
                                appMenu.minWidth)
        implicitHeight: Math.min(appMenu.contentItem.implicitHeight + appMenu.padding * 2, appMenu.maxHeight)
        radius: 12
        border.color: Theme.border
        border.width: 1
        color: Theme.surfaceRaised
        clip: true
    }

    delegate: MenuItem {
        id: menuItem
        readonly property var extras: menuItem.action
        readonly property bool shown: !extras || extras.shown !== false
        readonly property string hint: extras && extras.hint ? extras.hint : ""
        visible: shown
        implicitWidth: Math.max(appMenu.minWidth, menuMetrics.advanceWidth + (menuItem.checkable ? 40 : 12) + (!!menuItem.subMenu ? 40 : 12)
                                                  + (hint ? hintMetrics.advanceWidth + 24 : 0))
        implicitHeight: shown ? Ui.controlHeight : 0
        onImplicitWidthChanged: if (implicitWidth > appMenu.widestItem) appMenu.widestItem = implicitWidth
        Component.onCompleted: if (implicitWidth > appMenu.widestItem) appMenu.widestItem = implicitWidth

        TextMetrics {
            id: menuMetrics
            font: menuItem.font
            text: menuItem.text
        }
        TextMetrics {
            id: hintMetrics
            font: hintLabel.font
            text: menuItem.hint
        }

        HoverHandler {
            cursorShape: menuItem.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        }

        arrow: Item {
            x: parent.width - width
            implicitWidth: 40
            implicitHeight: Ui.controlHeight
            visible: !!menuItem.subMenu
            AppIcon {
                anchors.centerIn: parent
                name: "chevron-right"
                size: Ui.sp(16)
                color: menuItem.highlighted ? Theme.textPrimary : Theme.accent
            }
        }

        indicator: Item {
            implicitWidth: 40
            implicitHeight: Ui.controlHeight
            Rectangle {
                width: 18
                height: 18
                anchors.centerIn: parent
                visible: menuItem.checkable
                border.color: Theme.accent
                radius: 4
                Rectangle {
                    width: 10
                    height: 10
                    anchors.centerIn: parent
                    visible: menuItem.checked
                    color: Theme.accent
                    radius: 3
                }
            }
        }

        contentItem: Text {
            id: menuLabel
            leftPadding: menuItem.checkable ? 40 : 12
            rightPadding: (!!menuItem.subMenu) ? 40 : 12
            text: menuItem.text
            font: menuItem.font
            opacity: menuItem.enabled ? 1.0 : 0.4
            color: Theme.textPrimary
            horizontalAlignment: Text.AlignLeft
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.NoWrap
            elide: Text.ElideNone

            Text {
                id: hintLabel
                visible: menuItem.hint !== ""
                anchors { right: parent.right; rightMargin: menuLabel.rightPadding; verticalCenter: parent.verticalCenter }
                text: menuItem.hint
                font.family: Ui.fontFamily
                font.pixelSize: Ui.sp(Theme.compactSize)
                color: menuItem.highlighted ? Theme.textPrimary : Theme.textMuted
                opacity: menuItem.enabled ? 1.0 : 0.5
            }
        }

        background: Rectangle {
            implicitWidth: menuItem.implicitWidth
            implicitHeight: Ui.controlHeight
            opacity: menuItem.enabled ? 1 : 0.3
            color: menuItem.down ? Qt.alpha(Theme.accent, 0.28) : menuItem.highlighted ? Qt.alpha(Theme.accent, 0.16) : "transparent"
            radius: 8
            border.width: 0
            Behavior on color {
                ColorAnimation {
                    duration: 110
                }
            }
        }
    }
}
