pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App

AppPopup {
    id: dialog
    modal: true
    focus: true
    padding: 18
    clip: true
    closePolicy: Popup.CloseOnEscape
    backgroundRadius: 14

    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    width:  Math.min(640, parent ? parent.width  - 60 : 640)
    height: Math.min(660, parent ? parent.height - 60 : 660)

    property string showLink: ""
    property string showTitle: ""
    property string currentProvider: ""
    property int    resumePrefill: 1

    readonly property int rowHeight: 34

    function openFor(index)   { openWith(App.library.entryAt(index)) }
    function openForLink(link) { openWith(App.library.entryForLink(link)) }

    function openWith(e) {
        if (!e.valid) return
        dialog.showLink = e.link
        dialog.showTitle = e.title || ""
        dialog.currentProvider = e.provider || ""
        dialog.resumePrefill = (e.lastWatchedIndex >= 0 ? e.lastWatchedIndex : 0) + 1
        resumeSpin.value = dialog.resumePrefill
        searchField.text = dialog.showTitle
        resultsView.currentIndex = -1
        App.migrateSearch.cancel()

        providerCombo.currentIndex = 0
        for (let i = 0; i < providerCombo.count; i++) {
            if (providerCombo.textAt(i) !== dialog.currentProvider) {
                providerCombo.currentIndex = i
                break
            }
        }
        dialog.open()
        dialog.runSearch()
    }

    function runSearch() {
        const q = searchField.text.trim()
        if (q.length === 0) return
        resultsView.currentIndex = -1
        App.searchOnProvider(providerCombo.currentText, q)
    }

    function step(delta) {
        const count = resultsView.count
        if (count <= 0 || App.migrateSearch.isLoading) return
        resultsView.currentIndex = Math.max(0, Math.min(count - 1, resultsView.currentIndex + delta))
        resultsView.positionViewAtIndex(resultsView.currentIndex, ListView.Contain)
    }

    function migrate() {
        if (resultsView.currentIndex < 0 || App.migrateSearch.isLoading) return
        App.migrateShow(dialog.showLink, resultsView.currentIndex, resumeSpin.value)
        dialog.close()
    }

    onOpened: searchField.forceActiveFocus()

    contentItem: ColumnLayout {
        spacing: 10

        Text {
            Layout.fillWidth: true
            text: qsTr("Migrate %1").arg(dialog.showTitle)
            color: Theme.textPrimary
            font.pixelSize: Ui.sp(Theme.bodySize)
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }

        Text {
            Layout.fillWidth: true
            Layout.bottomMargin: 2
            visible: dialog.currentProvider.length > 0
            text: qsTr("Currently on %1 · pick the same show on another provider").arg(dialog.currentProvider)
            color: Theme.textMuted
            font.pixelSize: Ui.sp(Theme.metadataSize)
            elide: Text.ElideRight
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            AppComboBox {
                id: providerCombo
                text: "text"
                fontSize: Theme.metadataSize
                model: App.providers
                currentIndex: 0
                implicitHeight: dialog.rowHeight
                Layout.preferredWidth: 150
                onActivated: dialog.runSearch()
            }

            AppTextField {
                id: searchField
                placeholderText: qsTr("Search title…")
                showClearButton: true
                fontSize: Theme.metadataSize
                implicitHeight: dialog.rowHeight
                Layout.fillWidth: true
                onAccepted: dialog.runSearch()
                Keys.onDownPressed: {
                    if (resultsView.count === 0 || App.migrateSearch.isLoading) return
                    resultsView.forceActiveFocus()
                    dialog.step(1)
                }
            }

            AppButton {
                text: qsTr("Search")
                fontSize: Theme.metadataSize
                radius: 10
                leftPadding: 16
                rightPadding: 16
                implicitHeight: dialog.rowHeight
                onClicked: dialog.runSearch()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 320
            radius: 10
            color: Theme.surfaceDeep
            border.color: Theme.border
            border.width: 1
            clip: true

            ListView {
                id: resultsView
                anchors.fill: parent
                anchors.margins: 4
                visible: !App.migrateSearch.isLoading
                model: App.migrateSearch
                spacing: 4
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: AppScrollBar {}
                currentIndex: -1
                activeFocusOnTab: true

                Keys.onPressed: event => {
                    if (event.key === Qt.Key_Down) {
                        dialog.step(1)
                    } else if (event.key === Qt.Key_Up) {
                        if (resultsView.currentIndex <= 0) searchField.forceActiveFocus()
                        else dialog.step(-1)
                    } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        dialog.migrate()
                    } else {
                        return
                    }
                    event.accepted = true
                }

                delegate: ItemDelegate {
                    id: row
                    required property int index
                    required property string title
                    required property string cover
                    required property string latestTxt
                    width: ListView.view.width
                    height: 78
                    padding: 8

                    background: Rectangle {
                        radius: 8
                        color: row.ListView.isCurrentItem ? Qt.alpha(Theme.accent, 0.18)
                             : row.hovered ? Theme.surfaceAlt : "transparent"
                        border.color: row.ListView.isCurrentItem ? Theme.accent : "transparent"
                        border.width: 1
                        Behavior on color { ColorAnimation { duration: 80 } }
                    }

                    contentItem: RowLayout {
                        spacing: 10

                        CoverTile {
                            Layout.preferredWidth: 44
                            Layout.preferredHeight: 62
                            Layout.alignment: Qt.AlignVCenter
                            color: Theme.surface
                            source: row.cover
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 3
                            Text {
                                Layout.fillWidth: true
                                text: row.title
                                color: Theme.textPrimary
                                font.pixelSize: Ui.sp(Theme.compactSize)
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }
                            Text {
                                visible: row.latestTxt.length > 0
                                text: row.latestTxt
                                color: row.ListView.isCurrentItem ? Theme.textSecondary : Theme.textMuted
                                font.pixelSize: Ui.sp(Theme.metadataSize)
                            }
                        }

                        AppIcon {
                            visible: row.ListView.isCurrentItem
                            Layout.alignment: Qt.AlignVCenter
                            name: "check"
                            size: 18
                            color: Theme.accent
                        }
                    }

                    onClicked: resultsView.currentIndex = row.index
                }
            }

            FocusRing { shown: resultsView.activeFocus; outset: 0; radius: 10 }

            AppSpinner {
                anchors.centerIn: parent
                visible: App.migrateSearch.isLoading
                running: visible
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - 40
                visible: !App.migrateSearch.isLoading && App.migrateSearch.count === 0
                text: App.migrateSearch.failure !== "" ? App.migrateSearch.failure
                                                       : qsTr("Search a provider to find the matching show")
                color: Theme.textMuted
                font.pixelSize: Ui.sp(Theme.compactSize)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Text {
                visible: resultsView.currentIndex >= 0
                text: qsTr("Resume at episode")
                color: Theme.textSecondary
                font.pixelSize: Ui.sp(Theme.metadataSize)
                Layout.alignment: Qt.AlignVCenter
            }

            AppSpinBox {
                id: resumeSpin
                visible: resultsView.currentIndex >= 0
                from: 1
                to: 9999
                value: dialog.resumePrefill
                implicitHeight: dialog.rowHeight
                implicitWidth: 100
                Layout.alignment: Qt.AlignVCenter
            }

            Item { Layout.fillWidth: true }

            AppButton {
                text: qsTr("Cancel")
                secondary: true
                fontSize: Theme.metadataSize
                radius: 10
                leftPadding: 18
                rightPadding: 18
                implicitHeight: dialog.rowHeight
                onClicked: dialog.close()
            }

            AppButton {
                text: qsTr("Migrate")
                fontSize: Theme.metadataSize
                radius: 10
                enabled: resultsView.currentIndex >= 0 && !App.migrateSearch.isLoading
                leftPadding: 18
                rightPadding: 18
                implicitHeight: dialog.rowHeight
                onClicked: dialog.migrate()
            }
        }
    }
}
