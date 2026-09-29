pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import App
import QtQuick.Layouts
import "../Components"
import ".."

Item {
    id: logPage
    HoverHandler {
        cursorShape: Qt.ArrowCursor
    }

    AppMenu {
        id: rowMenu
        property string message: ""
        property bool request: false
        Action {
            text: rowMenu.request ? qsTr("Copy link") : qsTr("Copy")
            onTriggered: App.copyToClipboard(rowMenu.message)
        }
        Action {
            text: qsTr("Open in browser")
            enabled: rowMenu.request && /^https?:\/\//i.test(rowMenu.message)
            onTriggered: Qt.openUrlExternally(rowMenu.message)
        }
        function show(item, x, y, message, request) {
            rowMenu.message = message
            rowMenu.request = request
            const p = item.mapToItem(logPage, x, y)
            rowMenu.popup(p.x, p.y)
        }
    }

    // An ini round-trip hands back strings.
    function storedBool(key, fallback) {
        const value = App.settings.value(key, fallback)
        return value === true || value === "true" || value === 1 || value === "1"
    }

    // Restored on load, written back on every change.
    Component.onCompleted: {
        const levels = Number(App.settings.value("logging/levels", App.logView.levels))
        if (!isNaN(levels) && levels > 0) App.logView.levels = levels
        App.logView.requests = storedBool("logging/requests", true)
        App.logView.mpv      = storedBool("logging/showMpv", true)
        logList.positionViewAtEnd()
    }

    function persistFilters() {
        App.settings.setValue("logging/levels", App.logView.levels)
        App.settings.setValue("logging/requests", App.logView.requests)
        App.settings.setValue("logging/showMpv", App.logView.mpv)
        logList.positionViewAtEnd()
    }

    Card {
        id: topBar
        radius: 12
        anchors { top: parent.top; left: parent.left; right: parent.right; margins: 10 }
        implicitHeight: topBarCol.implicitHeight + 12

        ColumnLayout {
            id: topBarCol
            anchors { fill: parent; leftMargin: 14; rightMargin: 10; topMargin: 6; bottomMargin: 6 }
            spacing: 4

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: Globals.controlHeight
                spacing: 8

                Text {
                    text: qsTr("Logs")
                    color: Theme.textPrimary
                    font.pixelSize: Globals.sp(Theme.headingSize)
                    font.weight: Font.DemiBold
                    verticalAlignment: Text.AlignVCenter
                }
                Text {
                    text: logList.count === 1 ? qsTr("1 entry") : qsTr("%1 entries").arg(logList.count)
                    color: Theme.textMuted
                    font.pixelSize: Globals.sp(Theme.metadataSize)
                    verticalAlignment: Text.AlignVCenter
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }

                AppButton {
                    text: qsTr("Clear")
                    radius: 9
                    secondary: true
                    Layout.fillHeight: true
                    onClicked: App.logList.clear()
                }
            }

            Flow {
                Layout.fillWidth: true
                Layout.bottomMargin: 2
                spacing: 14

                Repeater {
                    // `level` is the string the logger stamps on an entry.
                    model: [
                        { level: "error", label: qsTr("Errors") },
                        { level: "warn",  label: qsTr("Warnings") },
                        { level: "info",  label: qsTr("Info") },
                        { level: "ok",    label: qsTr("Success") },
                        { level: "step",  label: qsTr("Steps") },
                        { level: "raw",   label: qsTr("Raw") }
                    ]
                    delegate: AppCheckBox {
                        required property var modelData
                        text: modelData.label
                        // Read so the binding refreshes with it.
                        checked: App.logView.levels >= 0 && App.logView.levelEnabled(modelData.level)
                        onClicked: {
                            App.logView.setLevelEnabled(modelData.level, checked)
                            logPage.persistFilters()
                        }
                    }
                }

                AppCheckBox {
                    text: qsTr("Requests")
                    checked: App.logView.requests
                    onClicked: { App.logView.requests = checked; logPage.persistFilters() }
                }

                AppCheckBox {
                    // Gates the mpv relay at the source.
                    text: qsTr("MPV")
                    checked: App.settings.mpvLogEnabled
                    onClicked: {
                        App.settings.mpvLogEnabled = checked
                        App.logView.mpv = checked
                        logPage.persistFilters()
                    }
                }
            }
        }
    }

    ListView {
        id: logList
        anchors { left: parent.left; right: parent.right; top: topBar.bottom; bottom: parent.bottom; margins: 10; rightMargin: Globals.sp(Theme.scrollbarGutter); topMargin: 6 }
        model: App.logView
        clip: true
        spacing: 4
        boundsBehavior: Flickable.StopAtBounds
        reuseItems: true
        cacheBuffer: 1200

        ScrollBar.vertical: AppScrollBar {
            parent: logList.parent
            anchors { top: logList.top; left: logList.right; bottom: logList.bottom }
            width: 6
            barColor: Theme.textMuted
        }

        // Follow the tail while it is in view.
        onCountChanged: {
            if (atYEnd || contentY >= contentHeight - height - 100) positionViewAtEnd()
        }

        delegate: Item {
            id: logItem
            width: ListView.view.width
            height: card.implicitHeight
            required property string message
            required property string time
            required property string type
            required property string level
            required property bool request

            readonly property color levelColor: Theme.logColor(level)
            readonly property bool problem: level === "error" || level === "warn"

            Rectangle {
                id: card
                width: parent.width
                radius: 8
                color: Theme.surface
                border.color: Theme.border
                border.width: 1
                implicitHeight: col.implicitHeight + 16

                ColumnLayout {
                    id: col
                    anchors { fill: parent; margins: 8 }
                    spacing: 5

                    RowLayout {
                        spacing: 8
                        Text {
                            text: logItem.time
                            color: Theme.textMuted
                            font.pixelSize: Globals.sp(15)
                        }
                        Rectangle {
                            radius: 5
                            color: logItem.levelColor
                            implicitWidth: badge.implicitWidth + 12
                            implicitHeight: badge.implicitHeight + 5
                            Text {
                                id: badge
                                anchors.centerIn: parent
                                text: logItem.type
                                color: Theme.inkOn(logItem.levelColor)
                                font.pixelSize: Globals.sp(14)
                                font.weight: Font.DemiBold
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }

                    TextEdit {
                        id: messageText
                        visible: !logItem.request
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: logItem.message
                        textFormat: TextEdit.PlainText
                        wrapMode: TextEdit.WrapAnywhere
                        readOnly: true
                        selectByMouse: true
                        persistentSelection: true
                        activeFocusOnPress: true
                        selectionColor: Theme.accent
                        selectedTextColor: Theme.onAccent
                        color: logItem.request ? Theme.textAccent
                             : logItem.problem ? logItem.levelColor : Theme.textSecondary
                        font.family: Globals.fontFamily
                        font.pixelSize: Globals.sp(Theme.bodySize)
                        HoverHandler { cursorShape: Qt.IBeamCursor }
                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: (point) => rowMenu.show(messageText, point.position.x, point.position.y,
                                messageText.selectedText.length > 0 ? messageText.selectedText : logItem.message,
                                logItem.request && messageText.selectedText.length === 0)
                        }
                    }

                    Text {
                        id: requestText
                        visible: logItem.request
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: logItem.message
                        color: Theme.textAccent
                        font.family: Globals.fontFamily
                        font.pixelSize: Globals.sp(Theme.bodySize)
                        wrapMode: Text.NoWrap
                        maximumLineCount: 1
                        elide: Text.ElideMiddle
                        clip: true
                        verticalAlignment: Text.AlignVCenter

                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onTapped: (point) => rowMenu.show(requestText, point.position.x,
                                point.position.y, logItem.message, true)
                        }
                    }

                }
            }
        }
    }

    IconButton {
        anchors { right: logList.right; bottom: logList.bottom; margins: 14 }
        visible: opacity > 0.01
        opacity: logList.atYEnd ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: 140 } }
        implicitWidth: 42
        implicitHeight: 42
        boxRadius: 21
        active: true
        hoverColor: hovered ? Theme.accentLight : Theme.accent
        iconName: "arrow-down-to-line"
        iconSize: 20
        iconColor: Theme.onAccent
        iconHoverColor: Theme.onAccent
        tip: qsTr("Jump to latest")
        onClicked: logList.positionViewAtEnd()
    }
}
