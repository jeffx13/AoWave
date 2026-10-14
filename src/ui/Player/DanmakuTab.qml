pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import App

Item {
    id: root
    required property MpvPlayer player

    PanelScroll {
        SettingRow {
            label: qsTr("Show danmaku")
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
            sublabel: qsTr("Makes comments stand out from the picture")
            AppComboBox {
                anchors.verticalCenter: parent.verticalCenter
                focusPolicy: Qt.NoFocus
                model: [qsTr("None"), qsTr("Outline"), qsTr("Outline + shadow")]
                currentIndex: App.settings.danmakuOutline
                onActivated: App.settings.danmakuOutline = currentIndex
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            text: qsTr("Independent of Sub size and Sub position. Comments use the primary subtitle slot while on.")
            color: Theme.onOverlayDim
            font.pixelSize: Ui.sp(15)
            wrapMode: Text.WordWrap
        }

        PanelSlider {
            label: qsTr("Opacity")
            from: 10; to: 100; stepSize: 5
            unitSuffix: "%"
            value: App.settings.danmakuOpacity
            onMoved: (v) => App.settings.danmakuOpacity = v
        }

        PanelSlider {
            label: qsTr("Size")
            from: 50; to: 200; stepSize: 10
            unitSuffix: "%"
            value: App.settings.danmakuFontScale
            onMoved: (v) => App.settings.danmakuFontScale = v
        }

        PanelSlider {
            label: qsTr("Speed")
            from: 25; to: 400; stepSize: 25
            unitSuffix: "%"
            value: App.settings.danmakuSpeed
            onMoved: (v) => App.settings.danmakuSpeed = v
        }

        PanelSlider {
            label: qsTr("Area")
            from: 10; to: 100; stepSize: 5
            unitSuffix: "%"
            value: App.settings.danmakuArea
            onMoved: (v) => App.settings.danmakuArea = v
        }

        PanelSlider {
            label: qsTr("Density")
            from: 0; to: 200; stepSize: 10
            value: App.settings.danmakuMaxOnScreen
            onMoved: (v) => App.settings.danmakuMaxOnScreen = v
        }

        PanelSlider {
            label: qsTr("Hide spam")
            from: 0; to: 11; stepSize: 1
            value: App.settings.danmakuMinWeight
            onMoved: (v) => App.settings.danmakuMinWeight = v
        }

        SettingRow {
            label: qsTr("Bold")
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
            font.pixelSize: Ui.sp(Theme.bodySize)
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

        AppButton {
            Layout.alignment: Qt.AlignRight
            Layout.rightMargin: 14
            Layout.topMargin: 4
            text: qsTr("Reset")
            secondary: true
            fontSize: 16
            radius: 8
            onClicked: App.settings.resetDanmakuAppearance()
        }

        Item { Layout.fillHeight: true }
    }
}
