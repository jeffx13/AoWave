pragma ComponentBehavior: Bound
import QtQuick
import App

Rectangle {
    id: sideBar

    property bool chromeVisible: true
    signal pageRequested(int page)

    readonly property int rail: 56
    property bool locked: false
    property bool lockedExpanded: false
    property bool hoverExpanded: false
    readonly property bool expanded: locked ? lockedExpanded : hoverExpanded

    function requestExpand() { if (!locked) expandTimer.restart() }

    visible: width > 0
    focus: false
    clip: true
    width: chromeVisible ? (expanded ? 168 : rail) : 0
    Behavior on width { NumberAnimation { duration: 170; easing.type: Easing.OutCubic } }

    gradient: Gradient {
        GradientStop { position: 0.0; color: Theme.surface }
        GradientStop { position: 1.0; color: Theme.surfaceDeep }
    }

    Rectangle {
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
        width: 1
        color: Theme.border
    }

    HoverHandler { onHoveredChanged: if (!hovered) { expandTimer.stop(); sideBar.hoverExpanded = false } }
    Timer { id: expandTimer; interval: 200; onTriggered: if (!sideBar.locked) sideBar.hoverExpanded = true }

    component SideItem: Item {
        id: si
        property int page: 0
        property string icon: ""
        property string label: ""
        property bool needsShow: false
        property int badge: 0
        width: parent ? parent.width : 0
        height: 54
        readonly property bool isSelected: Ui.page === page
        readonly property bool isEnabled: needsShow ? App.show.exists : true
        opacity: isEnabled ? 1.0 : 0.35
        Behavior on opacity { NumberAnimation { duration: 140 } }

        function activate() { if (isEnabled) sideBar.pageRequested(page) }
        activeFocusOnTab: isEnabled
        Keys.onSpacePressed: activate()
        Keys.onReturnPressed: activate()
        Keys.onEnterPressed: activate()
        Accessible.role: Accessible.PageTab
        Accessible.name: label
        Accessible.selected: isSelected
        Accessible.focusable: true
        Accessible.onPressAction: activate()

        Rectangle {
            anchors { fill: parent; leftMargin: 8; rightMargin: 8; topMargin: 3; bottomMargin: 3 }
            radius: 12
            color: sideBar.expanded && (si.isSelected || itemHover.hovered) ? Theme.hoverFill : "transparent"
            Behavior on color { ColorAnimation { duration: 140 } }
            FocusRing { shown: si.activeFocus; outset: 0; radius: 12 }
        }

        Rectangle {
            x: 0
            anchors.verticalCenter: parent.verticalCenter
            width: 3
            height: si.isSelected ? 22 : 0
            radius: 1.5
            color: Theme.accent
            Behavior on height { NumberAnimation { duration: 220; easing.type: Easing.OutBack } }
        }

        SideBarIcon {
            id: sideIcon
            glyph: si.icon
            active: si.isSelected
            hovered: itemHover.hovered && si.isEnabled
            size: 36
            x: (sideBar.rail - width) / 2
            anchors.verticalCenter: parent.verticalCenter
        }
        Rectangle {
            visible: si.badge > 0
            anchors { right: sideIcon.right; top: sideIcon.top; margins: -3 }
            width: Math.max(height, badgeText.implicitWidth + 8)
            height: Math.min(Ui.sp(18), 20)
            radius: height / 2
            color: Theme.danger
            Text {
                id: badgeText
                anchors.centerIn: parent
                text: si.badge > 9 ? "9+" : si.badge
                color: Theme.inkOn(Theme.danger)
                font.pixelSize: Math.min(Ui.sp(12), 13)
                font.bold: true
            }
        }
        Text {
            anchors { left: parent.left; leftMargin: sideBar.rail; right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
            text: si.label
            color: si.isSelected ? Theme.textPrimary : itemHover.hovered ? Theme.textPrimary : Theme.textSecondary
            font.pixelSize: Ui.sp(17)
            font.weight: si.isSelected ? Font.DemiBold : Font.Medium
            elide: Text.ElideRight
            opacity: sideBar.expanded ? 1.0 : 0.0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }
        HoverHandler { id: itemHover; onHoveredChanged: if (hovered) sideBar.requestExpand() }
        MouseArea {
            anchors.fill: parent
            cursorShape: si.isEnabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
            onClicked: si.activate()
        }
        AppToolTip {
            text: si.isEnabled ? si.label : qsTr("Open a show to see its details")
            visible: itemHover.hovered && (!sideBar.expanded || !si.isEnabled)
        }
    }

    Column {
        anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: 10 }
        spacing: 2

        Item {
            width: parent.width
            height: 40
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                x: (sideBar.rail - width) / 2
                implicitWidth: 32
                implicitHeight: 32
                boxRadius: 9
                iconName: "pin"
                iconSize: 16
                active: sideBar.locked
                hoverColor: sideBar.locked ? Theme.accentMuted : Theme.hoverFill
                iconColor: sideBar.locked ? Theme.textAccent : Theme.textMuted
                iconHoverColor: sideBar.locked ? Theme.textAccent : Theme.textPrimary
                tip: sideBar.locked ? qsTr("Unlock sidebar") : (sideBar.expanded ? qsTr("Lock sidebar open") : qsTr("Lock sidebar collapsed"))
                onClicked: {
                    if (sideBar.locked) {
                        sideBar.locked = false
                    } else {
                        sideBar.lockedExpanded = sideBar.expanded
                        sideBar.locked = true
                    }
                }
            }
        }

        SideItem { page: MainWindow.Explore;   icon: "search";     label: qsTr("Explore") }
        SideItem { page: MainWindow.Details;     icon: "details";    label: qsTr("Details"); needsShow: true }
        SideItem { page: MainWindow.Library;  icon: "library";    label: qsTr("Library"); badge: Ui.newEpisodeShows }
        SideItem { page: MainWindow.Player;   icon: "tv";         label: qsTr("Player") }
        SideItem { page: MainWindow.Downloads; icon: "download";   label: qsTr("Downloads"); badge: App.downloads.downloading }
    }

    Column {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; bottomMargin: 10 }
        spacing: 2
        SideItem { page: MainWindow.History;  icon: "history";     label: qsTr("History") }
        SideItem { page: MainWindow.Logs;      icon: "list-checks"; label: qsTr("Logs") }
        SideItem { page: MainWindow.Settings; icon: "settings";    label: qsTr("Settings") }
    }
}
