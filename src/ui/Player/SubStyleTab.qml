pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import App

Item {
    id: root
    readonly property var player: Ui.mpv

    readonly property var fonts: [""].concat(["Segoe UI", "Arial", "Verdana", "Tahoma", "Trebuchet MS", "Georgia",
                                              "Microsoft YaHei", "Yu Gothic", "Malgun Gothic"]
                                             .filter(family => Qt.fontFamilies().includes(family)))
    readonly property var colors: [
        { hex: "#FFFFFF", name: qsTr("White") }, { hex: "#FFE45C", name: qsTr("Yellow") },
        { hex: "#8FE3FF", name: qsTr("Blue") }, { hex: "#A8F08A", name: qsTr("Green") },
        { hex: "#FFB8D9", name: qsTr("Pink") }
    ]

    PanelScroll {
        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            implicitHeight: Math.max(Ui.sp(96), sample.contentHeight + Ui.sp(28))
            radius: 10
            clip: true
            // literal colours stand in for a video frame
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#3a4a6b" }
                GradientStop { position: 1.0; color: "#121826" }
            }
            Rectangle {
                anchors.centerIn: sample
                width: sample.contentWidth + 12
                height: sample.contentHeight + 4
                color: Qt.rgba(0, 0, 0, App.settings.subBackground / 100)
                visible: App.settings.subBackground > 0
            }
            Text {
                id: sample
                anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: Ui.sp(14) }
                width: Math.min(implicitWidth, parent.width - 24)
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("I will be back before the festival ends.")
                color: App.settings.subColor
                // mpv's sans-serif is Arial through libass on Windows, where Qt's is Tahoma
                font.family: App.settings.subFont !== "" ? App.settings.subFont : "Arial"
                font.pixelSize: Ui.sp(22 * App.settings.subFontSize / 40)
                font.bold: App.settings.subBold
                style: App.settings.subBackground > 0 ? Text.Normal
                     : App.settings.subOutline > 0 ? Text.Outline
                     : App.settings.subShadow > 0 ? Text.Raised : Text.Normal
                styleColor: "black"
            }
        }

        PanelSlider {
            label: qsTr("Sub size")
            from: 50; to: 200; stepSize: 5
            unitSuffix: "%"
            // sub-scale is subFontSize / 40, so a step of 5% is 2
            value: App.settings.subFontSize * 2.5
            onMoved: (v) => {
                const size = Math.round(v / 5) * 2
                if (root.player) root.player.setMpvProperty("sub-scale", size / 40.0)
                App.settings.subFontSize = size
            }
        }
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            text: qsTr("Signs placed by styled subtitles keep their own size.")
            color: Theme.onOverlayDim
            font.pixelSize: Ui.sp(Theme.captionSize)
            wrapMode: Text.WordWrap
        }

        PanelSlider {
            label: qsTr("Sub position")
            from: 0; to: 100; stepSize: 1
            unitSuffix: "%"
            value: App.settings.subPos
            onMoved: (v) => {
                if (root.player) root.player.setSubPos(v)
                App.settings.subPos = v
            }
        }

        SettingRow {
            label: qsTr("Font")
            AppComboBox {
                anchors.verticalCenter: parent.verticalCenter
                focusPolicy: Qt.NoFocus
                model: root.fonts.map(family => family === "" ? qsTr("Default") : family)
                currentIndex: Math.max(0, root.fonts.indexOf(App.settings.subFont))
                onActivated: (index) => App.settings.subFont = root.fonts[index]
            }
        }

        SettingRow {
            label: qsTr("Colour")
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                Repeater {
                    model: root.colors
                    delegate: Rectangle {
                        id: swatch
                        required property var modelData
                        readonly property bool current: App.settings.subColor.toUpperCase() === modelData.hex
                        width: Ui.sp(24)
                        height: width
                        radius: width / 2
                        color: modelData.hex
                        border.width: current ? 3 : 1
                        border.color: current ? Theme.accent : Theme.overlayLine
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: modelData.name
                        Accessible.checked: current
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: App.settings.subColor = swatch.modelData.hex }
                    }
                }
            }
        }

        SettingRow {
            label: qsTr("Bold")
            AppSwitch {
                anchors.verticalCenter: parent.verticalCenter
                focusPolicy: Qt.NoFocus
                checked: App.settings.subBold
                onToggled: App.settings.subBold = checked
            }
        }

        PanelSlider {
            label: qsTr("Outline")
            from: 0; to: 6; stepSize: 0.5
            decimals: 1
            value: App.settings.subOutline
            onMoved: (v) => App.settings.subOutline = v
        }

        PanelSlider {
            label: qsTr("Shadow")
            from: 0; to: 6; stepSize: 0.5
            decimals: 1
            value: App.settings.subShadow
            onMoved: (v) => App.settings.subShadow = v
        }

        PanelSlider {
            label: qsTr("Background box")
            from: 0; to: 100; stepSize: 5
            unitSuffix: "%"
            value: App.settings.subBackground
            onMoved: (v) => App.settings.subBackground = v
        }

        SettingRow {
            label: qsTr("Restyle styled subtitles")
            sublabel: qsTr("Overrides built-in subtitle styles, common in fansubs")
            AppSwitch {
                anchors.verticalCenter: parent.verticalCenter
                focusPolicy: Qt.NoFocus
                checked: App.settings.subOverrideStyled
                onToggled: App.settings.subOverrideStyled = checked
            }
        }

        AppButton {
            Layout.alignment: Qt.AlignRight
            Layout.rightMargin: 14
            Layout.topMargin: 4
            text: qsTr("Reset")
            secondary: true
            compact: true
            onClicked: {
                App.settings.resetSubStyle()
                if (!root.player) return
                root.player.setMpvProperty("sub-scale", App.settings.subFontSize / 40.0)
                root.player.setSubPos(App.settings.subPos)
            }
        }
    }
}
