pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import App

ColumnLayout {
    id: panel

    property string showLink: ""
    property string showTitle: ""
    readonly property string searchTitle: showTitle.replace(/[(\[【（][^)\]】）]*[)\]】）]/g, " ")
                                                   .replace(/\s+/g, " ").trim()
    property string provider: ""
    property int watched: 0
    signal linksChanged()

    property var services: App.trackers.authenticatedNames()

    visible: services.length > 0
    Layout.fillWidth: true
    spacing: 12

    Connections {
        target: App.trackers
        function onChanged() { panel.services = App.trackers.authenticatedNames() }
    }

    Repeater {
        model: panel.services
        delegate: Section {}
    }

    component Caption: Text {
        color: Theme.textMuted
        font.pixelSize: Ui.sp(Theme.metadataSize)
        elide: Text.ElideRight
        Layout.fillWidth: true
    }

    component Section: Card {
        id: sec
        required property string modelData
        readonly property string service: modelData
        readonly property var features: App.trackers.features(service)
        readonly property var scoreScale: features.scale ?? ({ max: 10, decimals: 0 })
        readonly property int mult: Math.pow(10, scoreScale.decimals ?? 0)
        readonly property color accent: features.accent ?? Theme.accent

        property bool linked: false
        property bool searching: false
        property bool busy: false
        property var results: []
        property var entry: ({ valid: false })
        property var draft: ({})
        property string note: ""
        property bool expanded: false
        readonly property string summary: {
            if (!entry.valid) return ""
            const at = (features.statuses ?? []).indexOf(entry.status ?? "")
            const parts = []
            if (at >= 0 && features.labels) parts.push(features.labels[at])
            parts.push((entry.total ?? 0) > 0 ? (entry.progress ?? 0) + "/" + entry.total : String(entry.progress ?? 0))
            if ((entry.score ?? 0) > 0) parts.push("\u2605 " + entry.score)
            return parts.join("  \u00b7  ")
        }

        Layout.fillWidth: true
        implicitHeight: col.implicitHeight + 28
        clip: true
        radius: 12
        border.color: Qt.alpha(accent, 0.45)

        function today() { return Qt.formatDate(new Date(), "yyyy-MM-dd") }

        function reload() {
            results = []; note = ""; busy = false; expanded = false
            entry = { valid: false }; draft = {}
            const local = App.library.trackerLink(panel.showLink, service)
            linked = local.valid
            searching = !linked
            if (panel.showLink === "") return
            if (linked) open(local.remoteId)
            else if (service === "AniList" && panel.provider === "Miruro")
                results = [{ remoteId: panel.showLink, title: panel.showTitle, year: "", episodes: 0 }]
            else search(panel.searchTitle)
        }
        function open(remoteId) {
            busy = true
            note = qsTr("Loading…")
            App.trackers.link(service, panel.showLink, remoteId)
        }
        function search(text) {
            if (text.trim() === "") return
            busy = true
            note = qsTr("Searching…")
            App.trackers.search(service, text)
        }
        function set(key, value) {
            const d = Object.assign({}, draft)
            d[key] = value
            const statuses = features.statuses
            if (key === "progress" && d.total > 0 && value >= d.total && d.status !== statuses[2])
                d.status = statuses[2]
            if (d.status === statuses[2] && draft.status !== statuses[2]) {
                if (!d.finished) d.finished = today()
                if (d.total > 0) d.progress = d.total
            }
            if (d.status === statuses[0] && !d.started) d.started = today()
            draft = d
        }
        function save() {
            const out = Object.assign({}, draft, { onList: true })
            if (!entry.onList && !out.status) out.status = features.statuses[1]
            busy = true
            note = qsTr("Saving…")
            App.trackers.pushEntry(service, panel.showLink, out)
        }

        Component.onCompleted: reload()
        Connections {
            target: panel
            function onShowLinkChanged() { sec.reload() }
        }
        Connections {
            target: App.trackers
            function onSearchFinished(tracker, results) {
                if (tracker !== sec.service) return
                sec.busy = false
                sec.results = results
                sec.note = results.length === 0 ? qsTr("No matches.") : ""
            }
            function onEntryLoaded(tracker, link, loaded) {
                if (tracker !== sec.service || link !== panel.showLink) return
                sec.busy = false
                if (!loaded.valid) { sec.note = qsTr("%1 did not respond.").arg(tracker); return }
                sec.entry = loaded
                sec.draft = Object.assign({}, loaded)
                if (!loaded.onList) {
                    sec.set("status", sec.features.statuses[panel.watched > 0 ? 0 : 1])
                    sec.set("progress", Math.min(panel.watched, loaded.total > 0 ? loaded.total : panel.watched))
                }
                sec.linked = true
                sec.searching = false
                if (!loaded.onList) sec.expanded = true
                panel.linksChanged()
                sec.note = loaded.onList ? "" : qsTr("Not on your list yet.")
            }
            function onPushFinished(tracker, link, ok, error, saved, automatic) {
                if (tracker !== sec.service || link !== panel.showLink) return
                sec.busy = false
                if (!ok) { sec.note = error !== "" ? error : qsTr("Not saved."); return }
                sec.entry = Object.assign({}, saved, { onList: true, remoteProgress: saved.progress })
                sec.draft = Object.assign({}, sec.entry)
                sec.note = automatic ? qsTr("Episode %1 synced.").arg(saved.progress) : qsTr("Saved.")
            }
        }

        ColumnLayout {
            id: col
            anchors { fill: parent; margins: 14 }
            spacing: 10

            RowLayout {
                id: headerRow
                Layout.fillWidth: true
                spacing: 8
                TapHandler {
                    enabled: sec.entry.valid && !sec.searching
                    onTapped: (point) => {
                        const hit = headerRow.childAt(point.position.x, point.position.y)
                        if (hit && "clicked" in hit) return
                        sec.expanded = !sec.expanded
                    }
                }
                HoverHandler { cursorShape: sec.entry.valid && !sec.searching ? Qt.PointingHandCursor : Qt.ArrowCursor }
                Rectangle { implicitWidth: 4; implicitHeight: Ui.sp(20); radius: 2; color: sec.accent }
                Text {
                    text: sec.service
                    color: sec.accent
                    font { pixelSize: Ui.sp(Theme.labelSize); bold: true }
                }
                Text {
                    Layout.fillWidth: true
                    text: sec.entry.valid ? sec.entry.title : (sec.linked ? "" : qsTr("Not linked"))
                    color: Theme.textSecondary
                    elide: Text.ElideRight
                    font.pixelSize: Ui.sp(Theme.metadataSize)
                }
                Text {
                    visible: !sec.expanded && !sec.searching && sec.summary !== ""
                    text: sec.summary
                    color: Theme.textPrimary
                    font.pixelSize: Ui.sp(Theme.metadataSize)
                }
                AppSpinner { visible: sec.busy; running: sec.busy; radius: 8; color: sec.accent }
                IconButton {
                    visible: sec.entry.valid && (sec.entry.siteUrl ?? "") !== ""
                    iconName: "external-link"
                    iconSize: 16
                    tip: qsTr("Open on %1").arg(sec.service)
                    onClicked: Qt.openUrlExternally(sec.entry.siteUrl)
                }
                AppButton {
                    visible: sec.linked && !sec.searching && (sec.expanded || (!sec.busy && !sec.entry.valid))
                    text: qsTr("Change")
                    secondary: true
                    onClicked: { sec.searching = true; if (sec.results.length === 0) sec.search(panel.searchTitle) }
                }
                AppButton {
                    visible: sec.linked && (sec.expanded || sec.searching || (!sec.busy && !sec.entry.valid))
                    text: qsTr("Unlink")
                    secondary: true
                    onClicked: {
                        App.library.clearTrackerLink(panel.showLink, sec.service)
                        panel.linksChanged()
                        sec.linked = false
                        sec.entry = { valid: false }
                        sec.draft = {}
                        sec.searching = true
                        if (sec.results.length === 0) sec.search(panel.searchTitle)
                    }
                }
                AppIcon {
                    visible: sec.entry.valid && !sec.searching
                    name: "chevron-down"
                    size: Ui.sp(16)
                    color: Theme.textMuted
                    rotation: sec.expanded ? 180 : 0
                    Behavior on rotation { NumberAnimation { duration: 120 } }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: sec.searching
                spacing: 6
                AppTextField {
                    id: queryField
                    Layout.fillWidth: true
                    text: panel.searchTitle
                    placeholderText: qsTr("Search %1…").arg(sec.service)
                    onAccepted: sec.search(text)
                }
                AppButton { text: qsTr("Search"); onClicked: sec.search(queryField.text) }
            }
            RowLayout {
                Layout.fillWidth: true
                visible: sec.searching && sec.results.length > 0
                spacing: 6
                AppComboBox {
                    id: resultBox
                    Layout.fillWidth: true
                    model: sec.results.map(r => r.title + (r.year ? "  (" + r.year + ")" : "")
                                               + (r.episodes > 0 ? "  · " + r.episodes + " ep" : ""))
                    onModelChanged: {
                        const want = panel.searchTitle.toLowerCase()
                        const exact = sec.results.findIndex(r => (r.title ?? "").trim().toLowerCase() === want)
                        currentIndex = exact >= 0 ? exact : (sec.results.length > 0 ? 0 : -1)
                    }
                }
                AppButton {
                    text: qsTr("Link")
                    enabled: resultBox.currentIndex >= 0
                    onClicked: sec.open(sec.results[resultBox.currentIndex].remoteId)
                }
            }

            GridLayout {
                id: grid
                Layout.fillWidth: true
                visible: sec.entry.valid && !sec.searching && sec.expanded
                columns: Math.max(1, Math.min(3, Math.floor(width / Ui.sp(220))))
                uniformCellWidths: true
                columnSpacing: 12
                rowSpacing: 8

                ColumnLayout {
                    Layout.fillWidth: true
                    Caption { text: qsTr("Status") }
                    AppComboBox {
                        Layout.fillWidth: true
                        model: sec.features.labels ?? []
                        currentIndex: (sec.features.statuses ?? []).indexOf(sec.draft.status ?? "")
                        onActivated: (index) => sec.set("status", sec.features.statuses[index])
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Caption {
                        text: sec.scoreScale.decimals > 0 ? qsTr("Score (0 - %1, one decimal)").arg(sec.scoreScale.max)
                                                     : qsTr("Score (0 - %1)").arg(sec.scoreScale.max)
                    }
                    AppSpinBox {
                        Layout.fillWidth: true
                        from: 0
                        to: sec.scoreScale.max * sec.mult
                        value: Math.round((sec.draft.score ?? 0) * sec.mult)
                        textFromValue: (v) => (v / sec.mult).toFixed(sec.scoreScale.decimals ?? 0)
                        valueFromText: (t) => Math.round(parseFloat(t) * sec.mult)
                        onValueModified: sec.set("score", value / sec.mult)
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Caption {
                        text: (sec.draft.total ?? 0) > 0 ? qsTr("Episodes (of %1)").arg(sec.draft.total)
                                                         : qsTr("Episodes")
                    }
                    AppSpinBox {
                        Layout.fillWidth: true
                        from: 0
                        to: (sec.draft.total ?? 0) > 0 ? sec.draft.total : 9999
                        value: sec.draft.progress ?? 0
                        onValueModified: sec.set("progress", value)
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: sec.features.rewatches ?? false
                    Caption { text: qsTr("Rewatches") }
                    AppSpinBox {
                        Layout.fillWidth: true
                        from: 0; to: 999
                        value: sec.draft.repeat ?? 0
                        onValueModified: sec.set("repeat", value)
                    }
                }
                Repeater {
                    model: (sec.features.dates ?? false) ? [
                        { key: "started", label: qsTr("Started on") },
                        { key: "finished", label: qsTr("Finished on") }
                    ] : []
                    delegate: ColumnLayout {
                        id: dateCell
                        required property var modelData
                        Layout.fillWidth: true
                        Caption { text: dateCell.modelData.label }
                        AppTextField {
                            Layout.fillWidth: true
                            placeholderText: "YYYY-MM-DD"
                            text: sec.draft[dateCell.modelData.key] ?? ""
                            validator: RegularExpressionValidator { regularExpression: /^\d{0,4}(-\d{0,2}(-\d{0,2})?)?$/ }
                            onTextEdited: if (/^(\d{4}(-\d{2}(-\d{2})?)?)?$/.test(text))
                                              sec.set(dateCell.modelData.key, text)
                        }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: sec.features.favourite ?? false
                    Caption { text: qsTr("Favourite") }
                    IconButton {
                        iconName: sec.draft.favourite ? "heart-filled" : "heart"
                        iconColor: sec.draft.favourite ? "#E85D75" : Theme.textMuted
                        iconHoverColor: "#E85D75"
                        tip: sec.draft.favourite ? qsTr("Remove from favourites") : qsTr("Add to favourites")
                        onClicked: sec.set("favourite", !sec.draft.favourite)
                    }
                }
            }

            AppTextField {
                Layout.fillWidth: true
                visible: grid.visible && (sec.features.notes ?? false)
                placeholderText: qsTr("Notes on %1").arg(sec.service)
                text: sec.draft.notes ?? ""
                onTextEdited: sec.set("notes", text)
            }

            RowLayout {
                Layout.fillWidth: true
                visible: sec.expanded || sec.searching || sec.note !== ""
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    visible: sec.note !== ""
                    text: sec.note
                    color: Theme.textSecondary
                    wrapMode: Text.Wrap
                    font.pixelSize: Ui.sp(Theme.metadataSize)
                }
                Item { Layout.fillWidth: true; visible: sec.note === "" }
                AppButton {
                    visible: grid.visible
                    enabled: !sec.busy
                    text: sec.entry.onList ? qsTr("Save") : qsTr("Add to list")
                    onClicked: sec.save()
                }
            }
        }
    }
}
