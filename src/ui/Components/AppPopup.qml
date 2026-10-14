import QtQuick
import QtQuick.Controls

Popup {
    property alias backgroundRadius: bg.radius
    // declared in its opener, or a press on the button closes then reopens
    property bool toggledByParent: false

    padding: 0
    closePolicy: toggledByParent ? Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                                 : Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function toggle() {
        if (visible) close()
        else open()
    }

    Overlay.modal: Rectangle { color: Theme.scrim }

    enter: Transition { NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: 120; easing.type: Easing.OutCubic } }
    exit:  Transition { NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; duration: 90;  easing.type: Easing.InCubic  } }

    background: Card { id: bg }
}
