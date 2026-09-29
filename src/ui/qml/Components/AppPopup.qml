import QtQuick
import QtQuick.Controls
import ".."

Popup {
    property alias backgroundRadius: bg.radius
    // For a popup declared inside the button that opens it: a press on that button is then the
    // button's toggle, rather than closing the popup on the press for the click to reopen it.
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
