pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import "../Components"
import App
import ".."

Item {
    id: root
    required property MpvPlayer player

    Flickable {
        anchors { fill: parent; margins: 12 }
        contentHeight: danmakuCol.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: danmakuCol
            width: parent.width
            spacing: 6

            component SettingRow: Rectangle {
                Layout.fillWidth: true
                implicitHeight: 48
                radius: 10
                color: rowHover.hovered ? Theme.overlayLine : "transparent"
                Behavior on color { ColorAnimation { duration: 100 } }

                property alias label: rowLabel.text
                property alias sublabel: rowSub.text
                default property alias content: rowSlot.data

                HoverHandler { id: rowHover }

                RowLayout {
                    anchors { fill: parent; leftMargin: 14; rightMargin: 14 }
                    spacing: 10

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Text {
                            id: rowLabel
                            Layout.fillWidth: true
                            color: Theme.onOverlayDim
                            font.pixelSize: Globals.sp(Theme.bodySize)
                            elide: Text.ElideRight
                        }
                        Text {
                            id: rowSub
                            Layout.fillWidth: true
                            visible: text !== ""
                            color: Theme.onOverlayMuted
                            font.pixelSize: Globals.sp(14)
                            elide: Text.ElideRight
                        }
                    }

                    Item {
                        id: rowSlot
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
                label: qsTr("Show Danmaku")
                sublabel: qsTr("Bullet comments, where the provider has them")
                AppSwitch {
                    anchors.verticalCenter: parent.verticalCenter
                    focusPolicy: Qt.NoFocus
                    checked: App.settings.danmakuEnabled
                    onToggled: App.settings.danmakuEnabled = checked
                }
            }

            SettingRow {
                label: qsTr("Outline")
                sublabel: qsTr("How comments separate from the picture behind them")
                AppComboBox {
                    anchors.verticalCenter: parent.verticalCenter
                    focusPolicy: Qt.NoFocus
                    model: [qsTr("None"), qsTr("Outline"), qsTr("Outline + Shadow")]
                    currentIndex: App.settings.danmakuOutline
                    onActivated: App.settings.danmakuOutline = currentIndex
                }
            }

            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                text: qsTr("Independent of Sub Size and Sub Position. Comments use the primary "
                           + "subtitle slot while they are on.")
                color: Theme.onOverlayDim
                font.pixelSize: Globals.sp(15)
                wrapMode: Text.WordWrap
            }

            PanelSlider {
                label: qsTr("Danmaku Opacity")
                from: 10; to: 100; stepSize: 5
                unitSuffix: "%"
                value: App.settings.danmakuOpacity
                onMoved: (v) => App.settings.danmakuOpacity = v
            }

            PanelSlider {
                label: qsTr("Danmaku Size")
                from: 50; to: 200; stepSize: 10
                unitSuffix: "%"
                value: App.settings.danmakuFontScale
                onMoved: (v) => App.settings.danmakuFontScale = v
            }

            PanelSlider {
                label: qsTr("Danmaku Speed")
                from: 25; to: 400; stepSize: 25
                unitSuffix: "%"
                value: App.settings.danmakuSpeed
                onMoved: (v) => App.settings.danmakuSpeed = v
            }

            PanelSlider {
                label: qsTr("Danmaku Area")
                from: 10; to: 100; stepSize: 5
                unitSuffix: "%"
                value: App.settings.danmakuArea
                onMoved: (v) => App.settings.danmakuArea = v
            }

            PanelSlider {
                label: qsTr("Danmaku Density")
                from: 0; to: 200; stepSize: 10
                value: App.settings.danmakuMaxOnScreen
                onMoved: (v) => App.settings.danmakuMaxOnScreen = v
            }

            PanelSlider {
                label: qsTr("Hide Spam")
                from: 0; to: 11; stepSize: 1
                value: App.settings.danmakuMinWeight
                onMoved: (v) => App.settings.danmakuMinWeight = v
            }

            SettingRow {
                label: qsTr("Bold Danmaku")
                AppSwitch {
                    anchors.verticalCenter: parent.verticalCenter
                    focusPolicy: Qt.NoFocus
                    checked: App.settings.danmakuBold
                    onToggled: App.settings.danmakuBold = checked
                }
            }

            Text {
                Layout.leftMargin: 14
                Layout.topMargin: 6
                text: qsTr("Hide comment types")
                color: Theme.onOverlayMuted
                font.pixelSize: Globals.sp(Theme.bodySize)
                font.bold: true
            }

            Flow {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                spacing: 14
                MiniToggle { label: qsTr("Scrolling"); checked: App.settings.danmakuBlockScroll; onToggled: App.settings.danmakuBlockScroll = checked }
                MiniToggle { label: qsTr("Top");       checked: App.settings.danmakuBlockTop;    onToggled: App.settings.danmakuBlockTop = checked }
                MiniToggle { label: qsTr("Bottom");    checked: App.settings.danmakuBlockBottom; onToggled: App.settings.danmakuBlockBottom = checked }
                MiniToggle { label: qsTr("Colour");    checked: App.settings.danmakuBlockColour; onToggled: App.settings.danmakuBlockColour = checked }
                MiniToggle { label: qsTr("Repeats");   checked: App.settings.danmakuBlockRepeat; onToggled: App.settings.danmakuBlockRepeat = checked }
            }

            Item {
                Layout.fillWidth: true
                implicitHeight: 40

                AppButton {
                    anchors.centerIn: parent
                    text: qsTr("Reset Danmaku Appearance")
                    backgroundDefaultColor: Theme.overlayFillActive
                    contentItemTextColor: Theme.onOverlay
                    onClicked: App.settings.resetDanmakuAppearance()
                }
            }

            Item { Layout.fillHeight: true }
        }
    }
}
