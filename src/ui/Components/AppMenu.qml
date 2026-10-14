pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import App

Menu {
    id: appMenu
    modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 4
    transformOrigin: Item.TopLeft

    property int minWidth: Ui.sp(160)
    property string iconName: ""
    property bool shown: true
    // a modal menu swallows the right click that closes it; hand it on
    property real openedAt: 0
    onAboutToShow: {
        openedAt = Date.now()
        let widest = 0
        for (let i = 0; i < count; ++i) {
            const item = itemAt(i)
            if (item && item.implicitHeight > 0) widest = Math.max(widest, item.implicitWidth)
        }
        widestItem = widest
    }
    onClosed: if (modal) MainWindow.replayRightClick(openedAt)
    property real widestItem: 0
    property bool hasIcons: false
    readonly property int maxHeight: Math.round(Screen.desktopAvailableHeight * 0.6)
    readonly property int rowHeight: Ui.sp(32)
    readonly property int glyphSize: Ui.sp(16)
    readonly property int lead: hasIcons ? glyphSize + 20 : 10

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 110; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: 0.98; to: 1.0; duration: 110; easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 80; easing.type: Easing.InCubic }
    }

    font.family: Ui.fontFamily
    font.pixelSize: Ui.sp(Theme.compactSize)

    background: Rectangle {
        implicitWidth: Math.max(Math.max(appMenu.contentItem.implicitWidth, appMenu.widestItem) + appMenu.padding * 2,
                                appMenu.minWidth)
        implicitHeight: Math.min(appMenu.contentItem.implicitHeight + appMenu.padding * 2, appMenu.maxHeight)
        radius: 10
        border.color: Theme.border
        border.width: 1
        color: Theme.surfaceRaised
        clip: true
    }

    delegate: MenuItem {
        id: menuItem
        readonly property var extras: menuItem.action
        readonly property var sub: menuItem.subMenu
        readonly property bool shown: sub ? sub.shown !== false : !extras || extras.shown !== false
        readonly property bool destructive: !!extras && extras.destructive === true
        readonly property string hint: extras && extras.hint ? extras.hint : ""
        readonly property string glyph: sub ? String(sub.iconName ?? "")
                                       : menuItem.checkable ? (menuItem.checked ? "check" : "")
                                       : menuItem.icon.name
        readonly property color ink: destructive ? Theme.danger : Theme.textPrimary
        visible: shown
        padding: 0
        spacing: 0
        implicitWidth: Math.max(appMenu.minWidth, appMenu.lead + menuMetrics.advanceWidth + (menuItem.subMenu ? 30 : 12)
                                                  + (hint ? hintMetrics.advanceWidth + 24 : 0))
        implicitHeight: shown ? appMenu.rowHeight : 0
        onImplicitWidthChanged: if (implicitWidth > appMenu.widestItem) appMenu.widestItem = implicitWidth
        onGlyphChanged: if (glyph !== "") appMenu.hasIcons = true
        Component.onCompleted: {
            if (glyph !== "" || checkable) appMenu.hasIcons = true
            if (implicitWidth > appMenu.widestItem) appMenu.widestItem = implicitWidth
        }

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

        indicator: Item {
            implicitWidth: appMenu.lead
            implicitHeight: appMenu.rowHeight
            AppIcon {
                anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
                visible: menuItem.glyph !== ""
                name: menuItem.glyph
                size: appMenu.glyphSize
                color: menuItem.destructive ? Theme.danger : menuItem.highlighted ? Theme.textPrimary : Theme.textSecondary
                opacity: menuItem.enabled ? 1.0 : 0.4
            }
        }

        arrow: Item {
            x: parent.width - width
            implicitWidth: 30
            implicitHeight: appMenu.rowHeight
            visible: !!menuItem.subMenu
            AppIcon {
                anchors.centerIn: parent
                name: "chevron-right"
                size: Ui.sp(14)
                color: menuItem.highlighted ? Theme.textPrimary : Theme.textMuted
            }
        }

        contentItem: Text {
            id: menuLabel
            leftPadding: appMenu.lead
            rightPadding: menuItem.subMenu ? 30 : 12
            text: menuItem.text
            font: menuItem.font
            opacity: menuItem.enabled ? 1.0 : 0.4
            color: menuItem.ink
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
                font.pixelSize: Ui.sp(Theme.metadataSize)
                color: menuItem.highlighted ? Theme.textSecondary : Theme.textMuted
                opacity: menuItem.enabled ? 1.0 : 0.5
            }
        }

        background: Rectangle {
            implicitWidth: menuItem.implicitWidth
            implicitHeight: appMenu.rowHeight
            opacity: menuItem.enabled ? 1 : 0.3
            readonly property color tint: menuItem.destructive ? Theme.danger : Theme.accent
            color: menuItem.down ? Qt.alpha(tint, 0.26) : menuItem.highlighted ? Qt.alpha(tint, 0.15) : "transparent"
            radius: 6
            border.width: 0
            Behavior on color { ColorAnimation { duration: 90 } }
        }
    }
}
