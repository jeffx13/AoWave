pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import App

ColumnLayout {
    id: root
    required property MpvPlayer player
    readonly property int maxSeconds: 24 * 3600
    spacing: 8

    component SkipCard: Rectangle {
        id: skipCard
        property string label
        property bool   active
        default property alias content: extraSlot.data

        signal cardToggled()

        Layout.fillWidth: true
        implicitHeight: skipContent.implicitHeight + 20
        radius: 12
        color: Theme.overlayFillSoft
        border.color: skipCard.active ? Theme.accent : Theme.overlayFill
        border.width: 1
        Behavior on border.color { ColorAnimation { duration: 150 } }

        ColumnLayout {
            id: skipContent
            anchors {
                fill: parent
                margins: 10
            }
            spacing: 8

            RowLayout {
                spacing: 8
                Text {
                    text: skipCard.label
                    color: Theme.onOverlayMuted
                    font.pixelSize: Ui.sp(Theme.bodySize)
                    Layout.fillWidth: true
                }
                AppCheckBox {
                    focusPolicy: Qt.NoFocus
                    checked: skipCard.active
                    onToggled: skipCard.cardToggled()
                }
            }

            Item {
                id: extraSlot
                Layout.fillWidth: true
                implicitHeight: childrenRect.height
            }
        }
    }

    SkipCard {
        label: qsTr("Skip opening")
        active: root.player.skipOP
        onCardToggled: root.player.skipOP = !root.player.skipOP

        RowLayout {
            width: parent.width
            spacing: 8
            Text { text: qsTr("Start (s)");  color: Theme.onOverlayDim; font.pixelSize: Ui.sp(Theme.bodySize) }
            AppSpinBox {
                Layout.fillWidth: true
                value: root.player.skipOPStart
                from: 0
                to: root.maxSeconds
                focusPolicy: Qt.NoFocus
                stepSize: 10
                onValueModified: root.player.skipOPStart = value
            }
            Text { text: qsTr("Length (s)"); color: Theme.onOverlayDim; font.pixelSize: Ui.sp(Theme.bodySize) }
            AppSpinBox {
                Layout.fillWidth: true
                value: root.player.skipOPLength
                from: 0
                to: root.maxSeconds
                focusPolicy: Qt.NoFocus
                stepSize: 10
                onValueModified: root.player.skipOPLength = value
            }
        }
    }

    SkipCard {
        label: qsTr("Skip ending")
        active: root.player.skipED
        onCardToggled: root.player.skipED = !root.player.skipED

        RowLayout {
            width: parent.width
            spacing: 8
            Text { text: qsTr("Length (s)"); color: Theme.onOverlayDim; font.pixelSize: Ui.sp(Theme.bodySize) }
            AppSpinBox {
                Layout.fillWidth: true
                value: root.player.skipEDLength
                from: 0
                to: root.maxSeconds
                focusPolicy: Qt.NoFocus
                stepSize: 10
                onValueModified: root.player.skipEDLength = value
            }
        }
    }
}
