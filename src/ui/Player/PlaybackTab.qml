pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App

Item {
    id: root
    required property MpvPlayer player

    component DelayRow: Rectangle {
        id: delayRow
        required property string label
        required property real delay
        property real range: 60
        property real step: 0.1
        property int decimals: 1
        signal picked(real seconds)

        Layout.fillWidth: true
        implicitHeight: 56
        radius: 10
        color: "transparent"
        onDelayChanged: if (!delaySlider.pressed) delaySlider.value = delay

        ColumnLayout {
            anchors { fill: parent; leftMargin: 14; rightMargin: 14 }
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Text {
                    text: delayRow.label
                    color: Theme.onOverlayDim
                    font.pixelSize: Ui.sp(Theme.bodySize)
                }

                Item { Layout.fillWidth: true }

                Text {
                    text: qsTr("Reset")
                    color: resetArea.containsMouse ? Theme.accent : Theme.onOverlayDim
                    font.pixelSize: Ui.sp(Theme.metadataSize)
                    visible: delayRow.delay !== 0

                    MouseArea {
                        id: resetArea
                        anchors.fill: parent
                        anchors.margins: -6
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: delayRow.picked(0)
                    }
                }
            }

            AppSlider {
                id: delaySlider
                objectName: "panelSlider"
                Layout.fillWidth: true
                from: -delayRow.range; to: delayRow.range; stepSize: delayRow.step
                snapMode: Slider.SnapAlways
                value: delayRow.delay
                unitSuffix: "s"
                decimals: delayRow.decimals
                onMoved: delayRow.picked(value)
            }
        }
    }

    PanelScroll {
        SettingRow {
            labelColor: Theme.onOverlayMuted
            label: qsTr("Subtitles")
            AppSwitch {
                anchors.verticalCenter: parent.verticalCenter
                focusPolicy: Qt.NoFocus
                checked: root.player.subVisible
                onToggled: root.player.subVisible = checked
            }
        }

        SettingRow {
            labelColor: Theme.onOverlayMuted
            label: qsTr("Seek previews")
            AppSwitch {
                anchors.verticalCenter: parent.verticalCenter
                checked: App.settings.seekPreviewsEnabled
                onToggled: App.settings.seekPreviewsEnabled = checked
            }
        }

        SettingRow {
            labelColor: Theme.onOverlayMuted
            label: qsTr("Mute")
            AppSwitch {
                anchors.verticalCenter: parent.verticalCenter
                focusPolicy: Qt.NoFocus
                checked: root.player.muted
                onToggled: root.player.muted = checked
            }
        }

        SettingRow {
            labelColor: Theme.onOverlayMuted
            label: qsTr("Normalise volume")
            AppSwitch {
                anchors.verticalCenter: parent.verticalCenter
                focusPolicy: Qt.NoFocus
                checked: App.settings.normalizeAudio
                onToggled: App.settings.normalizeAudio = checked
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            Layout.preferredHeight: 1
            color: Theme.overlayLine
        }

        PanelSlider {
            label: qsTr("Volume")
            from: 0; to: 200
            unitSuffix: "%"
            value: root.player.volume
            onMoved: (v) => root.player.volume = v
        }

        PanelSlider {
            label: qsTr("Speed")
            from: 0.1; to: 4.0; stepSize: 0.05
            unitSuffix: "x"; decimals: 2
            value: root.player.speed
            onMoved: (v) => root.player.speed = v
        }

        DelayRow {
            label: qsTr("Sub delay")
            delay: root.player.subDelay
            onPicked: seconds => root.player.subDelay = seconds
        }

        DelayRow {
            label: qsTr("Audio delay")
            delay: root.player.audioDelay
            range: 10
            step: 0.05
            decimals: 2
            onPicked: seconds => root.player.audioDelay = seconds
        }

        Item { Layout.fillHeight: true }
    }
}
