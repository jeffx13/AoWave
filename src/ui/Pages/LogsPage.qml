pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import App
import QtQuick.Layouts

Item {
    id: logPage
    HoverHandler {
        cursorShape: Qt.ArrowCursor
    }

    AppMenu {
        id: textMenu
        property string link: ""
        Action {
            text: qsTr("Copy")
            enabled: logText.selectedText.length > 0
            onTriggered: App.copyToClipboard(logPage.plain(logText.selectedText))
        }
        Action {
            text: qsTr("Copy all")
            onTriggered: App.copyToClipboard(logPage.plain(logText.getText(0, logText.length)))
        }
        Action {
            text: qsTr("Copy link")
            enabled: textMenu.link !== ""
            onTriggered: App.copyToClipboard(textMenu.link)
        }
        Action {
            text: qsTr("Open in browser")
            enabled: /^https?:\/\//i.test(textMenu.link)
            onTriggered: Qt.openUrlExternally(textMenu.link)
        }
    }

    function storedBool(key, fallback) {
        const value = App.settings.value(key, fallback)
        return value === true || value === "true" || value === 1 || value === "1"
    }

    Component.onCompleted: {
        const levels = Number(App.settings.value("logging/levels", App.logView.levels))
        if (!isNaN(levels) && levels > 0) App.logView.levels = levels
        App.logView.requests = storedBool("logging/requests", true)
        App.logView.mpv      = storedBool("logging/showMpv", true)
        rebuild()
    }

    function persistFilters() {
        App.settings.setValue("logging/levels", App.logView.levels)
        App.settings.setValue("logging/requests", App.logView.requests)
        App.settings.setValue("logging/showMpv", App.logView.mpv)
    }

    function esc(s) {
        return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;")
    }
    function css(c) {
        const q = Qt.color(c)
        return Qt.rgba(q.r, q.g, q.b, 1).toString()
    }
    readonly property string paragraphBreak: String.fromCharCode(0x2029)
    readonly property string lineBreak: String.fromCharCode(0x2028)
    readonly property string tab: String.fromCharCode(9)
    readonly property var tags: ["Web", "Player", "Danmaku", "Subtitles", "Sources", "Library", "Downloads", "Sync", "App"]
    FontMetrics { id: timeMetrics; font: logText.font }
    FontMetrics { id: tagMetrics; font.family: logText.font.family; font.pixelSize: logText.font.pixelSize; font.bold: true }
    readonly property int columnWidth: {
        // advanceWidth() is a call, not a property: reading the fonts re-measures once they settle or change.
        void timeMetrics.font
        void tagMetrics.font
        return Math.ceil(Math.max(timeMetrics.advanceWidth("00:00:00"),
                                  ...tags.map(t => tagMetrics.advanceWidth(t)))) + Ui.sp(16)
    }
    readonly property int messageIndent: 2 * columnWidth
    property string lastKey: ""

    function tagOf(e) { return e.request ? "Web" : e.type }
    function keyOf(e) { return e.time + "|" + tagOf(e) }
    function badgeColor(status) {
        const code = Number((/\((\d+)\)/.exec(status) || [])[1] || 0)
        return code >= 200 && code < 300 ? Theme.success : code >= 500 || code === 0 ? Theme.danger : Theme.warning
    }
    function header(e) {
        return '<span style="color:' + css(Theme.textMuted) + '">' + esc(e.time) + '</span>' + tab
             + '<span style="font-weight:600; color:' + css(Theme.logColor(e.level)) + '">' + esc(tagOf(e)) + '</span>' + tab
    }
    function message(e) {
        if (e.request) {
            return '<span style="font-weight:600; color:' + css(badgeColor(e.type)) + '">'
                 + esc(e.type.replace(/[()]/g, "")) + '</span>&nbsp;&nbsp;'
                 + '<a href="' + esc(e.message) + '" style="color:' + css(Theme.textAccent)
                 + '; text-decoration:none">' + esc(e.message) + '</a>'
        }
        const problem = e.level === "error" || e.level === "warn"
        return '<span style="color:' + css(problem ? Theme.logColor(e.level) : Theme.textSecondary) + '">'
             + esc(e.message) + '</span>'
    }
    // append() gives a new block the previous block's format and ignores the fragment's, so every block is alike.
    function group(e, messages) {
        return '<p style="margin:' + Ui.sp(5) + 'px 0 ' + Ui.sp(5) + 'px ' + messageIndent + 'px; text-indent:-'
             + messageIndent + 'px; white-space:pre-wrap">' + header(e) + messages.join('<br>') + '</p>'
    }
    function draw(e) {
        const key = keyOf(e)
        if (key === lastKey && logText.length > 0) {
            logText.insert(logText.length, '<br><span style="white-space:pre-wrap">' + message(e) + '</span>')
            ++groupRows[groupRows.length - 1]
        } else {
            groupStarts.push(logText.length > 0 ? logText.length + 1 : 0)
            logText.append(group(e, [message(e)]))
            groupRows.push(1)
        }
        lastKey = key
        queueDividers()
    }
    function plain(t) {
        return t.split(paragraphBreak).join("\n").split(lineBreak).join("\n" + tab + tab)
                .replace(/\u00a0/g, " ").replace(/ +$/gm, "")
    }
    readonly property string drawStyle: String(Theme.textMuted) + Theme.textAccent + Theme.textSecondary
                                        + Theme.logColor("info") + Theme.success + messageIndent
    onDrawStyleChanged: queueRebuild()

    property var groupStarts: []
    property var dividerYs: []
    property bool dividersQueued: false
    function queueDividers() {
        if (dividersQueued) return
        dividersQueued = true
        Qt.callLater(updateDividers)
    }
    function updateDividers() {
        dividersQueued = false
        const top = flick.contentY - 50, bottom = flick.contentY + flick.height + 50
        let lo = 0, hi = groupStarts.length
        while (lo < hi) {
            const mid = (lo + hi) >> 1
            if (logText.positionToRectangle(groupStarts[mid]).y < top) lo = mid + 1
            else hi = mid
        }
        const ys = []
        for (let i = Math.max(1, lo); i < groupStarts.length; ++i) {
            const y = logText.positionToRectangle(groupStarts[i]).y
            if (y > bottom) break
            ys.push(y - Ui.sp(5))
        }
        dividerYs = ys
    }

    property int staleTop: 0
    property var groupRows: []
    property bool rebuildQueued: false
    property bool dirty: false
    onVisibleChanged: if (visible && dirty) { dirty = false; queueRebuild() }
    function queueRebuild() {
        if (!visible) { dirty = true; return }
        if (rebuildQueued) return
        rebuildQueued = true
        Qt.callLater(rebuild)
    }
    function rebuild() {
        rebuildQueued = false
        const follow = atEnd()
        const parts = [], rows = []
        let first = null, messages = [], key = ""
        for (let i = 0; i < App.logView.count; ++i) {
            const e = App.logView.entry(i)
            if (keyOf(e) !== key) {
                if (first) { parts.push(group(first, messages)); rows.push(messages.length) }
                first = e; messages = []; key = keyOf(e)
            }
            messages.push(message(e))
        }
        if (first) { parts.push(group(first, messages)); rows.push(messages.length) }
        logText.text = parts.join("")
        groupRows = rows
        const all = logText.getText(0, logText.length)
        const starts = rows.length > 0 ? [0] : []
        for (let at = all.indexOf(paragraphBreak); at >= 0; at = all.indexOf(paragraphBreak, at + 1)) starts.push(at + 1)
        groupStarts = starts
        queueDividers()
        lastKey = key
        staleTop = 0
        if (follow) Qt.callLater(toEnd)
    }
    function trimTop() {
        let groups = 0
        while (groupRows.length > 0 && staleTop >= groupRows[0]) {
            staleTop -= groupRows.shift()
            ++groups
        }
        if (groups === 0) return
        const text = logText.getText(0, logText.length)
        let at = -1
        for (let n = 0; n < groups && (n === 0 || at >= 0); ++n) at = text.indexOf(paragraphBreak, at + 1)
        if (at < 0) { queueRebuild(); return }
        const follow = atEnd()
        const cut = logText.positionToRectangle(at + 1).y - logText.topPadding
        const y = flick.contentY
        logText.remove(0, at + 1)
        groupStarts = groupStarts.slice(groups).map(start => start - (at + 1))
        queueDividers()
        if (follow) Qt.callLater(toEnd)
        else flick.contentY = Math.max(0, y - cut)
    }
    function reveal(r) {
        if (r.y < flick.contentY) flick.contentY = Math.max(0, r.y)
        else if (r.y + r.height > flick.contentY + flick.height)
            flick.contentY = Math.min(flick.contentHeight - flick.height, r.y + r.height - flick.height)
    }

    readonly property Flickable flick: logFlick
    function atEnd() {
        return !flick || flick.contentY >= flick.contentHeight - flick.height - 40
    }
    function toEnd() {
        if (flick) flick.contentY = Math.max(0, flick.contentHeight - flick.height)
    }

    Connections {
        target: App.logView
        function onRowsInserted(parent, first, last) {
            if (!logPage.visible) { logPage.dirty = true; return }
            if (logPage.rebuildQueued) return
            if (last !== App.logView.count - 1) { logPage.queueRebuild(); return }
            const follow = logPage.atEnd()
            for (let i = first; i <= last; ++i) logPage.draw(App.logView.entry(i))
            if (follow) Qt.callLater(logPage.toEnd)
        }
        function onRowsRemoved(parent, first, last) {
            if (!logPage.visible) { logPage.dirty = true; return }
            if (logPage.rebuildQueued) return
            if (first !== 0) { logPage.queueRebuild(); return }
            logPage.staleTop += last - first + 1
            if (logPage.staleTop >= 100) logPage.trimTop()
        }
        function onModelReset() { logPage.queueRebuild() }
        function onFiltersChanged() { logPage.queueRebuild() }
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
                Layout.preferredHeight: Ui.controlHeight
                spacing: 8

                Text {
                    text: qsTr("Logs")
                    color: Theme.textPrimary
                    font.pixelSize: Ui.sp(Theme.headingSize)
                    font.weight: Font.DemiBold
                    verticalAlignment: Text.AlignVCenter
                }
                Text {
                    text: App.logView.count === 1 ? qsTr("1 entry") : qsTr("%1 entries").arg(App.logView.count)
                    color: Theme.textMuted
                    font.pixelSize: Ui.sp(Theme.metadataSize)
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
                    model: [
                        { level: "error", label: qsTr("Errors") },
                        { level: "warn",  label: qsTr("Warnings") },
                        { level: "info",  label: qsTr("Info") },
                        { level: "ok",    label: qsTr("Success") },
                        { level: "step",  label: qsTr("Steps") }
                    ]
                    delegate: AppCheckBox {
                        required property var modelData
                        text: modelData.label
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
                    text: qsTr("mpv")
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

    Card {
        id: logCard
        radius: 10
        anchors { left: parent.left; right: parent.right; top: topBar.bottom; bottom: parent.bottom; margins: 10; topMargin: 6 }

        ScrollView {
            id: logScroll
            anchors { fill: parent; margins: 4; rightMargin: Ui.sp(Theme.scrollbarGutter) }
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical: AppScrollBar {
                parent: logScroll.parent
                anchors { top: logScroll.top; left: logScroll.right; bottom: logScroll.bottom; leftMargin: 2 }
                width: 6
                barColor: Theme.textMuted
            }

            // A TextArea in a ScrollView throws the view back to its caret on every new line; hence a plain TextEdit.
            Flickable {
                id: logFlick
                contentWidth: width
                contentHeight: logText.height
                boundsBehavior: Flickable.StopAtBounds
                onContentYChanged: logPage.queueDividers()
                onHeightChanged: logPage.queueDividers()

                Repeater {
                    model: logPage.dividerYs
                    delegate: Rectangle {
                        required property real modelData
                        x: 8
                        width: logFlick.width - 16
                        y: modelData
                        height: 1
                        color: Theme.border
                    }
                }

                TextEdit {
                    id: logText
                    width: logFlick.width
                    textFormat: TextEdit.RichText
                    readOnly: true
                    selectByMouse: true
                    persistentSelection: true
                    wrapMode: TextEdit.WrapAnywhere
                    color: Theme.textSecondary
                    selectionColor: Theme.accent
                    selectedTextColor: Theme.onAccent
                    font.family: Ui.fontFamily
                    tabStopDistance: logPage.columnWidth
                    font.pixelSize: Ui.sp(Theme.bodySize)
                    leftPadding: 8
                    rightPadding: 8
                    topPadding: 6
                    bottomPadding: 6
                    ContextMenu.menu: null
                    onCursorRectangleChanged: if (dragSelect.active) logPage.reveal(cursorRectangle)
                    onContentHeightChanged: logPage.queueDividers()

                    PointHandler { id: dragSelect; acceptedButtons: Qt.LeftButton }

                    HoverHandler { cursorShape: logText.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.IBeamCursor }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: (point) => {
                            textMenu.link = logText.linkAt(point.position.x, point.position.y)
                            const p = logText.mapToItem(logPage, point.position.x, point.position.y)
                            textMenu.popup(p.x, p.y)
                        }
                    }
                }
            }
        }
    }

    IconButton {
        anchors { right: logCard.right; bottom: logCard.bottom; margins: 14 }
        readonly property bool away: logPage.flick !== null
                                     && logPage.flick.contentY + logPage.flick.height < logPage.flick.contentHeight - 40
        visible: opacity > 0.01
        opacity: away ? 1 : 0
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
        onClicked: logPage.toEnd()
    }
}
