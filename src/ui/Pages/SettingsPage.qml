pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtQuick.Layouts
import App
import QtQuick.Dialogs

Page {
    id: settingsPage
    focus: true
    background: Rectangle { color: "transparent" }

    readonly property string query: searchField.text.trim().toLowerCase()

    function textOf(item) {
        let text = ""
        for (const key of ["title", "label", "sublabel", "text"])
            if (typeof item[key] === "string") text += item[key].toLowerCase() + "\n"
        for (const child of item.children) text += textOf(child)
        return text
    }

    function find(q) {
        const found = []
        const walk = (item, card) => {
            const isCard = typeof item.category === "string" && typeof item.title === "string"
            if (!isCard && card && typeof item.label === "string" && item.label.toLowerCase().includes(q)
                    && !found.some(entry => entry.label === item.label))
                found.push({ label: item.label, card: card.title })
            for (const child of item.children) walk(child, isCard ? item : card)
        }
        walk(rootCol, null)
        return found
    }
    function reveal(label) { searchField.text = label }

    function matchCount(category) {
        let count = 0
        for (const child of rootCol.children)
            if (child.category === category && child.title !== undefined && textOf(child).includes(query)) ++count
        return count
    }

    property string category: "appearance"
    onCategoryChanged: settingsScroll.contentY = 0
    readonly property var categories: [
        { id: "appearance", icon: "eye", name: qsTr("Appearance") },
        { id: "player", icon: "play", name: qsTr("Player") },
        { id: "accounts", icon: "refresh-cw", name: qsTr("Accounts & sync") },
        { id: "providers", icon: "server", name: qsTr("Providers") },
        { id: "integrations", icon: "captions", name: qsTr("Integrations") },
        { id: "network", icon: "arrow-up-down", name: qsTr("Network") },
        { id: "shortcuts", icon: "details", name: qsTr("Keyboard shortcuts") }
    ]

    header: Item {
        height: 48
        Text {
            anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 16 }
            text: qsTr("Settings"); color: Theme.textPrimary; font { pixelSize: Ui.sp(Theme.headingSize); bold: true }
        }
    }

    FileDialog {
        id: runtimeDialog
        title: qsTr("JavaScript runtime")
        nameFilters: [qsTr("Deno or Node (deno.exe node.exe)")]
        onAccepted: App.useJsRuntime(selectedFile)
    }

    FolderDialog {
        id: animeJaNaiDialog
        title: qsTr("AnimeJaNai")
        onAccepted: App.settings.animeJaNaiDir = decodeURIComponent(selectedFolder.toString().replace(/^file:\/\/\//, ""))
    }

    ColorDialog {
        id: accentDialog
        onAccepted: App.settings.accentColor = selectedColor.toString()
    }

    Component {
        id: themeSwatch
        Rectangle {
            id: swatch
            required property var modelData
            readonly property var colours: Theme.palette(swatch.modelData.name)
            readonly property bool active: Theme.resolvedName === swatch.modelData.name

            width: 160; height: swatchCol.implicitHeight + 20; radius: 12
            gradient: Gradient {
                GradientStop { position: 0.0; color: swatch.colours.background }
                GradientStop { position: 1.0; color: swatch.colours.bgBottom }
            }
            border.color: swatch.active ? Theme.accent : swatchHover.hovered ? Theme.borderStrong : swatch.colours.border
            border.width: swatch.active ? 2 : 1
            scale: swatchHover.hovered ? 1.03 : 1.0
            Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

            Column {
                id: swatchCol
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 10 }
                spacing: 8
                Row {
                    spacing: 5
                    Rectangle {
                        width: 24; height: 14; radius: 4
                        color: swatch.colours.surface
                        border.color: swatch.colours.border
                        border.width: 1
                    }
                    Rectangle {
                        width: 44; height: 14; radius: 4
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: swatch.colours.accent }
                            GradientStop { position: 1.0; color: swatch.colours.accent2 }
                        }
                    }
                    Rectangle { width: 14; height: 14; radius: 4; color: swatch.colours.success }
                    Rectangle { width: 14; height: 14; radius: 4; color: swatch.colours.danger }
                }
                Row {
                    width: parent.width
                    spacing: 6
                    Text {
                        width: parent.width - aaSample.width - parent.spacing
                        text: swatch.modelData.label
                        color: swatch.colours.textPrimary
                        font.pixelSize: Ui.sp(Theme.metadataSize)
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }
                    Text {
                        id: aaSample
                        text: "Aa"
                        color: swatch.colours.textMuted
                        font.pixelSize: Ui.sp(Theme.metadataSize)
                    }
                }
            }
            AppIcon {
                visible: swatch.active
                anchors { top: parent.top; right: parent.right; topMargin: 7; rightMargin: 8 }
                name: "check"; size: 16; color: Theme.accent
            }
            HoverHandler { id: swatchHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: App.settings.themeName = swatch.modelData.name }
        }
    }

    component AccountRow: RowLayout {
        id: acct
        property string service: ""
        property bool   signedIn: false
        property string account: ""
        property string offlineHint: ""
        signal signIn()
        signal signOut()

        Layout.fillWidth: true
        spacing: 10

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1
            Text {
                text: acct.service
                color: Theme.textSecondary
                font.pixelSize: Ui.sp(Theme.bodySize)
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Text {
                Layout.fillWidth: true
                text: !acct.signedIn ? acct.offlineHint
                     : acct.account !== "" ? qsTr("Signed in as %1").arg(acct.account)
                                           : qsTr("Signed in")
                color: acct.signedIn && acct.account !== "" ? Theme.textAccent : Theme.textMuted
                font.pixelSize: Ui.sp(15)
                font.weight: acct.signedIn && acct.account !== "" ? Font.Medium : Font.Normal
                elide: Text.ElideRight
            }
        }

        AppButton {
            text: acct.signedIn ? qsTr("Sign out") : qsTr("Sign in")
            secondary: acct.signedIn
            onClicked: acct.signedIn ? acct.signOut() : acct.signIn()
        }
    }

    component SettingsCard: Card {
        id: card
        property alias title: titleText.text
        property string category
        visible: settingsPage.query === "" ? category === settingsPage.category
                                           : settingsPage.textOf(card).includes(settingsPage.query)
        // `data`, not `children`: a Connections declared in a card cannot assign to a QQuickItem list.
        default property alias content: cardCol.data
        Layout.fillWidth: true
        implicitHeight: cardCol.implicitHeight + 24
        radius: 10

        ColumnLayout {
            id: cardCol
            anchors { fill: parent; margins: 12 }
            spacing: 10
            Text { id: titleText; color: Theme.accent; font { pixelSize: Ui.sp(Theme.labelSize); bold: true } }
        }
    }

    ColumnLayout {
        id: rail
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom; margins: 16; rightMargin: 0 }
        width: Ui.sp(220)
        spacing: 4

        AppTextField {
            id: searchField
            Layout.fillWidth: true
            Layout.bottomMargin: 8
            placeholderText: qsTr("Search settings")
            showClearButton: true
        }

        Repeater {
            model: settingsPage.categories
            delegate: Rectangle {
                id: railItem
                required property var modelData
                readonly property bool current: settingsPage.query === "" && settingsPage.category === modelData.id
                readonly property int matches: settingsPage.query === "" ? -1 : settingsPage.matchCount(modelData.id)
                Layout.fillWidth: true
                implicitHeight: Ui.sp(40)
                radius: 10
                opacity: matches === 0 ? 0.4 : 1
                color: current ? Theme.accentSoft : railHover.hovered ? Theme.hoverFill : "transparent"
                Behavior on color { ColorAnimation { duration: 120 } }
                Accessible.role: Accessible.PageTab
                Accessible.name: modelData.name
                HoverHandler { id: railHover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    onTapped: {
                        searchField.text = ""
                        settingsPage.category = railItem.modelData.id
                    }
                }
                RowLayout {
                    anchors { fill: parent; leftMargin: 12; rightMargin: 12 }
                    spacing: 10
                    AppIcon {
                        name: railItem.modelData.icon
                        size: 18
                        color: railItem.current ? Theme.accent : Theme.textMuted
                    }
                    Text {
                        Layout.fillWidth: true
                        text: railItem.modelData.name
                        color: railItem.current ? Theme.accent : Theme.textSecondary
                        font.pixelSize: Ui.sp(Theme.bodySize)
                        font.weight: railItem.current ? Font.DemiBold : Font.Normal
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        visible: railItem.matches > 0
                        implicitWidth: Math.max(implicitHeight, countText.implicitWidth + 12)
                        implicitHeight: Ui.sp(20)
                        radius: implicitHeight / 2
                        color: Theme.accentSoft
                        Text {
                            id: countText
                            anchors.centerIn: parent
                            text: railItem.matches
                            color: Theme.accent
                            font.pixelSize: Ui.sp(13)
                            font.bold: true
                        }
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }
    }

    Flickable {
        id: settingsScroll
        anchors { left: rail.right; right: parent.right; top: parent.top; bottom: parent.bottom }
        contentHeight: rootCol.implicitHeight + 32
        clip: true; boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: AppScrollBar { }

        ColumnLayout {
            id: rootCol
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 16; rightMargin: 30 }
            spacing: 12

            EmptyState {
                visible: settingsPage.query !== "" && !rootCol.children.some(child => child.visible && child.title !== undefined)
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 48
                icon: "search"
                title: qsTr("No settings match")
                hint: qsTr("Try another word.")
            }

            SettingsCard {
                title: qsTr("Appearance")
                category: "appearance"

                LabeledRow {
                    label: qsTr("App font")
                    AppComboBox {
                        Layout.preferredWidth: Math.min(280, settingsPage.width / 3)
                        model: [qsTr("Default")].concat(App.settings.fontFamilies)
                        currentIndex: App.settings.appFont === "" ? 0 : App.settings.fontFamilies.indexOf(App.settings.appFont) + 1
                        onActivated: App.settings.appFont = currentIndex === 0 ? "" : App.settings.fontFamilies[currentIndex - 1]
                    }
                }

                LabeledRow {
                    label: qsTr("Interface language")
                    AppComboBox {
                        property var codes: ["en", "zh_CN", "zh_TW", "ja", "ko"]
                        model: ["English", "简体中文", "繁體中文", "日本語", "한국어"]
                        currentIndex: codes.indexOf(App.settings.uiLanguage)
                        onActivated: App.settings.uiLanguage = codes[currentIndex]
                    }
                }

                LabeledRow {
                    label: qsTr("Reduce motion")
                    AppSwitch { checked: App.settings.reduceMotion; onToggled: App.settings.reduceMotion = checked }
                }

                LabeledRow {
                    label: qsTr("High contrast")
                    sublabel: qsTr("Stronger text and borders, in every theme")
                    AppSwitch { checked: App.settings.highContrast; onToggled: App.settings.highContrast = checked }
                }

                Text { text: qsTr("Theme"); color: Theme.textSecondary; font.pixelSize: Ui.sp(Theme.labelSize) }

                Text { text: qsTr("Dark"); color: Theme.textMuted; font.pixelSize: Ui.sp(Theme.compactSize) }
                Flow {
                    Layout.fillWidth: true
                    spacing: 10
                    Repeater { model: Theme.swatches(false); delegate: themeSwatch }
                }

                Text { text: qsTr("Light"); color: Theme.textMuted; font.pixelSize: Ui.sp(Theme.compactSize) }
                Flow {
                    Layout.fillWidth: true
                    spacing: 10
                    Repeater { model: Theme.swatches(true); delegate: themeSwatch }
                }

                RowLayout {
                    Layout.fillWidth: true; spacing: 10
                    Text { text: qsTr("Accent"); color: Theme.textSecondary; font.pixelSize: Ui.sp(Theme.labelSize) }
                    Rectangle { Layout.preferredWidth: 28; Layout.preferredHeight: 28; radius: 6; color: Theme.accent; border.color: Theme.border; border.width: 1 }
                    Item { Layout.fillWidth: true }
                    AppButton { text: qsTr("Change"); onClicked: { accentDialog.selectedColor = Theme.accent; accentDialog.open() } }
                    AppButton { text: qsTr("Reset"); secondary: true; onClicked: App.settings.accentColor = "" }
                }

                LabeledRow {
                    label: qsTr("Window size")
                    sublabel: qsTr("Applies when not maximised")
                    AppComboBox {
                        placeholderText: qsTr("Custom (%1 x %2)").arg(App.settings.windowWidth).arg(App.settings.windowHeight)
                        Layout.preferredWidth: Math.min(280, settingsPage.width / 3)
                        readonly property var presets: Ui.sizePresets.filter(
                            (p) => p.w <= Screen.desktopAvailableWidth && p.h <= Screen.desktopAvailableHeight)
                        model: presets.map((p) => p.w + " x " + p.h)
                        currentIndex: {
                            for (let i = 0; i < presets.length; i++)
                                if (presets[i].w === App.settings.windowWidth
                                    && presets[i].h === App.settings.windowHeight) return i
                            return -1
                        }
                        onActivated: {
                            const preset = presets[currentIndex]
                            if (!preset) return
                            App.settings.windowWidth = preset.w
                            App.settings.windowHeight = preset.h
                        }
                    }
                }

                LabeledSlider {
                    label: qsTr("UI scale")
                    sublabel: qsTr("Size of text and controls")
                    from: 0.8; to: 1.4; stepSize: 0.05
                    unitSuffix: "x"; decimals: 2
                    value: App.settings.uiScale
                    commitOnRelease: true
                    onMoved: (v) => App.settings.uiScale = v
                }

                LabeledSlider {
                    label: qsTr("Card size")
                    sublabel: qsTr("Covers in Explore and the library")
                    from: 60; to: 200; stepSize: 10
                    unitSuffix: "%"
                    value: App.settings.cardSize
                    onMoved: (v) => App.settings.cardSize = v
                }
            }

            Repeater {
                model: App.providers.accounts
                delegate: SettingsCard {
                    id: accountCard
                    required property var modelData
                    readonly property var site: modelData
                    title: site.label
                    category: "accounts"

                    AccountSummary { account: accountCard.site }

                    Text {
                        Layout.fillWidth: true
                        visible: !accountCard.site.signedIn && accountCard.site.loginStatus === ""
                        text: accountCard.site.hint
                        color: Theme.textMuted
                        font.pixelSize: Ui.sp(Theme.compactSize)
                        wrapMode: Text.Wrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: accountCard.site.pastePrompt !== "" && accountCard.site.loginStatus === ""
                        spacing: 6
                        AppTextField {
                            id: sessionField
                            Layout.fillWidth: true
                            placeholderText: accountCard.site.pastePrompt
                            onAccepted: {
                                if (accountCard.site.signInWithText(text)) text = ""
                                Qt.callLater(() => focus = false)
                            }
                        }
                        AppButton {
                            text: qsTr("Use session")
                            secondary: true
                            enabled: sessionField.text.trim() !== ""
                            onClicked: if (accountCard.site.signInWithText(sessionField.text)) sessionField.text = ""
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        visible: accountCard.site.loginStatus !== ""
                        spacing: 14

                        Rectangle {
                            Layout.preferredWidth: Ui.sp(148)
                            Layout.preferredHeight: Ui.sp(148)
                            radius: 8
                            color: "white"
                            visible: accountCard.site.loginQr !== ""
                            Image {
                                anchors.fill: parent
                                anchors.margins: 4
                                source: accountCard.site.loginQr
                                smooth: false
                                mipmap: false
                                fillMode: Image.PreserveAspectFit
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6

                            Text {
                                Layout.fillWidth: true
                                text: accountCard.site.loginStatus
                                color: Theme.textPrimary
                                font.pixelSize: Ui.sp(Theme.bodySize)
                                font.weight: Font.Medium
                                wrapMode: Text.Wrap
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("In the Bilibili app, tap the scanner in the top corner of the home screen "
                                           + "and scan this code. It expires in three minutes.")
                                color: Theme.textMuted
                                font.pixelSize: Ui.sp(Theme.compactSize)
                                wrapMode: Text.Wrap
                            }
                            RowLayout {
                                spacing: 6
                                AppButton {
                                    text: qsTr("Copy link for phone")
                                    secondary: true
                                    onClicked: accountCard.site.copyLoginLink()
                                }
                                AppButton {
                                    text: qsTr("Cancel")
                                    secondary: true
                                    onClicked: accountCard.site.cancelLogin()
                                }
                            }
                        }
                    }

                    LabeledRow {
                        visible: accountCard.site.syncsProgress
                        enabled: accountCard.site.signedIn
                        opacity: enabled ? 1.0 : 0.45
                        label: qsTr("Sync progress with Bilibili")
                        sublabel: qsTr("Sends your playback position to your account")
                        AppSwitch { checked: App.settings.bilibiliSync; onToggled: App.settings.bilibiliSync = checked }
                    }

                    Repeater {
                        model: accountCard.site.syncsProgress ? Object.keys(App.library.syncOutboxCounts) : []
                        delegate: LabeledRow {
                            id: pendingRow
                            required property string modelData
                            readonly property int pending: App.library.syncOutboxCounts[modelData] ?? 0
                            label: pending === 1 ? qsTr("1 update waiting for %1").arg(modelData)
                                                 : qsTr("%1 updates waiting for %2").arg(pending).arg(modelData)
                            sublabel: qsTr("Positions not accepted yet. Sent with the next playback there")
                            AppButton {
                                text: qsTr("Discard")
                                secondary: true
                                fontSize: 18
                                radius: 8
                                onClicked: App.library.clearSyncOutbox(pendingRow.modelData)
                            }
                        }
                    }
                }
            }

            SettingsCard {
                title: qsTr("Watch lists")
                category: "accounts"

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Sign in to add a tracking panel to each show's details page.")
                    color: Theme.textMuted
                    font.pixelSize: Ui.sp(Theme.compactSize)
                    wrapMode: Text.Wrap
                }

                Repeater {
                    model: App.trackers
                    delegate: ColumnLayout {
                        id: trackerRow
                        required property string name
                        required property bool authenticated
                        required property string account
                        Layout.fillWidth: true
                        spacing: 2

                        AccountRow {
                            service: trackerRow.name
                            signedIn: trackerRow.authenticated
                            account: trackerRow.account
                            offlineHint: App.trackers.canAuthenticate(trackerRow.name)
                                         ? qsTr("Not signed in")
                                         : App.trackers.needsClientSecret(trackerRow.name)
                                           ? qsTr("Needs a client ID and secret below")
                                           : qsTr("Needs a client ID below")
                            enabled: trackerRow.authenticated
                                     || App.trackers.canAuthenticate(trackerRow.name)
                                     || idField.text.trim() !== ""
                            // A click on the button may not unfocus a field first, leaving a pasted id uncommitted.
                            onSignIn: {
                                App.trackers.setClientId(trackerRow.name, idField.text)
                                App.trackers.setClientSecret(trackerRow.name, secretField.text)
                                App.trackers.authenticate(trackerRow.name)
                            }
                            onSignOut: App.trackers.signOut(trackerRow.name)
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: 2
                            visible: !trackerRow.authenticated
                            spacing: 6
                            AppTextField {
                                id: idField
                                Layout.fillWidth: true
                                text: App.trackers.clientId(trackerRow.name)
                                placeholderText: qsTr("%1 client ID").arg(trackerRow.name)
                                onEditingFinished: App.trackers.setClientId(trackerRow.name, text)
                            }
                            AppTextField {
                                id: secretField
                                Layout.fillWidth: true
                                secret: true
                                text: App.trackers.clientSecret(trackerRow.name)
                                placeholderText: App.trackers.needsClientSecret(trackerRow.name)
                                                 ? qsTr("%1 client secret").arg(trackerRow.name)
                                                 : qsTr("%1 client secret (optional)").arg(trackerRow.name)
                                onEditingFinished: App.trackers.setClientSecret(trackerRow.name, text)
                            }
                            AppButton {
                                text: qsTr("Register")
                                secondary: true
                                onClicked: Qt.openUrlExternally(App.trackers.registrationUrl(trackerRow.name))
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            Layout.leftMargin: 2
                            visible: !trackerRow.authenticated
                            text: App.trackers.registrationHint(trackerRow.name)
                            color: Theme.textMuted
                            font.pixelSize: Ui.sp(Theme.compactSize)
                            wrapMode: Text.Wrap
                        }
                    }
                }

                LabeledRow {
                    enabled: App.trackers.authenticatedCount > 0
                    opacity: enabled ? 1.0 : 0.45
                    label: qsTr("Update lists automatically")
                    sublabel: qsTr("Sends progress once an episode counts as watched")
                    AppSwitch { checked: App.settings.trackerAutoPush; onToggled: App.settings.trackerAutoPush = checked }
                }
            }

            SettingsCard {
                title: qsTr("Integrations")
                category: "integrations"

                LabeledRow {
                    label: qsTr("SubDL API key")
                    sublabel: qsTr("Needed for subtitle search. Free at subdl.com")
                    AppTextField {
                        text: App.settings.subdlApiKey
                        secret: true
                        Layout.preferredWidth: Math.min(280, settingsPage.width / 3)
                        onEditingFinished: App.settings.subdlApiKey = text
                    }
                }

                LabeledRow {
                    label: qsTr("Subtitle languages")
                    sublabel: qsTr("Languages SubDL searches for")
                    AppMultiSelect {
                        Layout.preferredWidth: Math.min(280, settingsPage.width / 3)
                        model: App.subtitleSearch.languages
                        selected: App.settings.subdlLanguages.split(",").filter(code => code.length > 0)
                        onToggled: (code, on) => {
                            const next = selected.filter(c => c !== code)
                            if (on) next.push(code)
                            if (next.length > 0) App.settings.subdlLanguages = next.join(",")
                        }
                    }
                }

                LabeledRow {
                    label: qsTr("Discord Rich Presence")
                    sublabel: qsTr("Shows what you watch on Discord")
                    AppSwitch { checked: App.settings.discordEnabled; onToggled: App.settings.discordEnabled = checked }
                }

                LabeledRow {
                    label: qsTr("Open with for video files")
                    sublabel: qsTr("Adds this app to Open with for MP4, MKV, AVI, WebM and MOV. Set the default in "
                                   + "Windows")
                    AppSwitch { checked: App.openWith; onToggled: App.openWith = checked }
                }

                LabeledRow {
                    label: qsTr("New episode notifications")
                    sublabel: qsTr("A Windows notification for new episodes in your library. The bell lists them "
                                   + "either way")
                    AppSwitch { checked: App.settings.episodeNotifications; onToggled: App.settings.episodeNotifications = checked }
                }

            }

            SettingsCard {
                title: qsTr("Player")
                category: "player"

                LabeledRow {
                    label: qsTr("Use yt-dlp")
                    sublabel: App.ytdlpBusy ? qsTr("Working…") : App.ytdlpStatus
                    Component.onCompleted: App.refreshYtdlpVersion()
                    AppButton {
                        text: qsTr("Update")
                        secondary: true
                        fontSize: 18
                        radius: 8
                        enabled: !App.ytdlpBusy
                        onClicked: App.updateYtdlp()
                    }
                    AppSwitch { checked: App.settings.mpvYtdlEnabled; onToggled: App.settings.mpvYtdlEnabled = checked }
                }

                // YouTube's challenges need a JavaScript runtime; yt-dlp finds only Deno by itself.
                LabeledRow {
                    label: qsTr("JavaScript runtime")
                    sublabel: App.jsRuntimeBusy ? qsTr("Looking…")
                            : App.jsRuntime !== "" ? App.jsRuntime
                            : qsTr("None found. yt-dlp needs Deno 2.3+ or Node 22+ for YouTube")
                    AppButton {
                        text: qsTr("Find")
                        secondary: true
                        fontSize: 18
                        radius: 8
                        enabled: !App.jsRuntimeBusy
                        onClicked: App.findJsRuntime()
                    }
                    AppButton {
                        text: qsTr("Browse")
                        secondary: true
                        fontSize: 18
                        radius: 8
                        enabled: !App.jsRuntimeBusy
                        onClicked: runtimeDialog.open()
                    }
                }

                LabeledRow {
                    label: qsTr("Prefer dubbed audio")
                    sublabel: qsTr("Tries dub servers first (off = subbed)")
                    AppSwitch { checked: App.settings.preferDub; onToggled: App.settings.preferDub = checked }
                }

                LabeledRow {
                    label: qsTr("In-app picture-in-picture")
                    sublabel: qsTr("Keeps the video in a corner of other pages while it plays. Drag to move; pull the "
                                   + "inner corner to resize")
                    AppSwitch { checked: App.settings.miniPlayer; onToggled: App.settings.miniPlayer = checked }
                }

                LabeledRow {
                    label: qsTr("Picture-in-picture when hidden")
                    sublabel: qsTr("Moves a playing video to a small window on top when the app is minimised or covered")
                    AppSwitch { checked: App.settings.autoPip; onToggled: App.settings.autoPip = checked }
                }

                LabeledRow {
                    label: qsTr("Picture adjustments")
                    sublabel: qsTr("Adds a Picture tab to the player panel for upscaling, brightness, contrast and "
                                   + "more. Off keeps the original picture, except for upscalers picked with Ctrl+number")
                    AppSwitch { checked: App.settings.pictureTab; onToggled: App.settings.pictureTab = checked }
                }

                LabeledRow {
                    label: qsTr("AnimeJaNai")
                    sublabel: App.settings.animeJaNaiDir !== "" ? App.settings.animeJaNaiDir
                            : qsTr("Folder of the unpacked AnimeJaNai package, for AI upscaling in the Picture tab. "
                                   + "Needs a libmpv built with its filter")
                    AppButton {
                        text: qsTr("Browse")
                        secondary: true
                        fontSize: 18
                        radius: 8
                        onClicked: animeJaNaiDialog.open()
                    }
                }

                LabeledSlider {
                    label: qsTr("Mark watched at")
                    sublabel: qsTr("Episode counts as watched past this much")
                    from: 0; to: 100; stepSize: 5
                    unitSuffix: "%"
                    value: App.settings.watchedPercent
                    onMoved: (v) => App.settings.watchedPercent = v
                }

                Flow {
                    Layout.fillWidth: true; spacing: 6
                    AppButton { text: qsTr("Data folder"); secondary: true; onClicked: Qt.openUrlExternally("file:///" + App.settings.dataDir) }
                    AppButton { text: qsTr("mpv folder"); secondary: true; onClicked: Qt.openUrlExternally("file:///" + App.settings.dataDir + "/mpv") }
                    AppButton { text: qsTr("mpv.conf"); secondary: true; onClicked: Qt.openUrlExternally("file:///" + App.settings.dataDir + "/mpv/mpv.conf") }
                    AppButton { text: qsTr("settings.ini"); secondary: true; onClicked: Qt.openUrlExternally(App.settings.path) }
                    AppButton { text: qsTr("Licences"); secondary: true; visible: App.settings.noticesPath !== ""; onClicked: Qt.openUrlExternally("file:///" + App.settings.noticesPath) }
                }
            }

            SettingsCard {
                id: keysCard
                title: qsTr("Keyboard shortcuts")
                category: "shortcuts"
                property string recording: ""
                onRecordingChanged: {
                    Ui.recordingKey = recording !== ""
                    if (recording !== "") keyCatcher.forceActiveFocus()
                }
                readonly property var names: ({
                    playPause: qsTr("Play or pause"),
                    seekBack: qsTr("Back 5 seconds"),
                    seekForward: qsTr("Forward 5 seconds"),
                    skipBack: qsTr("Back 90 seconds"),
                    skipForward: qsTr("Forward 90 seconds"),
                    previousEpisode: qsTr("Previous episode"),
                    nextEpisode: qsTr("Next episode"),
                    previousPlaylist: qsTr("Previous playlist"),
                    nextPlaylist: qsTr("Next playlist"),
                    volumeUp: qsTr("Volume up"),
                    volumeDown: qsTr("Volume down"),
                    mute: qsTr("Mute"),
                    faster: qsTr("Faster"),
                    slower: qsTr("Slower"),
                    doubleSpeed: qsTr("Double speed"),
                    fullscreen: qsTr("Fullscreen"),
                    pip: qsTr("Picture-in-picture"),
                    leave: qsTr("Exit fullscreen or picture-in-picture"),
                    subtitles: qsTr("Show or hide subtitles"),
                    abLoop: qsTr("A-B loop"),
                    panel: qsTr("Side panel"),
                    playlist: qsTr("Playlist"),
                    title: qsTr("Show episode name"),
                    peek: qsTr("Show controls"),
                    screenshot: qsTr("Screenshot"),
                    copyFrame: qsTr("Copy frame"),
                    copyLink: qsTr("Copy video link"),
                    openFile: qsTr("Open file"),
                    openFolder: qsTr("Open folder"),
                    openDownloads: qsTr("Open download folder"),
                    openClipboard: qsTr("Open copied link"),
                    reload: qsTr("Reload"),
                    quickSearch: qsTr("Command palette"),
                    back: qsTr("Back"),
                    forward: qsTr("Forward"),
                    nextPage: qsTr("Next page"),
                    previousPage: qsTr("Previous page"),
                    pageSearch: qsTr("Go to Explore"),
                    pageInfo: qsTr("Go to Details"),
                    pageLibrary: qsTr("Go to Library"),
                    pagePlayer: qsTr("Go to Player"),
                    pageDownloads: qsTr("Go to Downloads"),
                    pageLogs: qsTr("Go to Logs"),
                    bossKey: qsTr("Boss key"),
                    bossScreen: qsTr("Cover screen"),
                    close: qsTr("Close window")
                })

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: qsTr("Click Change, then press the new key. A key used by another action moves here. Esc "
                                   + "cancels.")
                        color: Theme.textMuted
                        font.pixelSize: Ui.sp(15)
                    }
                    AppButton {
                        text: qsTr("Reset all")
                        secondary: true
                        fontSize: 18
                        radius: 8
                        onClicked: App.shortcuts.resetAll()
                    }
                }

                Item {
                    id: keyCatcher
                    Keys.onPressed: (event) => {
                        event.accepted = true
                        if ([Qt.Key_Shift, Qt.Key_Control, Qt.Key_Alt, Qt.Key_Meta].includes(event.key)) return
                        if (event.key !== Qt.Key_Escape)
                            App.shortcuts.bind(keysCard.recording, App.shortcuts.keyText(event.key, event.modifiers))
                        keysCard.recording = ""
                    }
                    onActiveFocusChanged: if (!activeFocus) keysCard.recording = ""
                }

                Repeater {
                    model: App.shortcuts.actions
                    delegate: ColumnLayout {
                        id: keyRow
                        required property var modelData
                        required property int index
                        readonly property var keys: App.shortcuts.bindings[modelData.id] ?? []
                        readonly property string name: keysCard.names[modelData.id] ?? modelData.id
                        visible: settingsPage.query === "" || qsTr("Keyboard shortcuts").toLowerCase().includes(settingsPage.query)
                                 || name.toLowerCase().includes(settingsPage.query)
                                 || keys.some(key => key.toLowerCase().includes(settingsPage.query))
                        Layout.fillWidth: true
                        spacing: 4

                        Text {
                            visible: keyRow.index === 0 || App.shortcuts.actions[keyRow.index - 1].group !== keyRow.modelData.group
                            Layout.topMargin: keyRow.index === 0 ? 0 : 8
                            text: keyRow.modelData.group === "player" ? qsTr("Player") : qsTr("Everywhere")
                            color: Theme.textMuted
                            font.pixelSize: Ui.sp(15)
                            font.bold: true
                        }
                        LabeledRow {
                            label: keyRow.name
                            Text {
                                text: keysCard.recording === keyRow.modelData.id ? qsTr("Press a key…")
                                    : keyRow.keys.length > 0 ? keyRow.keys.join("  /  ") : qsTr("None")
                                color: keysCard.recording === keyRow.modelData.id ? Theme.accent
                                     : keyRow.keys.length > 0 ? Theme.textPrimary : Theme.textMuted
                                font.pixelSize: Ui.sp(Theme.bodySize)
                            }
                            IconButton {
                                visible: keyRow.modelData.custom
                                iconName: "refresh-cw"
                                iconSize: 15
                                implicitWidth: Ui.sp(30)
                                implicitHeight: Ui.sp(30)
                                tip: qsTr("Reset to default")
                                onClicked: App.shortcuts.reset(keyRow.modelData.id)
                            }
                            AppButton {
                                text: qsTr("Change")
                                secondary: true
                                fontSize: 16
                                radius: 8
                                Layout.preferredHeight: Ui.sp(30)
                                onClicked: keysCard.recording = keysCard.recording === keyRow.modelData.id ? "" : keyRow.modelData.id
                            }
                        }
                    }
                }
            }

            SettingsCard {
                title: qsTr("Providers")
                category: "providers"

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Drag to reorder. Explore, Migrate and Tab cycling follow this order; Explore opens on "
                               + "the first enabled provider. The badge shows this session's success rate and median "
                               + "response time.")
                    color: Theme.textMuted
                    font.pixelSize: Ui.sp(Theme.compactSize)
                    wrapMode: Text.Wrap
                }

                Connections {
                    target: App.providers
                    function onProviderTested(name, ok, results, elapsedMs, error) {
                        testResult.text = ok
                            ? qsTr("%1: %2 results in %3 ms").arg(name).arg(results).arg(elapsedMs)
                            : qsTr("%1 failed: %2").arg(name)
                                  .arg(error !== "" ? error : qsTr("no results"))
                        testResult.ok = ok
                    }
                }

                Text {
                    id: testResult
                    property bool ok: true
                    Layout.fillWidth: true
                    visible: text !== ""
                    color: ok ? Theme.success : Theme.danger
                    font.pixelSize: Ui.sp(Theme.compactSize)
                    wrapMode: Text.Wrap
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 16
                    AppCheckBox {
                        text: qsTr("All")
                        tristate: true
                        checkState: App.providers.allCheckState
                        nextCheckState: function() { return checkState === Qt.Checked ? Qt.Unchecked : Qt.Checked }
                        onClicked: App.providers.setAllEnabled(checkState === Qt.Checked)
                    }
                    Repeater {
                        model: App.providers.languageGroups
                        delegate: AppCheckBox {
                            required property var modelData
                            text: modelData.label
                            tristate: true
                            checkState: modelData.checkState
                            nextCheckState: function() { return checkState === Qt.Checked ? Qt.Unchecked : Qt.Checked }
                            onClicked: App.providers.setLanguageEnabled(modelData.language, checkState === Qt.Checked)
                        }
                    }
                }

                Column {
                    id: providerOrder
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    spacing: 6
                    readonly property real rowHeight: Ui.sp(58)
                    readonly property real rowStep: rowHeight + spacing
                    readonly property int count: App.providers.ordered.length
                    readonly property int firstEnabled: App.providers.ordered.findIndex(p => p.enabled)
                    property int dragFrom: -1
                    property int dropAt: -1

                    Repeater {
                        model: App.providers.ordered
                        delegate: Rectangle {
                            id: providerRow
                            required property var modelData
                            required property int index
                            readonly property bool dragging: providerOrder.dragFrom === index
                            readonly property real shift: {
                                const from = providerOrder.dragFrom, to = providerOrder.dropAt
                                if (from < 0 || dragging) return 0
                                if (from < to && index > from && index <= to) return -providerOrder.rowStep
                                if (from > to && index >= to && index < from) return providerOrder.rowStep
                                return 0
                            }
                            property real dragOffset: 0

                            width: providerOrder.width
                            height: providerOrder.rowHeight
                            radius: 12
                            z: dragging ? 2 : 0
                            color: dragging || rowHover.hovered ? Theme.surfaceRaised : Theme.surfaceAlt
                            border.width: 1
                            border.color: dragging ? Theme.accent : Theme.border
                            Behavior on color { ColorAnimation { duration: 120 } }
                            transform: Translate {
                                y: providerRow.dragging ? providerRow.dragOffset : providerRow.shift
                                Behavior on y {
                                    enabled: !providerRow.dragging
                                    NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                                }
                            }

                            property var stats: ({})
                            function refresh() { stats = App.providers.health(modelData.name) }
                            Component.onCompleted: refresh()
                            onVisibleChanged: if (visible) refresh()
                            Connections {
                                target: App.providers
                                function onHealthChanged() { providerRow.refresh() }
                            }

                            HoverHandler { id: rowHover }

                            Item {
                                anchors { left: parent.left; top: parent.top; bottom: parent.bottom; right: controls.left }
                                opacity: providerRow.modelData.enabled ? 1 : 0.5
                                Behavior on opacity { NumberAnimation { duration: 150 } }

                                Grid {
                                    id: grip
                                    anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                                    columns: 2
                                    spacing: 3
                                    Repeater {
                                        model: 6
                                        Rectangle {
                                            width: 4; height: 4; radius: 2
                                            color: rowHover.hovered || providerRow.dragging ? Theme.textSecondary : Theme.textMuted
                                        }
                                    }
                                }

                                Rectangle {
                                    id: place
                                    anchors { left: grip.right; leftMargin: 12; verticalCenter: parent.verticalCenter }
                                    width: Ui.sp(28); height: width; radius: width / 2
                                    color: providerRow.index === providerOrder.firstEnabled ? Theme.accent : Theme.surface
                                    border.width: 1
                                    border.color: providerRow.index === providerOrder.firstEnabled ? Theme.accent : Theme.border
                                    Text {
                                        anchors.centerIn: parent
                                        text: providerRow.index + 1
                                        color: providerRow.index === providerOrder.firstEnabled ? Theme.onAccent : Theme.textSecondary
                                        font.pixelSize: Ui.sp(Theme.compactSize)
                                        font.weight: Font.DemiBold
                                    }
                                }

                                Column {
                                    anchors { left: place.right; leftMargin: 12; right: parent.right; rightMargin: 8
                                              verticalCenter: parent.verticalCenter }
                                    spacing: 2
                                    Row {
                                        spacing: 8
                                        Text {
                                            text: providerRow.modelData.name
                                            color: Theme.textPrimary
                                            font.pixelSize: Ui.sp(Theme.bodySize)
                                            font.weight: Font.DemiBold
                                        }
                                        Rectangle {
                                            visible: providerRow.index === providerOrder.firstEnabled
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: firstLabel.implicitWidth + 12
                                            height: Ui.sp(20)
                                            radius: height / 2
                                            color: Qt.alpha(Theme.accent, 0.16)
                                            Text {
                                                id: firstLabel
                                                anchors.centerIn: parent
                                                text: qsTr("Explore opens here")
                                                color: Theme.textAccent
                                                font.pixelSize: Ui.sp(13)
                                            }
                                        }
                                    }
                                    Row {
                                        spacing: 8
                                        Text {
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: providerRow.modelData.language
                                            color: Theme.textMuted
                                            font.pixelSize: Ui.sp(Theme.compactSize)
                                        }
                                        Rectangle {
                                            anchors.verticalCenter: parent.verticalCenter
                                            visible: providerRow.stats.requests > 0
                                            width: healthText.implicitWidth + 14
                                            height: Ui.sp(20)
                                            radius: height / 2
                                            color: providerRow.stats.healthy ? Qt.alpha(Theme.success, 0.14)
                                                                             : Qt.alpha(Theme.danger, 0.16)
                                            Text {
                                                id: healthText
                                                anchors.centerIn: parent
                                                text: Math.round((providerRow.stats.successRate ?? 0) * 100) + "%"
                                                      + (providerRow.stats.medianMs > 0
                                                         ? " · " + providerRow.stats.medianMs + "ms" : "")
                                                color: providerRow.stats.healthy ? Theme.success : Theme.danger
                                                font.pixelSize: Ui.sp(13)
                                            }
                                            HoverHandler { id: healthHover }
                                            AppToolTip {
                                                visible: healthHover.hovered
                                                text: qsTr("%1 of %2 requests failed this session")
                                                      .arg(providerRow.stats.failures ?? 0)
                                                      .arg(providerRow.stats.requests ?? 0)
                                            }
                                        }
                                    }
                                }

                                // Not a DragHandler: preventStealing keeps the page's Flickable from taking a vertical drag.
                                MouseArea {
                                    anchors.fill: parent
                                    preventStealing: true
                                    cursorShape: providerRow.dragging ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                                    property real startY: 0
                                    onPressed: (mouse) => {
                                        startY = mapToItem(providerOrder, mouse.x, mouse.y).y
                                        providerRow.dragOffset = 0
                                        providerOrder.dropAt = providerRow.index
                                        providerOrder.dragFrom = providerRow.index
                                    }
                                    onPositionChanged: (mouse) => {
                                        if (!pressed) return
                                        const dy = mapToItem(providerOrder, mouse.x, mouse.y).y - startY
                                        providerRow.dragOffset = dy
                                        providerOrder.dropAt = Math.max(0, Math.min(providerOrder.count - 1,
                                            providerRow.index + Math.round(dy / providerOrder.rowStep)))
                                    }
                                    function finish() {
                                        const from = providerOrder.dragFrom, to = providerOrder.dropAt
                                        providerOrder.dragFrom = -1
                                        providerOrder.dropAt = -1
                                        providerRow.dragOffset = 0
                                        if (from >= 0 && to >= 0 && from !== to) App.providers.moveProvider(from, to)
                                    }
                                    onReleased: finish()
                                    onCanceled: finish()
                                }
                            }

                            Row {
                                id: controls
                                anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
                                spacing: 4

                                IconButton {
                                    anchors.verticalCenter: parent.verticalCenter
                                    implicitWidth: 28; implicitHeight: 28
                                    boxRadius: 8
                                    iconName: "refresh-cw"
                                    iconSize: 14
                                    tip: qsTr("Run a test search")
                                    onClicked: App.providers.testProvider(providerRow.modelData.name)
                                }
                                IconButton {
                                    anchors.verticalCenter: parent.verticalCenter
                                    implicitWidth: 28; implicitHeight: 28
                                    boxRadius: 8
                                    iconName: "arrow-up"
                                    iconSize: 14
                                    enabled: providerRow.index > 0
                                    opacity: enabled ? 1 : 0.3
                                    tip: qsTr("Move up")
                                    onClicked: App.providers.moveProvider(providerRow.index, providerRow.index - 1)
                                }
                                IconButton {
                                    anchors.verticalCenter: parent.verticalCenter
                                    implicitWidth: 28; implicitHeight: 28
                                    boxRadius: 8
                                    iconName: "arrow-down"
                                    iconSize: 14
                                    enabled: providerRow.index < providerOrder.count - 1
                                    opacity: enabled ? 1 : 0.3
                                    tip: qsTr("Move down")
                                    onClicked: App.providers.moveProvider(providerRow.index, providerRow.index + 1)
                                }
                                Item { width: 6; height: 1 }
                                AppSwitch {
                                    anchors.verticalCenter: parent.verticalCenter
                                    checked: providerRow.modelData.enabled
                                    onClicked: App.providers.setProviderEnabled(providerRow.modelData.name, checked)
                                    AppToolTip {
                                        visible: parent.hovered
                                        text: parent.checked ? qsTr("Turn off") : qsTr("Turn on")
                                    }
                                }
                            }
                        }
                    }
                }
            }

            SettingsCard {
                title: qsTr("Network")
                category: "network"

                LabeledRow {
                    label: qsTr("Proxy")
                    sublabel: App.settings.proxy === "" ? qsTr("Applies to providers, the downloader and mpv")
                            : App.settings.proxyEnabled ? qsTr("On") : qsTr("Off (address kept)")
                    AppTextField {
                        text: App.settings.proxy
                        placeholderText: qsTr("http://127.0.0.1:7890")
                        showClearButton: true
                        Layout.preferredWidth: 260
                        onEditingFinished: App.settings.proxy = text
                        onTextChanged: if (text === "" && App.settings.proxy !== "") App.settings.proxy = ""
                    }
                    AppSwitch {
                        enabled: App.settings.proxy !== ""
                        opacity: enabled ? 1.0 : 0.45
                        checked: App.settings.proxyEnabled && App.settings.proxy !== ""
                        onToggled: App.settings.proxyEnabled = checked
                    }
                }

                LabeledRow {
                    label: qsTr("Accept untrusted certificates")
                    sublabel: qsTr("Connects to stream hosts whose certificates Windows does not trust. Off, those "
                                   + "hosts fail. Sign-ins and accounts are always checked")
                    AppSwitch { checked: App.settings.lenientTls; onToggled: App.settings.lenientTls = checked }
                }
            }

            Item { Layout.fillHeight: true }
        }
    }
}
