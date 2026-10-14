import QtQuick
import QtQuick.Controls

Popup {
    id: popup
    // declared in its opener, or a press on the button closes then reopens
    property bool toggledByParent: false
    readonly property bool overlay: Ui.inOverlay(parent)

    padding: 0
    closePolicy: toggledByParent ? Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                                 : Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function toggle() {
        if (visible) close()
        else open()
    }

    Overlay.modal: Rectangle { color: Theme.scrim }

    enter: Transition { NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: Theme.dur(120); easing.type: Easing.OutCubic } }
    exit:  Transition { NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; duration: Theme.dur(90);  easing.type: Easing.InCubic  } }

    background: Rectangle {
        radius: 12
        color: popup.overlay ? Theme.overlayScrim : Theme.surfaceRaised
        border.color: popup.overlay ? Theme.overlayFillActive : Theme.borderStrong
        border.width: 1
    }
}
