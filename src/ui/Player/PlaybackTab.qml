pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App

Item {
    id: root
    required property MpvPlayer player

    PanelScroll {
        SettingRow {
            label: qsTr("Subtitles")
            AppSwitch {
                anchors.verticalCenter: parent.verticalCenter
                focusPolicy: Qt.NoFocus
                checked: root.player.subVisible
                onToggled: root.player.subVisible = checked
            }
        }

        SettingRow {
            label: qsTr("Seek previews")
            AppSwitch {
                anchors.verticalCenter: parent.verticalCenter
                checked: App.settings.seekPreviewsEnabled
                onToggled: App.settings.seekPreviewsEnabled = checked
            }
        }

        SettingRow {
            label: qsTr("Mute")
            AppSwitch {
                anchors.verticalCenter: parent.verticalCenter
                focusPolicy: Qt.NoFocus
                checked: root.player.muted
                onToggled: root.player.muted = checked
            }
        }

        SettingRow {
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
            resetValue: 1
            onMoved: (v) => root.player.speed = v
        }

        PanelSlider {
            label: qsTr("Sub delay")
            from: -60; to: 60; stepSize: 0.1
            snapMode: Slider.SnapAlways
            unitSuffix: "s"; decimals: 1
            value: root.player.subDelay
            resetValue: 0
            onMoved: (v) => root.player.subDelay = v
        }

        PanelSlider {
            label: qsTr("Audio delay")
            from: -10; to: 10; stepSize: 0.05
            snapMode: Slider.SnapAlways
            unitSuffix: "s"; decimals: 2
            value: root.player.audioDelay
            resetValue: 0
            onMoved: (v) => root.player.audioDelay = v
        }

        Item { Layout.fillHeight: true }
    }
}
