pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import App
import ".."

Menu {
    id: appMenu
    modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 6
    spacing: 2
    transformOrigin: Item.TopLeft

    property int minWidth: Globals.sp(200)
    // A modal menu swallows the right click that closes it; this hands it on, so right-clicking
    // another row opens that row's menu straight away. A left click still only closes it.
    property real openedAt: 0
    onAboutToShow: openedAt = Date.now()
    onClosed: if (modal) AppShell.replayRightClick(openedAt)
    // The style sizes a menu by its background, which knows nothing of the items: without this,
    // an item wider than minWidth is cut off.
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

    font.family: Globals.fontFamily
    font.pixelSize: Globals.sp(Theme.bodySize)

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
        // An Action may declare `shown` to leave the menu rather than grey out, and `hint` for a key.
        readonly property var extras: menuItem.action   // untyped: the extras are the Action's own
        readonly property bool shown: !extras || extras.shown !== false
        readonly property string hint: extras && extras.hint ? extras.hint : ""
        visible: shown
        implicitWidth: Math.max(appMenu.minWidth, menuMetrics.advanceWidth + (menuItem.checkable ? 40 : 12) + (!!menuItem.subMenu ? 40 : 12)
                                                  + (hint ? hintMetrics.advanceWidth + 24 : 0))
        implicitHeight: shown ? Globals.controlHeight : 0
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

        arrow: Canvas {
            x: parent.width - width
            implicitWidth: 40
            implicitHeight: Globals.controlHeight
            visible: !!menuItem.subMenu
            onPaint: {
                var ctx = getContext("2d");
                ctx.reset();
                ctx.fillStyle = menuItem.highlighted ? Theme.textPrimary : Theme.accent;
                ctx.moveTo(15, 10);
                ctx.lineTo(width - 15, height / 2);
                ctx.lineTo(15, height - 10);
                ctx.closePath();
                ctx.fill();
            }
        }

        indicator: Item {
            implicitWidth: 40
            implicitHeight: Globals.controlHeight
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
                font.family: Globals.fontFamily
                font.pixelSize: Globals.sp(Theme.compactSize)
                color: Theme.textMuted
                opacity: menuItem.enabled ? 1.0 : 0.5
            }
        }

        background: Rectangle {
            implicitWidth: menuItem.implicitWidth
            implicitHeight: Globals.controlHeight
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
