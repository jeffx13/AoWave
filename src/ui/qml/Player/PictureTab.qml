pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import "../Components"
import App
import ".."

Item {
    id: root

    readonly property var picture: App.settings.picture
    readonly property var presets: [
        { name: qsTr("Normal"), values: { brightness: 0, contrast: 0, saturation: 0, gamma: 0, sharpen: 0, deband: false } },
        { name: qsTr("Vivid"), values: { brightness: 0, contrast: 8, saturation: 25, gamma: 0, sharpen: 0, deband: false } },
        // Flat colour areas show banding; a little sharpening restores line art.
        { name: qsTr("Anime"), values: { brightness: 0, contrast: 4, saturation: 10, gamma: 0, sharpen: 0.3, deband: true } },
        { name: qsTr("Dark scene"), values: { brightness: 6, contrast: 0, saturation: 0, gamma: 20, sharpen: 0, deband: false } }
    ]

    function matches(values) {
        for (const key in values)
            if (Math.abs(Number(root.picture[key]) - Number(values[key])) > 0.001) return false
        return true
    }

    Flickable {
        anchors { fill: parent; margins: 12 }
        contentHeight: pictureCol.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: pictureCol
            width: parent.width
            spacing: 6

            component PanelSlider: LabeledSlider {
                stacked: true
                labelColor: Theme.onOverlayDim
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                Layout.topMargin: 6
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
            PanelSlider {
                label: qsTr("Sharpen")
                from: 0; to: 1; stepSize: 0.05
                decimals: 2
                value: root.picture.sharpen
                onMoved: (v) => App.settings.picture = { sharpen: v }
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
                            font.pixelSize: Globals.sp(Theme.bodySize)
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Smooths the steps in flat gradients, common in anime")
                            color: Theme.onOverlayMuted
                            font.pixelSize: Globals.sp(14)
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
        }
    }
}
