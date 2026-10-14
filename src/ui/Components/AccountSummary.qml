import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: summary

    required property var account
    readonly property bool signedIn: account.signedIn
    readonly property color tone: account.tone === "ok" ? Theme.success
                                : account.tone === "warn" ? Theme.warning
                                : account.tone === "bad" ? Theme.danger : Theme.textMuted
    readonly property string who: signedIn ? (account.name !== "" ? account.name : qsTr("Signed in")) : qsTr("Not signed in")

    Layout.fillWidth: true
    spacing: 10

    RowLayout {
        Layout.fillWidth: true
        spacing: 12

        Rectangle {
            Layout.preferredWidth: Ui.sp(46)
            Layout.preferredHeight: Ui.sp(46)
            Layout.alignment: summary.signedIn ? Qt.AlignVCenter : Qt.AlignTop
            radius: width / 2
            color: summary.signedIn ? Theme.accentMuted : Theme.surfaceAlt
            Text {
                anchors.centerIn: parent
                text: (summary.signedIn && summary.account.name !== "" ? summary.account.name : summary.account.label).charAt(0).toUpperCase()
                color: summary.signedIn ? Theme.textAccent : Theme.textMuted
                font { pixelSize: Ui.sp(20); weight: Font.Bold }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4

            Text {
                Layout.fillWidth: true
                text: summary.who
                color: Theme.textPrimary
                font { pixelSize: Ui.sp(17); weight: Font.DemiBold }
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                visible: !summary.signedIn
                text: summary.account.offlineHint
                color: Theme.textMuted
                font.pixelSize: Ui.sp(Theme.compactSize)
                wrapMode: Text.Wrap
            }
        }

        AppButton {
            Layout.alignment: Qt.AlignTop
            text: summary.signedIn ? qsTr("Sign out") : qsTr("Sign in")
            secondary: summary.signedIn
            onClicked: summary.signedIn ? summary.account.signOut() : summary.account.signIn()
        }
    }

    Rectangle {
        visible: summary.signedIn && summary.account.membership !== ""
        Layout.fillWidth: true
        Layout.maximumWidth: pillSize.implicitWidth + 20
        implicitHeight: pill.implicitHeight + 10
        radius: Math.min(height / 2, 14)
        color: Qt.alpha(summary.tone, 0.16)
        border { width: 1; color: Qt.alpha(summary.tone, 0.5) }
        Text {
            id: pill
            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 10 }
            text: summary.account.membership
            color: summary.tone
            font { pixelSize: Ui.sp(Theme.compactSize); weight: Font.Medium }
            wrapMode: Text.Wrap
        }
        Text {
            id: pillSize
            visible: false
            text: pill.text
            font: pill.font
        }
    }

    Rectangle {
        Layout.fillWidth: true
        visible: summary.signedIn && summary.account.caution !== "" && summary.account.problem === ""
        implicitHeight: cautionRow.implicitHeight + 20
        radius: 8
        color: Qt.alpha(Theme.warning, 0.14)
        ColumnLayout {
            id: cautionRow
            anchors { fill: parent; margins: 10 }
            spacing: 8
            Text {
                Layout.fillWidth: true
                text: summary.account.caution
                color: Theme.textPrimary
                font.pixelSize: Ui.sp(Theme.compactSize)
                wrapMode: Text.Wrap
            }
            AppButton {
                text: qsTr("Sign in again")
                secondary: true
                onClicked: summary.account.signIn()
            }
        }
    }

    Rectangle {
        Layout.fillWidth: true
        visible: summary.signedIn && summary.account.problem !== ""
        implicitHeight: problemRow.implicitHeight + 20
        radius: 8
        color: Theme.dangerSoft
        ColumnLayout {
            id: problemRow
            anchors { fill: parent; margins: 10 }
            spacing: 8
            Text {
                Layout.fillWidth: true
                text: summary.account.problem
                color: Theme.textPrimary
                font.pixelSize: Ui.sp(Theme.compactSize)
                wrapMode: Text.Wrap
            }
            AppButton {
                text: qsTr("Sign in again")
                onClicked: summary.account.signIn()
            }
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        visible: summary.signedIn
        color: Theme.border
    }

    Repeater {
        model: summary.signedIn ? [{ label: qsTr("Session"), value: summary.account.expiry },
                                   { label: qsTr("Last checked"), value: summary.account.checked }].concat(summary.account.details) : []
        delegate: RowLayout {
            id: detail
            required property var modelData
            Layout.fillWidth: true
            visible: modelData.value !== ""
            spacing: 14
            Text {
                Layout.preferredWidth: Ui.sp(120)
                Layout.alignment: Qt.AlignTop
                text: detail.modelData.label
                color: Theme.textMuted
                font.pixelSize: Ui.sp(Theme.compactSize)
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                text: detail.modelData.value
                color: Theme.textSecondary
                font.pixelSize: Ui.sp(Theme.compactSize)
                wrapMode: Text.Wrap
            }
        }
    }

    AppButton {
        visible: summary.signedIn
        text: summary.account.busy ? qsTr("Checking…") : qsTr("Check now")
        secondary: true
        enabled: !summary.account.busy
        onClicked: summary.account.refresh()
    }
}
