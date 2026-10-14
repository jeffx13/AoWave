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

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 56
            radius: 10
            color: "transparent"

            ColumnLayout {
                anchors { fill: parent; leftMargin: 14; rightMargin: 14 }
                spacing: 2

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Text {
                        text: qsTr("Sub delay")
                        color: Theme.onOverlayDim
                        font.pixelSize: Ui.sp(Theme.bodySize)
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        text: qsTr("Reset")
                        color: resetDelayArea.containsMouse ? Theme.accent : Theme.onOverlayDim
                        font.pixelSize: Ui.sp(16)
                        visible: root.player.subDelay !== 0

                        MouseArea {
                            id: resetDelayArea
                            anchors.fill: parent
                            anchors.margins: -6
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.player.subDelay = 0
                        }
                    }
                }

                AppSlider {
                    id: subDelaySlider
                    Layout.fillWidth: true
                    from: -60; to: 60; stepSize: 0.1
                    snapMode: Slider.SnapAlways
                    value: root.player.subDelay
                    unitSuffix: "s"
                    decimals: 1
                    onMoved: root.player.subDelay = value

                    Connections {
                        target: root.player
                        function onSubDelayChanged() {
                            if (!subDelaySlider.pressed)
                                subDelaySlider.value = root.player.subDelay
                        }
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
