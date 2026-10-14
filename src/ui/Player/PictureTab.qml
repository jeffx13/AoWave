pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import App

Item {
    id: root

    readonly property var picture: App.settings.picture
    readonly property var presets: [
        { name: qsTr("Normal"), values: { brightness: 0, contrast: 0, saturation: 0, gamma: 0, deband: false } },
        { name: qsTr("Vivid"), values: { brightness: 0, contrast: 8, saturation: 25, gamma: 0, deband: false } },
        { name: qsTr("Anime"), values: { brightness: 0, contrast: 4, saturation: 10, gamma: 0, deband: true } },
        { name: qsTr("Dark scene"), values: { brightness: 6, contrast: 0, saturation: 0, gamma: 20, deband: false } }
    ]

    function matches(values) {
        for (const key in values)
            if (Math.abs(Number(root.picture[key]) - Number(values[key])) > 0.001) return false
        return true
    }

    PanelScroll {
        RowLayout {
            id: upscalerRow
            readonly property var options: Ui.mpv ? Ui.mpv.upscalers : []
            // A saved pick not offered here (AnimeJaNai's folder gone, VSR on another GPU) plays as none and shows as Off.
            readonly property string current: {
                const id = root.picture.upscaler || ""
                return options.some(option => option.id === id) ? id : ""
            }
            readonly property bool nvidiaVsr: options.some(option => option.id === "vsr" && option.nvidia)
            readonly property bool interpolates: options.some(option => option.id === "animejanai" && option.interpolation)
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 10
            Text {
                Layout.fillWidth: true
                text: qsTr("Upscaler")
                color: Theme.onOverlayDim
                font.pixelSize: Ui.sp(Theme.bodySize)
            }
            AppComboBox {
                focusPolicy: Qt.NoFocus
                model: upscalerRow.options.map(option => option.name)
                currentIndex: Math.max(0, upscalerRow.options.findIndex(option => option.id === upscalerRow.current))
                onActivated: (index) => App.settings.picture = { upscaler: upscalerRow.options[index].id }
            }
        }
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: text !== ""
            wrapMode: Text.WordWrap
            text: upscalerRow.current === "vsr"
                  ? (upscalerRow.nvidiaVsr
                     ? qsTr("Scales to the player's size on the graphics card. Turn on Super Resolution in the NVIDIA App; its quality setting applies.")
                     : qsTr("Scales to the player's size on the graphics card."))
                  : upscalerRow.current === "animejanai"
                  ? qsTr("Upscales anime with AI models. The first video at a new size builds an engine first.")
                  : ""
            color: Theme.onOverlayMuted
            font.pixelSize: Ui.sp(14)
        }

        Flow {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 8
            Repeater {
                model: root.presets
                delegate: AppButton {
                    required property var modelData
                    readonly property bool current: root.matches(modelData.values)
                    text: modelData.name
                    secondary: !current
                    fontSize: 16
                    radius: 8
                    onClicked: App.settings.picture = modelData.values
                }
            }
        }

        PanelSlider {
            label: qsTr("Brightness")
            from: -50; to: 50; stepSize: 1
            value: root.picture.brightness
            onMoved: (v) => App.settings.picture = { brightness: v }
        }
        PanelSlider {
            label: qsTr("Contrast")
            from: -50; to: 50; stepSize: 1
            value: root.picture.contrast
            onMoved: (v) => App.settings.picture = { contrast: v }
        }
        PanelSlider {
            label: qsTr("Saturation")
            from: -100; to: 100; stepSize: 1
            value: root.picture.saturation
            onMoved: (v) => App.settings.picture = { saturation: v }
        }
        PanelSlider {
            label: qsTr("Gamma")
            from: -50; to: 50; stepSize: 1
            value: root.picture.gamma
            onMoved: (v) => App.settings.picture = { gamma: v }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 48
            radius: 10
            color: debandHover.hovered ? Theme.overlayLine : "transparent"
            HoverHandler { id: debandHover }
            RowLayout {
                anchors { fill: parent; leftMargin: 14; rightMargin: 14 }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        text: qsTr("Deband")
                        color: Theme.onOverlayDim
                        font.pixelSize: Ui.sp(Theme.bodySize)
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Smooths gradient banding, common in anime")
                        color: Theme.onOverlayMuted
                        font.pixelSize: Ui.sp(14)
                        elide: Text.ElideRight
                    }
                }
                AppSwitch {
                    focusPolicy: Qt.NoFocus
                    checked: root.picture.deband
                    onToggled: App.settings.picture = { deband: checked }
                }
            }
        }

        SettingRow {
            visible: upscalerRow.current === "animejanai" && upscalerRow.interpolates
            label: qsTr("Frame interpolation")
            sublabel: qsTr("Doubles 24 to 30 fps video with RIFE when the graphics card has spare capacity")
            AppSwitch {
                focusPolicy: Qt.NoFocus
                checked: App.settings.frameInterpolation
                onToggled: App.settings.frameInterpolation = checked
            }
        }

        SettingRow {
            label: qsTr("Smooth motion")
            sublabel: qsTr("Evens out 24 and 30 fps on a 60 or 144 Hz screen; only used when it helps")
            AppSwitch {
                focusPolicy: Qt.NoFocus
                checked: App.settings.smoothMotion
                onToggled: App.settings.smoothMotion = checked
            }
        }
    }
}
