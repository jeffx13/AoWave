pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import App
import ".."

// Hidden unless a service is signed in.
Card {
    id: panel

    property string showLink: ""
    property string showTitle: ""

    // A plain function call is not a binding.
    property var services: []
    property int activeIndex: 0
    readonly property string activeService: activeIndex >= 0 && activeIndex < services.length
                                            ? services[activeIndex] : ""

    property var searchResults: []
    property var entry: ({ valid: false })
    property string status: ""

    // AniList reports its own scale, and a spinner fixed at 0-100 writes 85 into a POINT_10 list.
    readonly property var scoreScale: activeService !== "" ? App.trackers.scoreScale(activeService)
                                                      : ({ max: 10, step: 1, decimals: 0 })

    visible: services.length > 0
    Layout.fillWidth: true
    implicitHeight: visible ? col.implicitHeight + 24 : 0
    radius: 10

    Component.onCompleted: refreshServices()
    function refreshServices() {
        services = App.trackers.authenticatedNames()
        if (activeIndex >= services.length) activeIndex = 0
        reload()
    }

    function reload() {
        if (activeService === "" || showLink === "") { entry = { valid: false }; return }
        entry = App.library.trackerLink(showLink, activeService)
        queryField.text = panel.showTitle
        status = ""
        // Look the show up straight away rather than making the user search.
        if (!entry.valid && panel.showTitle !== "")
            search(panel.showTitle)
        else
            searchResults = []
    }
    function search(text) {
        if (text.trim() === "") return
        status = qsTr("Searching %1...").arg(activeService)
        App.trackers.search(activeService, text)
    }
    onActiveServiceChanged: reload()
    onShowLinkChanged: { activeIndex = 0; searchResults = []; reload() }

    Connections {
        target: App.trackers
        function onSearchFinished(tracker, results) {
            if (tracker !== panel.activeService) return
            panel.searchResults = results
            panel.status = results.length === 0 ? qsTr("No matches on %1.").arg(tracker) : ""
        }
        function onPushFinished(tracker, ok) {
            if (tracker !== panel.activeService) return
            panel.status = ok ? qsTr("Saved to %1.").arg(tracker)
                              : qsTr("%1 did not accept the update.").arg(tracker)
        }
        function onEntryLoaded(tracker, link, loaded) {
            if (tracker !== panel.activeService || link !== panel.showLink) return
            panel.entry = loaded
            if (loaded.valid)
                App.library.setTrackerLink(link, tracker, loaded.remoteId, loaded.status ?? "",
                                           loaded.score ?? 0, loaded.progress ?? 0)
        }
        function onChanged() { panel.refreshServices() }
    }

    ColumnLayout {
        id: col
        anchors { fill: parent; margins: 12 }
        spacing: 10

        Text {
            text: qsTr("Tracking")
            color: Theme.accent
            font { pixelSize: Globals.sp(Theme.labelSize); bold: true }
        }

        // One segment per signed-in service.
        Row {
            Layout.fillWidth: true
            spacing: 6
            Repeater {
                model: panel.services
                delegate: Rectangle {
                    id: seg
                    required property string modelData
                    required property int index
                    readonly property bool selected: panel.activeIndex === seg.index
                    width: Math.max(Globals.sp(90), segLabel.implicitWidth + 24)
                    height: Globals.sp(30)
                    radius: 8
                    color: selected ? Theme.accent : (segHover.hovered ? Theme.surfaceAlt : "transparent")
                    border.color: selected ? Theme.accent : Theme.border
                    border.width: 1
                    Text {
                        id: segLabel
                        anchors.centerIn: parent
                        text: seg.modelData
                        color: seg.selected ? Theme.onAccent : Theme.textSecondary
                        font.pixelSize: Globals.sp(Theme.metadataSize)
                        font.bold: seg.selected
                    }
                    HoverHandler { id: segHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: panel.activeIndex = seg.index }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            AppTextField {
                id: queryField
                Layout.fillWidth: true
                placeholderText: qsTr("Search this service...")
                onAccepted: panel.search(text)
            }
            AppButton {
                text: qsTr("Search")
                onClicked: panel.search(queryField.text)
            }
        }

        AppComboBox {
            id: resultBox
            Layout.fillWidth: true
            visible: panel.searchResults.length > 0
            model: panel.searchResults.map(function(r) {
                return r.title + (r.year ? "  (" + r.year + ")" : "")
                     + (r.episodes > 0 ? "  \u00b7 " + r.episodes + " ep" : "")
            })
            // An exact title match makes linking one click.
            onModelChanged: {
                const want = panel.showTitle.trim().toLowerCase()
                for (let i = 0; i < panel.searchResults.length; i++) {
                    if ((panel.searchResults[i].title ?? "").trim().toLowerCase() === want) {
                        currentIndex = i
                        return
                    }
                }
                currentIndex = panel.searchResults.length > 0 ? 0 : -1
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text {
                Layout.fillWidth: true
                text: panel.entry.valid ? qsTr("Linked") : qsTr("Not tracked")
                color: panel.entry.valid ? Theme.success : Theme.textMuted
                font.pixelSize: Globals.sp(Theme.metadataSize)
            }
            AppButton {
                text: panel.entry.valid ? qsTr("Relink") : qsTr("Link")
                enabled: resultBox.visible && resultBox.currentIndex >= 0
                onClicked: {
                    const picked = panel.searchResults[resultBox.currentIndex]
                    if (picked) App.trackers.link(panel.activeService, panel.showLink, picked.remoteId)
                }
            }
            AppButton {
                text: qsTr("Unlink")
                secondary: true
                visible: panel.entry.valid
                onClicked: {
                    App.library.clearTrackerLink(panel.showLink, panel.activeService)
                    panel.entry = { valid: false }
                }
            }
        }

        LabeledRow {
            visible: panel.entry.valid
            label: qsTr("Status")
            AppComboBox {
                id: statusBox
                Layout.preferredWidth: Globals.sp(180)
                model: App.trackers.statusVocabulary(panel.activeService)
                currentIndex: Math.max(0, model.indexOf(panel.entry.status ?? ""))
            }
        }

        LabeledRow {
            visible: panel.entry.valid
            label: qsTr("Score")
            sublabel: panel.scoreScale.decimals > 0 ? qsTr("0 - %1, one decimal").arg(panel.scoreScale.max)
                                               : qsTr("0 - %1").arg(panel.scoreScale.max)
            AppSpinBox {
                id: scoreBox
                from: 0
                to: panel.scoreScale.max
                value: Math.min(panel.scoreScale.max, Math.round(panel.entry.score ?? 0))
            }
        }

        LabeledRow {
            visible: panel.entry.valid
            label: qsTr("Episodes Watched")
            AppSpinBox {
                id: progressBox
                from: 0; to: 9999
                value: panel.entry.progress ?? 0
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: panel.entry.valid
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Save to %1").arg(panel.activeService)
                onClicked: {
                    App.library.setTrackerLink(panel.showLink, panel.activeService,
                                               panel.entry.remoteId,
                                               statusBox.currentText, scoreBox.value, progressBox.value)
                    panel.status = qsTr("Saving...")
                    App.trackers.pushEntry(panel.activeService, panel.entry.remoteId,
                                           statusBox.currentText, scoreBox.value, progressBox.value)
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: panel.status !== ""
            text: panel.status
            color: Theme.textSecondary
            wrapMode: Text.Wrap
            font.pixelSize: Globals.sp(Theme.metadataSize)
        }
    }
}
