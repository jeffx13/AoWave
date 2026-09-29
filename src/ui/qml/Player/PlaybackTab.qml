pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../Components"
import App
import ".."

Item {
    id: root
    required property MpvPlayer player

    Flickable {
        anchors {
            fill: parent
            margins: 12
        }
        contentHeight: generalCol.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: generalCol
            width: parent.width
            spacing: 6

            component SettingRow: Rectangle {
                Layout.fillWidth: true
                implicitHeight: 48
                radius: 10
                color: settingHover.hovered ? Theme.overlayLine : "transparent"
                Behavior on color { ColorAnimation { duration: 100 } }

                property alias label: settingLabel.text
                default property alias content: settingSlot.data

                HoverHandler { id: settingHover }

                RowLayout {
                    anchors {
                        fill: parent
                        leftMargin: 14
                        rightMargin: 14
                    }
                    spacing: 12

                    Text {
                        id: settingLabel
                        color: Theme.onOverlayMuted
                        font.pixelSize: Globals.sp(Theme.bodySize)
                        Layout.fillWidth: true
                    }

                    Item {
                        id: settingSlot
                        Layout.preferredWidth: childrenRect.width
                        Layout.preferredHeight: parent.height
                    }
                }
            }

            component PanelSlider: LabeledSlider {
                stacked: true
                labelColor: Theme.onOverlayDim
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                Layout.topMargin: 6
            }

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
                onMoved: (v) => root.player.speed = v
            }

            PanelSlider {
                label: qsTr("Sub Size")
                from: 20; to: 80; stepSize: 1
                unitSuffix: "px"
                value: App.settings.subFontSize
                onMoved: (v) => {
                    // sub-scale resizes ASS and text subs; 40 = 1.0x.
                    root.player.setMpvProperty("sub-scale", v / 40.0)
                    App.settings.subFontSize = v
                }
            }

            PanelSlider {
                label: qsTr("Sub Position")
                from: 0; to: 100; stepSize: 1
                unitSuffix: "%"
                value: App.settings.subPos
                onMoved: (v) => {
                    root.player.setSubPos(v)
                    App.settings.subPos = v
                }
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
                            text: qsTr("Sub Delay")
                            color: Theme.onOverlayDim
                            font.pixelSize: Globals.sp(Theme.bodySize)
                        }

                        Item { Layout.fillWidth: true }

                        Text {
                            text: qsTr("Reset")
                            color: resetDelayArea.containsMouse ? Theme.accent : Theme.onOverlayDim
                            font.pixelSize: Globals.sp(16)
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
                        from: -60; to: 60; stepSize: 0.1   // matches the clamp in setSubDelay
                        // Without this stepSize only applies to keys and wheel.
                        snapMode: Slider.SnapAlways
                        value: root.player.subDelay
                        unitSuffix: "s"
                        decimals: 1
                        onMoved: root.player.subDelay = value

                        // Dragging drops the binding above.
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
}
