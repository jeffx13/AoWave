import QtQuick
import QtQuick.Layouts

GridLayout {
    id: root

    property alias label:      labelText.text
    property alias sublabel:   subText.text
    property alias from:       slider.from
    property alias to:         slider.to
    property alias stepSize:   slider.stepSize
    property alias snapMode:   slider.snapMode
    property alias value:      slider.value
    property alias unitSuffix: slider.unitSuffix
    property alias decimals:   slider.decimals
    property bool  stacked: false
    property bool  commitOnRelease: false
    // no Reset button while NaN
    property real  resetValue: NaN
    readonly property bool overlay: Ui.inOverlay(root)
    signal moved(real v)

    function format(v) {
        return (slider.decimals > 0 ? Number(v).toFixed(slider.decimals) : Math.round(v)) + slider.unitSuffix
    }

    Layout.fillWidth: true
    columns: stacked ? 2 : 3
    columnSpacing: 12
    rowSpacing: stacked ? 2 : 0

    ColumnLayout {
        Layout.row: 0
        Layout.column: 0
        Layout.fillWidth: root.stacked
        spacing: 0
        Text {
            id: labelText
            // a layout's maximum width comes from its children: without this the grid never fills its row
            Layout.fillWidth: true
            elide: Text.ElideRight
            color: root.overlay ? Theme.onOverlayMuted : Theme.textSecondary
            font.pixelSize: Ui.sp(Theme.bodySize)
        }
        Text {
            id: subText
            visible: text.length > 0
            color: root.overlay ? Theme.onOverlayDim : Theme.textMuted
            font.pixelSize: Ui.sp(Theme.captionSize)
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }

    RowLayout {
        Layout.row: 0
        Layout.column: root.stacked ? 1 : 2
        Layout.minimumHeight: isNaN(root.resetValue) ? 0 : resetButton.implicitHeight
        spacing: 8

        AppButton {
            id: resetButton
            visible: !isNaN(root.resetValue) && Math.abs(slider.value - root.resetValue) > 1e-6
            compact: true
            secondary: true
            text: qsTr("Reset")
            Accessible.name: qsTr("Reset %1").arg(root.label)
            onClicked: root.moved(root.resetValue)
        }
        Text {
            Layout.minimumWidth: Math.max(valueMetrics.advanceWidth(root.format(slider.from)),
                                          valueMetrics.advanceWidth(root.format(slider.to)))
            horizontalAlignment: Text.AlignRight
            text: root.format(slider.value)
            color: root.overlay ? Theme.onOverlay : Theme.textPrimary
            font.pixelSize: Ui.sp(Theme.bodySize)
            FontMetrics { id: valueMetrics; font.pixelSize: Ui.sp(Theme.bodySize) }
        }
    }

    AppSlider {
        id: slider
        Layout.row: root.stacked ? 1 : 0
        Layout.column: root.stacked ? 0 : 1
        Layout.columnSpan: root.stacked ? 2 : 1
        Layout.fillWidth: true
        property bool dragged: false
        onMoved: {
            if (root.commitOnRelease && pressed) dragged = true
            else root.moved(value)
        }
        onPressedChanged: {
            if (pressed || !dragged) return
            dragged = false
            root.moved(value)
        }
    }
}
