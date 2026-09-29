pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import "../Components"
import App
import ".."

// How plain-text subtitles look. Every change lands on the playing video at once, so the
// subtitle on screen is the real preview; the sample below stands in when there is none.
Item {
    id: root

    // Offered only where installed.
    readonly property var fonts: [""].concat(["Segoe UI", "Arial", "Verdana", "Tahoma", "Trebuchet MS", "Georgia",
                                              "Microsoft YaHei", "Yu Gothic", "Malgun Gothic"]
                                             .filter(family => Qt.fontFamilies().includes(family)))
    readonly property var colors: ["#FFFFFF", "#FFE45C", "#8FE3FF", "#A8F08A", "#FFB8D9"]

    Flickable {
        anchors { fill: parent; margins: 12 }
        contentHeight: styleCol.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: styleCol
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

            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                implicitHeight: Globals.sp(96)
                radius: 10
                clip: true
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#3a4a6b" }
                    GradientStop { position: 1.0; color: "#121826" }
                }
                Rectangle {
                    anchors.centerIn: sample
                    width: sample.implicitWidth + 12
                    height: sample.implicitHeight + 4
                    color: Qt.rgba(0, 0, 0, App.settings.subBackground / 100)
                    visible: App.settings.subBackground > 0
                }
                Text {
                    id: sample
                    anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: Globals.sp(14) }
                    text: qsTr("I'll be back before the festival ends.")
                    color: App.settings.subColor
                    font.family: App.settings.subFont !== "" ? App.settings.subFont : "Arial"   // near mpv's sans-serif
                    font.pixelSize: Globals.sp(22)
                    font.bold: App.settings.subBold
                    style: App.settings.subBackground > 0 ? Text.Normal
                         : App.settings.subOutline > 0 ? Text.Outline
                         : App.settings.subShadow > 0 ? Text.Raised : Text.Normal
                    styleColor: "black"
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
                            required property string modelData
                            readonly property bool current: App.settings.subColor.toUpperCase() === modelData
                            width: Globals.sp(24)
                            height: width
                            radius: width / 2
                            color: modelData
                            border.width: current ? 3 : 1
                            border.color: current ? Theme.accent : Theme.overlayLine
                            Accessible.role: Accessible.RadioButton
                            Accessible.name: modelData
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: App.settings.subColor = swatch.modelData }
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
                label: qsTr("Background Box")
                from: 0; to: 100; stepSize: 5
                unitSuffix: "%"
                value: App.settings.subBackground
                onMoved: (v) => App.settings.subBackground = v
            }

            SettingRow {
                label: qsTr("Restyle Styled Subtitles")
                sublabel: qsTr("Apply this to subtitles that bring their own look, as fansubs often do")
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
                fontSize: 16
                radius: 8
                onClicked: App.settings.resetSubStyle()
            }
        }
    }
}
